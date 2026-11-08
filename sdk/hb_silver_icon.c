/*
 * hb_silver_icon.c — serve custom icon bitmaps via the bitmap cache's
 * alternate-resolver hook. See hb_silver_icon.h.
 *
 */

#include "hb_silver_icon.h"

#define ALT_RESOLVER_FIELD 0x08a453b8u   /* cache this(0x089a78d0) + 0x9dae8 */
#define ORIG_ALT_RESOLVER  0x0801b0e9u   /* stock resolver (thumb)          */
#define ADDR_BUF_BUMPREF   0x0841abd3u   /* bumps the cache buffer's ref count (this) */

typedef long (*alt_resolver_t)(long id, void *out_ptr);
typedef void (*bump_ref_t)(void *buf);

/* ---- disk-backed lazy serving --------------------------------------------
 * Instead of pinning all icons in heap forever (register_bitmaps), register the
 * ids as EMPTY in-range entries (size 0) so the cache asks our alt-resolver for
 * them on demand. The resolver serves each from a per-icon DISK file through a
 * small LRU of OS-heap buffers — so the icons live on disk and only the visible
 * working set is resident (heap bounded regardless of app count). The disk read
 * uses the OS file API on the UI task, (hb_fs_read declared in hb_sdk.h, included
 * via hb_silver_icon.h.) */

/* 16 x 50 KB instead of 20 x 56 KB saves ~320 KB of boot heap (nano7kit).
 * Every icon.bin is exactly 112x112 ARGB + a 32-byte header = 50,208 bytes
 * (enforced by tools/package_app.py), so 0xC800 always fits one. */
#define ICON_LRU_N    16u                 /* resident icons (LRU) */
#define ICON_BUF_SZ   0xC800u             /* >= a 112x112 ARGB bitmap + header   */
_Static_assert(ICON_BUF_SZ >= 112u * 112u * 4u + 32u, "icon buffer holds one icon.bin");
#define ICON_DISK_DIR "/Apps/Icons/"

/* Lazy-resolver state: which ids we serve and the LRU of OS-heap buffers. It
 * lives in the resident's .bss (inside its heap-loaded image). It used to sit
 * at fixed 0x09135b00 so it could survive a no-reboot SCSI re-push, but that
 * address is live OS heap, and the untethered loader runs the resident exactly
 * once per boot, so there is no re-push to survive. */
#define ICON_STATE_MAGIC 0x49434e53u      /* 'ICNS' — state established + valid   */

typedef struct {
    uint32_t magic;
    uint32_t blob;                        /* non-lazy serve path (hb_silver_icon_serve) */
    long     first;                       /* first cache id of our icons          */
    int      count;                       /* number of lazily-served icons        */
    uint32_t rr;                          /* round-robin eviction cursor          */
    uint32_t names;                       /* heap char[count][64]: folder/legacy name */
    uint32_t hashes[64];                  /* zero = legacy icon; otherwise manifest FNV */
    uint8_t bundled[64];                  /* /Apps/<folder>/icon.bin instead of legacy */
    struct { long id; uint8_t *buf; } lru[ICON_LRU_N];
} icon_state_t;

#define ICON_NAME_MAX 64u                 /* folder, or legacy filename          */

static icon_state_t s_icon_state;
#define IST  ((volatile icon_state_t *)&s_icon_state)

/* Bundles keep icons beside their code. Legacy catalogs use Icons/<name>.bin;
 * blank legacy slots fall back to Icons/<k>.bin. */
static void icon_path(char *out, long k)
{
    int bundled = k >= 0 && k < 64 && IST->bundled[k];
    const char *d = bundled ? "/Apps/" : ICON_DISK_DIR;
    const char *suffix = bundled ? "/icon.bin" : ".bin";
    const char *nm = 0;
    int i = 0, t;

    if (IST->names && k >= 0 && k < IST->count)
        nm = (const char *)(uintptr_t)(IST->names + (uint32_t)k * ICON_NAME_MAX);

    while (*d) out[i++] = *d++;
    if (nm && nm[0]) {
        int j;
        for (j = 0; nm[j] && j < (int)ICON_NAME_MAX - 1; j++) out[i++] = nm[j];
    } else {
        char num[12]; int s = 0;
        if (k == 0) num[s++] = '0';
        else { long v = k; while (v) { num[s++] = (char)('0' + v % 10); v /= 10; } }
        for (t = s - 1; t >= 0; t--) out[i++] = num[t];
    }
    while (*suffix) out[i++] = *suffix++;
    out[i] = 0;
}

