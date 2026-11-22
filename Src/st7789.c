/**
 * @file  st7789.c
 * @brief ST7789V TFT driver on SPI1 (Sitronix ST7789V datasheet).
 *
 * Every transfer: CS low, D/C low for the command byte, D/C high for its
 * parameters or pixel data, CS high. Commands use 8-bit SPI frames; pixel
 * data uses 16-bit frames, so one RGB565 pixel is one write to SPI1_DR
 * (high byte first, as the panel expects with COLMOD = 0x55).
 */
#include "st7789.h"
#include "board.h"
#include "spi.h"
#include "stm32f103_regs.h"
#include "timebase.h"

/* Command set (datasheet, "System function command" table) */
#define ST_SWRESET  0x01U
#define ST_RDDID    0x04U
#define ST_SLPOUT   0x11U
#define ST_NORON    0x13U
#define ST_INVOFF   0x20U
#define ST_INVON    0x21U
#define ST_DISPON   0x29U
#define ST_CASET    0x2AU
#define ST_RASET    0x2BU
#define ST_RAMWR    0x2CU
#define ST_MADCTL   0x36U
#define ST_COLMOD   0x3AU

#define COLMOD_RGB565  0x55U   /* 65k colours, 16 bits per pixel */

/* Backlight PWM: TIM3 counts at TIM_APB1_HZ = 48 MHz. 2400 counts per
 * period gives 20 kHz (too fast to see flicker, above hearing) and 24 counts
 * per percent. */
#define BL_PWM_HZ      20000UL
#define BL_PWM_PERIOD  (TIM_APB1_HZ / BL_PWM_HZ)

static inline void pin_set(uint32_t port, uint32_t pin)   { GPIO_BSRR(port) = 1UL << pin; }
static inline void pin_clear(uint32_t port, uint32_t pin) { GPIO_BRR(port) = 1UL << pin; }

/* Start a command: CS low, command byte with D/C low, then D/C high so the
 * following bytes are parameters or pixel data. CS stays low. */
static void begin_command(uint8_t cmd)
{
    (void)spi1_frame16(0);
    pin_clear(GPIOA_BASE, LCD_CS_PIN);
    pin_clear(GPIOA_BASE, LCD_DC_PIN);
    (void)spi1_send(cmd);
    (void)spi1_flush();                 /* D/C must not change mid-byte */
    pin_set(GPIOA_BASE, LCD_DC_PIN);
}

static void write_command(uint8_t cmd, const uint8_t *params, uint32_t len)
{
    begin_command(cmd);
    for (uint32_t i = 0; i < len; i++) {
        (void)spi1_send(params[i]);
    }
    (void)spi1_flush();
    pin_set(GPIOA_BASE, LCD_CS_PIN);
}

/* RDDID: one dummy clock, then ID1 ID2 ID3 (datasheet "RDDID (04h)").
 * Needs the SDO wire; without it MISO's pull-down gives 0. */
static uint32_t read_id(void)
{
    uint8_t b[4] = { 0U, 0U, 0U, 0U };

    begin_command(ST_RDDID);
    for (uint32_t i = 0; i < 4U; i++) {
        if (spi1_transfer(0x00U, &b[i]) != 0) {
            break;
        }
    }
    pin_set(GPIOA_BASE, LCD_CS_PIN);

    uint32_t raw = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    return (raw >> 7) & 0xFFFFFFUL;     /* drop the dummy bit */
}

/* TIM3 channel 3 in PWM mode 1 on PB0 (TIM3_CH3, no remap). */
static void backlight_pwm_init(void)
{
    RCC_APB1ENR |= RCC_APB1ENR_TIM3EN;

    TIM3_PSC   = 0U;                              /* count at 48 MHz         */
    TIM3_ARR   = BL_PWM_PERIOD - 1U;              /* 2400 counts -> 20 kHz   */
    TIM3_CCR3  = 0U;                              /* start dark              */
    TIM3_CCMR2 = TIM_CCMR2_OC3M_PWM1 | TIM_CCMR2_OC3PE;
    TIM3_CCER  = TIM_CCER_CC3E;
    TIM3_CR1   = TIM_CR1_ARPE;
    TIM3_EGR   = TIM_EGR_UG;                      /* load PSC/ARR/CCR3 now   */
    TIM3_CR1  |= TIM_CR1_CEN;

    /* Hand PB0 to the timer only now that its output is defined (low). */
    gpio_config(GPIOB_BASE, LCD_BL_PIN, GPIO_MODE_AF_PP_2MHZ);
}

