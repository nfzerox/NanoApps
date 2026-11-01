/*
 * hb_lv_surface.c — fixed-VA LVGL Silver-app runtime. See hb_lv_surface.h.
 *
 * Statically linked into each LVGL app at 0x09280000 (LVGL_ENABLE=1, fixed pool,
 * .bss below the 0x09200000 compositor heap). Self-contained: reads the hardware microsecond tick register and the OS touch list directly — no callouts. The resident
 * loads the app into its own heap arena and calls payload_entry(0, fb, w, h, shared).
 */
#include "lvgl/lvgl.h"
#include "hb_lv_surface.h"
#include "hb_surface_input.h"
#include "hb_shared.h"
#ifdef HB_LV_COMPOSITOR
#include "hb_compositor.h"
#endif

/* Hardware free-running microsecond counter (same source as hb_time). */
#define HWTIMER_CNTL (*(volatile unsigned int *)0x3c700484u)

static lv_display_t *s_disp;

/* ---- tick ---- */
static uint32_t tick_cb(void)
{
    static int      inited;
    static uint32_t last_us, us_rem, ms;
    uint32_t now = HWTIMER_CNTL, d, total, add;
    if (!inited) { last_us = now; inited = 1; return 0; }
    d = now - last_us; last_us = now;
    total = us_rem + d; add = total / 1000u; us_rem = total - add * 1000u; ms += add;
    return ms;
}

/* ---- draw buffer / display buffer ----
 * Normally LVGL renders DIRECTly into the display buffer (s_draw == s_fb) and the
 * flush just forces opaque alpha in place. With HB_LV_COMPOSITOR, LVGL renders into a
 * separate compositor-addressable GPU pixel buffer (s_draw) and the flush copies the dirty
 * area out to the resident's display buffer (s_fb). The copy loop handles both:
 * when s_draw == s_fb it's an in-place alpha-fixup. */
static unsigned int *s_draw;   /* LVGL draw buffer base (GPU pixel buffer pixels or s_fb) */

/* ---- flush (DIRECT mode) ---- */
static void flush_cb(lv_display_t *d, const lv_area_t *area, unsigned char *px)
{
    unsigned int *src = s_draw;
    unsigned int *dst = (unsigned int *)lv_display_get_user_data(d);   /* s_fb */
    int fw = lv_display_get_horizontal_resolution(d);
    int x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2, x, y, aw = x2 - x1 + 1;
    (void)px;
    for (y = y1; y <= y2; y++) {
        unsigned int *s = src + y * fw + x1;
        unsigned int *o = dst + y * fw + x1;
        for (x = 0; x < aw; x++) o[x] = s[x] | 0xff000000u;
    }
    lv_display_flush_ready(d);
}

/* ---- touch ---- map the shared surface touch source onto LVGL's indev. */
static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    hb_spoint_t p;
    (void)indev;
    hb_surface_touch_read(&p);
    data->point.x = p.x;
    data->point.y = p.y;
    data->state = p.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ---- per-frame pump (resident calls this each OS-timer tick) ---- */
/* Optional per-frame hook an app registers with hb_lv_set_frame_cb() (e.g. a
 * game's physics/logic tick). 0 = none. Owned by the runtime. */
void (*hb_lv_app_frame_cb)(void);

void hb_lv_set_frame_cb(void (*cb)(void)) { hb_lv_app_frame_cb = cb; }

/* Optional post-render hook: runs AFTER lv_timer_handler, i.e. it's the last
   thing to touch the framebuffer before the resident composites it — so it draws
   ON TOP of LVGL (e.g. a hardware overlay). */
void (*hb_lv_app_post_cb)(void);
void hb_lv_set_post_cb(void (*cb)(void)) { hb_lv_app_post_cb = cb; }

/* ---- display wake lock ----
 * Keep the panel lit while a touch-less app runs (benchmark, slideshow, clock,
 * now-playing). The native idle/dim-permission path can't gate us — the OS's
 * idle/dim-permission check does a type-check on the top controller, which
 * fails on our factory-pushed controller, so it never consults the screen flag
 * (proven on device: the dim-permission check returned 0 yet it dimmed). Instead
 * we reset the idle timer like a touch — the OS power/idle singleton's event-send
 * with what we take to be the touch-activity event — on a 10 s LVGL timer (well
 * under the ~20-40 s dim timeout; ~6 calls/min, not per-frame). Released
 * automatically when the app's arena is freed on switch. */
#define ADDR_SYSMODEL_GETINST   0x0842ae80u   /* the OS power/idle singleton getter */
#define ADDR_SYSMODEL_SENDEVENT 0x084069d8u   /* power/idle singleton event-send call */
#define HB_KEVENT_TOUCHACTIVITY 4   /* the event value that reset the idle clock   */

static lv_timer_t *s_wake_timer;

static void wake_poke(void)
{
    typedef void *(*gi_t)(void);
    typedef void  (*se_t)(void *, int);
    void *m = ((gi_t)(ADDR_SYSMODEL_GETINST | 1u))();
    if (m) ((se_t)(ADDR_SYSMODEL_SENDEVENT | 1u))(m, HB_KEVENT_TOUCHACTIVITY);
}

static void wake_poke_cb(lv_timer_t *t) { (void)t; wake_poke(); }

void hb_wake_lock(bool on)
{
    if (on) {
        if (!s_wake_timer)
            s_wake_timer = lv_timer_create(wake_poke_cb, 10000, (void *)0);
        wake_poke();   /* reset the timer immediately, then every 10 s */
    } else if (s_wake_timer) {
        lv_timer_delete(s_wake_timer);
        s_wake_timer = 0;
    }
}