__attribute__((section(".text.hook"), used, noinline))
static long icon_lazy_resolver(long id, void *out_ptr)
{
    long k = id - IST->first;
    char path[96];
    uint8_t *buf;
    uint32_t slot, n;
    int i;

    if (k < 0 || k >= IST->count)
        return ((alt_resolver_t)(uintptr_t)ORIG_ALT_RESOLVER)(id, out_ptr);

    for (i = 0; i < (int)ICON_LRU_N; i++) {          /* LRU hit -> serve it        */
        if (IST->lru[i].buf && IST->lru[i].id == id) {
            *(void **)out_ptr = IST->lru[i].buf;
            ((bump_ref_t)(uintptr_t)ADDR_BUF_BUMPREF)(IST->lru[i].buf);
            return id;
        }
    }

    slot = IST->rr; IST->rr = (IST->rr + 1u) % ICON_LRU_N;      /* miss -> load      */
    buf = IST->lru[slot].buf;
    if (!buf)
        return ((alt_resolver_t)(uintptr_t)ORIG_ALT_RESOLVER)(id, out_ptr);
    icon_path(path, k);
    n = hb_fs_read(path, buf, ICON_BUF_SZ);
    if (n >= 28u && IST->hashes[k]) {
        uint32_t h = 0x811c9dc5u, j;
        for (j = 0; j < n; j++) h = (h ^ buf[j]) * 0x01000193u;
        if (h != IST->hashes[k]) n = 0;
    }
    if (n < 28u) {                                    /* read failed -> chain        */
        IST->lru[slot].id = -1;
        return ((alt_resolver_t)(uintptr_t)ORIG_ALT_RESOLVER)(id, out_ptr);
    }
    *(uint32_t *)(uintptr_t)(buf + 24u) = (uint32_t)id;  /* bitmap header property-id */
    IST->lru[slot].id = id;
    *(void **)out_ptr = buf;
    ((bump_ref_t)(uintptr_t)ADDR_BUF_BUMPREF)(buf);
    return id;
}

/* Installed as the cache's alternate resolver. Runs on the UI task during icon
 * rendering. For ids in our blob, hand back our bitmap; else chain to stock. */
__attribute__((section(".text.hook"), used, noinline))
static long icon_alt_resolver(long id, void *out_ptr)
{
    uint32_t blob = IST->blob;
    if (blob) {
        uint32_t data_start = *(uint32_t *)(uintptr_t)(blob + 4u);
        uint32_t count      = *(uint32_t *)(uintptr_t)(blob + 16u);
        uint32_t *table     = (uint32_t *)(uintptr_t)(blob + 28u);
        uint32_t k;
        for (k = 0; k < count && k < 64u; k++) {
            if (table[k * 3u] == (uint32_t)id) {
                void *bmh = (void *)(uintptr_t)(blob + data_start + table[k * 3u + 1u]);
                *(void **)out_ptr = bmh;                 /* bitmap-header smart-ptr char* */
                ((bump_ref_t)(uintptr_t)ADDR_BUF_BUMPREF)(bmh);  /* balance the ref      */
                return id;
            }
        }
    }
    return ((alt_resolver_t)(uintptr_t)ORIG_ALT_RESOLVER)(id, out_ptr);
}

int hb_silver_icon_serve(uint32_t blob_addr)
{
    if (!blob_addr || *(uint32_t *)(uintptr_t)(blob_addr + 12u) != 0x424d6170u) /* 'paMB' */
        return 0;
    IST->blob = blob_addr;
    *(uint32_t *)(uintptr_t)ALT_RESOLVER_FIELD =
        ((uint32_t)(uintptr_t)&icon_alt_resolver) | 1u;
    return 1;
}

