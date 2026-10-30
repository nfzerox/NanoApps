/* silver_resident — the resident system daemon. Loads at 0x09130000 and stays
 * in the OS address space to host every homebrew app on the home screen.
 *
 * It installs a small set of OS hooks (re-aimed at this build on every deploy,
 * so a plain re-push takes effect with no power-cycle):
 *   - home controller action dispatch slot (vtable 0x087cb9dc, +0x190): runs in
 *     UI-task context; (re)registers the on-disk app pack (/Apps/AllApps.pack)
 *     and builds the launch registry. Forwards to the original 0x082ae8dd.
 *   - raw message hook: claims our "sbitem.hb." taps and launches the app.
 *   - the resource-DB lookup trampoline: serves our compiled screen DB for our
 *     id range, forwarding everything else.
 *   - the touch manager's touch-posting entry: latches a touch mailbox the surface indev reads.
 *   - triple click Home: screenshot / screen-record.
 *   - the template's layer-creation hook: injects the app's framebuffer surface.
 *
 * On launch the resident provides the surface (framebuffer + present + touch +
 * clock); the app's statically-linked SDK runtime owns its own loop. Per-app
 * identity (ids, screen/layout, label, icon) comes from the pack's launch
 * records, so nothing here is hardcoded per app.
 */
#include "hb_sdk.h"
#include "hb_silver_res.h"
#include "hb_silver_uitask.h"
#include "hb_silver_icon.h"
#include "hb_silver_app.h"
#include "hb_silver_gfx.h"
#include "hb_reloc.h"
#include "hb_app_api.h"
#include "hb_touch_mb.h"
#include "hb_heap.h"
#include "hb_record.h"
#include "hb_app_bundle.h"

/* The apps come from the on-disk pack — ids, labels, and icons live in each
 * launch record, not here. Setup runs in the (proven-stable) ACTION hook; the
 * RAW-MESSAGE hook claims our taps; the icon registrar EXTENDS the bitmap cache
 * table and serves our bitmaps at appended ids past the OS range. */
#define FLAG_ADDR      0x09135ffcu   /* per-deploy icon-serve guard (cleared at entry) */

/* Our OWN screen: a precompiled resource DB (SCRN/SCST/SLst/...) for
 * HBCustom_Screen, injected into the chain so the factory can build it (the
 * template's canvas is empty, so our injected view is the only content). The DB
 * ships inside the app pack; hb_app_register_pack registers it using the object
 * + chain-node scratch below. */
#define SILVERDB_OBJ   0x09137000u   /* resource-db object storage            */
#define SILVERDB_NODE  0x09137100u   /* chain node for our screen DB          */
/* Screen/layout ids come from each app's launch record (the bundle), so the
   resident doesn't hardcode them. The compiled resource DB (tools/screens) has one
   surface screen 229474306 / layout 229474305. */
#define OUR_ID_BASE    0x0dad8000u   /* our resource id base (above OS's range)*/

/* The screen factory reads SCRN/SCST/template from the main SilverDB
 * (the resource-DB lookup), not the UI-task chain we publish into, so our
 * screen is invisible to it. the resource getter is a direct bl, no
 * vtable slot, so we patch its entry (function trampoline) to serve our ids
 * from our injected DB. Its prologue (0x08409e86: push;mov;ldrb;mov;mov) is
 * position-independent for 10 bytes (first branch at +0x0e). */
#define ADDR_GETRESOURCE   0x08409e86u
#define GETRES_PROLOGUE0   0xb5f8u    /* 'push {r3-r7,lr}' — unpatched marker  */
#define GETRES_TRAMP_ADDR  0x09137200u /* trampoline (callable original resource getter) */

typedef const void *(*getres_t)(void *db, uint32_t type, long id, uint32_t *size);

__attribute__((section(".text.hook"), used, noinline))
static const void *getres_hook(void *db, uint32_t type, long id, uint32_t *size)
{
    /* serve our compiled screen's resources (our id range) from our injected DB
     * — which the main SilverDB lacks; forward everything else unchanged. */
    if ((uint32_t)id >= OUR_ID_BASE) {
        const void *r = ((getres_t)(GETRES_TRAMP_ADDR | 1u))((void *)SILVERDB_OBJ,
                                                             type, id, size);
        if (r)
            return r;
    }
    return ((getres_t)(GETRES_TRAMP_ADDR | 1u))(db, type, id, size);
}

/* Event-path touch: hook the touch manager's touch-posting entry (called for EVERY touch event,
 * before it drains the hardware touch list). We read the raw touch list at
 * that moment and LATCH a mailbox the LVGL surface indev reads — so a tap is
 * never missed (vs polling the transient list, which only caught ~1/4 of taps).
 * Mailbox: [magic, x, y, pressed] at 0x09135f40 (matches hb_lv_surface.c). */
#define ADDR_POSTTOUCH     0x084a27f6u
#define POSTTOUCH_PROLOGUE 0xe92du        /* 'stmdb sp!,{r1-r9,lr}' — unpatched   */
#define POSTTOUCH_TRAMP    0x09137c00u    /* callable original touch-posting entry        */
#define TOUCH_LIST_HEAD    0x089a5298u
#define TOUCH_MB_ADDR      HB_TOUCH_MB_ADDR    /* shared writer/reader mailbox (hb_touch_mb.h) */
#define TOUCH_MB_MAGIC     HB_TOUCH_MB_MAGIC
typedef int (*posttouch_t)(void *self);

__attribute__((section(".text.hook"), used, noinline))
static int hb_posttouch_hook(void *self)
{
    volatile uint32_t *head = (volatile uint32_t *)TOUCH_LIST_HEAD;
    volatile int32_t  *mb   = (volatile int32_t *)TOUCH_MB_ADDR;
    if (head[5] != 0) {
        uint32_t end_node = head[4], first;
        if (end_node >= 0x08000000u && end_node < 0x10000000u) {
            first = *(volatile uint32_t *)end_node;
            if (first >= 0x08000000u && first < 0x10000000u) {
                volatile int32_t *v = (volatile int32_t *)(first + 8);
                uint8_t st = *((volatile uint8_t *)(first + 8) + 4);
                mb[1] = (int32_t)(int16_t)v[2];      /* x */
                mb[2] = (int32_t)(int16_t)v[3];      /* y */
                mb[3] = (st != 1) ? 1 : 0;           /* pressed unless the touch ended (status 1) */
                mb[0] = (int32_t)TOUCH_MB_MAGIC;
            }
        }
    } else {
        mb[3] = 0;                                   /* no touch -> released */
        mb[0] = (int32_t)TOUCH_MB_MAGIC;
    }
    return ((posttouch_t)(POSTTOUCH_TRAMP | 1u))(self);
}

/* System-wide screenshot on triple-click Home.
 *
 * The OS already has a display-capture entry at 0x08240860: it grabs the display
 * and writes /screenshotNNNN.bmp. Home triple-click lands in a handler at 0x08241750
 * that routes to Accessibility. We re-aim that handler's entry at the hook below:
 * when the RAM toggle is set, capture a screenshot (the handler's `self` is the
 * global home controller, so we invoke the capturer at 0x08240860 on it directly);
 * otherwise we fall back to the stock Accessibility path.
 *
 * We reimplement the original rather than forward through a trampoline because its
 * prologue (push {r4,lr}; bl …; cbz …) has PC-relative ops that can't be relocated
 * verbatim. The reimplementation follows the disassembled original: read the current
 * springboard view (0x083f4dfc); if there is none, or it is not in edit mode (its
 * vtable slot at +0x268 reports false), invoke the OS accessibility-shortcut
 * handler. The toggle is a plain RAM byte the Screenshot app writes
 * (resets to off on reboot). */
#define A_HOME_TRIPLE         0x08241750u  /* Home triple-click handler entry             */
#define A_SCREENSHOT_KEY      0x08240860u  /* display-capture entry (self = controller)   */
#define A_GET_CUR_SBVIEW      0x083f4dfcu  /* read the current springboard view           */
#define SBVIEW_EDITING_OFF 0x268u       /* slot we read as the edit mode flag (the original gates on its false return) */
#define A_ACCESS_GETINST      0x082a80f4u  /* OS accessibility-shortcut handler getter    */
#define A_ACCESS_ON_TRIPLE    0x082a82a0u  /* accessibility triple-click handler(self)    */
#define SCREENSHOT_FLAG_ADDR  0x09135ff0u  /* RAM toggle: 1 => triple-click Home screenshots   */

typedef void *(*getvoid_t)(void);
typedef int   (*int_self_t)(void *self);
typedef void  (*void_self_t)(void *self);

/* Screen-recording capture hooks the singleton's message handler — a hot path
 * (every system message). Set HB_REC_INTERCEPT to 0 to compile out the intercept
 * entirely (the timer hook, its install, and the record branch below); screenshots
 * still work, recording just becomes unavailable. */
#ifndef HB_REC_INTERCEPT
#define HB_REC_INTERCEPT 1
#endif

#if HB_REC_INTERCEPT
static void hb_rec_timer_arm(void);   /* defined below (with the capture timer)     */
#endif

