/**
 * @file  lcd_preview.c
 * @brief Run the firmware's TFT dashboard (Src/ui.c, gfx.c, fonts) on a PC.
 *
 * The four st7789.h window calls are replaced by a 240x320 framebuffer, and a
 * simulated sensor slowly tilts back and forth. After 12 s of simulated time
 * the framebuffer is written as a PPM image. It also measures how many bytes
 * each ui_task() call would send over SPI, to check the per-call budget.
 *
 * Build and run from the project root:
 *   gcc -std=c11 -O2 -Wall -Wextra -IInc tools/lcd_preview/lcd_preview.c \
 *       Src/ui.c Src/gfx.c Src/font_data.c -lm -o build/lcd_preview
 *   ./build/lcd_preview build/lcd_preview.ppm            # sensor working
 *   ./build/lcd_preview build/no_sensor.ppm --no-sensor  # sensor missing
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "st7789.h"
#include "telemetry.h"
#include "ui.h"

volatile telemetry_t g_telemetry;

/* ---- fake panel --------------------------------------------------------- */
static uint16_t s_fb[LCD_HEIGHT][LCD_WIDTH];
static uint16_t s_x0, s_x1, s_y1, s_cx, s_cy;
static uint64_t s_bytes;                     /* SPI bytes, incl. commands */

uint32_t lcd_init(uint16_t background)
{
    for (uint32_t y = 0; y < LCD_HEIGHT; y++) {
        for (uint32_t x = 0; x < LCD_WIDTH; x++) {
            s_fb[y][x] = background;
        }
    }
    return 0x858552UL;                       /* what a real ST7789V answers */
}

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    s_x0 = x0; s_x1 = x1; s_y1 = y1; s_cx = x0; s_cy = y0;
    s_bytes += 3U + 8U;                      /* CASET, RASET, RAMWR + 8 params */
}

static void put(uint16_t c)
{
    if ((s_cy <= s_y1) && (s_cx < LCD_WIDTH) && (s_cy < LCD_HEIGHT)) {
        s_fb[s_cy][s_cx] = c;
    }
    if (++s_cx > s_x1) {
        s_cx = s_x0;
        s_cy++;
    }
    s_bytes += 2U;
}

void lcd_write_pixels(const uint16_t *pixels, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        put(pixels[i]);
    }
}

void lcd_fill_pixels(uint16_t colour, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        put(colour);
    }
}

void lcd_end(void) {}
void lcd_backlight(uint8_t percent) { (void)percent; }

/* ---- simulated sensor (MPU6500 family, +-2 g, +-250 deg/s) -------------- */
static void sample(uint32_t t_ms)
{
    const double PI = 3.14159265358979;
    double t = t_ms / 1000.0;
    double roll  = 25.0 * sin(2.0 * PI * t / 4.0);
    double pitch = 12.0 * sin(2.0 * PI * t / 6.0 + 1.0);
    double droll  = 25.0 * 2.0 * PI / 4.0 * cos(2.0 * PI * t / 4.0);
    double dpitch = 12.0 * 2.0 * PI / 6.0 * cos(2.0 * PI * t / 6.0 + 1.0);
    double r = roll * PI / 180.0, p = pitch * PI / 180.0;
    double g[3] = { -sin(p), sin(r) * cos(p), cos(r) * cos(p) };
    double noise = 0.004 * sin(t_ms * 1.7) ;

    for (int i = 0; i < 3; i++) {
        g_telemetry.accel[i] = (int16_t)lrint((g[i] + noise) * 16384.0);
    }
    g_telemetry.gyro[0] = (int16_t)lrint(droll * 131.0);
    g_telemetry.gyro[1] = (int16_t)lrint(dpitch * 131.0);
    g_telemetry.gyro[2] = (int16_t)lrint(-1.8 * 131.0);
    g_telemetry.temp    = (int16_t)lrint((27.4 - 21.0) * 333.87);
    g_telemetry.sample_count = g_telemetry.sample_count + 1U;
}

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "lcd_preview.ppm";
    int no_sensor = (argc > 2) && (strcmp(argv[2], "--no-sensor") == 0);
    uint64_t worst = 0, calls = 0, total = 0;

    g_telemetry.who_am_i = 0x70U;
    ui_init();
    printf("ui_init: %llu SPI bytes (%.0f ms at 12 MHz)\n",
           (unsigned long long)s_bytes, s_bytes * 8.0 / 12e6 * 1e3);

    /* ui_init took ~0.6 s. Sampling stops at 12.5 s so the last value
     * update (12.6 s) is fully drawn before the picture is taken. */
    for (uint32_t t = 600; t <= 12750; t++) {
        if ((t == 900) && !no_sensor) {
            g_telemetry.status |= TM_STATUS_MPU_OK;
        }
        if (!no_sensor && (t % 10U) == 0U && t <= 12500U) {
            sample(t);
        }
        for (int k = 0; k < 4; k++) {                  /* several loop passes per ms */
            uint64_t before = s_bytes;
            ui_task(t);
            uint64_t n = s_bytes - before;
            worst = (n > worst) ? n : worst;
            total += n;
            calls++;
        }
    }
    printf("ui_task: worst call %llu SPI bytes = %.2f ms at 12 MHz; average %.1f%% of SPI time busy\n",
           (unsigned long long)worst, worst * 8.0 / 12e6 * 1e3, total * 8.0 / 12e6 / 12.15 * 100.0);
    (void)calls;

    FILE *f = fopen(out, "wb");
    if (f == NULL) {
        perror(out);
        return 1;
    }
    fprintf(f, "P6\n%u %u\n255\n", LCD_WIDTH, LCD_HEIGHT);
    for (uint32_t y = 0; y < LCD_HEIGHT; y++) {
        for (uint32_t x = 0; x < LCD_WIDTH; x++) {
            uint16_t c = s_fb[y][x];
            unsigned char rgb[3] = {
                (unsigned char)(((c >> 11) & 0x1F) * 255 / 31),
                (unsigned char)(((c >> 5) & 0x3F) * 255 / 63),
                (unsigned char)((c & 0x1F) * 255 / 31),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("wrote %s\n", out);
    return 0;
}