#define BMC_THIS          0x089a78d0u
#define BMC_FIRST_OFF     0x00u
#define BMC_NUM_OFF       0x04u
#define BMC_ENTRIES_OFF   0x08u
#define BMC_HITLIST_OFF   0x0cu
#define BMC_ENTRY_SZ      24u
#define BMC_E_BITMAP      0u
#define BMC_E_MOFFSET     4u
#define BMC_E_MSIZE       8u
#define BMC_E_REFCNT      12u
#define BMC_E_NEXT        16u
#define BMC_E_PREV        20u
#define BMH_PROPID_OFF    24u
#define BMC_PINNED_REF    0x10000u   /* high start so inc-ref/dec-ref never hit 0 */

#define BMC_ALLOCATOR_OFF 0x10u
#define ADDR_BMC_INITPOOL 0x084195dau   /* the pool init(blk,sz) entry */
typedef int (*bmc_initpool_t)(void *allocator, void *block, uint32_t size);

int hb_silver_icon_grow_cache(uint32_t bytes, void *(*alloc)(uint32_t))
{
    void *buf, *allocator;
    if (!alloc || bytes < 0x10000u)
        return 0;
    buf = alloc(bytes);
    if (!buf)
        return 0;
    allocator = (void *)(uintptr_t)(BMC_THIS + BMC_ALLOCATOR_OFF);
    return ((bmc_initpool_t)(uintptr_t)(ADDR_BMC_INITPOOL | 1u))(allocator, buf, bytes)
           ? (int)bytes : 0;
}

void hb_silver_icon_reset_resolver(void)
{
    /* If the alt-resolver field still points into DRAM (a previous resident's
     * icon_alt_resolver, now dead), restore the stock resolver so empty-slot
     * lookups don't jump through a stale pointer. The stock value is firmware
     * code (below 0x08800000), so a DRAM value can only be ours. */
    uint32_t v = *(uint32_t *)(uintptr_t)ALT_RESOLVER_FIELD;
    if (v != ORIG_ALT_RESOLVER && v >= 0x08d00000u && v < 0x0a000000u)
        *(uint32_t *)(uintptr_t)ALT_RESOLVER_FIELD = ORIG_ALT_RESOLVER;
}

int hb_silver_icon_rearm(void)
{
    /* Re-deploy without reboot: the cache table still holds our empty entries and
     * the LRU buffers a previous build allocated are still live (state persisted
     * in IST), but the alt-resolver field points at the PREVIOUS build's now-
     * overwritten icon_lazy_resolver. Re-point it at THIS build's function so the
     * empty entries resolve again (else home icons render blank and their
     * artwork-based tap regions die). Only valid when the persistent state says
     * we have lazily-served icons. */
    if (IST->magic != ICON_STATE_MAGIC || IST->count <= 0)
        return 0;
    *(uint32_t *)(uintptr_t)ALT_RESOLVER_FIELD =
        ((uint32_t)(uintptr_t)&icon_lazy_resolver) | 1u;
    return 1;
}

int hb_silver_icon_live(void)
{
    /* Are our extended bitmap-cache entries STILL present? The cache's entry table
     * is OS-heap and survives a no-reboot re-push (-> true: just rearm the
     * resolver), but a reboot (cold OR DRAM-preserving panic) re-inits the cache to
     * its stock size (-> false: full re-register). This is the robust "already set
     * up this boot" gate. We deliberately do NOT use the OS label-chain tag: it can
     * be dropped across a re-push even while our cache entries persist, which would
     * wrongly clear the resolver and blank every homebrew icon. */
    uint32_t cfirst, num, idx;
    uint8_t *entries, *e;
    if (IST->magic != ICON_STATE_MAGIC || IST->count <= 0 || IST->first <= 0)
        return 0;
    cfirst  = *(uint32_t *)(uintptr_t)(BMC_THIS + BMC_FIRST_OFF);
    num     = *(uint32_t *)(uintptr_t)(BMC_THIS + BMC_NUM_OFF);
    entries = *(uint8_t **)(uintptr_t)(BMC_THIS + BMC_ENTRIES_OFF);
    if (!entries || (uint32_t)IST->first < cfirst)
        return 0;
    idx = (uint32_t)IST->first - cfirst;
    if (idx + (uint32_t)IST->count > num)          /* our range fell out of the table */
        return 0;
    e = entries + idx * BMC_ENTRY_SZ;
    if (*(uint32_t *)(e + BMC_E_MSIZE) != 0)       /* slot is no longer our empty entry */
        return 0;
    return 1;
}

