/*
 * hb_mipi.c — direct MIPI DSIM MMIO driver for N7G LCD.
 *
 * Writes to the display DSIM controller at 0x3d800000 to send DCS
 * commands directly to the LCD panel. Used by hb_fill_rect /
 * hb_fill_screen.
 *
 * Pixel format: assumes the OS has set 24bpp RGB888 (DCS 0x3A = 0x77),
 * which is N7G's default. We write 3 bytes per pixel (R, G, B).
 */

#include "hb_sdk.h"

/* Freestanding build has no libc. Compiler still synthesizes memset
   for zero-init patterns. Provide it here so the SDK supplies it to
   any app that needs it. Weak so apps that already ship their own
   memset/memcpy (e.g. clock.c, paint.c) keep working unchanged. */
__attribute__((weak)) void *memset(void *s, int c, unsigned int n)
{
    unsigned char *p = (unsigned char *)s;
    for (unsigned i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

__attribute__((weak)) void *memcpy(void *dst, const void *src, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

#define MIPI_BASE     0x3d800000u
#define DSIM_CONFIG   (*(volatile uint32_t *)(MIPI_BASE + 0x10))
#define DSIM_PKTHDR   (*(volatile uint32_t *)(MIPI_BASE + 0x34))
#define DSIM_PAYLOAD  (*(volatile uint32_t *)(MIPI_BASE + 0x38))
#define DSIM_FIFOCTRL (*(volatile uint32_t *)(MIPI_BASE + 0x44))

#define DSIM_FullLSfr        (1u << 21)
#define DSIM_EmptyHSfr       (1u << 22)
#define DSIM_EmptyLSfr       (1u << 20)
#define DSIM_TYPE_LONG_WRITE 0x29

/* Forward decls of shadow-framebuffer hooks from hb_screenshot.c. */
extern void hb_screenshot_shadow_set_window(int16_t x, int16_t y,
                                            int16_t w, int16_t h);
extern void hb_screenshot_shadow_fill(int16_t x, int16_t y,
                                      int16_t w, int16_t h,
                                      hb_color_t color);
extern void hb_screenshot_shadow_text_strip(uint8_t row_byte, uint8_t scale,
                                            hb_color_t fg, hb_color_t bg);
extern void hb_screenshot_shadow_blit_glyph(int16_t x, int16_t y,
                                            int16_t w, int16_t h,
                                            const uint8_t *gray,
                                            uint8_t threshold,
                                            hb_color_t fg, hb_color_t bg);

/* Spin until both header- and payload-FIFOs report empty (long packet
   finished transmitting). Caps iteration count to avoid wedging on
   chip stalls. */
static inline void wait_long_done(void) {
    for (uint32_t t = 0; t < 50000000; t++) {
        if ((DSIM_FIFOCTRL & (DSIM_EmptyHSfr | DSIM_EmptyLSfr))
            == (DSIM_EmptyHSfr | DSIM_EmptyLSfr)) return;
        __asm__ volatile("nop");
    }
}
static inline void wait_payload_fifo_space(void) {
    for (uint32_t t = 0; t < 10000000; t++) {
        if (!(DSIM_FIFOCTRL & DSIM_FullLSfr)) return;
        __asm__ volatile("nop");
    }
}

/* Push a long-write packet to the LCD: CDS payload bytes followed by
   the long-write packet header that triggers transmission.
   Pixel data must already be in `data[0..length)`. */
static void mipi_long(const uint8_t *data, uint32_t length) {
    DSIM_CONFIG &= ~(1u << 28);
    uint32_t payload = 0; uint32_t i;
    for (i = 0; i < length; i++) {
        payload = (payload >> 8) | ((uint32_t)data[i] << 24);
        if ((i & 3) == 3) { wait_payload_fifo_space(); DSIM_PAYLOAD = payload; }
    }
    uint32_t rem = i & 3;
    if (rem) {
        wait_payload_fifo_space();
        if (rem == 3)      DSIM_PAYLOAD = (payload >> 8);
        else if (rem == 2) DSIM_PAYLOAD = (payload >> 16);
        else if (rem == 1) DSIM_PAYLOAD = (payload >> 24);
    }
    DSIM_PKTHDR = DSIM_TYPE_LONG_WRITE | ((length & 0xFFFFu) << 8);
    wait_long_done();
}

/* Set the LCD's drawable window via DCS SET_COLUMN_ADDRESS (0x2A) and
   SET_PAGE_ADDRESS (0x2B). All subsequent writes target this region. */
static void mipi_set_window(int16_t x, int16_t y, int16_t w, int16_t h) {
    int16_t x1 = x + w - 1, y1 = y + h - 1;
    uint8_t bc[5] = { 0x2A, (uint8_t)((x  >> 8) & 0xFF), (uint8_t)(x  & 0xFF),
                            (uint8_t)((x1 >> 8) & 0xFF), (uint8_t)(x1 & 0xFF) };
    mipi_long(bc, 5);
    uint8_t br[5] = { 0x2B, (uint8_t)((y  >> 8) & 0xFF), (uint8_t)(y  & 0xFF),
                            (uint8_t)((y1 >> 8) & 0xFF), (uint8_t)(y1 & 0xFF) };
    mipi_long(br, 5);
}

/* Push a chunk of solid-color pixels. cmd is 0x2C (MEMORY_WRITE_START)
   for the first chunk in a window, 0x3C (MEMORY_WRITE_CONTINUE) after.
   Limit ~600 pixels/chunk to stay under FIFO size. */
static void mipi_fill_chunk(uint8_t cmd_byte, hb_color_t color, uint32_t n_pixels) {
    uint32_t total_len = 1u + n_pixels * 3u;
    DSIM_CONFIG &= ~(1u << 28);
    uint32_t payload = 0;
    uint32_t i = 0;
    uint8_t r = (uint8_t)(color >> 16);
    uint8_t g = (uint8_t)(color >> 8);
    uint8_t b = (uint8_t)color;

    payload = (payload >> 8) | ((uint32_t)cmd_byte << 24);
    i++;
    for (uint32_t p = 0; p < n_pixels; p++) {
        payload = (payload >> 8) | ((uint32_t)r << 24);
        if ((i & 3) == 3) { wait_payload_fifo_space(); DSIM_PAYLOAD = payload; }
        i++;
        payload = (payload >> 8) | ((uint32_t)g << 24);
        if ((i & 3) == 3) { wait_payload_fifo_space(); DSIM_PAYLOAD = payload; }
        i++;
        payload = (payload >> 8) | ((uint32_t)b << 24);
        if ((i & 3) == 3) { wait_payload_fifo_space(); DSIM_PAYLOAD = payload; }
        i++;
    }
    uint32_t rem = i & 3;
    if (rem) {
        wait_payload_fifo_space();
        if (rem == 3)      DSIM_PAYLOAD = (payload >> 8);
        else if (rem == 2) DSIM_PAYLOAD = (payload >> 16);
        else if (rem == 1) DSIM_PAYLOAD = (payload >> 24);
    }
    DSIM_PKTHDR = DSIM_TYPE_LONG_WRITE | ((total_len & 0xFFFFu) << 8);
    wait_long_done();
}

void hb_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, hb_color_t color) {
    hb_screenshot_shadow_fill(x, y, w, h, color);
    mipi_set_window(x, y, w, h);
    const uint32_t total = (uint32_t)w * (uint32_t)h, CHUNK = 600;
    uint32_t pushed = 0;
    int first = 1;
    while (pushed < total) {
        uint32_t n = (total - pushed > CHUNK) ? CHUNK : (total - pushed);
        mipi_fill_chunk(first ? 0x2C : 0x3C, color, n);
        pushed += n;
        first = 0;
    }
}

void hb_fill_screen(hb_color_t color) {
    hb_fill_rect(0, 0, HB_SCREEN_W, HB_SCREEN_H, color);
}

/* Used by hb_text.c: push one horizontal strip (HB_DIGIT_W pixels wide,
   HB_TEXT_SCALE pixels tall) of foreground/background pixels driven by
   one byte of glyph bitmap. Single MIPI long-write per strip. */
void hb_text_push_strip(uint8_t cmd, uint8_t row_byte,
                        hb_color_t fg, hb_color_t bg) {
    hb_screenshot_shadow_text_strip(row_byte, HB_TEXT_SCALE, fg, bg);

    const uint32_t strip_pixels = HB_DIGIT_W * HB_TEXT_SCALE;
    const uint32_t total_len = 1u + strip_pixels * 3u;
    DSIM_CONFIG &= ~(1u << 28);
    uint32_t payload = 0; uint32_t i = 0;
    uint8_t fr=(fg>>16), fgg=(fg>>8), fb=(uint8_t)fg;
    uint8_t br=(bg>>16), bgg=(bg>>8), bb=(uint8_t)bg;

    payload = (payload >> 8) | ((uint32_t)cmd << 24); i++;
    for (int sub_v = 0; sub_v < HB_TEXT_SCALE; sub_v++) {
        for (int bit = 7; bit >= 0; bit--) {
            int on = (row_byte >> bit) & 1;
            uint8_t r = on ? fr  : br;
            uint8_t g = on ? fgg : bgg;
            uint8_t b = on ? fb  : bb;
            for (int sc = 0; sc < HB_TEXT_SCALE; sc++) {
                payload = (payload >> 8) | ((uint32_t)r << 24);
                if ((i & 3) == 3) { wait_payload_fifo_space(); DSIM_PAYLOAD = payload; } i++;
                payload = (payload >> 8) | ((uint32_t)g << 24);
                if ((i & 3) == 3) { wait_payload_fifo_space(); DSIM_PAYLOAD = payload; } i++;
                payload = (payload >> 8) | ((uint32_t)b << 24);
                if ((i & 3) == 3) { wait_payload_fifo_space(); DSIM_PAYLOAD = payload; } i++;
            }
        }
    }
    uint32_t rem = i & 3;
    if (rem) {
        wait_payload_fifo_space();
        if (rem == 3)      DSIM_PAYLOAD = (payload >> 8);
        else if (rem == 2) DSIM_PAYLOAD = (payload >> 16);
        else if (rem == 1) DSIM_PAYLOAD = (payload >> 24);
    }
    DSIM_PKTHDR = DSIM_TYPE_LONG_WRITE | ((total_len & 0xFFFFu) << 8);
    wait_long_done();
}

/* Helper only visible to hb_text.c */
void hb_mipi_set_window(int16_t x, int16_t y, int16_t w, int16_t h) {
    hb_screenshot_shadow_set_window(x, y, w, h);
    mipi_set_window(x, y, w, h);
}

/* Blit a grayscale bitmap glyph (w*h bytes, 8bpp) to the LCD. Thresholds
   each source byte: >= threshold draws fg, else (if draw_bg) draws bg
   else SKIPPED (so under-text isn't clobbered). Chunks at ~400 px to
   stay under FIFO size.
   Drawing pixels in raster scan order with set_window'd target rect. */
void hb_mipi_blit_glyph(int16_t x, int16_t y,
                        int16_t w, int16_t h,
                        const uint8_t *gray, uint8_t threshold,
                        hb_color_t fg, hb_color_t bg, int draw_bg)
{
    if (w <= 0 || h <= 0) return;
    hb_screenshot_shadow_blit_glyph(x, y, w, h, gray, threshold, fg, bg);
    mipi_set_window(x, y, w, h);

    const uint32_t total = (uint32_t)w * (uint32_t)h;
    const uint32_t CHUNK = 400;
    uint32_t pushed = 0;
    int first = 1;
    uint8_t fr=(uint8_t)(fg>>16), fgg=(uint8_t)(fg>>8), fb=(uint8_t)fg;
    uint8_t br=(uint8_t)(bg>>16), bgg=(uint8_t)(bg>>8), bb=(uint8_t)bg;

    /* If draw_bg is false and any pixel falls below threshold, we'd
       need to leave the existing framebuffer alone — but DSIM can't
       "skip" pixels in a memory_write. Workaround: substitute the
       caller's bg color for under-threshold pixels even when
       draw_bg=false (caller passes fg/bg matching their backdrop). */

    while (pushed < total) {
        uint32_t n = (total - pushed > CHUNK) ? CHUNK : (total - pushed);
        uint32_t total_len = 1u + n * 3u;
        DSIM_CONFIG &= ~(1u << 28);
        uint32_t payload = 0; uint32_t i = 0;
        uint8_t cmd = first ? 0x2C : 0x3C;
        payload = (payload >> 8) | ((uint32_t)cmd << 24); i++;

        for (uint32_t p = 0; p < n; p++) {
            uint8_t v = gray[pushed + p];
            int on = (v >= threshold);
            uint8_t r = on ? fr  : br;
            uint8_t g = on ? fgg : bgg;
            uint8_t b = on ? fb  : bb;
            payload = (payload >> 8) | ((uint32_t)r << 24);
            if ((i & 3) == 3) { wait_payload_fifo_space(); DSIM_PAYLOAD = payload; } i++;
            payload = (payload >> 8) | ((uint32_t)g << 24);
            if ((i & 3) == 3) { wait_payload_fifo_space(); DSIM_PAYLOAD = payload; } i++;
            payload = (payload >> 8) | ((uint32_t)b << 24);
            if ((i & 3) == 3) { wait_payload_fifo_space(); DSIM_PAYLOAD = payload; } i++;
        }
        uint32_t rem = i & 3;
        if (rem) {
            wait_payload_fifo_space();
            if (rem == 3)      DSIM_PAYLOAD = (payload >> 8);
            else if (rem == 2) DSIM_PAYLOAD = (payload >> 16);
            else if (rem == 1) DSIM_PAYLOAD = (payload >> 24);
        }
        DSIM_PKTHDR = DSIM_TYPE_LONG_WRITE | ((total_len & 0xFFFFu) << 8);
        wait_long_done();

        pushed += n;
        first = 0;
        (void)draw_bg;  /* see comment above — we always emit bg for off pixels */
    }
}
