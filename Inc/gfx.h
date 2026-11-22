/**
 * @file  gfx.h
 * @brief Rectangles and fixed-width text on top of the st7789.h window calls.
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "font.h"

/* 8-bit R, G, B -> RGB565, evaluated at compile time for constants */
#define RGB565(r, g, b)  ((uint16_t)((((r) & 0xF8U) << 8) | (((g) & 0xFCU) << 3) | ((b) >> 3)))

void     gfx_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t colour);
void     gfx_char(uint16_t x, uint16_t y, char c, const font_t *font, uint16_t fg, uint16_t bg);
uint16_t gfx_text(uint16_t x, uint16_t y, const char *s, const font_t *font, uint16_t fg, uint16_t bg);

#endif /* GFX_H */