long hb_silver_icon_register_bitmaps(const uint32_t *bmh_addrs, const uint32_t *lens,
                                     int count, void *(*alloc)(uint32_t))
{
    uint32_t first, old_n, new_n, lo, hi, i;
    uint8_t *old_tab, *old_hit, *new_tab, *new_hit;
    int k;

    if (!bmh_addrs || !lens || !alloc || count <= 0 || count > 64)
        return 0;

    first   = *(uint32_t *)(uintptr_t)(BMC_THIS + BMC_FIRST_OFF);
    old_n   = *(uint32_t *)(uintptr_t)(BMC_THIS + BMC_NUM_OFF);
    old_tab = *(uint8_t **)(uintptr_t)(BMC_THIS + BMC_ENTRIES_OFF);
    old_hit = *(uint8_t **)(uintptr_t)(BMC_THIS + BMC_HITLIST_OFF);
    if (!old_tab || old_n == 0u || old_n > 100000u)                 /* sanity */
        return 0;

    new_n = old_n + (uint32_t)count;
    new_tab = (uint8_t *)alloc(new_n * BMC_ENTRY_SZ);
    if (!new_tab)
        return 0;

    /* copy the existing entries verbatim */
    for (i = 0; i < old_n * BMC_ENTRY_SZ; i++)
        new_tab[i] = old_tab[i];

    /* re-point the copied entries' ring links (next/prev) into the new table */
    lo = (uint32_t)(uintptr_t)old_tab;
    hi = lo + old_n * BMC_ENTRY_SZ;
    for (i = 0; i < old_n; i++) {
        uint32_t o;
        for (o = BMC_E_NEXT; o <= BMC_E_PREV; o += 4u) {
            uint32_t *pp = (uint32_t *)(new_tab + i * BMC_ENTRY_SZ + o);
            if (*pp >= lo && *pp < hi)
                *pp = (uint32_t)(uintptr_t)new_tab + (*pp - lo);
        }
    }

    /* append our entries: a non-zero size + bitmap-ptr so the lookup returns them
     * directly; refcount pinned HIGH so eviction/dec-ref never free our buffer; the
     * property-id patched to the assigned id so inc-ref indexes the right entry. */
    for (k = 0; k < count; k++) {
        uint8_t *e = new_tab + (old_n + (uint32_t)k) * BMC_ENTRY_SZ;
        uint32_t bmh = bmh_addrs[k];
        uint32_t id  = first + old_n + (uint32_t)k;
        *(uint32_t *)(uintptr_t)(bmh + BMH_PROPID_OFF) = id;
        *(uint32_t *)(e + BMC_E_BITMAP)  = bmh;
        *(uint32_t *)(e + BMC_E_MOFFSET) = 0;
        *(uint32_t *)(e + BMC_E_MSIZE)   = lens[k];
        *(uint32_t *)(e + BMC_E_REFCNT)  = BMC_PINNED_REF;
    }

    new_hit = ((uint32_t)(uintptr_t)old_hit >= lo && (uint32_t)(uintptr_t)old_hit < hi)
              ? new_tab + ((uint32_t)(uintptr_t)old_hit - lo) : new_tab;

    /* splice our block into the LRU ring after the head (an inc-ref move-to-front
     * derefs the next/prev links, so they must be valid; refcount high => never evicted) */
    {
        uint8_t *head  = new_hit;
        uint8_t *after = *(uint8_t **)(head + BMC_E_NEXT);
        for (k = 0; k < count; k++) {
            uint8_t *e = new_tab + (old_n + (uint32_t)k) * BMC_ENTRY_SZ;
            *(uint8_t **)(e + BMC_E_PREV) =
                (k == 0) ? head : new_tab + (old_n + (uint32_t)k - 1u) * BMC_ENTRY_SZ;
            *(uint8_t **)(e + BMC_E_NEXT) =
                (k + 1 == count) ? after
                                 : new_tab + (old_n + (uint32_t)k + 1u) * BMC_ENTRY_SZ;
        }
        *(uint8_t **)(head + BMC_E_NEXT)  = new_tab + old_n * BMC_ENTRY_SZ;
        *(uint8_t **)(after + BMC_E_PREV) =
            new_tab + (old_n + (uint32_t)count - 1u) * BMC_ENTRY_SZ;
    }

    /* commit: table + hitlist before the count (old table left allocated) */
    *(uint8_t **)(uintptr_t)(BMC_THIS + BMC_ENTRIES_OFF) = new_tab;
    *(uint8_t **)(uintptr_t)(BMC_THIS + BMC_HITLIST_OFF) = new_hit;
    *(uint32_t *)(uintptr_t)(BMC_THIS + BMC_NUM_OFF)     = new_n;
    return (long)(first + old_n);
}

