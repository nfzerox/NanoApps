/*
 * hb_text.c — text/digit rendering using a built-in 8x8 bitmap font.
 *
 * Uses hb_text_push_strip and hb_mipi_set_window from hb_mipi.c to draw
 * each digit as 8 vertical strips of (8*SCALE) wide pixels.
 *
 * Digit rendering is atomic per glyph (set window + 8 strips). The OS
 * sync-screen timer may still preempt between strips on screens with
 * active redraw — keep glyph count small or accept some racing.
 */

#include "hb_sdk.h"

/* Bitmap font, 8 bytes per glyph (bit 7 = leftmost column).
   Indices 0..9 are decimal digits, 10..15 are hex A..F. */
static const uint8_t digit_glyphs[16][8] = {
    {0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00}, /* 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 1 */
    {0x3C,0x42,0x04,0x08,0x30,0x40,0x7E,0x00}, /* 2 */
    {0x7C,0x02,0x02,0x3C,0x02,0x02,0x7C,0x00}, /* 3 */
    {0x0C,0x1C,0x2C,0x4C,0x7E,0x0C,0x0C,0x00}, /* 4 */
    {0x7E,0x40,0x40,0x7C,0x02,0x02,0x7C,0x00}, /* 5 */
    {0x3C,0x40,0x40,0x7C,0x42,0x42,0x3C,0x00}, /* 6 */
    {0x7E,0x02,0x04,0x08,0x10,0x20,0x20,0x00}, /* 7 */
    {0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00}, /* 8 */
    {0x3C,0x42,0x42,0x3E,0x02,0x02,0x3C,0x00}, /* 9 */
    {0x18,0x24,0x42,0x42,0x7E,0x42,0x42,0x00}, /* A */
    {0x7C,0x42,0x42,0x7C,0x42,0x42,0x7C,0x00}, /* B */
    {0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00}, /* C */
    {0x7C,0x42,0x42,0x42,0x42,0x42,0x7C,0x00}, /* D */
    {0x7E,0x40,0x40,0x7C,0x40,0x40,0x7E,0x00}, /* E */
    {0x7E,0x40,0x40,0x7C,0x40,0x40,0x40,0x00}, /* F */
};

/* Declared in hb_mipi.c */
extern void hb_text_push_strip(uint8_t cmd, uint8_t row_byte,
                               hb_color_t fg, hb_color_t bg);
extern void hb_mipi_set_window(int16_t x, int16_t y, int16_t w, int16_t h);

void hb_draw_digit(int16_t x, int16_t y, uint8_t digit,
                   hb_color_t fg, hb_color_t bg)
{
    if (digit > 15) digit = 0;  /* now supports 0-9 + A..F as 10..15 */
    hb_mipi_set_window(x, y, HB_DIGIT_W, HB_DIGIT_H);
    for (int row = 0; row < 8; row++) {
        hb_text_push_strip(row == 0 ? 0x2C : 0x3C,
                           digit_glyphs[digit][row], fg, bg);
    }
}

/* Render a 32-bit hex value as 8 hex characters (no '0x' prefix).
   Uses HB_DIGIT_W per character — total width = 8 * HB_DIGIT_W. */
void hb_draw_hex32(int16_t x, int16_t y, uint32_t v,
                   hb_color_t fg, hb_color_t bg)
{
    for (int i = 7; i >= 0; i--) {
        uint8_t nib = (v >> (i * 4)) & 0xF;
        hb_draw_digit(x, y, nib, fg, bg);
        x += HB_DIGIT_W;
    }
}

/* Render 16-bit hex value as 4 hex chars. Fits 240px screen. */
void hb_draw_hex16(int16_t x, int16_t y, uint16_t v,
                   hb_color_t fg, hb_color_t bg)
{
    for (int i = 3; i >= 0; i--) {
        uint8_t nib = (v >> (i * 4)) & 0xF;
        hb_draw_digit(x, y, nib, fg, bg);
        x += HB_DIGIT_W;
    }
}

/* Decimal decomposition without libgcc's __aeabi_uidivmod —
   subtract-based. Slow for big numbers but trivial for ≤7 digits. */
static const uint32_t pow10_tbl[10] = {
    1, 10, 100, 1000, 10000, 100000, 1000000,
    10000000, 100000000, 1000000000
};

void hb_draw_uint(int16_t x, int16_t y, uint32_t n, uint8_t n_digits,
                  hb_color_t fg, hb_color_t bg)
{
    if (n_digits == 0) return;
    if (n_digits > 10) n_digits = 10;

    /* Clamp n to fit in n_digits without using divmod. */
    uint32_t cap = pow10_tbl[n_digits];
    while (n >= cap) n -= cap;

    /* Extract each digit by repeated subtraction. */
    uint8_t digits[10];
    for (int i = 0; i < n_digits; i++) {
        uint32_t p = pow10_tbl[n_digits - 1 - i];
        digits[i] = 0;
        while (n >= p) { n -= p; digits[i]++; }
    }

    for (int i = 0; i < n_digits; i++) {
        hb_draw_digit(x + i * HB_DIGIT_W, y, digits[i], fg, bg);
    }
}
