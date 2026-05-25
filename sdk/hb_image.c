/*
 * hb_image.c — minimal image API for homebrew.
 *
 * For homebrew apps:
 *   - Pre-decode images on host, ship as BMP or raw RGB.
 *   - Or roll your own decoder per-app (e.g. stb_image.h).
 *
 * This file provides:
 *   - hb_blit_rgb24(...)  — push a 24bpp raw bitmap to the LCD.
 *   - hb_bmp_load_to(...) — read a Windows BMP file into a caller
 *     buffer + report w/h.
 *
 * BMP format support: uncompressed BI_RGB only, 24bpp (most common
 * for icon files). 32bpp drops alpha. Other depths return error.
 */

#include "hb_sdk.h"

/* Defined in hb_mipi.c — helper that pushes one row to the LCD via 
   MIPI long-write. Used here for blit. */
void hb_mipi_set_window(int16_t x, int16_t y, int16_t w, int16_t h);

/* Push a raw 24bpp RGB888 bitmap (one byte per channel, no padding)
   to the LCD at (x,y) with size w×h. Source layout: row-major,
   contiguous bytes R,G,B,R,G,B... */
void hb_blit_rgb24(int16_t x, int16_t y, int16_t w, int16_t h,
                   const uint8_t *src)
{
    if (w <= 0 || h <= 0 || !src) return;
    hb_mipi_set_window(x, y, w, h);

    /* The MIPI long-write helpers in hb_mipi.c are static. Use
       fill_rect for fallback if blit infrastructure needs expanding;
       for now do a per-pixel approach using rect-fill of 1×1. That's
       slow — TODO: add a dedicated mipi_long_pixels(src, n) helper.

       Quick-and-dirty: emit one fill per pixel. Works but slow. */
    for (int16_t row = 0; row < h; row++) {
        for (int16_t col = 0; col < w; col++) {
            uint32_t i = ((uint32_t)row * w + col) * 3u;
            hb_color_t c = HB_RGB(src[i], src[i+1], src[i+2]);
            hb_fill_rect(x + col, y + row, 1, 1, c);
        }
    }
}

/* Same for 16bpp RGB565 (more common — iPod ArtworkDB uses this). */
void hb_blit_rgb565(int16_t x, int16_t y, int16_t w, int16_t h,
                    const uint16_t *src)
{
    if (w <= 0 || h <= 0 || !src) return;
    for (int16_t row = 0; row < h; row++) {
        for (int16_t col = 0; col < w; col++) {
            uint16_t p = src[(uint32_t)row * w + col];
            uint8_t r = ((p >> 11) & 0x1f) << 3;
            uint8_t g = ((p >> 5)  & 0x3f) << 2;
            uint8_t b = (p         & 0x1f) << 3;
            hb_fill_rect(x + col, y + row, 1, 1, HB_RGB(r, g, b));
        }
    }
}

/* Minimal Windows BMP reader. Returns true on success and fills
   *out_w, *out_h, *out_bpp. Writes pixel bytes into out_pixels;
   caller is responsible for sizing the buffer (at least w*h*bpp/8
   bytes). Top-down or bottom-up source rows are normalized to
   top-down in the output. */
bool hb_bmp_load_to(const char *path, void *out_pixels, uint32_t out_capacity,
                    int16_t *out_w, int16_t *out_h, int *out_bpp)
{
    if (out_w)   *out_w   = 0;
    if (out_h)   *out_h   = 0;
    if (out_bpp) *out_bpp = 0;

    /* BMP header is up to 138 bytes. Read enough to parse + decide. */
    uint8_t hdr[138];
    if (hb_fs_read(path, hdr, sizeof hdr) < 54) return false;

    /* DIB header check */
    if (hdr[0] != 'B' || hdr[1] != 'M') return false;

    /* file size (le32) at +2 */
    uint32_t file_size =
        (uint32_t)hdr[2] | ((uint32_t)hdr[3] << 8) |
        ((uint32_t)hdr[4] << 16) | ((uint32_t)hdr[5] << 24);
    (void)file_size;

    /* pixel offset at +10 */
    uint32_t pix_off =
        (uint32_t)hdr[10] | ((uint32_t)hdr[11] << 8) |
        ((uint32_t)hdr[12] << 16) | ((uint32_t)hdr[13] << 24);

    /* BITMAPINFOHEADER (40 bytes) at +14:
       +0  size of this header (40 / 124 / ...)
       +4  width (le32)
       +8  height (le32; negative = top-down)
       +12 planes (le16)
       +14 bpp (le16)
       +16 compression (le32; 0 = BI_RGB)
       ... */
    int32_t w = (int32_t)(
        (uint32_t)hdr[18] | ((uint32_t)hdr[19] << 8) |
        ((uint32_t)hdr[20] << 16) | ((uint32_t)hdr[21] << 24));
    int32_t h = (int32_t)(
        (uint32_t)hdr[22] | ((uint32_t)hdr[23] << 8) |
        ((uint32_t)hdr[24] << 16) | ((uint32_t)hdr[25] << 24));
    int      bpp = (int)((uint32_t)hdr[28] | ((uint32_t)hdr[29] << 8));
    uint32_t compr = (uint32_t)hdr[30] | ((uint32_t)hdr[31] << 8) |
                     ((uint32_t)hdr[32] << 16) | ((uint32_t)hdr[33] << 24);
    if (compr != 0) return false;
    if (bpp != 24 && bpp != 32) return false;

    int top_down = (h < 0);
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0) return false;

    int  bytes_per_pixel = bpp / 8;
    /* BMP rows padded to multiple of 4 bytes */
    uint32_t row_stride_in  = (uint32_t)((w * bytes_per_pixel + 3) & ~3);
    uint32_t row_stride_out = (uint32_t)(w * bytes_per_pixel);
    uint32_t total_out = row_stride_out * (uint32_t)h;
    if (total_out > out_capacity) return false;

    /* Re-read the full file (we previously only read up to 138 B). */
    uint32_t need = pix_off + row_stride_in * (uint32_t)h;
    static uint8_t s_file_buf[64 * 1024];   /* scratch — caps at 64KB BMP */
    if (need > sizeof s_file_buf) return false;
    if (hb_fs_read(path, s_file_buf, need) < need) return false;

    /* Copy rows, flipping Y if BMP is bottom-up. Also flip BGR→RGB
       per pixel (BMP stores BGR, our blitter expects RGB). */
    uint8_t *out = (uint8_t *)out_pixels;
    for (int32_t row = 0; row < h; row++) {
        int32_t src_row = top_down ? row : (h - 1 - row);
        const uint8_t *in =
            s_file_buf + pix_off + (uint32_t)src_row * row_stride_in;
        uint8_t *outp = out + (uint32_t)row * row_stride_out;
        for (int32_t col = 0; col < w; col++) {
            outp[col*3 + 0] = in[col*bytes_per_pixel + 2]; /* R */
            outp[col*3 + 1] = in[col*bytes_per_pixel + 1]; /* G */
            outp[col*3 + 2] = in[col*bytes_per_pixel + 0]; /* B */
        }
    }

    if (out_w)   *out_w   = (int16_t)w;
    if (out_h)   *out_h   = (int16_t)h;
    if (out_bpp) *out_bpp = 24;  /* always RGB888 output */
    return true;
}
