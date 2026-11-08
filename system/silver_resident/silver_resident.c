/* silver_resident — the resident system daemon. It stays in the OS address
 * space to host every homebrew app on the home screen.
 *
 * It is a relocatable .hbapp (/Library/resident.hbapp) that the firmware bootstrap
 * (system/disk_bootstrap) loads into its own OS-heap block, once per boot. It
 * owns NO fixed DRAM: 0x0910xxxx-0x094xxxxx is all live OS heap, so every
 * trampoline, vtable copy, timer object, flag and the trace ring lives in this
 * image's .bss, and the state apps share is one hb_shared_t (hb_shared.h)
 * passed to each app's entry.
 *
 * It installs a small set of OS hooks:
 *   - home controller action dispatch slot (vtable 0x087cb9dc, +0x190): runs in
 *     UI-task context; (re)registers the on-disk app pack (/Library/AllApps.pack)
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
#include "compat_fixes.h"
#include "hb_silver_res.h"
#include "hb_silver_uitask.h"
#include "hb_silver_icon.h"
#include "hb_silver_app.h"
#include "hb_silver_gfx.h"
#include "hb_reloc.h"
#include "hb_app_api.h"
#include "hb_touch_mb.h"
#include "touch_latch.h"
#include "hb_heap.h"
#include "hb_record.h"
#include "hb_app_bundle.h"
#include "hb_app_catalog.h"
#include "hb_shared.h"
#include "hb_sysfx.h"

#define HB_MAX_APPS 64   /* registrar arrays + pagination cap (6/page x 10 pages) */

/* The direct-cave boot candidate leaves a read-only C6 reporter at this
 * firmware-owned address.  Guard every write with its magic so this resident
 * remains harmless when launched by an older/public bootstrap. */
#define BOOT_REPORT_BASE             0x086c9b84u
#define BOOT_REPORT_MAGIC_OFFSET     0x2cu
#define BOOT_REPORT_MAGIC            0x32505452u
#define BOOT_REPORT_RESIDENT_OFFSET  0x50u
#define BOOT_REPORT_REGISTER_OFFSET  0x54u
#define BOOT_REPORT_APP_COUNT_OFFSET 0x58u
#define BOOT_REPORT_PRELOAD_OFFSET   0x5cu
#define BOOT_REPORT_FAILMASK_OFFSET  0x60u
#define BOOT_REPORT_LAUNCH_OFFSET    0x64u
#define BOOT_REPORT_APP_ID_OFFSET    0x68u
#define BOOT_REPORT_APP_KIND_OFFSET  0x6cu
#define BOOT_REPORT_ENTRY_OFFSET     0x70u
#define BOOT_REPORT_HEAP_BOOT_FREE_OFFSET    0xa0u
#define BOOT_REPORT_HEAP_BOOT_LARGEST_OFFSET 0xa4u
#define BOOT_REPORT_LAUNCH_COUNT_OFFSET      0xa8u
#define BOOT_REPORT_LAUNCH_LOG_OFFSET        0xacu
#define BOOT_REPORT_LAUNCH_LOG_WORDS         5u
#define BOOT_REPORT_LAUNCH_LOG_CAPACITY      8u

static int hb_boot_report_live(void)
{
    return *(volatile uint32_t *)(BOOT_REPORT_BASE + BOOT_REPORT_MAGIC_OFFSET) ==
           BOOT_REPORT_MAGIC;
}

static void hb_boot_report(uint32_t offset, uint32_t value)
{
    if (!hb_boot_report_live())
        return;
    *(volatile uint32_t *)(BOOT_REPORT_BASE + offset) = value;
    __asm__ volatile("dsb sy" : : : "memory");
}

enum hb_launch_report_result {
    HB_LAUNCH_LOAD_FAILED = 1,
    HB_LAUNCH_NEW_STARTED = 2,
    HB_LAUNCH_REUSED_STARTED = 3,
    HB_LAUNCH_NEW_ENTRY_FAILED = 4,
    HB_LAUNCH_REUSED_ENTRY_FAILED = 5,
};

static uint32_t s_launch_report_count;

/* Each bounded record is: sequence, app id, result, free heap, largest block. */
static void hb_report_launch_memory(uint32_t app_id, uint32_t result)
{
    if (!hb_boot_report_live())
        return;
    uint32_t sequence = ++s_launch_report_count;
    uint32_t index = (sequence - 1u) % BOOT_REPORT_LAUNCH_LOG_CAPACITY;
    uint32_t offset = BOOT_REPORT_LAUNCH_LOG_OFFSET +
        index * BOOT_REPORT_LAUNCH_LOG_WORDS * 4u;
    hb_boot_report(BOOT_REPORT_LAUNCH_COUNT_OFFSET, sequence);
    hb_boot_report(offset + 0u, sequence);
    hb_boot_report(offset + 4u, app_id);
    hb_boot_report(offset + 8u, result);
    hb_boot_report(offset + 12u, hb_os_heap_free());
    hb_boot_report(offset + 16u, hb_os_heap_largest());
}

/* The state apps share with us (touch mailbox, aux block, Screenshot toggles,
 * trace logger). hb_shared points here from the first line of payload_entry. */
static hb_shared_t s_shared;

/* The apps come from the on-disk pack — ids, labels, and icons live in each
 * launch record, not here. Setup runs in the (proven-stable) ACTION hook; the
 * RAW-MESSAGE hook claims our taps; the icon registrar EXTENDS the bitmap cache
 * table and serves our bitmaps at appended ids past the OS range. */
static uint8_t s_hooks_patched;   /* firmware function patches applied (action_hook) */

/* The trace ring lives in our .bss and is published through the C6/98 boot
 * report for host reads. On the next boot, entry can also find
 * the PREVIOUS boot's ring by scanning DRAM (DRAM survives a panic reboot),
 * copy it here, and let action_hook write the copy to disk — one write per boot,
 * never on the UI event path. Decode with tools/decode_lasttrace.py. */
#define LASTTRACE_PATH  "/Library/lasttrace.bin"
static uint32_t s_trace_ring[HB_TRACE_BYTES / 4u] __attribute__((aligned(16)));
static uint32_t s_prev_ring[HB_TRACE_BYTES / 4u] __attribute__((aligned(16)));
static uint8_t  s_prev_found;     /* s_prev_ring holds the previous boot's ring */

static void save_last_trace(void)
{
    /* This candidate has a host-readable boot reporter, so avoid the legacy
     * one-write-per-boot trace file during the controlled untether test. */
    if (hb_boot_report_live())
        return;
    if (!s_prev_found)
        return;
    s_prev_found = 0;
    hb_fs_write(LASTTRACE_PATH, s_prev_ring, HB_TRACE_BYTES);
    s_prev_ring[0] = 0;           /* a stale copy must not win a later scan */
}

/* Our OWN screen: a precompiled resource DB (SCRN/SCST/SLst/...) for
 * HBCustom_Screen, injected into the chain so the factory can build it (the
 * template's canvas is empty, so our injected view is the only content). The DB
 * ships inside the app pack; hb_app_register_pack registers it using the object
 * + chain-node storage below. */
static uint8_t s_silverdb_obj[0x100]  __attribute__((aligned(8)));  /* resource-db object */
static uint8_t s_silverdb_node[0x100] __attribute__((aligned(8)));  /* its chain node     */
#define SILVERDB_OBJ   ((void *)s_silverdb_obj)
#define SILVERDB_NODE  ((void *)s_silverdb_node)

/* Function-patch trampolines (the callable originals): the relocated stock
 * prologue plus a jump back. Code, in our .bss — this heap block is where the
 * OS already runs every app from. */
static uint8_t s_getres_tramp[32]      __attribute__((aligned(8)));
static uint8_t s_posttouch_tramp[32]   __attribute__((aligned(8)));
static uint8_t s_dtts_tramp[24]        __attribute__((aligned(8)));
static uint8_t s_batt_level_tramp[16]  __attribute__((aligned(8)));
static uint8_t s_batt_status_tramp[16] __attribute__((aligned(8)));
#define TRAMP(t)  ((uint32_t)(uintptr_t)(t) | 1u)

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
static const uint8_t GETRES_STOCK[10] = {
    0xf8, 0xb5, 0x04, 0x46, 0x00, 0x78, 0x1d, 0x46, 0x0e, 0x46
};