long hb_silver_icon_register_lazy(int count, void *(*alloc)(uint32_t))
{
    uint32_t first, old_n, new_n, lo, hi, i;
    uint8_t *old_tab, *old_hit, *new_tab, *new_hit;
    int k;

    if (!alloc || count <= 0 || count > 64)
        return 0;

    first   = *(uint32_t *)(uintptr_t)(BMC_THIS + BMC_FIRST_OFF);
    old_n   = *(uint32_t *)(uintptr_t)(BMC_THIS + BMC_NUM_OFF);
    old_tab = *(uint8_t **)(uintptr_t)(BMC_THIS + BMC_ENTRIES_OFF);
    old_hit = *(uint8_t **)(uintptr_t)(BMC_THIS + BMC_HITLIST_OFF);
    if (!old_tab || old_n == 0u || old_n > 100000u)
        return 0;

    new_n = old_n + (uint32_t)count;
    new_tab = (uint8_t *)alloc(new_n * BMC_ENTRY_SZ);
    if (!new_tab)
        return 0;

    for (i = 0; i < old_n * BMC_ENTRY_SZ; i++)             /* copy existing entries */
        new_tab[i] = old_tab[i];
    lo = (uint32_t)(uintptr_t)old_tab;                     /* re-point copied ring  */
    hi = lo + old_n * BMC_ENTRY_SZ;
    for (i = 0; i < old_n; i++) {
        uint32_t o;
        for (o = BMC_E_NEXT; o <= BMC_E_PREV; o += 4u) {
            uint32_t *pp = (uint32_t *)(new_tab + i * BMC_ENTRY_SZ + o);
            if (*pp >= lo && *pp < hi)
                *pp = (uint32_t)(uintptr_t)new_tab + (*pp - lo);
        }
    }

    /* append EMPTY entries (size 0 -> the lookup routes to our alt-resolver on demand;
     * a null bitmap-ptr -> eviction never frees anything for them; refcount 0). */
    for (k = 0; k < count; k++) {
        uint8_t *e = new_tab + (old_n + (uint32_t)k) * BMC_ENTRY_SZ;
        *(uint32_t *)(e + BMC_E_BITMAP)  = 0;
        *(uint32_t *)(e + BMC_E_MOFFSET) = 0;
        *(uint32_t *)(e + BMC_E_MSIZE)   = 0;
        *(uint32_t *)(e + BMC_E_REFCNT)  = 0;
    }

    new_hit = ((uint32_t)(uintptr_t)old_hit >= lo && (uint32_t)(uintptr_t)old_hit < hi)
              ? new_tab + ((uint32_t)(uintptr_t)old_hit - lo) : new_tab;
    /* splice our block into the LRU ring after the head — an inc-ref (when a custom
     * icon renders) does move-to-front, which derefs the next/prev links, so they
     * must be valid. A null bitmap-ptr makes eviction skip them (nothing to free). */
    {
        uint8_t *head  = new_hit;
        uint8_t *after = *(uint8_t **)(head + BMC_E_NEXT);
        for (k = 0; k < count; k++) {
            uint8_t *e = new_tab + (old_n + (uint32_t)k) * BMC_ENTRY_SZ;
            *(uint8_t **)(e + BMC_E_PREV) =
                (k == 0) ? head : new_tab + (old_n + (uint32_t)k - 1u) * BMC_ENTRY_SZ;
            *(uint8_t **)(e + BMC_E_NEXT) =
                (k + 1 == count) ? after
                                 : new_tab + (old_n + (uint32_t)k + 1u) * BMC_ENTRY_SZ;
        }
        *(uint8_t **)(head + BMC_E_NEXT)  = new_tab + old_n * BMC_ENTRY_SZ;
        *(uint8_t **)(after + BMC_E_PREV) =
            new_tab + (old_n + (uint32_t)count - 1u) * BMC_ENTRY_SZ;
    }

    *(uint8_t **)(uintptr_t)(BMC_THIS + BMC_ENTRIES_OFF) = new_tab;
    *(uint8_t **)(uintptr_t)(BMC_THIS + BMC_HITLIST_OFF) = new_hit;
    *(uint32_t *)(uintptr_t)(BMC_THIS + BMC_NUM_OFF)     = new_n;

    /* set up the LRU + install the disk-backed resolver. State goes in the fixed,
     * reboot-stable struct so a later no-reboot re-push can re-arm the resolver
     * (hb_silver_icon_rearm) without re-extending the table or re-allocating the
     * LRU. This runs once per boot (register_to_os), so a stale IST from a warm
     * reboot is fully overwritten here. */
    IST->first = (long)(first + old_n);
    IST->count = count;
    IST->rr = 0;
    /* Per-icon bundle-name table (heap, OS-heap-stable across re-pushes). The
     * resolver names each disk file by it; blank entries fall back to "<k>.bin". */
    {
        char *nm = (char *)alloc((uint32_t)count * ICON_NAME_MAX);
        IST->names = (uint32_t)(uintptr_t)nm;
        if (nm)
            for (i = 0; i < (uint32_t)count * ICON_NAME_MAX; i++) nm[i] = 0;
    }
    for (k = 0; k < (int)ICON_LRU_N; k++) {
        IST->lru[k].buf = (uint8_t *)alloc(ICON_BUF_SZ);
        IST->lru[k].id  = -1;
    }
    IST->magic = ICON_STATE_MAGIC;
    *(uint32_t *)(uintptr_t)ALT_RESOLVER_FIELD =
        ((uint32_t)(uintptr_t)&icon_lazy_resolver) | 1u;
    return IST->first;
}

