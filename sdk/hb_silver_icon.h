/*
 * hb_silver_icon.h — serve custom home-screen icon bitmaps at runtime.
 *
 * Springboard bitmaps resolve through a disk-backed, id-range-limited cache
 * (loads from the on-disk image db at boot), so a novel id in the resource
 * chain is never consulted. But the cache calls a single "alternate resolver"
 * for in-range ids that have no on-disk data (empty slots). We install our own
 * alternate resolver: for ids present in a packed bitmap blob (the format the
 * ipodhax tool produces, which is byte-identical to the cache's in-memory
 * bitmap header), we hand back a pointer to our bitmap; everything else falls
 * through to the original resolver.
 *
 * Requirements:
 *  - The blob's ids must be EMPTY IN-RANGE cache slots (no on-disk bitmap), so
 *    ref-counting lands on an empty entry and never frees our buffer.
 *  - The blob must stay resident; pass its base address.
 *  - Call on the UI task (e.g. from the home-controller hook), after the blob
 *    is in memory and before/at icon registration.
 *
 * Addresses are for iPod nano 7 OS 1.1.2 (see hb_silver_icon.c for derivation).
 */
#ifndef HB_SILVER_ICON_H
#define HB_SILVER_ICON_H

#include "hb_sdk.h"

/*
 * Install the alternate-resolver hook so the packed bitmap blob at `blob_addr`
 * serves its ids as icons. Idempotent-ish: re-installing points the cache at
 * our resolver again (which always chains to the true original). Returns 1 on
 * success, 0 if the blob looks invalid.
 */
int hb_silver_icon_serve(uint32_t blob_addr);

/*
 * Register the bitmaps in `blob_addr` (a 'paMB' pack in STABLE memory — e.g. a
 * heap copy) as NEW entries appended to the bitmap cache's table, giving them
 * genuinely-unique in-range ids instead of squatting on the OS's empty slots
 * (which collide with scroll-thumb / Settings cell art via the global
 * alt-resolver). Allocates the enlarged entry table via `alloc` (OS heap),
 * copies + re-threads the existing entries, appends ours (refcount pinned, not
 * in the LRU ring so never evicted), and patches each bitmap's property-id to its
 * assigned id. Returns the FIRST assigned id (the k-th bitmap is first+k) and
 * sets *out_count; 0 on failure. Call ONCE per boot, on the UI task.
 */
long hb_silver_icon_register_table(uint32_t blob_addr, void *(*alloc)(uint32_t),
                                   int *out_count);

/*
 * Batch form: register `count` individual bitmaps (each `bmh_addrs[k]` points at
 * a bitmap header, `lens[k]` its byte size) as new cache entries in ONE table
 * extension. Returns the first assigned id (the k-th bitmap is first+k), 0 on
 * failure. Used by the .app registrar to register every app's icon at once.
 */
long hb_silver_icon_register_bitmaps(const uint32_t *bmh_addrs, const uint32_t *lens,
                                     int count, void *(*alloc)(uint32_t));

/*
 * Register `count` icons LAZILY (disk-backed): extend the cache table with empty
 * in-range entries + install a resolver that serves each from a per-icon disk
 * file ICON_DISK_DIR "<k>.bin" (k = id - first) through a small LRU of OS-heap
 * buffers (`alloc`). Unlike register_bitmaps, the bitmaps are NOT held in heap —
 * only the visible working set is resident, so heap stays bounded regardless of
 * count. Returns the first assigned cache id (= each app's icon id is first+k),
 * or 0 on failure. Call once per boot. NOTE: the resolver + LRU live in resident
 * .bss, so this must re-run on a fresh boot (gated like the rest of registration);
 * a bare re-push won't re-serve until reboot.
 */
long hb_silver_icon_register_lazy(int count, void *(*alloc)(uint32_t));

/*
 * Record the actual folder for the k-th lazily-served icon (0..count-1).
 * Bundled icons use /Apps/<folder>/icon.bin; legacy icons use Icons/<name>.bin.
 * Set name, hash and layout on the UI task before publishing the Home item.
 */
void hb_silver_icon_set_name(int k, const char *name);
void hb_silver_icon_set_hash(int k, uint32_t hash, int bundled);
/* Reuse the once-per-boot 64-slot extension; discard cached disk identities. */
long hb_silver_icon_refresh(void);

/*
 * True iff our extended bitmap-cache entries are still present (no-reboot
 * re-push) — false after any reboot reset the cache. The robust "already
 * registered this boot" gate; does NOT depend on the OS label-chain tag, which
 * the OS can drop across a re-push. Use it to decide rearm vs full re-register.
 */
int hb_silver_icon_live(void);

/*
 * Restore the stock bitmap-cache alternate resolver IF the field currently holds
 * a (stale) pointer into the resident region — e.g. after re-pushing over a
 * build that used the old hb_silver_icon_serve path. Safe no-op otherwise. Call
 * at load, before the cache is next used.
 */
void hb_silver_icon_reset_resolver(void);

/*
 * Re-arm the disk-backed lazy resolver to THIS build's function after a no-reboot
 * re-deploy. The cache table still holds our empty entries and the persisted LRU
 * (in fixed DRAM), but the resolver field points at the previous build's
 * overwritten code; this re-points it. Returns 1 if re-armed, 0 if there is no
 * valid persisted state (fresh boot — register_lazy will install it instead).
 * Call at resident entry when OS chain state shows apps are already registered.
 */
int hb_silver_icon_rearm(void);

/*
 * Grow the OS bitmap cache by adding `bytes` of OS heap (alloc, e.g. operator
 * new) to its allocator (the pool init is additive). The stock 625 KB pool is
 * already near-full with the OS's own icons, so registering many custom icons
 * makes it thrash; this gives it room so evictions stay rare and the home grid
 * stays responsive. Call ONCE per boot (the added pool persists). Returns the
 * bytes added, or 0 on failure.
 */
int hb_silver_icon_grow_cache(uint32_t bytes, void *(*alloc)(uint32_t size));

#endif /* HB_SILVER_ICON_H */
