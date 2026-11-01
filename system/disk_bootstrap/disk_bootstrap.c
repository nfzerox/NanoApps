/* Stable firmware bootstrap for the single /Library/resident.hbapp on disk.
 *
 * The firmware can reach Home before the media filesystem is available, or
 * while USB owns it. Keep this small bootstrap alive and retry after stock
 * Home handling / the USB-return model notification. Never execute a resident
 * until its checksum, ABI, firmware, version and relocation bounds validate.
 * Missing/bad files leave stock UI working and can be replaced over USB.
 * Once any resident code executes, do not attempt another in that boot.
 */
#include "hb_sdk.h"
#include "hb_silver_uitask.h"
#include "hb_reloc.h"
#include "hb_heap.h"
#include "hb_runtime_compat.h"

#define DISK_PATH     "/Library/resident.hbapp"
#define ACTION_SLOT   (HB_SILVER_HOME_VTABLE + HB_SILVER_HOME_DISPATCH_SLOT)
#define RAWMSG_SLOT   (HB_SILVER_HOME_VTABLE + HB_SILVER_HOME_RAWMSG_SLOT)
#define TEMPLATE_SLOT 0x087ab8a4u
#define STOCK_TEMPLATE 0x081e0a03u
#define REPORT_BASE   0x086c9b84u
#define REPORT_MAGIC  0x32505452u

static uint8_t s_busy, s_executed;
static uint32_t s_attempts;
typedef int (*action_t)(void *, const char *, void *);

static void report(uint32_t offset, uint32_t value)
{
    if (*(volatile uint32_t *)(REPORT_BASE + 0x2cu) == REPORT_MAGIC) {
        *(volatile uint32_t *)(REPORT_BASE + offset) = value;
        __asm__ volatile("dsb sy" : : : "memory");
    }
}

static int inside(uint32_t fn, const uint8_t *base, uint32_t size)
{
    uint32_t p = fn & ~1u, lo = (uint32_t)(uintptr_t)base;
    return (fn & 1u) && p >= lo && p - lo < size;
}

static void set_slot(uint32_t slot, uint32_t value)
{
    *(volatile uint32_t *)slot = value;
    hb_icache_invalidate(slot, 4);
}

/* Called only on the UI task. File I/O may yield; nested events stay stock. */
static int try_load(uint32_t phase)
{
    void *arena = 0, *entry;
    uint8_t *base = 0;
    uint32_t len, span, image, version;
    if (s_busy || s_executed) return 0;
    s_busy = 1;
    report(0x17cu, phase);
    report(0x174u, ++s_attempts);
    int ready = hb_fs_is_dir("/iPod_Control");
    report(0x178u, (uint32_t)ready);
    if (!ready || hb_fs_exists("/Library/disabled")) { s_busy = 0; return 0; }
    entry = hb_reloc_load_file_checked_at(DISK_PATH, 0, &arena, &len,
                                         &base, &span, &image, &version);
    report(0x170u, hb_reloc_file_status());
    if (!entry) { report(0x78u, 1); s_busy = 0; return 0; }
    s_executed = 1;
    report(0x74u, 4); /* public disk resident */
    report(0x7cu, (uint32_t)(uintptr_t)base);
    report(0x80u, span);
    ((void (*)(void *, uint32_t, uint32_t))entry)(base, span, version);
    if (!inside(*(volatile uint32_t *)ACTION_SLOT, base, image) ||
        !inside(*(volatile uint32_t *)RAWMSG_SLOT, base, image) ||
        !inside(*(volatile uint32_t *)TEMPLATE_SLOT, base, image)) {
        /* Retain the executed arena even on failure: it may contain live data.
         * Restore stock hooks and don't execute another image in this boot. */
        set_slot(RAWMSG_SLOT, HB_SILVER_HOME_RAWMSG_ORIG);
        set_slot(ACTION_SLOT, HB_SILVER_HOME_DISPATCH_ORIG);
        set_slot(TEMPLATE_SLOT, STOCK_TEMPLATE);
        report(0x78u, 2);
        s_busy = 0;
        return 0;
    }
    report(0x78u, 3);
    s_busy = 0;
    return 1;
}

static void initialize(void *self)
{
    /* A valid, unbound action name also works with older compatible residents:
     * their normal first-action setup runs, then stock ignores the unknown name. */
    ((action_t)(uintptr_t)*(volatile uint32_t *)ACTION_SLOT)(self, "NanoAppsInitialize", 0);
}

__attribute__((section(".text.hook"), used, noinline))
static int wait_action(void *self, const char *name, void *msg)
{
    if (try_load(1))
        return ((action_t)(uintptr_t)*(volatile uint32_t *)ACTION_SLOT)(self, name, msg);
    int result = ((action_t)HB_SILVER_HOME_DISPATCH_ORIG)(self, name, msg);
    if (try_load(2)) initialize(self);
    return result;
}

__attribute__((section(".text.hook"), used, noinline))
static int wait_raw(void *self, void *msg)
{
    int refresh = 0;
    if (msg && *(uint32_t *)((char *)msg + 4) == 30u) {
        const uint32_t *payload = *(const uint32_t **)((char *)msg + 8);
        refresh = payload && payload[1] == HB_SILVER_HOME_REFRESH_NOTICE;
    }
    char event[64];
    int result = hb_silver_rawmsg(self, msg, "sbitem.hb.", ".clicked", event, sizeof event);
    if (result == HB_SILVER_RAWMSG_CLAIMED) result = 1;
    if (refresh && try_load(3)) initialize(self);
    return result;
}

__attribute__((section(".text.hook"), used, noinline))
static void wait_template(void *self, void *holder, int tt, int b)
{
    ((void (*)(void *, void *, int, int))STOCK_TEMPLATE)(self, holder, tt, b);
}

/* Exact-bound firmware stage owns/zeroes this image and retains its one small
 * arena for the boot. Waiting handlers can safely finish after disk handoff. */
__attribute__((section(".text.entry"), used, noinline))
void payload_entry(void)
{
    report(0x160u, HB_RUNTIME_ABI);
    report(0x164u, 4); /* Library resident / root Data layout */
    report(0x168u, 0x4449534bu); /* DISK */
    report(0x78u, 4); /* waiting for the disk resident */
    set_slot(ACTION_SLOT, (uint32_t)(uintptr_t)&wait_action | 1u);
    set_slot(RAWMSG_SLOT, (uint32_t)(uintptr_t)&wait_raw | 1u);
    set_slot(TEMPLATE_SLOT, (uint32_t)(uintptr_t)&wait_template | 1u);
}
