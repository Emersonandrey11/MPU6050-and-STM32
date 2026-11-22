/**
 * @file  ui.c
 * @brief MPU6050 dashboard on the 240x320 ST7789V TFT.
 *
 * Screen (portrait):
 *   header      title + sensor status
 *   ACCEL       X / Y / Z in g          and the current range
 *   GYRO        X / Y / Z in deg/s      and the current range
 *   TEMP, TILT  die temperature, chip + sample rate, pitch and roll
 *   chart       sweeping plot of accel X / Y / Z (like an oscilloscope)
 *
 * Why it is built this way: the SPI runs at 12 MHz, so a full screen
 * (153,600 bytes) takes ~0.1 s to send - far too long for one main-loop
 * pass. The layout is drawn once, then only what changes is sent:
 *   - every value is a "field" that remembers the text on screen; only the
 *     characters that differ are redrawn, at most CHARS_PER_CALL per call;
 *   - the chart draws one 1-pixel column per CHART_PERIOD_MS and blanks the
 *     column ahead of it (a sweep), so it never needs to scroll.
 */
#include "ui.h"

#include <math.h>
#include <stdint.h>

#include "font.h"
#include "gfx.h"
#include "st7789.h"
#include "telemetry.h"

/* ---- Timing ------------------------------------------------------------- */
#define VALUE_PERIOD_MS   200U    /* numbers: 5 updates per second         */
#define RATE_PERIOD_MS    1000U   /* sample-rate measurement window        */
#define CHART_PERIOD_MS   40U     /* one chart column = 40 ms -> 9.6 s/sweep */
#define CHARS_PER_CALL    2U      /* ~0.15 ms per big glyph at 12 MHz      */

/* ---- Colours (RGB565) --------------------------------------------------- */
#define C_BG      RGB565(0x0A, 0x12, 0x20)
#define C_HDR     RGB565(0x16, 0x29, 0x4A)
#define C_TEXT    RGB565(0xEE, 0xF2, 0xF7)
#define C_MUTED   RGB565(0x8A, 0x97, 0xAB)
#define C_LINE    RGB565(0x1F, 0x32, 0x54)
#define C_GRID    RGB565(0x2A, 0x40, 0x66)
#define C_CURSOR  RGB565(0x3A, 0x5C, 0x94)
#define C_X       RGB565(0xF0, 0xA2, 0x4A)
#define C_Y       RGB565(0x6F, 0x9D, 0xFF)
#define C_Z       RGB565(0x4F, 0xC9, 0xA4)
#define C_TEMP    RGB565(0xFF, 0x7A, 0xA8)
#define C_OK      RGB565(0x4F, 0xC9, 0xA4)
#define C_ERR     RGB565(0xFF, 0x5D, 0x6C)

/* ---- Layout ------------------------------------------------------------- */
#define MARGIN     6U
#define HDR_H      24U
#define COL_W      80U                     /* three value columns */
#define ROW_ACC_L  30U                     /* label row            */
#define ROW_ACC_V  48U                     /* value row            */
#define ROW_GYR_L  78U
#define ROW_GYR_V  96U
#define ROW_MIX_L  126U
#define ROW_MIX_1  144U
#define ROW_MIX_2  166U
#define TILT_X     126U
#define ROW_CHT_L  195U
#define CHART_Y    212U
#define CHART_H    108U                    /* 212..319 */
#define CHART_MID  (CHART_H / 2U)

#define FIELD_MAX  15U

typedef struct {
    uint16_t      x, y;
    uint8_t       len;
    const font_t *font;
    uint16_t      fg, bg;
    char          want[FIELD_MAX];     /* text the screen should show     */
    char          shown[FIELD_MAX];    /* text the screen shows right now */
} field_t;

enum {
    F_STATUS, F_ARANGE, F_AX, F_AY, F_AZ, F_GRANGE, F_GX, F_GY, F_GZ,
    F_TEMP, F_INFO, F_PITCH, F_ROLL, F_COUNT
};

static field_t  s_field[F_COUNT];
static uint8_t  s_scan;                /* field the drawing scan is on   */
static uint32_t s_last_value_ms;
static uint32_t s_last_chart_ms;
static uint32_t s_rate_t0, s_rate_n0, s_rate_hz;
static uint16_t s_chart_x;
static int16_t  s_prev_y[3] = { -1, -1, -1 };
static uint32_t s_lcd_id;

static const uint16_t AXIS_COLOUR[3] = { C_X, C_Y, C_Z };

