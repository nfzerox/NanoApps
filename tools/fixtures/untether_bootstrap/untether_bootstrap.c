/* Historical loader fixture; not installed or packaged by NanoApps. */
/* Experimental Nano 7 / 39A10023 stage-two loader.
 *
 * Entry installs a deferred UI dispatcher. File access and resident startup
 * happen on a subsequent home-controller action, never in a font callback.
 * This does NOT provide the firmware-to-entry boot trigger.
 */
#include "hb_sdk.h"
#include "hb_silver_uitask.h"
#include "hb_heap.h"

#include "hb_reloc.h"

#define RESIDENT_PATH     "/Library/resident.hbapp"
#define DISABLE_PATH      "/Library/disabled"
#define PACK_PATH         "/Library/AllApps.pack"
#define ACTION_SLOT      (HB_SILVER_HOME_VTABLE + HB_SILVER_HOME_DISPATCH_SLOT)
#define RAWMSG_SLOT      (HB_SILVER_HOME_VTABLE + HB_SILVER_HOME_RAWMSG_SLOT)

typedef int (*dispatch_t)(void *, const char *, void *);
typedef int (*rawmsg_t)(void *, void *);
static uint32_t busy;

static void set_dispatch(uint32_t target)
{
    *(volatile uint32_t *)ACTION_SLOT = target;
    hb_icache_invalidate(ACTION_SLOT, 4);
}

static void set_rawmsg(uint32_t target)
{
    *(volatile uint32_t *)RAWMSG_SLOT = target;
    hb_icache_invalidate(RAWMSG_SLOT, 4);
}

static int stock_dispatch(void *self, const char *name, void *msg)
{
    return ((dispatch_t)HB_SILVER_HOME_DISPATCH_ORIG)(self, name, msg);
}

/* Homebrew items can survive in the Home Screen's persistent layout while the
 * resident hooks disappear on reboot.  Sending one of their unknown sbitem
 * events through the stock raw-message path can panic the UI.  Keep a minimal
 * gate armed from the first CFF entry until the resident replaces it.  Stock
 * input is reproduced by hb_silver_rawmsg; only our custom taps are swallowed.
 *
 * The gate deliberately stays installed when the disk disable marker exists.
 * That makes recovery mode safe: built-in UI keeps working and stale homebrew
 * icons are inert instead of reaching a stock handler that cannot launch them.
 */
__attribute__((used, noinline))
int untether_rawmsg(void *self, void *msg)
{
    char event[64];
    int result = hb_silver_rawmsg(self, msg, "sbitem.hb.", ".clicked",
                                  event, sizeof event);
    return result == HB_SILVER_RAWMSG_CLAIMED ? 1 : result;
}

/* Visible symbol for the machine-code emulator and hardware diagnostics. */
__attribute__((used, noinline))
int untether_dispatch(void *self, const char *name, void *msg)
{
    void *resident_arena = 0;
    uint8_t *resident_base = 0;
    uint32_t resident_span = 0, resident_len = 0;
    uint32_t next_action, next_rawmsg;
    void *entry;
    if (busy)
        return stock_dispatch(self, name, msg);
    busy = 1;

    /* A disk marker lets a disk-mode recovery disable this stage without
     * replacing firmware. Missing files leave the dispatcher armed for a
     * later action (e.g. after the iPod has mounted its filesystem). */
    if (hb_fs_exists(DISABLE_PATH)) {
        set_dispatch(HB_SILVER_HOME_DISPATCH_ORIG);
        goto forward;
    }
    if (!hb_fs_exists(PACK_PATH))
        goto forward;
    entry = hb_reloc_load_file(RESIDENT_PATH, &resident_arena, &resident_len,
                               &resident_base, &resident_span);
    if (!entry)
        goto forward;

    /* Refuse to clobber another installer. Check again after file access,
     * which may yield to another task. No executable RAM is changed on a
     * missing, truncated, incompatible or corrupt resident file. */
    if (*(volatile uint32_t *)ACTION_SLOT !=
            ((uint32_t)(uintptr_t)&untether_dispatch | 1u) ||
        *(volatile uint32_t *)RAWMSG_SLOT !=
            ((uint32_t)(uintptr_t)&untether_rawmsg | 1u))
        goto forward;
    /* Clear the temporary hook before entering resident code. Its entry
     * installs the permanent dispatcher.  hb_reloc_load_file already copied,
     * rebased and cache-flushed the image in its own heap block. */
    set_rawmsg(HB_SILVER_HOME_RAWMSG_ORIG);
    set_dispatch(HB_SILVER_HOME_DISPATCH_ORIG);
    ((void (*)(void *, uint32_t))entry)(resident_base, resident_span);
    next_action = *(volatile uint32_t *)ACTION_SLOT;
    next_rawmsg = *(volatile uint32_t *)RAWMSG_SLOT;
    busy = 0;
    if ((next_action & 1u) && next_action >= (uint32_t)resident_base &&
        next_action < (uint32_t)resident_base + resident_span &&
        (next_rawmsg & 1u) && next_rawmsg >= (uint32_t)resident_base &&
        next_rawmsg < (uint32_t)resident_base + resident_span)
        return ((dispatch_t)next_action)(self, name, msg);
    /* A partial resident install must never expose custom taps to stock. */
    hb_os_free(resident_arena);
    resident_arena = 0;
    set_rawmsg((uint32_t)(uintptr_t)&untether_rawmsg | 1u);
    set_dispatch(HB_SILVER_HOME_DISPATCH_ORIG);
    return stock_dispatch(self, name, msg);

forward:
    hb_os_free(resident_arena);             /* NULL-safe */
    busy = 0;
    return stock_dispatch(self, name, msg);
}

/* cff_entry.S supplies payload_entry. The stack stage has already zeroed this
 * relocatable image's BSS and invalidated its instruction cache. */
void untether_bootstrap_main(void)
{
    /* The stock slot is reloaded from firmware at every real reboot; using
     * it as the guard avoids trusting stale DRAM magic after a warm reboot.
     * Preserve an already-running resident or an unknown third-party hook. */
    uint32_t action = *(volatile uint32_t *)ACTION_SLOT;
    uint32_t rawmsg = *(volatile uint32_t *)RAWMSG_SLOT;
    uint32_t our_action = (uint32_t)(uintptr_t)&untether_dispatch | 1u;
    uint32_t our_rawmsg = (uint32_t)(uintptr_t)&untether_rawmsg | 1u;

    /* Install the raw-message safety gate before exposing the action hook.
     * A later entry can heal either half of an interrupted two-slot install.
     * Preserve resident and third-party hooks unless the other slot is ours. */
    if (rawmsg == HB_SILVER_HOME_RAWMSG_ORIG &&
        (action == HB_SILVER_HOME_DISPATCH_ORIG || action == our_action))
        set_rawmsg((uint32_t)(uintptr_t)&untether_rawmsg | 1u);
    if (action == HB_SILVER_HOME_DISPATCH_ORIG &&
        (rawmsg == HB_SILVER_HOME_RAWMSG_ORIG || rawmsg == our_rawmsg))
        set_dispatch((uint32_t)(uintptr_t)&untether_dispatch | 1u);
}