__attribute__((section(".text.hook"), used, noinline))
static void hb_home_triple_hook(void *self)
{
    uint8_t mode = *(volatile uint8_t *)SCREENSHOT_FLAG_ADDR;  /* 0 off,1 shot,2 rec */
    if (mode == 1u) {
        ((void_self_t)(A_SCREENSHOT_KEY | 1u))(self);   /* /screenshotNNNN.bmp */
        return;
    }
#if HB_REC_INTERCEPT
    if (mode == 2u) {                                   /* toggle a screen recording */
        if (hb_record_active())
            hb_record_stop();
        else if (hb_record_start())
            hb_rec_timer_arm();
        return;
    }
#endif
    {
        void *cur_view = ((getvoid_t)(A_GET_CUR_SBVIEW | 1u))();
        int editing = 0;
        if (cur_view) {
            uint32_t *vt = *(uint32_t **)cur_view;
            editing = ((int_self_t)(vt[SBVIEW_EDITING_OFF / 4u] | 1u))(cur_view);
        }
        if (!cur_view || editing == 0) {
            void *acc = ((getvoid_t)(A_ACCESS_GETINST | 1u))();
            if (acc)
                ((void_self_t)(A_ACCESS_ON_TRIPLE | 1u))(acc);
        }
    }
}

/* "Demo bar" 9:41 status-bar clock (a Screenshot-app toggle, for pretty shots).
 * The springboard status-bar clock is text drawn by an OS date/time-to-string
 * formatter (out, size, fmt, dt) for the short-time
 * formats. When the toggle is on we SWAP the date-time arg for a fixed 9:41 and let
 * the original format it (so encoding/locale stay correct); fmt is r3 (4th arg).
 * The status bar uses the 0x084a9adc overload, whose prologue has a PC-relative ldr
 * at +6 — it can't take the 10-byte trampoline jump, so it's patched with a 4-byte
 * B.W (relocating only the 4-byte stmdb). Runs per clock re-format (~per minute). */
#define STATUSBAR_FLAG_ADDR 0x09135fecu   /* RAM toggle: 1 => demo status bar (9:41)          */
#define A_DTTS              0x084a9adcu    /* OS date/time-to-string formatter: out buffer, size, format, date-time */
#define DTTS_PROLOGUE       0xe92du        /* stmdb sp!,{...} — unpatched marker               */
#define DTTS_TRAMP_ADDR     0x09137b80u    /* callable original (free DRAM slot)               */
#define CLOCK_FMT_A         19   /* the two format ids that render the short HH:MM clock     */
#define CLOCK_FMT_B         20   /* string; pinned by watching which id reaches this call    */
typedef int (*dtts_t)(void *self, void *out, unsigned size, int fmt, const void *dt);

/* date-time struct { u8 sec,min,hour,day,mon; u16 year; u8 wday }; the year reads
 * at +6 (the u16 is 2-byte aligned, so +5 is an unused byte). 9:41 AM. */
static const struct { uint8_t s, mi, h, d, mo; uint16_t y; uint8_t w; } k_dt941 =
    { 0, 41, 9, 1, 1, 2026, 1 };

__attribute__((section(".text.hook"), used, noinline))
static int hb_dtts_hook(void *self, void *out, unsigned size, int fmt, const void *dt)
{
    if (*(volatile uint8_t *)STATUSBAR_FLAG_ADDR &&
        (fmt == CLOCK_FMT_A || fmt == CLOCK_FMT_B))
        dt = &k_dt941;
    return ((dtts_t)(DTTS_TRAMP_ADDR | 1u))(self, out, size, fmt, dt);
}

/* Same "Demo bar" toggle: 100% battery, not charging. the power-status view's update
 * routine draws a level bitmap (chosen from a battery level index) plus a status
 * overlay (chosen from a power-status index — the charging bolt; the 'unknown'
 * status draws no overlay). Plugged into the host the iPod is charging, so it shows
 * full + a bolt. So we force the level to its maximum (full) and the status to
 * 'unknown' (no bolt). Both getters have a PC-relative op within 10 bytes, so use the 4-byte
 * B.W patch (relocate the 4-byte push+mov prologue, leave the PC-rel op in place).
 * Like the clock, the icon re-picks on the next power-status update (battery poll). */
#define A_BATT_LEVEL        0x08215684u   /* returns a level index into battery artwork */
#define A_BATT_STATUS       0x08215600u   /* returns a status index for overlay select       */
#define BATT_PROLOGUE       0xb570u       /* push {r4,r5,r6,lr} — unpatched marker     */
#define BATT_LEVEL_TRAMP    0x09137b00u
#define BATT_STATUS_TRAMP   0x09137b40u
#define BATT_LEVEL_FULL     22   /* level index that renders as a full battery (observed)  */
#define BATT_STATUS_NONE    30   /* status index that draws no charge overlay (observed)   */
typedef int (*batt_t)(void *self);

__attribute__((section(".text.hook"), used, noinline))
static int hb_batt_level_hook(void *self)
{
    if (*(volatile uint8_t *)STATUSBAR_FLAG_ADDR)
        return BATT_LEVEL_FULL;
    return ((batt_t)(BATT_LEVEL_TRAMP | 1u))(self);
}

__attribute__((section(".text.hook"), used, noinline))
static int hb_batt_status_hook(void *self)
{
    if (*(volatile uint8_t *)STATUSBAR_FLAG_ADDR)
        return BATT_STATUS_NONE;
    return ((batt_t)(BATT_STATUS_TRAMP | 1u))(self);
}

typedef int (*rawmsg_t)(void *self, void *msg);
typedef int (*dispatch_t)(void *self, const char *name, void *msg);

/* ---- custom-draw surface on our screen ----
 * Override our view's draw callback (vtable slot +0xbc) so we control its pixels:
 * draw a gradient with the OS gfx primitives (a gfx context, the view's inner
 * focus-rect, a filled rect). This is the foundation for blitting an
 * LVGL/GL buffer here. We can't swap a single slot on the shared view vtable
 * (it'd affect every view), so we copy the vtable, patch draw callback in the copy,
 * and point our view at the copy. */
#define DRAW_SLOT_OFF  0xbcu       /* draw-callback slot (found by overriding it and
                                     watching the view repaint); other slots unmapped */
#define VTABLE_COPY_ADDR   0x09137300u   /* our patched vtable copy */
#define VTABLE_COPY_SIZE   0x800u        /* generous (over-copy is safe; under is not) */
#define A_TGFX_CTOR        0x084243dcu   /* the gfx constructor */
#define A_TGFX_DTOR        0x0841abfcu   /* gfx object destructor */
#define A_FOCUS_EXBORDER   0x084035e4u   /* the view's inner focus-rect (takes a gfx context) */
#define A_LOCALBOUNDS      0x084035b6u   /* the view's inner-bounds (takes a rect) */
#define A_SETPENMODE       0x0841c0e0u   /* the gfx set pen mode */
#define A_VIEW_INVAL       0x08423b44u   /* the view's invalidate — re-schedule a draw */

/* Silver UI-task animation heartbeat (the marquee / animated-bitmap view
 * pattern): an OS timer object bound to our view posts a timer-message to the
 * view's message handler on the ui task (safe, unlike a raw OS init timer callback,
 * which runs off-task and crashed). The handler runs lv_timer_handler + Inval +
 * re-arm. An OS timer object is essentially a timer plus a user-data word. */
#define A_TIMER_CTOR       0x0841bdd0u   /* OS timer object ctor (timer, then 3 ptrs we pass) */
#define A_TIMER_SET        0x0841cd7au   /* event server set timer(t, longlong ms)  */
#define A_TIMER_START      0x0842484cu   /* event server start timer(t)             */
#define A_TIMER_STOP       0x0841c42cu   /* event server stop timer(t)              */
#define A_TIMER_DESTROY    0x0841cd68u   /* event server destroy timer(t), unregister */
#define TIMER_ADDR         0x09135c00u   /* OS timer object storage (survives re-exec) */
#define TIMER_MAGIC_ADDR   0x09135c80u   /* 'TIMR' when our timer is live          */
#define TIMER_MAGIC        0x54494d52u
/* Redraw heartbeat. The timer only Invals; the OS draws at its ~60 Hz
 * refresh. It is one-shot and re-armed when its message is handled, so the
 * period counts from that moment. At 16 ms (just under a 16.7 ms refresh) a
 * little latency made the next Inval land just after a refresh, and surfaces
 * locked at 30 Hz for seconds. Half a refresh can't miss one. */
#define TIMER_PERIOD_MS    8u
#define CLTIMERMSG         16u           /* timer message type (the message type field, NOT timer message) */
#define MSG_FTYPE_OFF      4u            /* the message type field (after the vtable ptr)      */
#define MSG_SLOT_OFF 0x40u         /* view message-handler slot (we patch it to catch messages) */
#define DTOR0_SLOT_OFF     0x00u         /* two adjacent teardown-like slots at +0x00/+0x04; we save */
#define DTOR1_SLOT_OFF     0x04u         /* and chain both rather than guess which one the OS calls  */
#define VIS_SLOT_OFF  0xecu         /* show/hide slot (bool arg; we stop on hide) */
#define EXIT_SLOT_OFF 0x104u

/* The custom-draw surface: an ARGB-8888 framebuffer WE own, blitted into the view each
 * draw via the native image-view copy path (hb_silver_gfx). The launched app's
 * LVGL/GL/raw backend renders into this buffer; if no app blob is active we fall
 * back to a test pattern (the blit is clipped + animated by the OS regardless).
 *
 * CRITICAL: the buffer is allocated from the OS HEAP (the OS allocator), not a
 * hand-picked DRAM address. SDRAM 0x08000000..0x0A000000 is one heap that grows
 * upward; a fixed high address like 0x092xxxxx is live heap the OS hands to
 * transition layers, so writing a framebuffer there corrupts the compositor and
 * crashes on the next composite (e.g. the quit zoom). The allocator never hands
 * the same block out twice, so a heap buffer is safe while the OS UI is live.
 * The small source device/context live in our zeroed .bss (inside the loaded
 * resident region, which is safe). */
