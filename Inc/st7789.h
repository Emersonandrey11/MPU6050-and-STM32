/**
 * @file  st7789.h
 * @brief ST7789V 240x320 TFT driver, 4-wire SPI, RGB565 (16 bits per pixel).
 *
 * Drawing model: open a rectangular window, stream its pixels left to right,
 * top to bottom, then close it:
 *
 *     lcd_set_window(x0, y0, x1, y1);
 *     lcd_write_pixels(buf, n);   // or lcd_fill_pixels(colour, n)
 *     lcd_end();
 *
 * gfx.c builds rectangles and text on these four calls only, so the same
 * drawing code also runs on a PC (tools/lcd_preview) to preview the screen.
 */
#ifndef ST7789_H
#define ST7789_H

#include <stdint.h>

#define LCD_WIDTH   240U
#define LCD_HEIGHT  320U

/* Reset and configure the panel, clear it to `background`, switch the
 * backlight on. Returns the 24-bit panel ID (0x858552 for an ST7789V), or 0
 * when SDO is not connected. Blocks for about 0.5 s (reset timings).
 * The backlight is set to LCD_BRIGHTNESS (board.h). */
uint32_t lcd_init(uint16_t background);

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void lcd_write_pixels(const uint16_t *pixels, uint32_t count);
void lcd_fill_pixels(uint16_t colour, uint32_t count);
void lcd_end(void);

/* Backlight brightness 0..100 % (20 kHz PWM on TIM3_CH3, average current
 * scales with it). */
void lcd_backlight(uint8_t percent);

#endif /* ST7789_H */