/* Set the actual folder name (or legacy filename) before publishing Home. */
void hb_silver_icon_set_name(int k, const char *name)
{
    char *slot;
    int j;
    if (!IST->names || k < 0 || k >= IST->count || !name)
        return;
    slot = (char *)(uintptr_t)(IST->names + (uint32_t)k * ICON_NAME_MAX);
    for (j = 0; name[j] && j < (int)ICON_NAME_MAX - 1; j++) slot[j] = name[j];
    slot[j] = 0;
}

void hb_silver_icon_set_hash(int k, uint32_t hash, int bundled)
{
    if (k >= 0 && k < IST->count && k < 64) {
        IST->hashes[k] = hash;
        IST->bundled[k] = bundled != 0;
    }
}

long hb_silver_icon_refresh(void)
{
    int k;
    if (!hb_silver_icon_live()) return 0;
    for (k = 0; k < (int)ICON_LRU_N; k++) IST->lru[k].id = -1;
    IST->rr = 0;
    return IST->first;
}

long hb_silver_icon_register_table(uint32_t blob_addr, void *(*alloc)(uint32_t),
                                   int *out_count)
{
    uint32_t data_start, count, *table, bmh[64], lens[64], k;
    long first;

    if (out_count)
        *out_count = 0;
    if (!blob_addr ||
        *(uint32_t *)(uintptr_t)(blob_addr + 12u) != 0x424d6170u)   /* 'paMB' */
        return 0;
    data_start = *(uint32_t *)(uintptr_t)(blob_addr + 4u);
    count      = *(uint32_t *)(uintptr_t)(blob_addr + 16u);
    table      = (uint32_t *)(uintptr_t)(blob_addr + 28u);
    if (count == 0u || count > 64u)
        return 0;
    for (k = 0; k < count; k++) {
        bmh[k]  = blob_addr + data_start + table[k * 3u + 1u];
        lens[k] = table[k * 3u + 2u];
    }
    first = hb_silver_icon_register_bitmaps(bmh, lens, (int)count, alloc);
    if (first && out_count)
        *out_count = (int)count;
    return first;
}
