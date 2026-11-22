/**
 * @file  gfx.c
 * @brief Rectangles and fixed-width text on top of the st7789.h window calls.
 *
 * A glyph is sent as one window: each font row is expanded into a small
 * line buffer of RGB565 pixels (bit set -> fg, clear -> bg) and streamed.
 * Drawing the background pixels too means old text is overwritten without
 * clearing first, which avoids flicker.
 */
#include "gfx.h"
#include "st7789.h"

void gfx_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t colour)
{
    if ((w == 0U) || (h == 0U)) {
        return;
    }
    lcd_set_window(x, y, (uint16_t)(x + w - 1U), (uint16_t)(y + h - 1U));
    lcd_fill_pixels(colour, (uint32_t)w * h);
    lcd_end();
}

void gfx_char(uint16_t x, uint16_t y, char c, const font_t *font, uint16_t fg, uint16_t bg)
{
    uint16_t line[16];
    uint8_t  code = (uint8_t)c;

    if ((code < font->first) || (code >= (uint32_t)font->first + font->count)) {
        code = (uint8_t)'?';
    }
    const uint16_t *rows = &font->rows[(uint32_t)(code - font->first) * font->height];

    lcd_set_window(x, y, (uint16_t)(x + font->width - 1U), (uint16_t)(y + font->height - 1U));
    for (uint32_t r = 0; r < font->height; r++) {
        uint16_t bits = rows[r];
        for (uint32_t col = 0; col < font->width; col++) {
            line[col] = (bits & (0x8000U >> col)) ? fg : bg;
        }
        lcd_write_pixels(line, font->width);
    }
    lcd_end();
}

uint16_t gfx_text(uint16_t x, uint16_t y, const char *s, const font_t *font, uint16_t fg, uint16_t bg)
{
    while (*s != '\0') {
        gfx_char(x, y, *s++, font, fg, bg);
        x = (uint16_t)(x + font->width);
    }
    return x;
}
