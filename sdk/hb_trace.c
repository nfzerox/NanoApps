/*
 * hb_trace.c — breadcrumb ring that survives a panic reboot.
 *
 * DRAM keeps its contents across the Nano 7's panic + reboot cycle, but there
 * is no fixed address that is safe to own: all of 0x0910xxxx-0x094xxxxx is live
 * OS heap (the old ring at 0x09120000 was overwritten by heap data). So the
 * ring lives INSIDE the resident's heap-loaded image, and the next boot finds
 * it again by scanning DRAM for a valid header (hb_trace_find_previous).
 *
 * Layout (16-byte aligned):
 *
 *   header (16 bytes):
 *     u32  magic     = 0x48425452  ('HBTR')
 *     u32  write_idx (next free entry index, wraps at HB_TRACE_MAX)
 *     u32  count     (total entries written, never wraps — for ordering)
 *     u32  seq       (boot sequence: previous ring's seq + 1)
 *
 *   entries (each 16 bytes, ring of 256 entries = 4 KB):
 *     char tag[8]   (e.g. "BOOT    ", "OPNEW   ")
 *     u32  value1
 *     u32  value2
 *
 *   Total: 16 + 16*256 = 4112 bytes (HB_TRACE_BYTES).
 *
 * Only the resident owns a ring (hb_trace_attach). Every other image — the
 * boot stub, apps — forwards hb_trace_log to the resident through
 * hb_shared->trace, or drops the entry if there is no resident yet.
 */

#include "hb_sdk.h"
#include "hb_shared.h"

#define HB_TRACE_MAGIC   0x48425452u   /* 'HBTR' */
#define HB_TRACE_MAX     256u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t write_idx;
    uint32_t count;
    uint32_t seq;
} hb_trace_hdr_t;

typedef struct __attribute__((packed)) {
    char     tag[8];
    uint32_t v1;
    uint32_t v2;
} hb_trace_entry_t;

static hb_trace_hdr_t *s_ring;         /* NULL unless this image owns the ring */

static hb_trace_entry_t *entries(void)
{
    return (hb_trace_entry_t *)(s_ring + 1);
}

void hb_trace_attach(void *ring, uint32_t seq)
{
    s_ring = (hb_trace_hdr_t *)ring;
    s_ring->seq = seq;
    hb_trace_reset();
}

void hb_trace_reset(void)
{
    if (!s_ring)
        return;
    s_ring->magic     = HB_TRACE_MAGIC;
    s_ring->write_idx = 0;
    s_ring->count     = 0;
    /* don't bother zeroing the entries — count tells the reader how many
       are valid */
}

void hb_trace_init(void)
{
    /* Kept for source compatibility: the ring is set up by hb_trace_attach. */
}

/* Copy up to 8 chars of tag, space-pad. */
static void copy_tag(char *dst, const char *src)
{
    int i = 0;
    while (i < 8 && src[i]) { dst[i] = src[i]; i++; }
    while (i < 8) { dst[i] = ' '; i++; }
}

void hb_trace_log(const char *tag, uint32_t v1, uint32_t v2)
{
    if (!s_ring) {
        if (hb_shared && hb_shared->trace && hb_shared->trace != hb_trace_log)
            hb_shared->trace(tag, v1, v2);
        return;
    }
    uint32_t idx = s_ring->write_idx;
    if (idx >= HB_TRACE_MAX) idx = 0;
    hb_trace_entry_t *e = &entries()[idx];
    copy_tag(e->tag, tag ? tag : "?");
    e->v1 = v1;
    e->v2 = v2;

    s_ring->write_idx = (idx + 1u) % HB_TRACE_MAX;
    s_ring->count++;
}

/* A plausible ring header: the magic, and write_idx == count mod 256 (the
 * invariant every log keeps), with at least one entry. */
static int ring_valid(const volatile uint32_t *h)
{
    return h[0] == HB_TRACE_MAGIC && h[2] != 0u && h[1] < HB_TRACE_MAX &&
           (h[2] % HB_TRACE_MAX) == h[1];
}

const void *hb_trace_find_previous(uint32_t *out_seq,
                                   const void *exclude_base,
                                   uint32_t exclude_span)
{
    const volatile uint32_t *p   = (const volatile uint32_t *)HB_TRACE_SCAN_LO;
    const volatile uint32_t *end = (const volatile uint32_t *)
                                   (HB_TRACE_SCAN_HI - HB_TRACE_BYTES);
    const void *best = 0;
    uint32_t best_seq = 0;
    uintptr_t exclude_lo = (uintptr_t)exclude_base;
    uintptr_t exclude_hi = exclude_lo + exclude_span;

    for (; p <= end; p += 4) {                   /* 16-byte stride */
        uintptr_t here = (uintptr_t)p;
        if (exclude_span && exclude_hi >= exclude_lo &&
            here < exclude_hi && here + HB_TRACE_BYTES > exclude_lo)
            continue;
        if (p[0] != HB_TRACE_MAGIC || (const void *)p == (const void *)s_ring)
            continue;
        if (ring_valid(p) && (!best || p[3] > best_seq)) {
            best = (const void *)p;
            best_seq = p[3];
        }
    }
    *out_seq = best_seq;
    return best;
}
