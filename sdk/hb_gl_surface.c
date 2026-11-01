/*
 * hb_gl_surface.c — runtime entry for a GL_SURFACE .hbapp. See hb_gl_surface.h.
 *
 * The resident hb_reloc-loads this blob into an arena and calls entry(0, fb, w, h, shared);
 * we run the app's one-time gl_app_init() and hand back the per-frame GL fn. The
 * resident stores it and calls it each frame inside the injected GL view's
 * begin-scene/end-scene. Same op/return convention as the LVGL surface runtime, so the
 * resident reuses its reloc-loader path. The executable arena is retained and
 * can be entered more than once, so reset .bss on every launch. GL views and
 * their driver resources may not survive a trip Home; keeping the old app-side
 * "initialised" flag would skip rebuilding them and use stale state. fb is
 * unused (GL renders into the view's own pixmap, not a CPU framebuffer).
 */
#include "hb_gl_surface.h"
#include "hb_shared.h"

typedef void (*gl_frame_fn)(int w, int h, uint32_t frame);

__attribute__((section(".text.entry"), used, noinline))
void *payload_entry(int op, void *fb, int w, int h, hb_shared_t *sh)
{
    unsigned int *p;
    extern unsigned int __bss_start__[], __bss_end__[];
    (void)fb; (void)w; (void)h;
    if (op != 0 || !sh || sh->magic != HB_SHARED_MAGIC || sh->version != HB_SHARED_VERSION)
        return 0;
    for (p = __bss_start__; p < __bss_end__; p++)
        *p = 0;
    hb_shared = sh;
    gl_app_init();
    return (void *)(gl_frame_fn)gl_app_frame;
}