#define FB_W            240
#define FB_H            432       /* nano 7g screen is 240x432 */
#define ADDR_OP_NEW     0x0842d444u   /* the OS heap alloc entry               */

typedef void (*gfx0_t)(void *o);
typedef void (*gfx1p_t)(void *o, void *p);
typedef void (*gfx1i_t)(void *o, int v);
typedef void *(*opnew_t)(unsigned int size);

static uint8_t   s_gfx_dev[HB_GFX_DEVICE_BYTES];   /* source draw-device storage */
static uint8_t   s_gfx_ctx[HB_GFX_CONTEXT_BYTES];  /* source draw-context storage */
static uint32_t *s_fb;                             /* heap framebuffer (NULL until built) */
static uint8_t   s_gfx_ready;                      /* surface built this deploy */

/* Fill the custom-draw framebuffer with a recognisable 2D gradient (R across, G down,
 * B fixed) so the blit's geometry + channel order are obvious on screen. */
static void hb_fill_test_pattern(uint32_t *fb)
{
    int x, y;
    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++) {
            uint32_t rr = (uint32_t)(x * 255 / (FB_W - 1));
            uint32_t gg = (uint32_t)(y * 255 / (FB_H - 1));
            fb[y * FB_W + x] = 0xff000000u | (rr << 16) | (gg << 8) | 0x40u;
        }
}

/* In-heap app binary for the current CUSTOM_SCREEN app (set on launch); copied to
 * the fixed surface VA and run. 0 = built-in gradient. */
static uint32_t s_surface_code;
static uint32_t s_surface_code_len;
/* For a relocatable .hbapp (HB_RELOC_MAGIC): the entry returned by hb_reloc_load
 * after the blob is rebased into an operator-new arena. 0 = use the legacy
 * fixed-VA path (raw .bin copied to LV_SURFACE_VA). s_surface_arena is the
 * arena's allocation, freed (matched free) when the next app loads — without
 * this each app-switch leaks ~1 MB and the heap exhausts after a few launches. */
static void    *s_surface_reloc_entry;
static void    *s_surface_arena;
/* An arena whose free was deferred from quit-time to the next launch — see
 * hb_surface_teardown for why. At most one outstanding at a time. */
static void    *s_pending_free_arena;
#define ADDR_OP_DELETE  0x0842d86cu   /* the OS heap free entry */
typedef void (*opdelete_t)(void *);
static uint32_t s_cur_app_id;       /* app whose binary is currently in the slot */

/* Reclaim a deferred arena (hb_surface_teardown stashes it here at quit). Only
 * safe once the exit transition that was in flight at quit has finished — i.e. at
 * the next launch, by which point the GPU has flushed the quit app's last frame. */
static void hb_surface_drain_pending(void)
{
    if (s_pending_free_arena) {
        ((opdelete_t)(ADDR_OP_DELETE | 1u))(s_pending_free_arena);
        s_pending_free_arena = 0;
    }
}

/* Per-frame function the surface blob returns (e.g. lv_timer_handler) — the
 * resident calls it each draw to advance animations + input, then re-blits. */
typedef void (*surface_frame_t)(void);
static surface_frame_t s_surface_frame;

/* A GL_SURFACE app's per-frame draw fn, returned by its .hbapp entry and called by
 * gl_draw_hook inside the GL view's begin-scene/end-scene. 0 = no GL app loaded
 * (fall back to the built-in cube). The app's code lives in s_surface_arena (the
 * same arena slot LVGL surface apps use — freed on the next app switch). */
typedef void (*gl_frame_fn_t)(int w, int h, uint32_t frame);
static gl_frame_fn_t s_gl_frame_fn;
static void *s_surface_view;            /* the custom-draw view (for the timer to Inval)    */
static uint8_t s_timer_started;         /* per-deploy: heartbeat timer set up      */

typedef void (*timer1_t)(void *t);
typedef void (*timer_ctor_t)(void *t, void *a1, void *a2, void *a3);
typedef void (*timer_set_t)(void *t, long long ms);
typedef int  (*msg_fn_t)(void *self, void *msg);

/* The view's real message handler, captured from its vtable +0x40 before we patch the
 * slot (this view's class may override the message handler). */
static msg_fn_t s_orig_msg;

/* Our view's message handler override (UI task). On our heartbeat timer's timer message:
 * advance LVGL (lv_timer_handler) + Inval the view (-> draw callback blits s_fb) +
 * re-arm the one shot timer. Forward everything else to the real message handler. */
static uint32_t s_ds_dbg;       /* bound the per-frame draw traces (anti-flood)  */

__attribute__((section(".text.hook"), used, noinline))
static int hb_msg_hook(void *self, void *msg)
{
    if (msg && *(uint32_t *)((char *)msg + MSG_FTYPE_OFF) == CLTIMERMSG) {
        /* Timer context (possibly off the draw task): do ONLY cheap, safe work —
         * mark the view dirty + re-arm. The heavy lv_timer_handler + blit runs in
         * draw_hook on the OS's draw task (where it's proven safe at init).
         * (No per-tick trace here, it flooded the ring.) */
        ((gfx0_t)(A_VIEW_INVAL | 1u))(self);                /* schedule a redraw   */
        ((timer1_t)(A_TIMER_START | 1u))((void *)TIMER_ADDR);   /* repeat          */
        return 1;
    }
    return s_orig_msg ? s_orig_msg(self, msg) : 0;
}

static void hb_surface_start_timer(void *view)
{
    void *t = (void *)TIMER_ADDR;
    uint32_t i;
    for (i = 0; i < 128u; i++)                              /* zero the timer obj  */
        ((uint8_t *)t)[i] = 0;
    s_surface_view = view;
    ((timer_ctor_t)(A_TIMER_CTOR | 1u))(t, view, 0, 0);     /* bind to our view    */
    ((timer_set_t)(A_TIMER_SET | 1u))(t, (long long)TIMER_PERIOD_MS);
    ((timer1_t)(A_TIMER_START | 1u))(t);
    *(uint32_t *)TIMER_MAGIC_ADDR = TIMER_MAGIC;
    hb_trace_log("TIMRSTRT", (uint32_t)(uintptr_t)view, TIMER_PERIOD_MS);
}

/* Stop the heartbeat so it can't fire into a freed view. Reset the per-view
 * arming flag so the NEXT launch starts a fresh timer. */
static void hb_surface_stop_timer(void)
{
    if (*(uint32_t *)TIMER_MAGIC_ADDR == TIMER_MAGIC) {
        ((timer1_t)(A_TIMER_STOP | 1u))((void *)TIMER_ADDR);     /* disarm        */
        ((timer1_t)(A_TIMER_DESTROY | 1u))((void *)TIMER_ADDR);  /* unregister    */
        *(uint32_t *)TIMER_MAGIC_ADDR = 0;
    }
    s_timer_started = 0;
    s_surface_view = 0;
}

/* The view's real destructors, captured before we patch slots 0/1 of the copy. */
typedef void (*dtor_t)(void *self);
static dtor_t s_orig_dtor0, s_orig_dtor1;

/* the view's visibility handler — the OS calls it with false when hiding the view
 * (quit transition), before destruction. We do it so the heartbeat can't fire into 
 * a hidden/freed view. */
typedef void (*vis_fn_t)(void *self, int show);
static vis_fn_t s_orig_visibility;

__attribute__((section(".text.hook"), used, noinline))
static void hb_visibility_hook(void *self, int show)
{
    hb_trace_log("XSHOW   ", (uint32_t)(uintptr_t)self, (uint32_t)show);
    if (!show)
        hb_surface_stop_timer();
    if (s_orig_visibility) s_orig_visibility(self, show);
}

#if HB_REC_INTERCEPT
/* ---- system-wide capture timer (screen recording) ----
 * An OS timer object bound to the singleton fires every ~33 ms; its
 * timer message lands in the singleton's message handler, which we hook. We act
 * ONLY on OUR timer (msg type == timer-message type AND the timer-message's timer field == our timer
 * object) and forward everything else to the stock message handler, so the OS's own
 * timers/messages are untouched. The stock message handler is persisted at a fixed DRAM
 * slot so a resident redeploy reaims safely (a patched slot points into our
 * region; an OS address means the slot is still pristine). */
#define A_GLOBAL_CNTLR_GET   0x083fb524u  /* the singleton getter */
#define REC_TIMER_ADDR       0x09135d00u  /* OS timer object storage (persists) */
#define REC_ORIG_HM_ADDR     0x09135d8cu  /* persisted stock global-cntlr message handler */
#define REC_PERIOD_MS        12u           /* +~25ms msg latency +capture -> ~25 fps */
#define TMSG_TIMER_TYPE      16u           /* the message type field for timer-message */
#define TMSG_FTIMER_OFF      8u            /* the timer-message's timer field (str r4,[r0,#8]) */
#define RESIDENT_REGION_LO   0x09130000u

static msg_fn_t s_orig_gc_msg;