void lcd_backlight(uint8_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }
    /* CCR3 = PERIOD at 100 %: greater than ARR, so the output stays high. The new
     * value takes effect at the next period (OC3PE preload): no glitch. */
    TIM3_CCR3 = (uint32_t)percent * (BL_PWM_PERIOD / 100U);
}

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    const uint8_t cols[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    const uint8_t rows[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };

    write_command(ST_CASET, cols, 4U);
    write_command(ST_RASET, rows, 4U);
    begin_command(ST_RAMWR);            /* CS stays low until lcd_end() */
    (void)spi1_frame16(1);
}

void lcd_write_pixels(const uint16_t *pixels, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        (void)spi1_send(pixels[i]);
    }
}

void lcd_fill_pixels(uint16_t colour, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        (void)spi1_send(colour);
    }
}

void lcd_end(void)
{
    (void)spi1_flush();
    pin_set(GPIOA_BASE, LCD_CS_PIN);
    (void)spi1_frame16(0);
}

uint32_t lcd_init(uint16_t background)
{
    RCC_APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN;

    /* Idle levels before the pins become outputs: CS high (deselected),
     * RESET high (running), backlight off until the screen is cleared
     * (PB0 is a GPIO low until the PWM timer takes it over). */
    pin_set(GPIOA_BASE, LCD_CS_PIN);
    pin_set(GPIOA_BASE, LCD_DC_PIN);
    pin_set(GPIOA_BASE, LCD_RST_PIN);
    pin_clear(GPIOB_BASE, LCD_BL_PIN);
    gpio_config(GPIOA_BASE, LCD_CS_PIN,  GPIO_MODE_OUT_PP_50MHZ);
    gpio_config(GPIOA_BASE, LCD_DC_PIN,  GPIO_MODE_OUT_PP_50MHZ);
    gpio_config(GPIOA_BASE, LCD_RST_PIN, GPIO_MODE_OUT_PP_2MHZ);
    gpio_config(GPIOB_BASE, LCD_BL_PIN,  GPIO_MODE_OUT_PP_2MHZ);
    backlight_pwm_init();

    spi1_init();

    /* Hardware reset: low for at least 10 us, then wait 120 ms before
     * sending commands (datasheet "Reset timing"). */
    delay_ms(5);
    pin_clear(GPIOA_BASE, LCD_RST_PIN);
    delay_ms(20);
    pin_set(GPIOA_BASE, LCD_RST_PIN);
    delay_ms(120);

    write_command(ST_SWRESET, 0, 0U);
    delay_ms(150);
    write_command(ST_SLPOUT, 0, 0U);    /* leave sleep: 120 ms before the next SLPIN */
    delay_ms(120);

    const uint8_t colmod = COLMOD_RGB565;
    const uint8_t madctl = LCD_MADCTL;
    write_command(ST_COLMOD, &colmod, 1U);
    write_command(ST_MADCTL, &madctl, 1U);
    write_command(LCD_INVERT ? ST_INVON : ST_INVOFF, 0, 0U);
    write_command(ST_NORON, 0, 0U);
    delay_ms(10);

    /* Clear the frame memory before the display turns on, so no random
     * pixels from power-up are visible. */
    lcd_set_window(0U, 0U, LCD_WIDTH - 1U, LCD_HEIGHT - 1U);
    lcd_fill_pixels(background, (uint32_t)LCD_WIDTH * LCD_HEIGHT);
    lcd_end();

    write_command(ST_DISPON, 0, 0U);
    delay_ms(20);
    lcd_backlight(LCD_BRIGHTNESS);

    return read_id();
}