/* ---- Text helpers ------------------------------------------------------- */

static uint32_t str_len(const char *s)
{
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* Append the decimal digits of v to *p (no sign). */
static void put_uint(char **p, uint32_t v)
{
    char tmp[10];
    uint32_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    while (n > 0U) {
        *(*p)++ = tmp[--n];
    }
}

static void put_str(char **p, const char *s)
{
    while (*s != '\0') {
        *(*p)++ = *s++;
    }
}

/* value / 10^decimals with an explicit sign, e.g. (981, 3) -> "+0.981".
 * Right-aligned in `width` characters; '#' if it does not fit. */
static void fmt_fixed(char *out, uint32_t width, int32_t value, uint32_t decimals)
{
    char     rev[16];
    uint32_t n = 0;
    uint32_t mag = (value < 0) ? (uint32_t)(-value) : (uint32_t)value;

    for (uint32_t d = 0; d < decimals; d++) {
        rev[n++] = (char)('0' + (mag % 10U));
        mag /= 10U;
    }
    if (decimals > 0U) {
        rev[n++] = '.';
    }
    do {
        rev[n++] = (char)('0' + (mag % 10U));
        mag /= 10U;
    } while ((mag != 0U) && (n < sizeof rev - 1U));
    rev[n++] = (value < 0) ? '-' : '+';

    uint32_t i = 0;
    if (n > width) {
        while (i < width) {
            out[i++] = '#';
        }
    } else {
        while (i < width - n) {
            out[i++] = ' ';
        }
        while (n > 0U) {
            out[i++] = rev[--n];
        }
    }
    out[i] = '\0';
}

/* ---- Fields ------------------------------------------------------------- */

static void field_define(uint32_t id, uint16_t x, uint16_t y, uint8_t len,
                         const font_t *font, uint16_t fg, uint16_t bg)
{
    field_t *f = &s_field[id];
    f->x = x;  f->y = y;  f->len = len;  f->font = font;  f->fg = fg;  f->bg = bg;
    for (uint32_t i = 0; i < len; i++) {
        f->want[i]  = ' ';
        f->shown[i] = ' ';      /* the cleared screen already shows spaces */
    }
}

/* Set the text (left-aligned, padded) and colour of a field. A colour
 * change forces every character to be redrawn. */
static void field_set(uint32_t id, const char *text, uint16_t fg)
{
    field_t *f = &s_field[id];
    uint32_t i = 0;

    if (fg != f->fg) {
        f->fg = fg;
        for (uint32_t k = 0; k < f->len; k++) {
            f->shown[k] = '\0';
        }
    }
    for (; (i < f->len) && (text[i] != '\0'); i++) {
        f->want[i] = text[i];
    }
    for (; i < f->len; i++) {
        f->want[i] = ' ';
    }
}

static void field_set_right(uint32_t id, const char *text, uint16_t fg)
{
    char buf[FIELD_MAX + 1U];
    uint32_t len = s_field[id].len;
    uint32_t n = str_len(text);
    uint32_t pad = (n < len) ? (len - n) : 0U;
    uint32_t i = 0;

    while (i < pad) {
        buf[i++] = ' ';
    }
    for (uint32_t k = 0; (i < len) && (text[k] != '\0'); k++) {
        buf[i++] = text[k];
    }
    buf[i] = '\0';
    field_set(id, buf, fg);
}

/* Redraw up to `budget` characters that differ from the screen. Continues
 * where the previous call stopped, so every field gets its turn. */
static void draw_dirty(uint32_t budget)
{
    for (uint32_t visited = 0; visited < F_COUNT; ) {
        field_t *f = &s_field[s_scan];
        int clean = 1;

        for (uint32_t i = 0; i < f->len; i++) {
            if (f->shown[i] != f->want[i]) {
                gfx_char((uint16_t)(f->x + i * f->font->width), f->y, f->want[i], f->font, f->fg, f->bg);
                f->shown[i] = f->want[i];
                clean = 0;
                if (--budget == 0U) {
                    return;
                }
            }
        }
        if (clean) {
            s_scan = (uint8_t)((s_scan + 1U) % F_COUNT);
            visited++;
        }
    }
}

/* ---- Chart -------------------------------------------------------------- */

/* Draw one 1-pixel-wide column of the chart. accel == 0 draws only the
 * background and grid; cursor != 0 uses the brighter sweep colour. */
static void chart_column(uint16_t x, const int16_t *accel, int cursor)
{
    static uint16_t col[CHART_H];
    uint16_t bg = cursor ? C_CURSOR : C_BG;

    for (uint32_t i = 0; i < CHART_H; i++) {
        col[i] = bg;
    }
    if (!cursor) {
        if ((x % 2U) == 0U) {
            col[CHART_MID] = C_GRID;                       /* 0 g          */
        }
        if ((x % 6U) == 0U) {
            col[CHART_MID / 2U] = C_GRID;                  /* +half range  */
            col[CHART_MID + CHART_MID / 2U] = C_GRID;      /* -half range  */
        }
        if ((x % 40U) == 0U) {                             /* every 1.6 s  */
            for (uint32_t i = 0; i < CHART_H; i += 4U) {
                col[i] = C_GRID;
            }
        }
    }
    if (accel != 0) {
        for (uint32_t a = 0; a < 3U; a++) {
            /* full scale (+-32768) -> +-(CHART_MID - 1) pixels, +y is up */
            int32_t y = (int32_t)CHART_MID - ((int32_t)accel[a] * (int32_t)(CHART_MID - 1U)) / 32768;
            int32_t y0 = (s_prev_y[a] < 0) ? y : s_prev_y[a];
            int32_t lo = (y0 < y) ? y0 : y;
            int32_t hi = (y0 < y) ? y : y0;
            for (int32_t i = lo; i <= hi; i++) {   /* vertical segment joins the points */
                col[i] = AXIS_COLOUR[a];
            }
            s_prev_y[a] = (int16_t)y;
        }
    }
    lcd_set_window(x, CHART_Y, x, (uint16_t)(CHART_Y + CHART_H - 1U));
    lcd_write_pixels(col, CHART_H);
    lcd_end();
}

static void chart_step(void)
{
    int16_t accel[3];
    for (uint32_t a = 0; a < 3U; a++) {
        accel[a] = g_telemetry.accel[a];
    }
    chart_column(s_chart_x, accel, 0);
    s_chart_x = (uint16_t)((s_chart_x + 1U) % LCD_WIDTH);
    chart_column(s_chart_x, 0, 1);
}

/* ---- Values ------------------------------------------------------------- */

static int is_mpu6500_family(uint8_t who)
{
    return (who == 0x70U) || (who == 0x71U) || (who == 0x73U) || (who == 0x74U);
}

static const char *chip_name(uint8_t who)
{
    switch (who) {
    case 0x68U: return "MPU-6050";
    case 0x70U: return "MPU-6500";
    case 0x71U: return "MPU-9250";
    case 0x73U: return "MPU-9255";
    case 0x74U: return "MPU-6515";
    default:    return "MPU-????";
    }
}

static int32_t round_to_int(float v)
{
    return (int32_t)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

static void update_values(uint32_t now)
{
    static const uint16_t ACCEL_RANGE[4] = { 2U, 4U, 8U, 16U };
    static const uint16_t GYRO_RANGE[4]  = { 250U, 500U, 1000U, 2000U };
    char buf[FIELD_MAX + 1U];
    char *p;

    uint32_t count = g_telemetry.sample_count;
    if ((now - s_rate_t0) >= RATE_PERIOD_MS) {
        s_rate_hz = ((count - s_rate_n0) * 1000U + (now - s_rate_t0) / 2U) / (now - s_rate_t0);
        s_rate_t0 = now;
        s_rate_n0 = count;
    }

    if ((g_telemetry.status & TM_STATUS_MPU_OK) == 0U) {
        field_set_right(F_STATUS, "NO SENSOR", C_ERR);
        static const uint8_t value_fields[] = { F_AX, F_AY, F_AZ, F_GX, F_GY, F_GZ, F_TEMP, F_PITCH, F_ROLL };
        for (uint32_t i = 0; i < sizeof value_fields; i++) {
            field_set_right(value_fields[i], "---", C_MUTED);
        }
        field_set(F_INFO, "check I2C wires", C_MUTED);
        return;
    }
    field_set_right(F_STATUS, "OK", C_OK);

    uint32_t afs = g_telemetry.accel_fs & 3U;
    uint32_t gfs = g_telemetry.gyro_fs & 3U;

    p = buf;  put_str(&p, FONT_PM);  put_uint(&p, ACCEL_RANGE[afs]);  put_str(&p, " g");  *p = '\0';
    field_set_right(F_ARANGE, buf, C_MUTED);
    p = buf;  put_str(&p, FONT_PM);  put_uint(&p, GYRO_RANGE[gfs]);  put_str(&p, FONT_DEG "/s");  *p = '\0';
    field_set_right(F_GRANGE, buf, C_MUTED);

    int16_t ax[3], gy[3];
    for (uint32_t i = 0; i < 3U; i++) {
        ax[i] = g_telemetry.accel[i];
        gy[i] = g_telemetry.gyro[i];
        /* raw * full_scale / 32768: mg for the accel, 0.1 deg/s for the gyro */
        int32_t mg  = ((int32_t)ax[i] * (int32_t)(2000L << afs)) / 32768;
        int32_t dps = ((int32_t)gy[i] * (int32_t)(2500L << gfs)) / 32768;
        fmt_fixed(buf, 7U, mg, 3U);
        field_set(F_AX + i, buf, AXIS_COLOUR[i]);
        fmt_fixed(buf, 7U, dps, 1U);
        field_set(F_GX + i, buf, AXIS_COLOUR[i]);
    }

    /* Die temperature in 0.01 deg C. The two chip families use different
     * formulas (the same ones as tools/mpu6050_monitor.py):
     * MPU6050 raw/340 + 36.53, MPU6500 family raw/333.87 + 21. */
    uint8_t who = g_telemetry.who_am_i;
    int32_t t_raw = g_telemetry.temp;
    int32_t centi = is_mpu6500_family(who) ? (t_raw * 10000) / 33387 + 2100
                                           : (t_raw * 100) / 340 + 3653;
    fmt_fixed(buf, 6U, centi, 2U);
    field_set(F_TEMP, buf, C_TEMP);

    p = buf;  put_str(&p, chip_name(who));  put_str(&p, " ");  put_uint(&p, s_rate_hz);  put_str(&p, " Hz");  *p = '\0';
    field_set(F_INFO, buf, C_MUTED);

    /* Tilt from gravity only (valid while the board is not accelerating).
     * The Cortex-M3 has no FPU, so this uses software floats: fine at 5 Hz. */
    float fx = (float)ax[0], fy = (float)ax[1], fz = (float)ax[2];
    float roll  = atan2f(fy, fz) * 57.29578f;
    float pitch = atan2f(-fx, sqrtf(fy * fy + fz * fz)) * 57.29578f;
    fmt_fixed(buf, 6U, round_to_int(pitch * 10.0f), 1U);
    field_set(F_PITCH, buf, C_TEXT);
    fmt_fixed(buf, 6U, round_to_int(roll * 10.0f), 1U);
    field_set(F_ROLL, buf, C_TEXT);
}

/* ---- Public ------------------------------------------------------------- */

static void hex24(char *out, uint32_t v)
{
    static const char HEX[] = "0123456789ABCDEF";
    for (int i = 5; i >= 0; i--) {
        out[i] = HEX[v & 0xFU];
        v >>= 4;
    }
    out[6] = '\0';
}

void ui_init(void)
{
    const font_t *fs = &font_small;
    const font_t *fb = &font_big;
    char id[16] = "LCD ";

    s_lcd_id = lcd_init(C_BG);

    /* header */
    gfx_fill_rect(0U, 0U, LCD_WIDTH, HDR_H, C_HDR);
    gfx_text(MARGIN, 5U, "MPU6050 monitor", fs, C_TEXT, C_HDR);

    /* section labels, axis letters, units and dividers */
    gfx_text(MARGIN, ROW_ACC_L, "ACCELEROMETER", fs, C_MUTED, C_BG);
    gfx_text(MARGIN, ROW_GYR_L, "GYROSCOPE", fs, C_MUTED, C_BG);
    for (uint32_t i = 0; i < 3U; i++) {
        const char axis[2] = { (char)('X' + i), '\0' };
        gfx_text((uint16_t)(i * COL_W + 3U), ROW_ACC_V + 2U, axis, fs, AXIS_COLOUR[i], C_BG);
        gfx_text((uint16_t)(i * COL_W + 3U), ROW_GYR_V + 2U, axis, fs, AXIS_COLOUR[i], C_BG);
    }
    gfx_fill_rect(0U, ROW_GYR_L - 6U, LCD_WIDTH, 1U, C_LINE);
    gfx_fill_rect(0U, ROW_MIX_L - 6U, LCD_WIDTH, 1U, C_LINE);
    gfx_fill_rect(0U, ROW_CHT_L - 5U, LCD_WIDTH, 1U, C_LINE);

    gfx_text(MARGIN, ROW_MIX_L, "TEMPERATURE", fs, C_MUTED, C_BG);
    gfx_text(MARGIN + 6U * fb->width + 2U, ROW_MIX_1 + 2U, FONT_DEG "C", fs, C_TEMP, C_BG);
    gfx_text(TILT_X, ROW_MIX_L, "TILT", fs, C_MUTED, C_BG);
    gfx_text(TILT_X, ROW_MIX_1 + 2U, "pitch", fs, C_MUTED, C_BG);
    gfx_text(TILT_X, ROW_MIX_2 + 2U, "roll", fs, C_MUTED, C_BG);
    gfx_text(TILT_X + 38U + 6U * fb->width + 1U, ROW_MIX_1, FONT_DEG, fs, C_TEXT, C_BG);
    gfx_text(TILT_X + 38U + 6U * fb->width + 1U, ROW_MIX_2, FONT_DEG, fs, C_TEXT, C_BG);

    /* chart title, legend and panel ID */
    uint16_t x = gfx_text(MARGIN, ROW_CHT_L, "ACCEL  ", fs, C_MUTED, C_BG);
    x = gfx_text(x, ROW_CHT_L, "X ", fs, C_X, C_BG);
    x = gfx_text(x, ROW_CHT_L, "Y ", fs, C_Y, C_BG);
    (void)gfx_text(x, ROW_CHT_L, "Z", fs, C_Z, C_BG);
    if ((s_lcd_id != 0U) && (s_lcd_id != 0xFFFFFFUL)) {
        hex24(&id[4], s_lcd_id);
    } else {
        id[4] = '-';
        id[5] = '\0';
    }
    (void)gfx_text((uint16_t)(LCD_WIDTH - MARGIN - str_len(id) * fs->width), ROW_CHT_L, id, fs, C_MUTED, C_BG);
    for (uint16_t cx = 0; cx < LCD_WIDTH; cx++) {
        chart_column(cx, 0, 0);
    }

    /* dynamic fields */
    field_define(F_STATUS, (uint16_t)(LCD_WIDTH - MARGIN - 9U * fs->width), 5U, 9U, fs, C_MUTED, C_HDR);
    field_define(F_ARANGE, (uint16_t)(LCD_WIDTH - MARGIN - 8U * fs->width), ROW_ACC_L, 8U, fs, C_MUTED, C_BG);
    field_define(F_GRANGE, (uint16_t)(LCD_WIDTH - MARGIN - 8U * fs->width), ROW_GYR_L, 8U, fs, C_MUTED, C_BG);
    for (uint32_t i = 0; i < 3U; i++) {
        uint16_t vx = (uint16_t)(i * COL_W + 13U);
        field_define(F_AX + i, vx, ROW_ACC_V, 7U, fb, AXIS_COLOUR[i], C_BG);
        field_define(F_GX + i, vx, ROW_GYR_V, 7U, fb, AXIS_COLOUR[i], C_BG);
    }
    field_define(F_TEMP,  MARGIN, ROW_MIX_1, 6U, fb, C_TEMP, C_BG);
    field_define(F_INFO,  MARGIN, ROW_MIX_2 + 2U, 15U, fs, C_MUTED, C_BG);
    field_define(F_PITCH, TILT_X + 38U, ROW_MIX_1, 6U, fb, C_TEXT, C_BG);
    field_define(F_ROLL,  TILT_X + 38U, ROW_MIX_2, 6U, fb, C_TEXT, C_BG);

    field_set_right(F_STATUS, "STARTING", C_MUTED);
    draw_dirty(F_COUNT * FIELD_MAX);
}

void ui_task(uint32_t now_ms)
{
    if ((now_ms - s_last_value_ms) >= VALUE_PERIOD_MS) {
        s_last_value_ms = now_ms;
        update_values(now_ms);
    }

    if (((g_telemetry.status & TM_STATUS_MPU_OK) != 0U) && ((now_ms - s_last_chart_ms) >= CHART_PERIOD_MS)) {
        /* Catch up by at most one period, so a long stall does not cause a burst. */
        s_last_chart_ms = ((now_ms - s_last_chart_ms) >= 2U * CHART_PERIOD_MS) ? now_ms
                                                                                : s_last_chart_ms + CHART_PERIOD_MS;
        chart_step();
        return;
    }

    draw_dirty(CHARS_PER_CALL);
}