__attribute__((section(".text.hook"), used, noinline))
static int hb_rec_gc_msg_hook(void *self, void *msg)
{
    if (msg
        && *(uint32_t *)((char *)msg + MSG_FTYPE_OFF) == TMSG_TIMER_TYPE
        && *(void **)((char *)msg + TMSG_FTIMER_OFF) == (void *)REC_TIMER_ADDR) {
        if (hb_record_active()) {
            hb_record_tick();                                       /* capture frame */
            ((timer1_t)(A_TIMER_START | 1u))((void *)REC_TIMER_ADDR);  /* re-arm     */
        }
        return 1;                                       /* consume our timer message */
    }
    return s_orig_gc_msg ? s_orig_gc_msg(self, msg) : 0;
}

/* Install the singleton-controller message handler filter once per deploy (no timer yet).
 * Re-aim safe: a patched slot points into our region; an OS address is pristine. */
static void hb_rec_install(void)
{
    void *g = ((getvoid_t)(A_GLOBAL_CNTLR_GET | 1u))();
    uint32_t *gvt, slot;
    if (!g) return;
    gvt = *(uint32_t **)g;
    slot = gvt[MSG_SLOT_OFF / 4u];
    if (slot < RESIDENT_REGION_LO)
        *(uint32_t *)REC_ORIG_HM_ADDR = slot;
    s_orig_gc_msg = (msg_fn_t)(uintptr_t)(*(uint32_t *)REC_ORIG_HM_ADDR);
    gvt[MSG_SLOT_OFF / 4u] =
        ((uint32_t)(uintptr_t)&hb_rec_gc_msg_hook) | 1u;
    hb_icache_invalidate((uint32_t)(uintptr_t)gvt, MSG_SLOT_OFF + 8u);
    hb_trace_log("RECINST ", (uint32_t)(uintptr_t)g, *(uint32_t *)REC_ORIG_HM_ADDR);
}

/* Arm the capture timer (bound to the singleton). Called on record START;
 * the hook re-arms each tick while recording and lets it lapse when it stops. */
static void hb_rec_timer_arm(void)
{
    void *g = ((getvoid_t)(A_GLOBAL_CNTLR_GET | 1u))();
    void *t = (void *)REC_TIMER_ADDR;
    uint32_t i;
    if (!g) return;
    for (i = 0; i < 128u; i++) ((uint8_t *)t)[i] = 0;
    ((timer_ctor_t)(A_TIMER_CTOR | 1u))(t, g, 0, 0);     /* (timer, singleton cntlr, 0, 0) */
    ((timer_set_t)(A_TIMER_SET | 1u))(t, (long long)REC_PERIOD_MS);
    ((timer1_t)(A_TIMER_START | 1u))(t);
}
#endif /* HB_REC_INTERCEPT */

/* Destructor hooks: stop the timer before the view is torn down, then chain to
 * the real dtor. Without this, the fixed-address timer outlives the view and
 * fires a timer message into freed memory -> crash on quit/home. */
__attribute__((section(".text.hook"), used, noinline))
static void hb_dtor_hook0(void *self)
{
    hb_trace_log("XDTOR0  ", (uint32_t)(uintptr_t)self, 0);
    hb_surface_stop_timer();
    if (s_orig_dtor0) s_orig_dtor0(self);
}

__attribute__((section(".text.hook"), used, noinline))
static void hb_dtor_hook1(void *self)
{
    hb_trace_log("XDTOR1  ", (uint32_t)(uintptr_t)self, 0);
    hb_surface_stop_timer();
    if (s_orig_dtor1) s_orig_dtor1(self);
}

/* The OS's global edge swipe-back singleton. Its pointer lives in this .data
 * slot, and the controller's swipe-back-enabled byte is at +0xD4 inside the
 * object. RE: the ctor at 0x0825ac84 stores this to [0x0881592c] and inits the
 * byte (strb #1,[r0,#0xd4]); the enable/disable setter writes the same byte
 * (strb r1,[r0,#0xd4]) then cancels any in-flight drag. When the byte is 0 the
 * gesture pre-handler's swipe-back-allowed check returns false immediately, so a
 * horizontal swipe no longer pops our screen. We do this for surface apps so an
 * LVGL/GL app owns all of its in-app gestures; the hardware Home button still
 * exits. */
#define A_SWIPEBACK_INSTANCE_PTR  0x0881592cu  /* edge swipe-back singleton slot */
#define SWIPEBACK_ENABLED_OFF     0xD4u        /* swipe-back enabled byte in the object */

static void hb_nav_backswipe_set(int enabled)
{
    void *cntlr = *(void **)A_SWIPEBACK_INSTANCE_PTR;
    if (!cntlr)
        return;                                /* not constructed yet, nothing to do */
    *((volatile uint8_t *)cntlr + SWIPEBACK_ENABLED_OFF) = enabled ? 1u : 0u;
    hb_trace_log("BSWIPE  ", (uint32_t)(uintptr_t)cntlr, (uint32_t)enabled);
}

/* Full surface teardown — proper quit/exit for BOTH surface kinds (LVGL + GL). Stop
 * the heartbeat, stop calling the app's per-frame fn, free the app's code arena, and
 * reset the current-app id so the next launch reloads fresh. Driven from exit-begin handler
 * (below): the app stops drawing the instant its screen leaves for home, and its
 * memory is reclaimed. */
static void hb_surface_teardown(void)
{
    /* Restore the OS edge-swipe-to-go-back our surface suppressed (once, on quit).
     * Safe here now that the disable is asserted on EVERY launch (templ_hook, incl.
     * the cached-view reuse path): a edge-swipe-back can never START inside a surface app,
     * so exit-begin handler only ever fires on a committed Home exit — never mid-gesture. (The
     * earlier long-swipe bug was a reused cached view coming up with edge-swipe-back still
     * enabled; re-enabling at exit-begin handler then fed the in-progress drag.) */
    hb_nav_backswipe_set(1);
    hb_surface_stop_timer();
    s_gl_frame_fn   = 0;
    s_surface_frame = 0;
    /* DEFER the arena free rather than doing it here. At exit-begin handler the exit
     * transition is only just starting and the GPU (a deferred tile renderer) may
     * still have the app's last GL/LVGL frame queued against this arena — freeing
     * it now is a use-after-free that crashes during the transition (the OpenGL
     * quit crash). Stash it and reclaim at the next launch, once the transition has
     * completed and the GPU has flushed. Bounded to one outstanding arena: any
     * prior pending one is drained before we stash a new one. */
    hb_surface_drain_pending();
    s_pending_free_arena = s_surface_arena;
    s_surface_arena = 0;
    s_cur_app_id = 0;
}

/* the view's exit-begin handler (its one arg identifies the incoming screen) — the
 * OS calls this on our view when its screen transitions away (to home or another
 * app). Tear the app down here, then forward. This is the callback the visibility
 * handler could not provide for a screen transition. */
typedef void (*exit_fn_t)(void *self, int dst);
static exit_fn_t s_orig_exit;

__attribute__((section(".text.hook"), used, noinline))
static void hb_exit_hook(void *self, int dst)
{
    hb_trace_log("XEXIT   ", (uint32_t)(uintptr_t)self, (uint32_t)dst);
    hb_surface_teardown();
    if (s_orig_exit) s_orig_exit(self, dst);
}


/* Fixed-VA LVGL surface: the app's code section (a binary linked at LV_SURFACE_VA
 * = 0x09280000) is copied there, I-cache flushed, then entry(0, fb, w, h) is
 * called and returns the per-frame fn. No relocation, no api callout table (the
 * relocatable-blob path corrupted LVGL — the relocatable-blob path corrupted LVGL's pool). Each app
 * is its own self-contained binary overwriting the slot, so switching apps just
 * re-copies + re-inits LVGL fresh. */
#define LV_SURFACE_VA   0x09280000u
#define LV_SURFACE_MAX  0x90000u        /* 576 KB slot (surface .bins are 200-450 KB) */
static int hb_run_surface_blob(void)
{
    typedef void *(*lvsurf_entry_t)(int op, void *fb, int w, int h);
    void *fn;
    if (!s_fb || !s_surface_code || !s_surface_code_len)
        return 0;
    if (s_surface_reloc_entry) {
        /* Relocatable blob: hb_reloc_load already copied+rebased the image into
         * an operator-new arena (and flushed its I-cache). Just call the rebased
         * entry — the app runs entirely from OS-heap memory, no fixed VA. */
        hb_trace_log("SURFRELO", (uint32_t)(uintptr_t)s_surface_reloc_entry, s_surface_code_len);
        fn = ((lvsurf_entry_t)s_surface_reloc_entry)(0, s_fb, FB_W, FB_H);
        s_surface_frame = (surface_frame_t)(uintptr_t)fn;
        hb_trace_log("SURFRUN ", (uint32_t)(uintptr_t)s_surface_frame, 0);
        return s_surface_frame ? 1 : 0;
    }
    /* the app binary was read to LV_SURFACE_VA by hb_app_launch; flush the
     * I-cache for the freshly written code, then call its entry. */
    hb_icache_invalidate(LV_SURFACE_VA, LV_SURFACE_MAX);
    hb_trace_log("SURFLOAD", LV_SURFACE_VA, s_surface_code_len);
    fn = ((lvsurf_entry_t)(LV_SURFACE_VA | 1u))(0, s_fb, FB_W, FB_H);
    s_surface_frame = (surface_frame_t)(uintptr_t)fn;
    hb_trace_log("SURFRUN ", (uint32_t)(uintptr_t)s_surface_frame, 0);
    return s_surface_frame ? 1 : 0;
}