__attribute__((used, noinline))
void lv_surface_frame(void)
{
    if (hb_lv_app_frame_cb)
        hb_lv_app_frame_cb();          /* ported app's per-frame game/logic tick */
    lv_timer_handler();
    if (hb_lv_app_post_cb)
        hb_lv_app_post_cb();           /* draws on top of LVGL (e.g. hb_compositor) */
}

/* The display framebuffer (LVGL draws into it directly; it's GPU-addressable).
   Exposed so an app's frame cb can target it with hardware 2D (hb_compositor). */
static void *s_fb;
static int   s_fbw, s_fbh;
#ifdef HB_LV_COMPOSITOR
static void *s_pb;   /* GPU pixel buffer handle backing s_draw */
#endif
/* The buffer LVGL renders into — the GPU pixel buffer with HB_LV_COMPOSITOR, else the
   resident's display fb. An app's frame cb targets this for hardware 2D. */
void *hb_lv_framebuffer(void) { return s_draw ? (void *)s_draw : s_fb; }
int   hb_lv_fb_width(void)  { return s_fbw; }
int   hb_lv_fb_height(void) { return s_fbh; }

static void lv_surface_setup(void *fb, int w, int h)
{
    lv_indev_t *indev;
    s_fb = fb; s_fbw = w; s_fbh = h;

    /* Choose the LVGL draw buffer. With HB_LV_COMPOSITOR, render into a cached
       compositor-addressable GPU pixel buffer (the hardware unit can target it; the flush
       copies it out to s_fb). Fall back to direct-into-fb if allocation fails. */
    s_draw = (unsigned int *)fb;
#ifdef HB_LV_COMPOSITOR
    {
        /* Cached: the compositor draw unit clean+invalidates each filled region (svc#70),
           so the GPU result is CPU-visible while software rendering keeps fast cached
           access. The copy-out reads coherently (SW regions from cache, compositor regions
           refreshed from memory). */
        void *px = 0;
        s_pb = hb_compositor_pixbuf_create(w, h, 1 /*cached*/, &px);
        if (px) s_draw = (unsigned int *)px;
    }
#endif

    lv_init();
#ifdef HB_LV_COMPOSITOR
    if (s_pb) {
        extern void lv_draw_compositor_init(void);
        extern void hb_lv_compositor_drawbuf_init(void);
        extern void hb_lv_compositor_register(void *pixels, void *pb);
        hb_lv_compositor_register(s_draw, s_pb);   /* main buffer = a compositor buffer */
        hb_lv_compositor_drawbuf_init();            /* layer/image bufs -> GPU pixel buffers */
        lv_draw_compositor_init();                  /* the hardware fill unit */
    }
#endif
    lv_tick_set_cb(tick_cb);
    s_disp = lv_display_create(w, h);
    lv_display_set_user_data(s_disp, fb);   /* flush_cb's copy-out target (s_fb) */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(s_disp, s_draw, NULL, (unsigned int)(w * h * 4),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush_cb);

    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);
    /* Pixels of pressed travel before a scroll starts (vs. a click). LVGL's
     * default is 10; a flick must cross this to scroll. It was cranked to 200
     * during early touch-reliability debugging, which made scrolling need a
     * long deliberate drag — now that the event-path touch is solid, use the
     * normal small threshold so flicks scroll. */
    lv_indev_set_scroll_limit(indev, 10);

    lv_app_main();          /* the app builds its UI */
    lv_refr_now(s_disp);
}

/* Entry point. The resident calls entry(0, fb, w, h, shared) on EVERY launch; we set up
 * LVGL + the app fresh and return the per-frame fn.
 *
 * Zero our .bss every launch — this resets lv_global (timers, objects) AND the
 * TLSF pool (LVGL's work_mem static array lives in .bss), so each launch starts
 * with a clean LVGL. The resident re-enters here on a cached relaunch WITHOUT
 * re-loading the image, so without this a second lv_init would stack a second UI
 * on top of the first (two boxes, double tap counts). The reloc loader's
 * one-time bss-zero isn't enough; we must re-zero per launch (the bss/text
 * symbols are rebased into the arena, so the loop is valid there too).
 *
 * Fixed-VA build only: ALSO flush the I-cache over our text — the .bin was
 * raw-loaded to 0x09280000. (Reloc: hb_reloc_load already flushed the arena.) */
__attribute__((section(".text.entry"), used, noinline))
void *payload_entry(int op, void *fb, int w, int h, hb_shared_t *sh)
{
    extern unsigned int __bss_start__[], __bss_end__[];
    unsigned int *p;

    if (op != 0 || !sh || sh->magic != HB_SHARED_MAGIC || sh->version != HB_SHARED_VERSION)
        return 0;
#ifndef HB_LV_RELOC
    {
        extern unsigned char __text_start__[], __text_end__[];
        register unsigned int r12 __asm__("r12") = 7;
        register unsigned int r0  __asm__("r0")  = (unsigned int)__text_start__;
        register unsigned int r1  __asm__("r1")  = (unsigned int)(__text_end__ - __text_start__);
        __asm__ volatile("svc #70" : "+r"(r12), "+r"(r0), "+r"(r1) :: "memory");
    }
#endif
    for (p = __bss_start__; p < __bss_end__; p++) *p = 0;
    hb_shared = sh;                     /* after the bss clear (it lives there) */
    lv_surface_setup(fb, w, h);
    return (void *)&lv_surface_frame;
}