typedef const void *(*getres_t)(void *db, uint32_t type, long id, uint32_t *size);

__attribute__((section(".text.hook"), used, noinline))
static const void *getres_hook(void *db, uint32_t type, long id, uint32_t *size)
{
    /* serve our compiled screen's resources (our id range) from our injected DB
     * — which the main SilverDB lacks; forward everything else unchanged. */
    if ((uint32_t)id >= OUR_ID_BASE) {
        const void *r = ((getres_t)TRAMP(s_getres_tramp))(SILVERDB_OBJ, type, id, size);
        if (r)
            return r;
    }
    return ((getres_t)TRAMP(s_getres_tramp))(db, type, id, size);
}

/* Event-path touch: hook the touch manager's touch-posting entry (called for EVERY touch event,
 * before it drains the hardware touch list). We read the raw touch list at
 * that moment and LATCH a mailbox the LVGL surface indev reads — so a tap is
 * never missed (vs polling the transient list, which only caught ~1/4 of taps).
 * Mailbox: [magic, x, y, pressed] in s_shared.touch (read by hb_touch_mb.c);
 * the sample latched is the newest one queued (touch_latch.h). */
#define ADDR_POSTTOUCH     0x084a27f6u
#define POSTTOUCH_PROLOGUE 0xe92du        /* 'stmdb sp!,{r1-r9,lr}' — unpatched   */
static const uint8_t POSTTOUCH_STOCK[12] = {
    0x2d, 0xe9, 0xfe, 0x43, 0x04, 0x46, 0x00, 0x68,
    0xd0, 0xf8, 0xc4, 0x10
};
#define TOUCH_LIST_HEAD    0x089a5298u
#define TOUCH_MB_MAGIC     HB_TOUCH_MB_MAGIC
typedef int (*posttouch_t)(void *self);