/* Allocate the heap framebuffer + build the reusable source context over it,
 * once per deploy. Safe to call on the UI task (screen-build / draw). */
static void hb_gfx_ensure(void)
{
    if (s_gfx_ready)
        return;
    if (!s_fb)
        s_fb = (uint32_t *)((opnew_t)(ADDR_OP_NEW | 1u))(FB_W * FB_H * 4u);
    if (!s_fb)
        return;
    if (!hb_run_surface_blob())          /* LVGL/GL blob renders into s_fb, else: */
        hb_fill_test_pattern(s_fb);
    hb_silver_gfx_argb_surface(s_gfx_dev, s_gfx_ctx, s_fb, FB_W, FB_H);
    s_gfx_ready = 1;
    hb_trace_log("XPALLOC ", (uint32_t)(uintptr_t)s_fb, FB_W * FB_H * 4u);
}

__attribute__((section(".text.hook"), used, noinline))
static void draw_hook(void *view, void *model, const void *r)
{
    uint8_t  gc[348];
    int32_t  lb[4];                 /* rect {top,left,bottom,right} */
    (void)model; (void)r;

    hb_gfx_ensure();                /* heap framebuffer + source context (one-time) */
    if (!s_gfx_ready)
        return;

    /* Start the animation heartbeat once. The timer's message handler only Invals (re-
     * schedules a draw) + re-arms; the heavy LVGL work happens HERE, on the OS's
     * draw task, where lv_refr is proven safe (calling it in the timer context
     * crashed — different task/stack). */
    if (s_surface_frame && !s_timer_started) {
        s_timer_started = 1;
        hb_surface_start_timer(view);
    }
    if (s_ds_dbg < 8u) hb_trace_log("DSFRAME ", (uint32_t)(uintptr_t)s_surface_frame, s_ds_dbg);
    if (s_surface_frame)
        s_surface_frame();                  /* lv_timer_handler: advance + render */

    ((gfx0_t)(A_TGFX_CTOR | 1u))(gc);
    ((gfx1p_t)(A_FOCUS_EXBORDER | 1u))(view, gc);
    ((gfx1p_t)(A_LOCALBOUNDS | 1u))(view, lb);
    ((gfx1i_t)(A_SETPENMODE | 1u))(gc, 37);          /* mode 37: opaque copy */

    /* blit our whole framebuffer into the view's local bounds. During the
     * zoom-down quit the OS re-renders us at shrinking bounds, so the copy
     * SCALES — skip degenerate rects (a 0/1-px dest divides-by-zero in the
     * scaler) so quitting doesn't crash. */
    {
        int32_t dw = lb[3] - lb[1], dh = lb[2] - lb[0];
        if (s_ds_dbg < 8u) {
            s_ds_dbg++;
            hb_trace_log("XPDRAW  ", ((uint32_t)(dw & 0xffff) << 16) | (uint32_t)(dh & 0xffff),
                         (uint32_t)(uintptr_t)view);
        }
        if (dw >= 2 && dh >= 2)
            hb_silver_gfx_blit(gc, s_gfx_ctx, FB_W, FB_H, lb, 0xff);
    }

    ((gfx0_t)(A_TGFX_DTOR | 1u))(gc);
}

/* Point `view` at a copy of its OWN vtable whose draw callback slot runs
 * draw_hook, so the OS renders our custom pixels for this view. Copies the
 * view's actual class vtable (works for view / composite view / etc.). */
static void hb_install_custom_draw(void *view)
{
    uint32_t *src = *(uint32_t **)view;               /* the view's current vtable */
    uint32_t *dst = (uint32_t *)VTABLE_COPY_ADDR;
    unsigned i;
    for (i = 0; i < VTABLE_COPY_SIZE / 4u; i++)
        dst[i] = src[i];
    dst[DRAW_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&draw_hook) | 1u;
    /* override message handler (vtable slot +0x40) so our heartbeat timer's timer-message
     * drives LVGL on the UI task. Capture the class's real message handler to forward
     * non-timer messages (it may be a subclass override, not view message handler). */
    s_orig_msg = (msg_fn_t)(uintptr_t)src[MSG_SLOT_OFF / 4u];
    dst[MSG_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_msg_hook) | 1u;
    hb_trace_log("HMINSTL ", (uint32_t)(uintptr_t)s_orig_msg,
                 (uint32_t)(uintptr_t)view);
    /* override both destructors so we stop the heartbeat before the view dies */
    s_orig_dtor0 = (dtor_t)(uintptr_t)src[DTOR0_SLOT_OFF / 4u];
    s_orig_dtor1 = (dtor_t)(uintptr_t)src[DTOR1_SLOT_OFF / 4u];
    dst[DTOR0_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_dtor_hook0) | 1u;
    dst[DTOR1_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_dtor_hook1) | 1u;
    /* visibility handler(false) on hide -> stop the heartbeat before the view is freed */
    s_orig_visibility = (vis_fn_t)(uintptr_t)src[VIS_SLOT_OFF / 4u];
    dst[VIS_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_visibility_hook) | 1u;
    /* exit-begin handler -> full teardown when the screen leaves for home (the real signal) */
    s_orig_exit = (exit_fn_t)(uintptr_t)src[EXIT_SLOT_OFF / 4u];
    dst[EXIT_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_exit_hook) | 1u;
    hb_icache_invalidate(VTABLE_COPY_ADDR, VTABLE_COPY_SIZE);
    *(uint32_t *)view = VTABLE_COPY_ADDR;             /* swap this view's vtable */
}

/* GL surface: a real GL view injected into the blank canvas for HB_APP_KIND_GL_SURFACE
 * apps. Its draw callback (run between the GL view draw's begin-scene/end-scene) renders GL into
 * the view's own GPU pixel buffer on the GPU; the OS composites it into the view tree
 * (sub-rect, clipped, z-ordered) — GL embedded in an app view, not a fullscreen overlay. */
#define GL_VTABLE_COPY_ADDR 0x09138000u   /* separate from the custom-draw view's copy */
#define GLVIEW_CTX_OFF      0x260u        /* the GL view's context field                */

static uint32_t s_gl_frame;

__attribute__((section(".text.hook"), used, noinline))
static void gl_draw_hook(void *view, void *model, const void *r)
{
    void *ctx = *(void **)((char *)view + GLVIEW_CTX_OFF);   /* GL-context field (set by the view's draw) */
    (void)model; (void)r;
    if (!ctx)
        return;
    /* animation heartbeat: reuse the LVGL timer (timer-message -> Inval -> the GL view's draw
       -> here). hb_msg_hook only Invals (s_surface_frame is 0 for GL apps). */
    if (!s_timer_started) {
        s_timer_started = 1;
        hb_surface_start_timer(view);
    }
    s_gl_frame++;
    /* Run the loaded GL app's per-frame draw fn into the current view context
       (between its begin-scene/end-scene). The app owns the content; the resident
       only drives the view + heartbeat. No app loaded -> nothing to draw. */
    if (s_gl_frame_fn) {
        (void)ctx;
        s_gl_frame_fn(FB_W, FB_H, s_gl_frame);
        return;
    }
}

/* Copy the view's GL view vtable + patch draw callback (GL) + the heartbeat message handler and
 * the timer-lifecycle (dtor/visibility handler) slots — same hooks as the custom-draw view, so the timer
 * drives the GL view's animation and stops cleanly on hide. Draw stays the GL view's draw
 * (its begin-scene/end-scene wraps our draw callback). */
static void hb_install_gl_draw(void *view)
{
    uint32_t *src = *(uint32_t **)view;
    uint32_t *dst = (uint32_t *)GL_VTABLE_COPY_ADDR;
    unsigned i;
    for (i = 0; i < VTABLE_COPY_SIZE / 4u; i++)
        dst[i] = src[i];
    dst[DRAW_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&gl_draw_hook) | 1u;
    s_orig_msg = (msg_fn_t)(uintptr_t)src[MSG_SLOT_OFF / 4u];
    dst[MSG_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_msg_hook) | 1u;
    s_orig_dtor0 = (dtor_t)(uintptr_t)src[DTOR0_SLOT_OFF / 4u];
    s_orig_dtor1 = (dtor_t)(uintptr_t)src[DTOR1_SLOT_OFF / 4u];
    dst[DTOR0_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_dtor_hook0) | 1u;
    dst[DTOR1_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_dtor_hook1) | 1u;
    s_orig_visibility = (vis_fn_t)(uintptr_t)src[VIS_SLOT_OFF / 4u];
    dst[VIS_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_visibility_hook) | 1u;
    /* exit-begin handler -> full teardown when the screen leaves for home (the real signal) */
    s_orig_exit = (exit_fn_t)(uintptr_t)src[EXIT_SLOT_OFF / 4u];
    dst[EXIT_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_exit_hook) | 1u;
    hb_icache_invalidate(GL_VTABLE_COPY_ADDR, VTABLE_COPY_SIZE);
    *(uint32_t *)view = GL_VTABLE_COPY_ADDR;
}

/* The kind of the app currently presenting our screen — selects which view the
 * layer-creation hook injection adds (custom-draw LVGL surface vs a hardware-GL GL view). */
