/*
 * hb_raw_surface.c — direct-draw Silver surface runtime. See hb_raw_surface.h.
 *
 * The resident loads the relocatable blob into an arena and calls
 * payload_entry(0, fb, w, h, shared); we zero .bss, hand the app the framebuffer, and
 * return the per-frame fn the resident calls on its ~60 fps heartbeat (after
 * which it blits the framebuffer to the screen). No LVGL — the app draws pixels
 * straight into fb.
 */
#include "hb_raw_surface.h"
#include "hb_shared.h"

static uint32_t *s_fb;
static int s_w, s_h;

uint32_t *hb_raw_fb(void) { return s_fb; }
int       hb_raw_w(void)  { return s_w; }
int       hb_raw_h(void)  { return s_h; }

void hb_raw_fill_rect(int x, int y, int w, int h, uint32_t c)
{
    int xx, yy;
    c |= 0xff000000u;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_w) w = s_w - x;
    if (y + h > s_h) h = s_h - y;
    if (w <= 0 || h <= 0) return;
    for (yy = 0; yy < h; yy++) {
        uint32_t *row = s_fb + (long)(y + yy) * s_w + x;
        for (xx = 0; xx < w; xx++) row[xx] = c;
    }
}

void hb_raw_fill(uint32_t c) { hb_raw_fill_rect(0, 0, s_w, s_h, c); }

void hb_raw_rect_outline(int x, int y, int w, int h, int t, uint32_t c)
{
    if (t <= 0) return;
    hb_raw_fill_rect(x, y, w, t, c);                 /* top    */
    hb_raw_fill_rect(x, y + h - t, w, t, c);         /* bottom */
    hb_raw_fill_rect(x, y, t, h, c);                 /* left   */
    hb_raw_fill_rect(x + w - t, y, t, h, c);         /* right  */
}

void hb_raw_disc(int cx, int cy, int r, uint32_t c)
{
    int dy, dx;
    c |= 0xff000000u;
    if (r < 1) r = 1;
    for (dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= s_h) continue;
        for (dx = -r; dx <= r; dx++) {
            int x = cx + dx;
            if (x < 0 || x >= s_w) continue;
            if (dx * dx + dy * dy > r * r) continue;
            s_fb[(long)y * s_w + x] = c;
        }
    }
}

void hb_raw_blit(int x, int y, int w, int h, const uint32_t *src)
{
    int xx, yy, sx0 = 0, sy0 = 0, sw = w;
    if (x < 0) { sx0 = -x; w += x; x = 0; }
    if (y < 0) { sy0 = -y; h += y; y = 0; }
    if (x + w > s_w) w = s_w - x;
    if (y + h > s_h) h = s_h - y;
    if (w <= 0 || h <= 0) return;
    for (yy = 0; yy < h; yy++) {
        uint32_t *d = s_fb + (long)(y + yy) * s_w + x;
        const uint32_t *s = src + (long)(sy0 + yy) * sw + sx0;
        for (xx = 0; xx < w; xx++) d[xx] = s[xx] | 0xff000000u;
    }
}

__attribute__((used, noinline))
static void raw_surface_frame(void)
{
    hb_spoint_t p;
    hb_surface_touch_read(&p);
    hb_raw_frame(&p);
}

/* Entry — the resident calls entry(0, fb, w, h, shared) on every launch. Zero our .bss
 * each launch (a cached relaunch re-enters here without reloading the image), set
 * up, and return the per-frame fn. The reloc loader already flushed the I-cache
 * over the arena, so no SVC flush here (raw apps are always RELOC). */
__attribute__((section(".text.entry"), used, noinline))
void *payload_entry(int op, void *fb, int w, int h, hb_shared_t *sh)
{
    extern unsigned int __bss_start__[], __bss_end__[];
    unsigned int *p;
    if (op != 0 || !sh || sh->magic != HB_SHARED_MAGIC || sh->version != HB_SHARED_VERSION)
        return 0;
    for (p = __bss_start__; p < __bss_end__; p++) *p = 0;
    hb_shared = sh;                     /* after the bss clear (it lives there) */
    s_fb = (uint32_t *)fb; s_w = w; s_h = h;
    hb_raw_init(w, h);
    return (void *)&raw_surface_frame;
}