__attribute__((section(".text.hook"), used, noinline))
static int hb_posttouch_hook(void *self)
{
    volatile int32_t *mb = s_shared.touch;
    int32_t x, y, pressed;
    int r = hb_tl_pick((volatile hb_tl_word *)TOUCH_LIST_HEAD, &x, &y, &pressed);
    if (r > 0) {
        mb[1] = x;
        mb[2] = y;
        mb[3] = pressed;
        mb[0] = (int32_t)TOUCH_MB_MAGIC;
    } else if (r == 0) {
        mb[3] = 0;                                   /* no touch -> released */
        mb[0] = (int32_t)TOUCH_MB_MAGIC;
    }
    /* Every queue's newest sample, for apps that track more than one finger
     * (hb_shared_t.touch_snap). The mailbox above is unchanged. */
    for (uint32_t q = 0; q < 8u; q++)
        hb_tl_newest((volatile hb_tl_word *)(TOUCH_LIST_HEAD + q * 0x18u),
                     s_shared.touch_snap.finger[q]);
    s_shared.touch_snap.seq++;
    s_shared.touch_snap.magic = HB_TOUCH_SNAP_MAGIC;
    return ((posttouch_t)TRAMP(s_posttouch_tramp))(self);
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
 * handler. The toggle is s_shared.screenshot_mode, which the Screenshot app
 * writes (screenshot mode again after every boot). */
#define A_HOME_TRIPLE         0x08241750u  /* Home triple-click handler entry             */
#define A_SCREENSHOT_KEY      0x08240860u  /* display-capture entry (self = controller)   */
#define A_GET_CUR_SBVIEW      0x083f4dfcu  /* read the current springboard view           */
#define SBVIEW_EDITING_OFF 0x268u       /* slot we read as the edit mode flag (the original gates on its false return) */
#define A_ACCESS_GETINST      0x082a80f4u  /* OS accessibility-shortcut handler getter    */
#define A_ACCESS_ON_TRIPLE    0x082a82a0u  /* accessibility triple-click handler(self)    */

typedef void *(*getvoid_t)(void);
typedef int   (*int_self_t)(void *self);
typedef void  (*void_self_t)(void *self);

/* Screen-recording capture hooks the singleton's message handler — a hot path
 * (every system message). Set HB_REC_INTERCEPT to 0 to compile out the intercept
 * entirely (the timer hook, its install, and the record branch below); screenshots
 * still work, recording just becomes unavailable. */
#ifndef HB_REC_INTERCEPT
#define HB_REC_INTERCEPT 0
#endif

#if HB_REC_INTERCEPT
static void hb_rec_timer_arm(void);   /* defined below (with the capture timer)     */
#endif

__attribute__((section(".text.hook"), used, noinline))
static void hb_home_triple_hook(void *self)
{
    uint8_t mode = s_shared.screenshot_mode;  /* 0 off,1 shot,2 rec */
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
 * at +6. Its trampoline redirects that literal load to an adjacent copy, then
 * returns with an absolute jump. Runs per clock re-format (~per minute). */
#define A_DTTS              0x084a9adcu    /* OS date/time-to-string formatter: out buffer, size, format, date-time */
#define DTTS_PROLOGUE       0xe92du        /* stmdb sp!,{...} — unpatched marker               */
static const uint8_t DTTS_RELOCATED[8] = {
    0x2d, 0xe9, 0xff, 0x5f, 0x04, 0x46, 0x02, 0x48
}; /* push; mov r4,r0; ldr r0,[pc,#8] -> trampoline+16 */
#define DTTS_LITERAL 0x084a9c5cu
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
    if (s_shared.statusbar_demo && (fmt == CLOCK_FMT_A || fmt == CLOCK_FMT_B))
        dt = &k_dt941;
    return ((dtts_t)TRAMP(s_dtts_tramp))(self, out, size, fmt, dt);
}

/* Same "Demo bar" toggle: 100% battery, not charging. the power-status view's update
 * routine draws a level bitmap (chosen from a battery level index) plus a status
 * overlay (chosen from a power-status index — the charging bolt; the 'unknown'
 * status draws no overlay). Plugged into the host the iPod is charging, so it shows
 * full + a bolt. So we force the level to its maximum (full) and the status to
 * 'unknown' (no bolt). Both getters have eight position-independent entry bytes;
 * an eight-byte absolute jump leaves their following branches/literals in place.
 * Like the clock, the icon re-picks on the next power-status update (battery poll). */
#define A_BATT_LEVEL        0x08215684u   /* returns a level index into battery artwork */
#define A_BATT_STATUS       0x08215600u   /* returns a status index for overlay select       */
#define BATT_PROLOGUE       0xb570u       /* push {r4,r5,r6,lr} — unpatched marker     */
static const uint8_t BATT_LEVEL_STOCK[8] = {
    0x70, 0xb5, 0x1e, 0x24, 0xd0, 0xf8, 0x9c, 0x01
};
static const uint8_t BATT_STATUS_STOCK[8] = {
    0x70, 0xb5, 0x05, 0x46, 0x1e, 0x24, 0x95, 0x20
};
#define BATT_LEVEL_FULL     22   /* level index that renders as a full battery (observed)  */
#define BATT_STATUS_NONE    30   /* status index that draws no charge overlay (observed)   */
typedef int (*batt_t)(void *self);

__attribute__((section(".text.hook"), used, noinline))
static int hb_batt_level_hook(void *self)
{
    if (s_shared.statusbar_demo)
        return BATT_LEVEL_FULL;
    return ((batt_t)TRAMP(s_batt_level_tramp))(self);
}

__attribute__((section(".text.hook"), used, noinline))
static int hb_batt_status_hook(void *self)
{
    if (s_shared.statusbar_demo)
        return BATT_STATUS_NONE;
    return ((batt_t)TRAMP(s_batt_status_tramp))(self);
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
#define VTABLE_COPY_SIZE   0x800u        /* generous (over-copy is safe; under is not) */
static uint32_t s_vtable_copy[VTABLE_COPY_SIZE / 4u] __attribute__((aligned(8)));
#define VTABLE_COPY_ADDR   ((uint32_t)(uintptr_t)s_vtable_copy)   /* our patched vtable copy */
/* The GL view's own vtable copy (see hb_install_gl_draw). */
static uint32_t s_gl_vtable_copy[VTABLE_COPY_SIZE / 4u] __attribute__((aligned(8)));
#define GL_VTABLE_COPY_ADDR ((uint32_t)(uintptr_t)s_gl_vtable_copy)  /* separate from the custom-draw view's copy */

/* The custom-draw view and the GL view are different classes and BOTH stay
 * alive (cached) on the shared canvas. A single set of saved "real" handlers
 * (message, visibility, destructors, exit) would be overwritten by whichever
 * view was installed last: after a GL app, the custom-draw view forwarded its
 * messages to the GL class's handlers and the iPod rebooted shortly after the
 * next LVGL app opened (found in nano7kit by Shimku). Keep one set per view,
 * chosen by the vtable copy the view runs on. */
#define HB_VIEW_CUSTOM 0u
#define HB_VIEW_GL     1u
static uint32_t hb_view_kind(const void *self)
{
    return (self && *(const uint32_t *)self == GL_VTABLE_COPY_ADDR) ? HB_VIEW_GL
                                                                     : HB_VIEW_CUSTOM;
}
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
static uint8_t  s_timer[128] __attribute__((aligned(8)));   /* OS timer object storage */
static uint32_t s_timer_magic;           /* TIMER_MAGIC while our timer is live    */
#define TIMER_ADDR         ((void *)s_timer)
#define TIMER_MAGIC        0x54494d52u   /* 'TIMR' */
#define TIMER_PERIOD_MS    8u           /* ~60 fps redraw heartbeat                */
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

/* The current CUSTOM_SCREEN app (set on launch). 0 = built-in gradient. */
static uint32_t s_surface_code;
static uint32_t s_surface_code_len;
/* The entry returned by hb_reloc_load_file after the .hbapp is rebased into its
 * own retained heap arena. s_surface_arena points at the current app's arena;
 * the per-app owner remains in s_app_arena[] until reboot. */
static void    *s_surface_reloc_entry;
static void    *s_surface_arena;
/* Executable app arenas are retained for the boot. Only app-owned auxiliary
 * data is deferred until the next launch so the GPU can finish the exit frame. */
static void    *s_pending_free_aux;
#define ADDR_OP_DELETE  0x0842d86cu   /* the OS heap free entry */
typedef void (*opdelete_t)(void *);
static uint32_t s_cur_app_id;       /* app whose binary is currently in the slot */
static uint16_t s_cur_app_kind = HB_APP_KIND_CUSTOM_SCREEN;
static uint8_t  s_cur_exec_load_state; /* 1 = newly loaded, 2 = retained/reused */

/* Reclaim a deferred arena (hb_surface_teardown stashes it here at quit). Only
 * safe once the exit transition that was in flight at quit has finished — i.e. at
 * the next launch, by which point the GPU has flushed the quit app's last frame. */
/* Free the unloading app's HB_APP_AUX_BLOCK (see hb_heap.h) with its arena. */
static void hb_free_app_aux(void)
{
    if (HB_APP_AUX_BLOCK) {
        ((opdelete_t)(ADDR_OP_DELETE | 1u))(HB_APP_AUX_BLOCK);
        HB_APP_AUX_BLOCK = 0;
    }
}

static void hb_surface_drain_pending(void)
{
    if (s_pending_free_aux) {
        ((opdelete_t)(ADDR_OP_DELETE | 1u))(s_pending_free_aux);
        s_pending_free_aux = 0;
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
static msg_fn_t s_orig_msg[2];

/* Our view's message handler override (UI task). On our heartbeat timer's timer message:
 * advance LVGL (lv_timer_handler) + Inval the view (-> draw callback blits s_fb) +
 * re-arm the one shot timer. Forward everything else to the real message handler. */
static uint32_t s_ds_dbg;       /* bound the per-frame draw traces (anti-flood)  */

__attribute__((section(".text.hook"), used, noinline))
static int hb_msg_hook(void *self, void *msg)
{
    if (msg && *(uint32_t *)((char *)msg + MSG_FTYPE_OFF) == CLTIMERMSG) {
        /* A posted tick can outlive stop/destroy while a cached view exits. */
        if (s_timer_magic != TIMER_MAGIC || self != s_surface_view) return 1;
        /* Timer context (possibly off the draw task): do ONLY cheap, safe work —
         * mark the view dirty + re-arm. The heavy lv_timer_handler + blit runs in
         * draw_hook on the OS's draw task (where it's proven safe at init).
         * (No per-tick trace here, it flooded the ring.) */
        ((gfx0_t)(A_VIEW_INVAL | 1u))(self);                /* schedule a redraw   */
        ((timer1_t)(A_TIMER_START | 1u))(TIMER_ADDR);           /* repeat          */
        return 1;
    }
    {
        msg_fn_t f = s_orig_msg[hb_view_kind(self)];
        return f ? f(self, msg) : 0;
    }
}

static void hb_surface_start_timer(void *view)
{
    void *t = TIMER_ADDR;
    uint32_t i;
    for (i = 0; i < sizeof s_timer; i++)                    /* zero the timer obj  */
        ((uint8_t *)t)[i] = 0;
    s_surface_view = view;
    ((timer_ctor_t)(A_TIMER_CTOR | 1u))(t, view, 0, 0);     /* bind to our view    */
    ((timer_set_t)(A_TIMER_SET | 1u))(t, (long long)TIMER_PERIOD_MS);
    ((timer1_t)(A_TIMER_START | 1u))(t);
    s_timer_magic = TIMER_MAGIC;
    hb_trace_log("TIMRSTRT", (uint32_t)(uintptr_t)view, TIMER_PERIOD_MS);
}

/* Stop the heartbeat so it can't fire into a freed view. Reset the per-view
 * arming flag so the NEXT launch starts a fresh timer. */
static void hb_surface_stop_timer(void)
{
    if (s_timer_magic == TIMER_MAGIC) {
        ((timer1_t)(A_TIMER_STOP | 1u))(TIMER_ADDR);             /* disarm        */
        ((timer1_t)(A_TIMER_DESTROY | 1u))(TIMER_ADDR);          /* unregister    */
        s_timer_magic = 0;
    }
    s_timer_started = 0;
    s_surface_view = 0;
}

/* The view's real destructors, captured before we patch slots 0/1 of the copy. */
typedef void (*dtor_t)(void *self);
static dtor_t s_orig_dtor0[2], s_orig_dtor1[2];

/* the view's visibility handler — the OS calls it with false when hiding the view
 * (quit transition), before destruction. We do it so the heartbeat can't fire into 
 * a hidden/freed view. */
typedef void (*vis_fn_t)(void *self, int show);
static vis_fn_t s_orig_visibility[2];

__attribute__((section(".text.hook"), used, noinline))
static void hb_visibility_hook(void *self, int show)
{
    hb_trace_log("XSHOW   ", (uint32_t)(uintptr_t)self, (uint32_t)show);
    if (!show)
        hb_surface_stop_timer();
    {
        vis_fn_t f = s_orig_visibility[hb_view_kind(self)];
        if (f) f(self, show);
    }
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
static uint8_t  s_rec_timer[128] __attribute__((aligned(8)));  /* OS timer object */
static uint32_t s_rec_orig_hm;            /* stock global-cntlr message handler */
#define REC_TIMER_ADDR       ((void *)s_rec_timer)
#define REC_PERIOD_MS        12u           /* +~25ms msg latency +capture -> ~25 fps */
#define TMSG_TIMER_TYPE      16u           /* the message type field for timer-message */
#define TMSG_FTIMER_OFF      8u            /* the timer-message's timer field (str r4,[r0,#8]) */

static msg_fn_t s_orig_gc_msg;

__attribute__((section(".text.hook"), used, noinline))
static int hb_rec_gc_msg_hook(void *self, void *msg)
{
    if (msg
        && *(uint32_t *)((char *)msg + MSG_FTYPE_OFF) == TMSG_TIMER_TYPE
        && *(void **)((char *)msg + TMSG_FTIMER_OFF) == REC_TIMER_ADDR) {
        if (hb_record_active()) {
            hb_record_tick();                                       /* capture frame */
            ((timer1_t)(A_TIMER_START | 1u))(REC_TIMER_ADDR);          /* re-arm     */
        }
        return 1;                                       /* consume our timer message */
    }
    return s_orig_gc_msg ? s_orig_gc_msg(self, msg) : 0;
}

/* Install the singleton-controller message handler filter once per boot (no timer
 * yet). Re-install safe: only a slot that isn't already our hook is saved. */
static void hb_rec_install(void)
{
    void *g = ((getvoid_t)(A_GLOBAL_CNTLR_GET | 1u))();
    uint32_t *gvt, slot;
    if (!g) return;
    gvt = *(uint32_t **)g;
    slot = gvt[MSG_SLOT_OFF / 4u];
    if (slot != (((uint32_t)(uintptr_t)&hb_rec_gc_msg_hook) | 1u))
        s_rec_orig_hm = slot;
    s_orig_gc_msg = (msg_fn_t)(uintptr_t)s_rec_orig_hm;
    gvt[MSG_SLOT_OFF / 4u] =
        ((uint32_t)(uintptr_t)&hb_rec_gc_msg_hook) | 1u;
    hb_icache_invalidate((uint32_t)(uintptr_t)gvt, MSG_SLOT_OFF + 8u);
    hb_trace_log("RECINST ", (uint32_t)(uintptr_t)g, s_rec_orig_hm);
}

/* Arm the capture timer (bound to the singleton). Called on record START;
 * the hook re-arms each tick while recording and lets it lapse when it stops. */
static void hb_rec_timer_arm(void)
{
    void *g = ((getvoid_t)(A_GLOBAL_CNTLR_GET | 1u))();
    void *t = REC_TIMER_ADDR;
    uint32_t i;
    if (!g) return;
    for (i = 0; i < sizeof s_rec_timer; i++) ((uint8_t *)t)[i] = 0;
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
    dtor_t f = s_orig_dtor0[hb_view_kind(self)];
    hb_surface_stop_timer();
    if (f) f(self);
}

__attribute__((section(".text.hook"), used, noinline))
static void hb_dtor_hook1(void *self)
{
    hb_trace_log("XDTOR1  ", (uint32_t)(uintptr_t)self, 0);
    dtor_t f = s_orig_dtor1[hb_view_kind(self)];
    hb_surface_stop_timer();
    if (f) f(self);
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
    /* Executable arenas stay allocated and are reused without rewriting for
     * the rest of this boot. Defer only the app's auxiliary data until the GPU
     * has finished the exit transition. */
    hb_surface_drain_pending();
    s_pending_free_aux = HB_APP_AUX_BLOCK;
    HB_APP_AUX_BLOCK = 0;
    s_surface_arena = 0;
    s_cur_app_id = 0;
}

/* the view's exit-begin handler (its one arg identifies the incoming screen) — the
 * OS calls this on our view when its screen transitions away (to home or another
 * app). Tear the app down here, then forward. This is the callback the visibility
 * handler could not provide for a screen transition. */
typedef void (*exit_fn_t)(void *self, int dst);
static exit_fn_t s_orig_exit[2];

__attribute__((section(".text.hook"), used, noinline))
static void hb_exit_hook(void *self, int dst)
{
    hb_trace_log("XEXIT   ", (uint32_t)(uintptr_t)self, (uint32_t)dst);
    exit_fn_t f = s_orig_exit[hb_view_kind(self)];
    hb_surface_teardown();
    if (f) f(self, dst);
}


/* Run the current surface app: hb_reloc_load_file already copied + rebased the
 * image into its own heap arena (and flushed its I-cache), so just call the
 * entry with our framebuffer and shared block; it returns the per-frame fn. */
static int hb_run_surface_blob(void)
{
    if (!s_fb || !s_surface_code || !s_surface_reloc_entry)
        return 0;
    hb_trace_log("SURFRELO", (uint32_t)(uintptr_t)s_surface_reloc_entry, s_surface_code_len);
    s_surface_frame = (surface_frame_t)(uintptr_t)
        ((hb_surface_entry_t)s_surface_reloc_entry)(0, s_fb, FB_W, FB_H, &s_shared);
    hb_trace_log("SURFRUN ", (uint32_t)(uintptr_t)s_surface_frame, 0);
    if (s_surface_frame) {
        hb_boot_report(BOOT_REPORT_LAUNCH_OFFSET, 1);
        hb_boot_report(BOOT_REPORT_APP_ID_OFFSET, s_cur_app_id);
        hb_boot_report(BOOT_REPORT_APP_KIND_OFFSET, s_cur_app_kind);
        hb_boot_report(BOOT_REPORT_ENTRY_OFFSET,
                       (uint32_t)(uintptr_t)s_surface_reloc_entry);
    }
    hb_report_launch_memory(s_cur_app_id,
        s_surface_frame
            ? (s_cur_exec_load_state == 1u ? HB_LAUNCH_NEW_STARTED
                                           : HB_LAUNCH_REUSED_STARTED)
            : (s_cur_exec_load_state == 1u ? HB_LAUNCH_NEW_ENTRY_FAILED
                                           : HB_LAUNCH_REUSED_ENTRY_FAILED));
    return s_surface_frame ? 1 : 0;
}

/* Allocate the heap framebuffer + build the reusable source context over it,
 * once per deploy. Safe to call on the UI task (screen-build / draw). */
static void hb_gfx_ensure(void)
{
    if (s_gfx_ready)
        return;
    if (!s_fb)
        s_fb = (uint32_t *)hb_os_alloc(FB_W * FB_H * 4u);   /* nothrow: NULL -> no surface */
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
    s_orig_msg[HB_VIEW_CUSTOM] = (msg_fn_t)(uintptr_t)src[MSG_SLOT_OFF / 4u];
    dst[MSG_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_msg_hook) | 1u;
    hb_trace_log("HMINSTL ", (uint32_t)(uintptr_t)s_orig_msg[HB_VIEW_CUSTOM],
                 (uint32_t)(uintptr_t)view);
    /* override both destructors so we stop the heartbeat before the view dies */
    s_orig_dtor0[HB_VIEW_CUSTOM] = (dtor_t)(uintptr_t)src[DTOR0_SLOT_OFF / 4u];
    s_orig_dtor1[HB_VIEW_CUSTOM] = (dtor_t)(uintptr_t)src[DTOR1_SLOT_OFF / 4u];
    dst[DTOR0_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_dtor_hook0) | 1u;
    dst[DTOR1_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_dtor_hook1) | 1u;
    /* visibility handler(false) on hide -> stop the heartbeat before the view is freed */
    s_orig_visibility[HB_VIEW_CUSTOM] = (vis_fn_t)(uintptr_t)src[VIS_SLOT_OFF / 4u];
    dst[VIS_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_visibility_hook) | 1u;
    /* exit-begin handler -> full teardown when the screen leaves for home (the real signal) */
    s_orig_exit[HB_VIEW_CUSTOM] = (exit_fn_t)(uintptr_t)src[EXIT_SLOT_OFF / 4u];
    dst[EXIT_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_exit_hook) | 1u;
    hb_icache_invalidate(VTABLE_COPY_ADDR, VTABLE_COPY_SIZE);
    *(uint32_t *)view = VTABLE_COPY_ADDR;             /* swap this view's vtable */
}

/* GL surface: a real GL view injected into the blank canvas for HB_APP_KIND_GL_SURFACE
 * apps. Its draw callback (run between the GL view draw's begin-scene/end-scene) renders GL into
 * the view's own GPU pixel buffer on the GPU; the OS composites it into the view tree
 * (sub-rect, clipped, z-ordered) — GL embedded in an app view, not a fullscreen overlay. */
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
    s_orig_msg[HB_VIEW_GL] = (msg_fn_t)(uintptr_t)src[MSG_SLOT_OFF / 4u];
    dst[MSG_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_msg_hook) | 1u;
    s_orig_dtor0[HB_VIEW_GL] = (dtor_t)(uintptr_t)src[DTOR0_SLOT_OFF / 4u];
    s_orig_dtor1[HB_VIEW_GL] = (dtor_t)(uintptr_t)src[DTOR1_SLOT_OFF / 4u];
    dst[DTOR0_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_dtor_hook0) | 1u;
    dst[DTOR1_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_dtor_hook1) | 1u;
    s_orig_visibility[HB_VIEW_GL] = (vis_fn_t)(uintptr_t)src[VIS_SLOT_OFF / 4u];
    dst[VIS_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_visibility_hook) | 1u;
    /* exit-begin handler -> full teardown when the screen leaves for home (the real signal) */
    s_orig_exit[HB_VIEW_GL] = (exit_fn_t)(uintptr_t)src[EXIT_SLOT_OFF / 4u];
    dst[EXIT_SLOT_OFF / 4u] = ((uint32_t)(uintptr_t)&hb_exit_hook) | 1u;
    hb_icache_invalidate(GL_VTABLE_COPY_ADDR, VTABLE_COPY_SIZE);
    *(uint32_t *)view = GL_VTABLE_COPY_ADDR;
}

/* The kind of the app currently presenting our screen — selects which view the
 * layer-creation hook injection adds (custom-draw LVGL surface vs a hardware-GL GL view). */

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
static uint8_t s_inject_armed;      /* one-shot: inject into the next present */
static void *s_inject_lvgl_child;   /* custom-draw color / LVGL surface view */
static void *s_inject_gl_child;     /* hardware-GL GL view           */
static void *s_inject_canvas;

__attribute__((section(".text.hook"), used, noinline))
static void templ_hook(void *self, void *holder, int tt, int b)
{
    /* When armed (tt==enter key=0), inject our own view into the incoming screen's
     * canvas during the layer pass: the custom-draw surface (LVGL) or a real
     * GL view (GL), per the launching app's kind. */
    if (s_inject_armed && tt == 0) {
        s_inject_armed = 0;
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
 * info. The tap router looks an app up by index and launches it by kind. The
 * pack is read from disk into an exact-size heap block that stays allocated (the
 * icon bitmaps it contains become cache entries). */
#ifndef APP_DISK_PATH                                /* optional legacy catalog input */
#define APP_DISK_PATH  "/Library/AllApps.pack"          /* installed app manifest on disk */
#endif
#define DEVCOLOR_PATH  "/Library/devcolor"      /* optional colour override index */
static hb_app_launch_t g_launch[HB_MAX_APPS];
static int             g_launch_count;
static uint32_t        s_resident_version;
static void           *s_live_pack, *s_retired_pack;
static hb_app_files_t   s_files[HB_MAX_APPS], s_next_files[HB_MAX_APPS];
static uint8_t         s_refreshing;
static uint8_t         s_apps_loaded;     /* block 3 runs once */
static void           *s_app_entry[HB_MAX_APPS];
static void           *s_app_arena[HB_MAX_APPS];
static uint32_t        s_app_code_len[HB_MAX_APPS];
static uint32_t        s_app_fail_mask;

static void hb_app_executable_path(char path[96], const hb_app_launch_t *L,
                                   const hb_app_files_t *files)
{
    int bundled = files->standalone;
    const char *pre = bundled ? "/Apps/" : "/Apps/Executables/";
    const char *suffix = bundled ? "/app.hbapp" : ".hbapp";
    const char *name = bundled ? files->folder : L->name;
    int i = 0, j;
    for (j = 0; pre[j]; j++) path[i++] = pre[j];
    for (j = 0; name[j]; j++) path[i++] = name[j];
    for (j = 0; suffix[j]; j++) path[i++] = suffix[j];
    path[i] = 0;
}

/* Load on first launch, retaining published bytes until unload/live refresh. */
static int hb_ensure_app_executable(int slot, const hb_app_launch_t *L)
{
    char path[96];
    uint32_t fsize;
    if (s_app_entry[slot])
        return 2;
    hb_app_executable_path(path, L, &s_files[slot]);
    fsize = hb_fs_size(path);
    if (fsize != s_files[slot].executable_size)
        return 0;
    /* No extra free-block check here: the SDK loader does it (image + 256 KB
     * against the largest block) and logs HEAVYNO; a second, stricter gate
     * here would refuse images that fit. */
    s_app_entry[slot] = s_files[slot].standalone ?
        hb_reloc_load_file_verified(path, &s_app_arena[slot], &s_app_code_len[slot],
                                    s_files[slot].executable_hash) :
        hb_reloc_load_file(path, &s_app_arena[slot], &s_app_code_len[slot], 0, 0);
    hb_trace_log("APPLAZY ", (uint32_t)slot,
                 (uint32_t)(uintptr_t)s_app_entry[slot]);
    if (!s_app_entry[slot])
        hb_trace_log("HEAVYREF", fsize, hb_os_heap_largest());
    return s_app_entry[slot] ? 1 : 0;
}

/* ---- App executable lifetime -------------------------------------
 * Launching an app first unloads every OTHER loaded app: its executable arena (code,
 * data, bss) goes back to the OS heap, so two big games never sit in memory together.
 * The catch: this task cannot invalidate the instruction cache (SVC #70 publishes the
 * data cache only), and the next app may be loaded where the old one's code ran, so
 * stale instruction lines could execute. Before entering code loaded after any unload,
 * run a 512 KB straight-line sled: every L1 I-cache set (32 KB) is refilled ~64 times
 * with sled lines, which evicts stale lines with overwhelming probability. The sled is
 * allocated once, at boot (memory no app code has used), and never changes or
 * moves, so its own cached lines are always correct. */
#define HB_SLED_BYTES (512u * 1024u)
static uint32_t *s_sled;
static uint8_t   s_sled_failed;
static uint8_t   s_any_unloaded;

static void hb_sled_init(void)
{
    uint32_t i, n = HB_SLED_BYTES / 4u;
    if (s_sled || s_sled_failed)
        return;
    /* Ask for exactly 512 KB (no +32 slack: a size-class heap would round
     * 512 KB + 32 up to the next class) and run the sled from the first 32-byte
     * line inside it. One line shorter; same eviction. */
    s_sled = (uint32_t *)hb_os_alloc(HB_SLED_BYTES);
    if (!s_sled) {
        s_sled_failed = 1;
        hb_trace_log("SLEDFAIL", HB_SLED_BYTES, 0);
        return;
    }
    if ((uintptr_t)s_sled & 31u) {
        s_sled = (uint32_t *)(((uintptr_t)s_sled + 31u) & ~(uintptr_t)31u);
        n -= 8u;
    }
    for (i = 0; i + 1u < n; i++)
        s_sled[i] = 0xE1A00000u;      /* ARM: mov r0, r0 */
    s_sled[n - 1u] = 0xE12FFF1Eu;     /* ARM: bx lr       */
    hb_icache_invalidate((uint32_t)(uintptr_t)s_sled, n * 4u);
    hb_trace_log("SLEDINIT", (uint32_t)(uintptr_t)s_sled, n * 4u);
}

static void hb_icache_sweep(void)
{
    if (!s_sled)
        return;
    ((void (*)(void))(uintptr_t)s_sled)();   /* even address: BLX enters ARM state */
    hb_trace_log("SWEEP   ", (uint32_t)(uintptr_t)s_sled, 0);
}

/* Free every loaded app except `keep` (the one being launched). */
static void hb_unload_other_apps(int keep)
{
    int i;
    if (!s_sled)
        return;                         /* no sweep available: keep the old retain-all rule */
    for (i = 0; i < g_launch_count; i++) {
        if (i == keep || !s_app_arena[i])
            continue;
        if (s_surface_arena == s_app_arena[i]) {
            s_surface_arena = 0;
            s_surface_reloc_entry = 0;
            s_surface_code = 0;
            s_surface_frame = 0;
            s_gl_frame_fn = 0;
        }
        hb_trace_log("UNLOAD  ", (uint32_t)i, (uint32_t)(uintptr_t)s_app_arena[i]);
        hb_os_free(s_app_arena[i]);
        s_app_arena[i] = 0;
        s_app_entry[i] = 0;
        s_app_code_len[i] = 0;
        s_any_unloaded = 1;
    }
}

/* Launch app record `L` from the home controller `self` (UI task). */
__attribute__((used, noinline))
static void hb_app_launch(void *self, const hb_app_launch_t *L)
{
    int slot = (int)(L - g_launch);
    int load_state = 0;
    if (slot < 0 || slot >= g_launch_count)
        return;
    if (s_refreshing) return;
    hb_trace_log("LAUNCH  ", L->app_id, L->app_kind);
    if (s_shared.prepare_unload && !s_shared.prepare_unload()) {
        hb_trace_log("APPBUSY ", L->app_id, 0);
        return; /* Never free audio-owned memory; a later tap retries the launch. */
    }
    s_shared.prepare_unload = 0;
    s_cur_app_kind = L->app_kind;     /* selects the injected view (templ_hook) */
    hb_surface_drain_pending();
    hb_free_app_aux();

    if (L->app_kind == HB_APP_KIND_CUSTOM_SCREEN ||
        L->app_kind == HB_APP_KIND_GL_SURFACE) {
        hb_sled_init();                 /* first launch: before any app code exists */
        hb_unload_other_apps(slot);
        load_state = hb_ensure_app_executable(slot, L);
        if (load_state == 1 && s_any_unloaded)
            hb_icache_sweep();          /* new code may sit where freed code ran */
        if (!load_state) {
            if (slot < 32)
                s_app_fail_mask |= 1u << slot;
            hb_boot_report(BOOT_REPORT_FAILMASK_OFFSET, s_app_fail_mask);
            hb_report_launch_memory(L->app_id, HB_LAUNCH_LOAD_FAILED);
            return;
        }
        if (slot < 32)
            s_app_fail_mask &= ~(1u << slot);
        hb_boot_report(BOOT_REPORT_FAILMASK_OFFSET, s_app_fail_mask);
        s_cur_exec_load_state = (uint8_t)load_state;
    }

    if (L->app_kind == HB_APP_KIND_GL_SURFACE) {
        /* The GL executable was copied once during registration and is never
         * rewritten or freed during this boot. Its entry resets per-launch state. */
        s_surface_frame = 0;
        s_gl_frame_fn = 0;
        s_surface_arena = s_app_arena[slot];
        s_cur_app_id = L->app_id;
        hb_trace_log("GLAPPBIN", s_app_code_len[slot], (uint32_t)L->app_id);
        if (s_app_entry[slot]) {
            s_gl_frame_fn = (gl_frame_fn_t)(uintptr_t)
                ((hb_surface_entry_t)s_app_entry[slot])(0, 0, FB_W, FB_H, &s_shared);
        }
        hb_trace_log("GLAPPFN ", (uint32_t)(uintptr_t)s_gl_frame_fn, (uint32_t)L->app_id);
        if (s_gl_frame_fn) {
            hb_boot_report(BOOT_REPORT_LAUNCH_OFFSET, 1);
            hb_boot_report(BOOT_REPORT_APP_ID_OFFSET, L->app_id);
            hb_boot_report(BOOT_REPORT_APP_KIND_OFFSET, L->app_kind);
            hb_boot_report(BOOT_REPORT_ENTRY_OFFSET,
                           (uint32_t)(uintptr_t)s_app_entry[slot]);
        }
        hb_report_launch_memory(L->app_id,
            s_gl_frame_fn
                ? (load_state == 1 ? HB_LAUNCH_NEW_STARTED
                                   : HB_LAUNCH_REUSED_STARTED)
                : (load_state == 1 ? HB_LAUNCH_NEW_ENTRY_FAILED
                                   : HB_LAUNCH_REUSED_ENTRY_FAILED));
        s_inject_armed = 1;
        hb_trace_log("PUSHSCR ", (uint32_t)L->screen_id, (uint32_t)L->layout_id);
        hb_silver_push_screen_id(self, L->screen_id, L->layout_id);
        hb_trace_log("PUSHED  ", L->app_id, 0);
        return;
    }

    if (L->app_kind == HB_APP_KIND_CUSTOM_SCREEN) {
        /* Select the executable retained during registration; never reload it
         * into a freed arena. The app entry reinitializes its BSS each launch. */
        s_surface_frame = 0;
        s_surface_arena = s_app_arena[slot];
        s_surface_reloc_entry = s_app_entry[slot];
        s_surface_code_len = s_app_code_len[slot];
        s_surface_code = s_surface_reloc_entry ? 1u : 0u;
        s_cur_app_id = L->app_id;
        hb_trace_log("APPBIN  ", s_surface_code_len, (uint32_t)L->app_id);
        hb_trace_log("APPRELO ", (uint32_t)(uintptr_t)s_surface_reloc_entry,
                     (uint32_t)L->app_id);
        s_gfx_ready = 0;                /* hb_gfx_ensure re-runs the (new) app   */
        s_surface_frame = 0;
        s_inject_armed = 1;            /* arm the custom-draw surface inject */
        hb_trace_log("PUSHSCR ", (uint32_t)L->screen_id, (uint32_t)L->layout_id);
        hb_silver_push_screen_id(self, L->screen_id, L->layout_id);
        hb_trace_log("PUSHED  ", L->app_id, 0);
    } else {
        hb_trace_log("FIREHND ", L->app_id, (uint32_t)L->os_handler);
        hb_silver_fire_handler(self, L->os_handler, (void *)1);
    }
}

static uint32_t hb_pack_size(const void *pack)
{
    const hb_app_pack_t *h = pack;
    const uint32_t *off = (const uint32_t *)((const uint8_t *)pack + 12u);
    if (!h->count) return 12u;
    const hb_app_bundle_t *last = (const hb_app_bundle_t *)((const uint8_t *)pack + off[h->count - 1u]);
    return off[h->count - 1u] + last->bundle_len;
}

/* Home's model refresh notification runs after USB eject/disconnect. A tap
 * checks again before routing, so a missed notification never exposes stale code.
 * All disk reads finish before Home/resource mutation. Nested callbacks are
 * consumed while refreshing, including callbacks caused by yielding file I/O.
 * Defer while an app is active or audio teardown is busy. Hide removed or
 * incompatible apps through the stock Home model, update labels/icons and drop
 * cached executables. Label nodes and icon extension are reused; retain at most
 * the current and one retired catalog for queued UI teardown. App file changes
 * need no reboot; replacing the resident itself still requires a restart. */
static uint32_t s_live_sum;       /* FNV-1a of the live catalog as loaded (before registration) */

/* Catalogs are compared by a checksum taken before registration: once
 * registered, the OS may touch the live pack's bytes, and a byte compare would
 * then call every rescan "changed" and retain a fresh catalog each time. */
static uint32_t hb_catalog_sum(const void *pack, const hb_app_files_t *files, uint32_t count)
{
    const uint8_t *p = pack;
    uint32_t h = 0x811c9dc5u, n = hb_pack_size(pack), i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
    p = (const uint8_t *)files;
    for (i = 0; i < count * (uint32_t)sizeof(files[0]); i++) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

/* Unload every app (aux block first, then images). 0 if code is loaded but the
 * I-cache sled is unavailable, so a later launch could not run safely. */
static int hb_unload_for_refresh(void)
{
    uint32_t i;
    int loaded = 0;
    for (i = 0; i < (uint32_t)g_launch_count; i++) if (s_app_arena[i]) loaded = 1;
    if (loaded) {
        hb_sled_init();
        if (!s_sled) return 0;
        hb_surface_drain_pending();
        hb_free_app_aux();
    }
    s_shared.prepare_unload = 0;
    hb_unload_other_apps(-1);
    return 1;
}

static int hb_refresh_apps(int returning_home)
{
    void *next;
    uint32_t i, j, count, sum;
    int changed;
    if (s_refreshing || !s_apps_loaded || !hb_fs_is_dir("/iPod_Control")) return 0;
    if (s_cur_app_id || (s_shared.prepare_unload && !s_shared.prepare_unload())) return 0;
    s_refreshing = 1;
    /* Back on Home every app is unloaded anyway. Do it BEFORE the scan: the
     * scan's blocks (and a retained catalog, if it changed) would otherwise sit
     * beside the quitting app's arena and split the big free block once that
     * arena is freed. From nano7kit by Shimku (MIT). */
    if (returning_home && !hb_unload_for_refresh()) { s_refreshing = 0; return 0; }
    next = hb_app_catalog_load(APP_DISK_PATH, s_resident_version, s_next_files, hb_trace_log);
    if (!next) { s_refreshing = 0; return 0; }
    count = ((const hb_app_pack_t *)next)->count;
    sum = hb_catalog_sum(next, s_next_files, count);
    changed = !s_live_pack || sum != s_live_sum;
    if (changed && !returning_home && !hb_unload_for_refresh()) {
        hb_os_free(next); s_refreshing = 0; return 0;
    }
    if (!changed) {
        if (returning_home) {
            hb_silver_icon_refresh();
            hb_silver_home_reload();
        }
        hb_os_free(next); s_refreshing = 0; return 1;
    }
    const uint32_t *off = (const uint32_t *)((const uint8_t *)next + 12u);
    for (i = 0; i < (uint32_t)g_launch_count; i++) {
        for (j = 0; j < count; j++) {
            const hb_app_bundle_t *b = (const hb_app_bundle_t *)((const uint8_t *)next + off[j]);
            const char *a = g_launch[i].sbid, *p = (const char *)b + b->sbid_off;
            while (*a && *a == *p) { a++; p++; }
            if (!*a && !*p) break;
        }
        if (j == count) hb_silver_home_hide_item(g_launch[i].sbid);
    }
    for (i = 0; i < count; i++) s_files[i] = s_next_files[i];
    s_live_sum = sum;
    g_launch_count = hb_app_register_pack(next, hb_os_alloc, hb_trace_log,
        g_launch, HB_MAX_APPS, SILVERDB_OBJ, SILVERDB_NODE,
        hb_silver_icon_live() ? 2 : 1, s_files);
    /* Resource objects now reference next. Retain one previous catalog through
     * the following Home cycle as well, so queued cell teardown can finish. */
    if (s_retired_pack) hb_os_free(s_retired_pack);
    s_retired_pack = s_live_pack;
    s_live_pack = next;
    s_app_fail_mask = 0;
    hb_boot_report(BOOT_REPORT_FAILMASK_OFFSET, 0);
    hb_boot_report(BOOT_REPORT_APP_COUNT_OFFSET, (uint32_t)g_launch_count);
    hb_trace_log("APPREF  ", (uint32_t)g_launch_count, 0);
    s_refreshing = 0;
    return 1;
}

/* Let the USB/navigation message finish, including cached-view teardown,
 * before rebuilding Home. A one-shot timer posts back to the Home controller's
 * UI message queue; no UI mutation runs on the timer/USB task. */
static uint8_t s_refresh_timer[128] __attribute__((aligned(8)));
static uint8_t s_refresh_timer_initialized, s_refresh_pending;

static void hb_schedule_app_refresh(void *self)
{
    if (s_refresh_pending) return;
    s_refresh_pending = 1;
    if (!s_refresh_timer_initialized) {
        ((timer_ctor_t)(A_TIMER_CTOR | 1u))(s_refresh_timer, self, 0, 0);
        s_refresh_timer_initialized = 1;
    }
    ((timer_set_t)(A_TIMER_SET | 1u))(s_refresh_timer, (long long)500);
    ((timer1_t)(A_TIMER_START | 1u))(s_refresh_timer);
}

__attribute__((section(".text.hook"), used, noinline))
static int hb_home_message_hook(void *self, void *msg)
{
    if (msg && *(uint32_t *)((char *)msg + MSG_FTYPE_OFF) == CLTIMERMSG &&
        *(void **)((char *)msg + 8u) == (void *)s_refresh_timer) {
        if (s_refresh_pending) {
            s_refresh_pending = 0;
            hb_refresh_apps(1);
        }
        return 1;
    }
    return ((msg_fn_t)HB_SILVER_HOME_MESSAGE_ORIG)(self, msg);
}

/* RAW-MESSAGE slot (+0x188): taps arrive here. Translate the message EXACTLY
 * once; if it's one of our custom items ("sbitem.hb.<folder>.clicked"), route it
 * and consume; otherwise the helper dispatches it directly (no re-forward,
 * which would translate the same message twice). */
__attribute__((section(".text.hook"), used, noinline))
static int rawmsg_hook(void *self, void *msg)
{
    char ev[64];
    int refresh_home = 0;
    /* Snapshot before forwarding: stock handlers can release message payloads.
     * USB return rebuilds Home via model notifications, not a Main action. */
    if (msg && *(uint32_t *)((char *)msg + MSG_FTYPE_OFF) == 30u) {
        const uint32_t *payload = *(const uint32_t **)((char *)msg + 8u);
        refresh_home = payload && payload[1] == HB_SILVER_HOME_REFRESH_NOTICE;
    }
    int r = hb_silver_rawmsg(self, msg, "sbitem.hb.", ".clicked", ev, sizeof ev);
    if (r == HB_SILVER_RAWMSG_CLAIMED) {
        if (!hb_refresh_apps(0)) return 1;
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
    /* Queue after stock notification handling; cached native views may still
     * be completing the USB transition when this inner handler returns. */
    if (refresh_home) hb_schedule_app_refresh(self);
    return r;
}

/* ACTION slot: setup runs here, in the proven-stable dispatch context. */
__attribute__((section(".text.hook"), used, noinline))
static int action_hook(void *self, const char *name, void *msg)
{
    /* (2) Once per boot: build every forwarding trampoline (in our .bss) from
     * the verified 39A10023 stock prologue, then point the firmware entry at our
     * hook. Always from the stock bytes, so a firmware entry a previous boot
     * left patched in DRAM is simply overwritten. */
    if (!s_hooks_patched) {
        s_hooks_patched = 1;
        int bt_fix = hb_bluetooth_fix();
        int dock_fix = hb_dock_fix();
        hb_trace_log("BTFIX   ", (uint32_t)bt_fix, 0);
        hb_trace_log("DOCKFIX ", (uint32_t)dock_fix, 0);
        hb_boot_report(0x154u, (uint32_t)bt_fix);
        hb_boot_report(0x158u, (uint32_t)dock_fix);
        hb_silver_patch_function_stock(ADDR_GETRESOURCE, (void *)&getres_hook,
                                       s_getres_tramp,
                                       GETRES_STOCK, sizeof GETRES_STOCK);
        hb_trace_log("GRPATCH ", *(uint32_t *)(uintptr_t)ADDR_GETRESOURCE, 0);

        hb_silver_patch_function_stock(ADDR_POSTTOUCH, (void *)&hb_posttouch_hook,
                                       s_posttouch_tramp,
                                       POSTTOUCH_STOCK, sizeof POSTTOUCH_STOCK);
        hb_trace_log("PTPATCH ", *(uint32_t *)(uintptr_t)ADDR_POSTTOUCH, 0);

        /* Re-aim Home-triple-click at our screenshot hook. We never forward through
         * a trampoline (we reimplement the original), so a plain repoint is
         * correct whether the entry is pristine or already redirected. */
        hb_silver_repoint_function(A_HOME_TRIPLE, (void *)&hb_home_triple_hook);
        hb_trace_log("SHOTPCH ", *(uint32_t *)(uintptr_t)A_HOME_TRIPLE, 0);

        /* All demo-bar hooks use absolute jumps: resident allocations can be
         * more than 16 MiB from firmware. Relocate the clock's PC-relative
         * literal explicitly; its original value remains the firmware's. */
        for (unsigned i = 0; i < sizeof DTTS_RELOCATED; i++)
            s_dtts_tramp[i] = DTTS_RELOCATED[i];
        *(uint32_t *)(s_dtts_tramp + 16) = *(const uint32_t *)DTTS_LITERAL;
        hb_silver_repoint_function_abs((uint32_t)(uintptr_t)(s_dtts_tramp + 8),
                                       (void *)(uintptr_t)(A_DTTS + 8));
        hb_icache_invalidate((uint32_t)(uintptr_t)s_dtts_tramp, 20);
        hb_silver_repoint_function_abs(A_DTTS, (void *)&hb_dtts_hook);
        hb_trace_log("DTTSPCH ", *(uint32_t *)(uintptr_t)A_DTTS, 0);

        hb_silver_patch_function_abs_stock(A_BATT_LEVEL, (void *)&hb_batt_level_hook,
                                           s_batt_level_tramp,
                                           BATT_LEVEL_STOCK, sizeof BATT_LEVEL_STOCK);
        hb_silver_patch_function_abs_stock(A_BATT_STATUS, (void *)&hb_batt_status_hook,
                                           s_batt_status_tramp,
                                           BATT_STATUS_STOCK, sizeof BATT_STATUS_STOCK);
        hb_trace_log("BATTPCH ", *(uint32_t *)(uintptr_t)A_BATT_LEVEL,
                                 *(uint32_t *)(uintptr_t)A_BATT_STATUS);

#if HB_REC_INTERCEPT
        hb_rec_install();       /* singleton-controller message handler filter for the capture timer */
#endif

        /* File I/O yields to other tasks, so it must not run until the firmware
         * entries above point at this image. */
        save_last_trace();
    }

    /* (3) Once per boot: register every app from the on-disk .app PACK. The pack
     * is read into an exact-size heap block that stays allocated (the icon bitmaps
     * it contains become bitmap-cache entries). The registrar does the batched
     * icon cache-table extension, adds the home items, and fills g_launch[]. */
    if (!s_apps_loaded) {
        hb_trace_log("APPLOAD ", 0, 0);
        s_apps_loaded = 1;
        s_refreshing = 1; /* registration may yield to a nested USB notice */

        /* Read the optional colour override before publishing Home items.
         * hb_silver_home_reload may render disk-backed icons immediately, and
         * those reads share hb_fs's deliberately single-operation cache. */
        char dc[4] = {0};
        int dc_idx = -1;
        if (hb_fs_read(DEVCOLOR_PATH, dc, sizeof dc - 1) > 0 &&
            dc[0] >= '0' && dc[0] <= '9') {
            dc_idx = dc[0] - '0';
            if (dc[1] >= '0' && dc[1] <= '9')
                dc_idx = dc_idx * 10 + (dc[1] - '0');
        }

        /* OS-mutating registration (icon table extension, labels, screen, home
         * items) is gated on whether our bitmap-cache entries are already LIVE
         * (hb_silver_icon_live): never extend the cache twice. The untethered
         * loader runs us once per boot, when the cache has just been reset. */
        int reg = !hb_silver_icon_live();
        /* The two blocks the resident keeps for the whole boot
         * (I-cache sled, LVGL framebuffer) are taken now, with the other boot
         * blocks, instead of at the first launch, where they landed beside
         * that app's arena and split the big free block once it was freed. */
        hb_sled_init();
        if (!s_fb)
            s_fb = (uint32_t *)hb_os_alloc(FB_W * FB_H * 4u);
        hb_trace_log("BOOTBLK ", (uint32_t)(uintptr_t)s_sled, (uint32_t)(uintptr_t)s_fb);
        /* Merge independent disk manifests before the single batched registrar
         * call. No shared catalog rewrite is needed to install a prebuilt app. */
        const void *pack = hb_app_catalog_load(APP_DISK_PATH, s_resident_version, s_files, hb_trace_log);
        s_live_pack = (void *)pack;
        if (pack)
            s_live_sum = hb_catalog_sum(pack, s_files, ((const hb_app_pack_t *)pack)->count);
        if (pack)
            g_launch_count = hb_app_register_pack(pack,
                                hb_os_alloc,    /* nothrow; every callee NULL-checks */
                                (void (*)(const char *, uint32_t, uint32_t))&hb_trace_log,
                                g_launch, HB_MAX_APPS,
                                SILVERDB_OBJ, SILVERDB_NODE, reg, s_files);
        hb_trace_log("APPSREG ", (uint32_t)g_launch_count, (uint32_t)reg);
        hb_boot_report(BOOT_REPORT_REGISTER_OFFSET, 1);
        hb_boot_report(BOOT_REPORT_APP_COUNT_OFFSET,
                       (uint32_t)g_launch_count);
        hb_boot_report(BOOT_REPORT_PRELOAD_OFFSET, 0);
        hb_boot_report(BOOT_REPORT_FAILMASK_OFFSET, 0);
        hb_boot_report(BOOT_REPORT_HEAP_BOOT_FREE_OFFSET,
                       hb_os_heap_free());
        hb_boot_report(BOOT_REPORT_HEAP_BOOT_LARGEST_OFFSET,
                       hb_os_heap_largest());

        /* Optional device-colour override: /Library/devcolor holds an index
         * (0 = Space Gray .. 11). RAM only, re-applied each boot; delete the file
         * to go back to the factory colour. */
        if (dc_idx >= 0 && dc_idx < hb_devcolor_count())
            hb_devcolor_set(dc_idx);
        if (dc_idx >= 0)
            hb_trace_log("DEVCOLOR", (uint32_t)dc_idx,
                         (uint32_t)hb_devcolor_get());
        s_refreshing = 0;
    } else if (name) {
        const char *p = name, *want = "HandleGotoMainScreen";
        while (*want && *want == *p) { want++; p++; }
        if (!*want && !*p) hb_schedule_app_refresh(self);
    }
    if (name) {
        const char *p = name, *want = "NanoAppsInitialize";
        while (*want && *want == *p) { want++; p++; }
        if (!*want && !*p) return 1;
    }
    return ((dispatch_t)HB_SILVER_HOME_DISPATCH_ORIG)(self, name, msg);
}

/* Entry, called once per boot by the heap bootstrap after it has loaded,
 * rebased and zeroed this image. Installs the home-controller hooks; the
 * bootstrap checks they point into this allocation before handing over. */
__attribute__((section(".text.entry"), used, noinline))
void payload_entry(void *image_base, uint32_t image_span, uint32_t resident_version)
{
    const void *prev;
    uint32_t seq;
    int same_boot;

    s_resident_version = resident_version;

    hb_boot_report(BOOT_REPORT_RESIDENT_OFFSET, 1);
    hb_boot_report(0x15cu, resident_version);
    hb_boot_report(0x160u, HB_APP_MANIFEST_ABI);
    hb_boot_report(0x16cu, 0x4c495645u); /* LIVE: app-only installs need no restart */
    s_shared.magic   = HB_SHARED_MAGIC;
    s_shared.version = HB_SHARED_VERSION;
    s_shared.trace   = hb_trace_log;
    s_shared.screenshot_mode = 1;      /* default Screenshot; the app sets mode 0/1/2 */
    hb_shared = &s_shared;

    /* Find the previous boot's ring (DRAM survives a panic reboot) before our
     * own ring is stamped; action_hook writes the copy to disk. */
    /* The prior-ring scan covers ordinary heap, including the address where
     * this image may have just landed. Exclude this allocator-owned block so
     * code/data bytes can never be misidentified as a surviving old ring. */
    if ((uintptr_t)image_base > (uintptr_t)&payload_entry ||
        image_span > 0x100000u ||
        (uintptr_t)&payload_entry - (uintptr_t)image_base >= image_span) {
        image_base = 0;
        image_span = 0;
    }
    prev = hb_trace_find_previous(&seq, image_base, image_span);
    if (prev) {
        const uint32_t *src = (const uint32_t *)prev;
        for (uint32_t i = 0; i < HB_TRACE_BYTES / 4u; i++)
            s_prev_ring[i] = src[i];
        s_prev_found = 1;
    }
    hb_trace_attach(s_trace_ring, seq + 1u);
    hb_boot_report(0x150u, (uint32_t)(uintptr_t)s_trace_ring);
    hb_trace_log("RESENTRY", (uint32_t)(uintptr_t)&payload_entry, seq + 1u);
    hb_trace_log("PREVRING", (uint32_t)(uintptr_t)prev, seq);
    hb_silver_rawmsg_trace = hb_trace_log;

    /* Our bitmap-cache entries can only be live if this image ran before in
     * this boot (not expected: the loader runs us once). If the cache was reset
     * by a reboot, clear any stale resolver pointer; registration re-installs it. */
    same_boot = hb_silver_icon_live();
    hb_trace_log("BOOTLIVE", (uint32_t)same_boot, 0);
    if (same_boot)
        hb_silver_icon_rearm();
    else
        hb_silver_icon_reset_resolver();
    /* Zeroed, heap-owned timer state never refers to an OS timer from a previous boot. */

    hb_silver_uitask_hook(HB_SILVER_HOME_VTABLE, HB_SILVER_HOME_MESSAGE_SLOT, (void *)&hb_home_message_hook);
    uint32_t p1 = hb_silver_uitask_hook(HB_SILVER_HOME_VTABLE,
                                        HB_SILVER_HOME_RAWMSG_SLOT, (void *)&rawmsg_hook);
    uint32_t p2 = hb_silver_uitask_hook(HB_SILVER_HOME_VTABLE,
                                        HB_SILVER_HOME_DISPATCH_SLOT, (void *)&action_hook);
    /* Hook the template view's layer-creation hook slot (forwards to the
     * hardcoded original). */
    uint32_t p3 = hb_silver_uitask_hook(TEMPLATE_VTABLE, TEMPLATE_LAYERS_SLOT,
                                        (void *)&templ_hook);
    hb_trace_log("ARMED   ", p1, p2);
    hb_trace_log("ARMEDCML", p3, 0);

#if HB_REC_INTERCEPT
    hb_rec_install();
#endif
}