static uint16_t s_cur_app_kind = HB_APP_KIND_CUSTOM_SCREEN;

/* Inject our own view into a screen the OS PRESENTS, so the OS's layer
 * pass composites it natively (post-hoc injection onto an existing screen builds
 * the view fine but it never gets a composited layer, so it doesn't draw). When
 * the presenter presents a screen it calls the layer-creation hook on the screen's
 * Silver template view, which recurses + layers every child. We hook that slot:
 * if armed, add-child our view to the template's canvas BEFORE the original runs,
 * so our view rides the same layer pass. */
#define TEMPLATE_VTABLE    0x087ab73cu  /* Silver template view class vtable     */
#define TEMPLATE_LAYERS_SLOT  0x168u       /* layer-creation hook slot offset      */
#define TEMPLATE_LAYERS_ORIG  0x081e0a02u  /* composite view layer-creation hook   */
#define INJECT_FLAG_ADDR   0x09135ff8u  /* one-shot: inject into the next present */
#define A_ADDCHILD         0x0802dee8u  /* composite view's child-attach — own it */
#define A_VIEW_PARENTSHOW  0x0847b274u  /* re-show the view under its parent on reuse */
#define A_ZORDER_RAISE  0x080372b8u  /* raise the view to the front of its z-order           */
#define VIEW_ZORDER_OFF   0x238u       /* the view's z-order field byte offset                 */

typedef void (*cml_t)(void *self, void *holder, int tt, int b);
typedef void (*addchild_t)(void *composite, void *child);
typedef void (*show1_t)(void *self, int on);

/* The custom-screen canvas is ONE persistent cached object shared by all custom-
 * screen apps. We cache one child view PER KIND — the custom-draw LVGL surface and the
 * hardware-GL GL view — and on each launch re-show the one matching the launching
 * app's kind while hiding the other. So switching between an LVGL app and a GL app
 * on the shared canvas shows the right view, with no per-launch accumulation. */
static void *s_inject_lvgl_child;   /* custom-draw color / LVGL surface view */
static void *s_inject_gl_child;     /* hardware-GL GL view           */
static void *s_inject_canvas;

__attribute__((section(".text.hook"), used, noinline))
static void templ_hook(void *self, void *holder, int tt, int b)
{
    /* When armed (tt==enter key=0), inject our own view into the incoming screen's
     * canvas during the layer pass: the custom-draw surface (LVGL) or a real
     * GL view (GL), per the launching app's kind. */
    uint8_t *inj = (uint8_t *)INJECT_FLAG_ADDR;
    if (*inj && tt == 0) {
        *inj = 0;
        void *canvas = hb_silver_template_canvas(self);
        hb_trace_log("TMPLCML ", (uint32_t)(uintptr_t)self, (uint32_t)(uintptr_t)canvas);
        if (canvas) {
            /* Suppress the OS edge-swipe-to-go-back for THIS surface launch. Done
             * here (per launch, when armed) — NOT only at view install — because the
             * custom-screen canvas is a cached view reused via parent-show, so install
             * runs only once; without re-asserting here a relaunch would inherit the
             * enabled state teardown last restored, letting a edge-swipe-back start and tear
             * us down mid-gesture (the freeze). Set once per launch here; teardown
             * (exit-begin handler) restores it on quit. NEVER in the per-frame draw hooks —
             * that's a hot path (panics). */
            hb_nav_backswipe_set(0);
            int gl = (s_cur_app_kind == HB_APP_KIND_GL_SURFACE);
            void **slot, *other;
            /* a different canvas object -> our cached children belong to a dead
               canvas; forget them so we build fresh on this one. */
            if (canvas != s_inject_canvas) {
                s_inject_lvgl_child = 0;
                s_inject_gl_child = 0;
                s_inject_canvas = canvas;
            }
            slot  = gl ? &s_inject_gl_child   : &s_inject_lvgl_child;
            other = gl ?  s_inject_lvgl_child :  s_inject_gl_child;
            if (other)   /* hide the other-kind child (visibility handler(false) stops its work) */
                ((show1_t)(A_VIEW_PARENTSHOW | 1u))(other, 0);
            if (*slot) {
                /* reuse the matching child: re-show + bring to top + repaint */
                ((show1_t)(A_VIEW_PARENTSHOW | 1u))(*slot, 1);
                ((show1_t)(A_ZORDER_RAISE | 1u))((char *)*slot + VIEW_ZORDER_OFF, 1);
                ((gfx0_t)(A_VIEW_INVAL | 1u))(*slot);
                hb_trace_log("TMPLREU ", (uint32_t)(uintptr_t)*slot, (uint32_t)gl);
            } else if (gl) {
                /* a real GL view filling the whole 240x432 canvas — hardware GL
                 * composited fullscreen by the OS via the view's own pixmap (no
                 * window-surface rotation/clipping; the OS sizes the pixmap to the
                 * view bounds and composites it at the right place/orientation). */
                void *v = hb_silver_make_gl_view(canvas, self, 0, 0, FB_W, FB_H);
                if (v) {
                    ((addchild_t)(A_ADDCHILD | 1u))(canvas, v);
                    hb_install_gl_draw(v);
                    hb_silver_realize_view(v, canvas);
                    *slot = v;
                }
                hb_trace_log("GLINJ   ", (uint32_t)(uintptr_t)v, 0);
            } else {
                void *v = hb_silver_make_color_view(canvas, self, 0, 0, FB_W, FB_H,
                                                    0xff2030e0u);
                if (v) {
                    *(uint32_t *)((char *)v + 0x1b0u) |= 1u;  /* flags field |= hit-test flag */
                    ((addchild_t)(A_ADDCHILD | 1u))(canvas, v);
                    hb_install_custom_draw(v);
                    hb_silver_realize_view(v, canvas);
                    *slot = v;
                }
                hb_trace_log("TMPLINJ ", (uint32_t)(uintptr_t)v, 0);
            }
        }
    }
    ((cml_t)(TEMPLATE_LAYERS_ORIG | 1u))(self, holder, tt, b);   /* forward */
}

/* ---- bundle-driven app registry ----
 * Apps are no longer a hardcoded table: a staged .app PACK (HBPK) is parsed by
 * hb_app_register_pack, which registers every app's icon (one batched cache-table
 * extension), label, and home item, and fills g_launch[] with each app's launch
 * info. The tap router looks an app up by index and launches it by kind. Host
 * stages the pack at APP_PACK_ADDR; we copy it to the heap (stable — the icon
 * bitmaps it contains become cache entries) before registering. */
#define APP_PACK_ADDR  0x09170000u
#define APP_DISK_PATH  "/Apps/AllApps.pack"          /* installed app manifest on disk */
#define APP_PACK_MAX   (1024u * 1024u)               /* pack = labels + shared screen (icons are disk-served) */
#define HB_MAX_APPS    64   /* registrar arrays + pagination cap at 64 (6/page x 10 pages) */

static hb_app_launch_t g_launch[HB_MAX_APPS];
static int             g_launch_count;
static uint8_t         s_apps_loaded;     /* per-deploy: block 3 runs once per re-exec */

/* Total byte length of a staged HBPK (max bundle offset + that bundle's len). */
static uint32_t hb_pack_size(const uint8_t *pack)
{
    uint32_t cnt = *(const uint32_t *)(pack + 8u);
    const uint32_t *offs = (const uint32_t *)(pack + 12u);
    uint32_t i, end = 0;
    if (cnt == 0u || cnt > 64u)
        return 0;
    for (i = 0; i < cnt; i++) {
        uint32_t blen = *(const uint32_t *)(pack + offs[i] + 44u);  /* bundle_len (hdr: +44 after name_off) */
        if (offs[i] + blen > end)
            end = offs[i] + blen;
    }
    return end;
}

/* A relocatable .hbapp was just read into LV_SURFACE_VA. Returns 1 if it has a
 * valid reloc header (so reloc_load can run). We LOG the expected vs actual size
 * (APPNEED: a short read would mean a truncated install) but do NOT gate on it —
 * just let it try to run; the trace tells us if a launch ever misbehaves. `got` =
 * bytes hb_fs_read returned. */
static int hb_blob_complete(uint32_t got)
{
    const uint32_t *h = (const uint32_t *)LV_SURFACE_VA;   /* magic,entry,img,span,nrel,align */
    if (got < 24u || h[0] != HB_RELOC_MAGIC)
        return 0;
    hb_trace_log("APPNEED ", 24u + h[2] + 4u * h[4], got);  /* expected, got (log only) */
    return 1;
}

/* Launch app record `L` from the home controller `self` (UI task). */
static void hb_app_launch(void *self, const hb_app_launch_t *L)
{
    s_cur_app_kind = L->app_kind;     /* selects the injected view (templ_hook) */

    if (L->app_kind == HB_APP_KIND_GL_SURFACE) {
        /* GL app: load its relocatable .hbapp into an arena (like an LVGL surface
         * app), call the entry to get the per-frame GL draw fn, and inject a
         * fullscreen GL view the OS composites. Free the previous app's arena FIRST
         * so relaunching/app-switching doesn't leak app code (and reset s_cur_app_id
         * so a CUSTOM_SCREEN app launched next reloads instead of reusing the freed
         * arena). The GL app blob is tiny (~3 KB) so we reload every launch. */
        typedef void *(*gl_entry_t)(int op, void *fb, int w, int h);
        char path[64];
        const char *pre = "/Apps/Executables/";
        int i = 0, j;
        uint32_t len;
        s_surface_frame = 0;
        s_gl_frame_fn = 0;
        hb_surface_drain_pending();     /* reclaim the last quit's deferred arena */
        if (s_surface_arena) {
            ((opdelete_t)(ADDR_OP_DELETE | 1u))(s_surface_arena);
            s_surface_arena = 0;
        }
        s_cur_app_id = 0;
        for (j = 0; pre[j]; j++) path[i++] = pre[j];
        for (j = 0; j < 23 && L->name[j]; j++) path[i++] = L->name[j];
        path[i++] = '.'; path[i++] = 'h'; path[i++] = 'b';
        path[i++] = 'a'; path[i++] = 'p'; path[i++] = 'p'; path[i] = 0;
        len = hb_fs_read(path, (void *)LV_SURFACE_VA, LV_SURFACE_MAX);
        hb_trace_log("GLAPPBIN", len, (uint32_t)L->app_id);
        if (hb_blob_complete(len)) {
            void *entry = hb_reloc_load((const void *)LV_SURFACE_VA,
                                        (void *(*)(uint32_t))(ADDR_OP_NEW | 1u),
                                        &s_surface_arena);
            if (entry)
                s_gl_frame_fn = (gl_frame_fn_t)(uintptr_t)((gl_entry_t)entry)(0, 0, FB_W, FB_H);
            hb_trace_log("GLAPPFN ", (uint32_t)(uintptr_t)s_gl_frame_fn, (uint32_t)L->app_id);
        }
        *(uint8_t *)INJECT_FLAG_ADDR = 1;
        hb_silver_push_screen_id(self, L->screen_id, L->layout_id);
        return;
    }

    if (L->app_kind == HB_APP_KIND_CUSTOM_SCREEN) {
        /* The app's relocatable blob lives on disk at /Apps/Executables/<bundle-name>.hbapp
         * (kept out of the pack so the pack stays small). Read it into the surface
         * slot when launching a DIFFERENT app, then re-init LVGL fresh. */
        if (L->app_id != s_cur_app_id) {
            char path[64];
            const char *pre = "/Apps/Executables/";
            int i = 0, j;
            /* Switching apps: stop the timer from calling the old app's frame fn,
             * then free the previous app's arena before allocating the next. */
            s_surface_frame = 0;
            hb_surface_drain_pending();  /* reclaim the last quit's deferred arena */
            if (s_surface_arena) {
                ((opdelete_t)(ADDR_OP_DELETE | 1u))(s_surface_arena);
                s_surface_arena = 0;
            }
            for (j = 0; pre[j]; j++) path[i++] = pre[j];
            for (j = 0; j < 23 && L->name[j]; j++) path[i++] = L->name[j];
            path[i++] = '.'; path[i++] = 'h'; path[i++] = 'b';
        path[i++] = 'a'; path[i++] = 'p'; path[i++] = 'p'; path[i] = 0;
            /* Read the app into the staging slot. A relocatable .hbapp (magic
             * 'HRL1') is loaded into an OS-heap arena and run from there (no fixed
             * VA); a raw .bin keeps the legacy fixed-VA path. The staging read at
             * LV_SURFACE_VA is transient — for a reloc app the code EXECUTES from
             * the arena, not here. */
            s_surface_code_len = hb_fs_read(path, (void *)LV_SURFACE_VA, LV_SURFACE_MAX);
            hb_trace_log("APPBIN  ", s_surface_code_len, (uint32_t)L->app_id);
            s_surface_reloc_entry = 0;
            if (hb_blob_complete(s_surface_code_len)) {
                s_surface_reloc_entry =
                    hb_reloc_load((const void *)LV_SURFACE_VA,
                                  (void *(*)(uint32_t))(ADDR_OP_NEW | 1u),
                                  &s_surface_arena);
                hb_trace_log("APPRELO ", (uint32_t)(uintptr_t)s_surface_reloc_entry,
                             (uint32_t)L->app_id);
                s_surface_code = s_surface_reloc_entry ? 1u : 0u;
            } else {
                s_surface_code = (s_surface_code_len >= 64u) ? LV_SURFACE_VA : 0;
            }
            s_cur_app_id = L->app_id;
        }
        s_gfx_ready = 0;                /* hb_gfx_ensure re-runs the (new) app   */
        s_surface_frame = 0;
        *(uint8_t *)INJECT_FLAG_ADDR = 1;            /* arm the custom-draw surface inject */
        hb_silver_push_screen_id(self, L->screen_id, L->layout_id);
    } else {
        hb_silver_fire_handler(self, L->os_handler, (void *)1);
    }
}

/* RAW-MESSAGE slot (+0x188): taps arrive here. Translate the message EXACTLY
 * once; if it's one of our custom items ("sbitem.hb.<folder>.clicked"), route it
 * and consume; otherwise the helper dispatches it directly (no re-forward,
 * which would translate the same message twice). */
__attribute__((section(".text.hook"), used, noinline))
static int rawmsg_hook(void *self, void *msg)
{
    char ev[64];
    int r = hb_silver_rawmsg(self, msg, "sbitem.hb.", ".clicked", ev, sizeof ev);
    if (r == HB_SILVER_RAWMSG_CLAIMED) {
        /* ev = "sbitem.<sbid>.clicked"; sbid ("hb.<folder>") begins after "sbitem."
         * (7 chars) and runs to ".clicked". Match it against the registry by sbid
         * STRING (never a numeric index) — an app keeps its identity + home-screen
         * position as others are added/removed. The '.' guard after the sbid
         * prevents a prefix collision (hb.pong vs hb.pong2). */
        const char *sb = ev + 7;
        int k;
        for (k = 0; k < g_launch_count; k++) {
            const char *a = g_launch[k].sbid, *p = sb;
            while (*a && *a == *p) { a++; p++; }
            if (*a == 0 && *p == '.') {       /* full sbid matched, then ".clicked" */
                hb_trace_log("HBTAP   ", (uint32_t)k, g_launch[k].app_id);
                hb_app_launch(self, &g_launch[k]);
                break;
            }
        }
        return 1;                            /* our app handled the tap */
    }
    return r;
}

/* ACTION slot: setup runs here, in the proven-stable dispatch context. */
__attribute__((section(".text.hook"), used, noinline))
static int action_hook(void *self, const char *name, void *msg)
{
    /* (2) Per-deploy: keep resource-getter pointed at THIS build's hook (full patch on
     * a fresh boot; re-aim on a re-push, the trampoline persists). Resident code
     * moves between builds, so this must refresh each deploy; FLAG_ADDR cleared at
     * entry. (Icon registration is per-BOOT, in block (3): it mutates the global
     * bitmap-cache table, which must not be re-extended on a bare re-push.) */
    uint8_t *icon_flag = (uint8_t *)FLAG_ADDR;
    if (*icon_flag == 0) {
        *icon_flag = 1;
        if (*(uint16_t *)(uintptr_t)ADDR_GETRESOURCE == GETRES_PROLOGUE0)
            hb_silver_patch_function(ADDR_GETRESOURCE, (void *)&getres_hook,
                                     (void *)GETRES_TRAMP_ADDR);
        else
            hb_silver_repoint_function(ADDR_GETRESOURCE, (void *)&getres_hook);
        hb_trace_log("GRPATCH ", *(uint32_t *)(uintptr_t)ADDR_GETRESOURCE, 0);

        /* Re-aim Home-triple-click at our screenshot hook. We never forward through
         * a trampoline (we reimplement the original), so a plain repoint each deploy
         * is correct whether the entry is pristine or already redirected. */
        hb_silver_repoint_function(A_HOME_TRIPLE, (void *)&hb_home_triple_hook);
        hb_trace_log("SHOTPCH ", *(uint32_t *)(uintptr_t)A_HOME_TRIPLE, 0);

        /* demo status-bar 9:41 clock: patch the locale short-time formatter
         * (0x084a9adc) with a 4-byte B.W — its prologue has a PC-relative op within
         * the first 10 bytes, so the 10-byte trampoline jump won't fit. */
        if (*(uint16_t *)(uintptr_t)A_DTTS == DTTS_PROLOGUE)
            hb_silver_patch_function_bw(A_DTTS, (void *)&hb_dtts_hook,
                                        (void *)DTTS_TRAMP_ADDR, 4);
        else
            hb_silver_repoint_function_bw(A_DTTS, (void *)&hb_dtts_hook);
        hb_trace_log("DTTSPCH ", *(uint32_t *)(uintptr_t)A_DTTS, 0);

        /* demo status-bar 100% battery (not charging): both index getters via B.W */
        if (*(uint16_t *)(uintptr_t)A_BATT_LEVEL == BATT_PROLOGUE)
            hb_silver_patch_function_bw(A_BATT_LEVEL, (void *)&hb_batt_level_hook,
                                        (void *)BATT_LEVEL_TRAMP, 4);
        else
            hb_silver_repoint_function_bw(A_BATT_LEVEL, (void *)&hb_batt_level_hook);
        if (*(uint16_t *)(uintptr_t)A_BATT_STATUS == BATT_PROLOGUE)
            hb_silver_patch_function_bw(A_BATT_STATUS, (void *)&hb_batt_status_hook,
                                        (void *)BATT_STATUS_TRAMP, 4);
        else
            hb_silver_repoint_function_bw(A_BATT_STATUS, (void *)&hb_batt_status_hook);
        hb_trace_log("BATTPCH ", *(uint32_t *)(uintptr_t)A_BATT_LEVEL,
                                 *(uint32_t *)(uintptr_t)A_BATT_STATUS);

#if HB_REC_INTERCEPT
        hb_rec_install();       /* singleton-controller message handler filter for the capture timer */
#endif
    }

    /* (3) Per-boot (gated on real OS chain state): publish the (shared) label
     * blob, then register every app from the staged .app PACK. We copy the pack
     * to the heap first so the icon bitmaps it contains (which become bitmap-cache
     * entries) live in stable memory. The registrar does the batched icon
     * cache-table extension, adds the home items, and fills g_launch[]. */
    if (!s_apps_loaded) {
        s_apps_loaded = 1;                 /* once per DEPLOY (.bss, zeroed at entry) */
        /* OS-mutating registration (icon table extension, labels, screen, home
         * items) runs only once per BOOT — gated on whether our bitmap-cache
         * entries are still LIVE (hb_silver_icon_live). That is robust across a
         * no-reboot re-push (entries persist -> reg=0, skip OS mutation) AND a
         * reboot (cache reset -> reg=1, re-register). We do NOT gate on the 'HBap'
         * label tag here: the OS can drop it across a re-push while our entries
         * persist, which would wrongly RE-extend the cache (double-registration) and
         * leave the home items pointing at the wrong icon ids. The tap table
         * g_launch[] is re-filled EVERY deploy so a re-push works without a reboot. */
        int reg = !hb_silver_icon_live();
        /* Install the event-path touch capture once per boot (patch is RAM, gone
         * after a reboot; reg tracks per-boot OS state). Only patch if unpatched. */
        if (reg && *(uint16_t *)(uintptr_t)ADDR_POSTTOUCH == POSTTOUCH_PROLOGUE) {
            /* 12-byte prologue (a ldr.w spans bytes 8-11), not the default 10 */
            hb_silver_patch_function_n(ADDR_POSTTOUCH, (void *)&hb_posttouch_hook,
                                       (void *)POSTTOUCH_TRAMP, 12);
            hb_trace_log("PTPATCH ", *(uint32_t *)(uintptr_t)ADDR_POSTTOUCH, 0);
        }
        /* Pack source: prefer the on-DISK pack (apps are real installed files;
         * the host writes it straight to /Apps via the device-host mount). Only
         * fall back to a DRAM-staged pack if disk has none — staging 280 KB into
         * live OS DRAM at 0x09170000 corrupts the running compositor and white-
         * screens, so disk is the safe default path. (hb_fs is proven safe.) */
        const uint8_t *staged = (const uint8_t *)APP_PACK_ADDR;
        const void *pack = 0;
        uint32_t psz = 0, src = 0;
        uint8_t *hp = (uint8_t *)((opnew_t)(ADDR_OP_NEW | 1u))(APP_PACK_MAX);
        if (hp) {
            uint32_t n = hb_fs_read(APP_DISK_PATH, hp, APP_PACK_MAX);
            if (n >= 16u && *(const uint32_t *)hp == HB_APP_PACK_MAGIC) {
                pack = hp; psz = n; src = 2;              /* 2 = read from disk */
            }
        }
        if (!pack && *(const uint32_t *)staged == HB_APP_PACK_MAGIC) {
            psz = hb_pack_size(staged);
            uint8_t *dp = psz ? (uint8_t *)((opnew_t)(ADDR_OP_NEW | 1u))(psz) : 0;
            if (dp) {
                for (uint32_t i = 0; i < psz; i++)
                    dp[i] = staged[i];
                pack = dp; src = 1;                       /* 1 = DRAM-staged */
                hb_fs_write(APP_DISK_PATH, dp, psz);      /* install to disk */
            }
        }
        hb_trace_log("APPSRC  ", src, psz);
        if (pack)
        g_launch_count = hb_app_register_pack(pack,
                            (void *(*)(uint32_t))(ADDR_OP_NEW | 1u),
                            (void (*)(const char *, uint32_t, uint32_t))&hb_trace_log,
                            g_launch, HB_MAX_APPS,
                            (void *)SILVERDB_OBJ, (void *)SILVERDB_NODE, reg);
        hb_trace_log("APPSREG ", (uint32_t)g_launch_count, (uint32_t)reg);
    }
    return ((dispatch_t)HB_SILVER_HOME_DISPATCH_ORIG)(self, name, msg);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_reset();
    hb_trace_log("RESIDENT", 0x09130000u, 0);

    /* Reboot-free iteration: ALWAYS (re)install the hooks with THIS build's
     * addresses, so a plain re-push takes effect without a power-cycle.
     * Forwarding uses the hardcoded stock originals, so re-hooking over our own
     * hook is safe. Clear the per-deploy icon-serve guard so action_hook
     * refreshes the resolver pointer to this build. App (re)registration is
     * gated separately on real OS chain state (hb_silver_resdb_has_tag), which
     * is correct across both a re-push and a (DRAM-preserving) panic-reboot. */
    *(uint8_t *)FLAG_ADDR = 0;
    *(uint8_t *)SCREENSHOT_FLAG_ADDR = 1;  /* default Screenshot; the app sets mode 0/1/2 */
    *(uint8_t *)STATUSBAR_FLAG_ADDR = 0;   /* battery-100% override off by default        */
    /* Icon resolver across a no-reboot re-push: if the apps are already registered
     * (OS chain tag present, so action_hook will SKIP re-registration), re-arm the
     * lazy resolver to THIS build's function from the persisted state — otherwise a
     * re-push leaves the resolver pointing at the previous (overwritten) build and
     * every home icon renders blank, killing the artwork-based tap regions. On a
     * fresh boot (tag absent) registration installs it, so just clear any stale
     * pointer here. */
    /* Icon resolver across a no-reboot re-push: re-arm it whenever our cache
     * entries are STILL LIVE (robust check — NOT the OS label tag, which the OS
     * drops across a re-push and would make us wrongly clear the resolver and
     * blank every homebrew icon). If the cache was reset by a reboot, clear any
     * stale pointer; the re-registration below re-installs it. */
    if (hb_silver_icon_live())
        hb_silver_icon_rearm();
    else
        hb_silver_icon_reset_resolver();
        /* stop a heartbeat timer left armed by a previous deploy — its callback now
     * points into overwritten resident code.
     *
     * Only if that deploy was in THIS boot. The magic and the OS timer object live
     * in resident DRAM, which survives a DRAM-preserving reboot (a force restart,
     * or a panic) — e.g. when the iPod restarts while an app is on screen, before
     * hb_surface_stop_timer runs. The OS event server the timer was registered with
     * does NOT survive: the object's links point into the previous boot's state, and
     * A_TIMER_STOP on it never returns. This entry runs inside the SCSI exec, so the
     * whole USB interface then hangs (the host resets the device every 10-30 s) and,
     * since the magic survives every further force restart, every later deploy hangs
     * the same way until DRAM is cleared.
     *
     * Same boot <=> our home dispatch hook is still installed: the OS image (and the
     * vtable we patch) is reloaded on every boot, cold or DRAM-preserving — the same
     * fact action_hook relies on for "full patch on a fresh boot" (GETRES_PROLOGUE0).
     * Read it BEFORE re-hooking below. A stale timer is just forgotten. */
    {
        uint32_t disp = *(uint32_t *)(uintptr_t)(HB_SILVER_HOME_VTABLE + HB_SILVER_HOME_DISPATCH_SLOT);
        int same_boot = disp >= 0x09130000u && disp < 0x09140000u;
        if (*(uint32_t *)TIMER_MAGIC_ADDR == TIMER_MAGIC) {
            if (same_boot)
                ((timer1_t)(A_TIMER_STOP | 1u))((void *)TIMER_ADDR);
            *(uint32_t *)TIMER_MAGIC_ADDR = 0;
            hb_trace_log("TIMRCLR ", (uint32_t)same_boot, disp);
        }
    }
    /* s_gfx_ready / s_fb live in .bss (zeroed at entry), so the custom-draw surface is
     * rebuilt — and the framebuffer re-allocated from the heap — this deploy. */

    uint32_t p1 = hb_silver_uitask_hook(HB_SILVER_HOME_VTABLE,
                                        HB_SILVER_HOME_RAWMSG_SLOT, (void *)&rawmsg_hook);
    uint32_t p2 = hb_silver_uitask_hook(HB_SILVER_HOME_VTABLE,
                                        HB_SILVER_HOME_DISPATCH_SLOT, (void *)&action_hook);
    /* Hook the template view's layer-creation hook slot (forwards to
     * the hardcoded original, so re-hooking over our own hook is safe). */
    *(uint8_t *)INJECT_FLAG_ADDR = 0;
    uint32_t p3 = hb_silver_uitask_hook(TEMPLATE_VTABLE, TEMPLATE_LAYERS_SLOT,
                                        (void *)&templ_hook);
    hb_trace_log("ARMED   ", p1, p2);
    hb_trace_log("ARMEDCML", p3, 0);

    /* Re-aim the singleton-controller message handler filter HERE (at exec), not lazily in
     * action_hook: after a re-deploy the patched slot points at the PREVIOUS build's
     * hook (now-overwritten code), and the OS sends the singleton messages
     * constantly — waiting for the first tap to re-aim would crash in that window. */
#if HB_REC_INTERCEPT
    hb_rec_install();
#endif
}
