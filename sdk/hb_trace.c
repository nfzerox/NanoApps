/*
 * hb_trace.c — RAM-resident trace buffer that survives soft reset.
 *
 * The N7G DRAM at 0x09100C00 survives kernel panics + reboots. We use
 * this to leave breadcrumbs as we run; after a crash, the host reads
 * them via SCSI to see exactly where we died.
 *
 * Layout at 0x09100C00:
 *
 *   header (16 bytes):
 *     u32  magic     = 0x48425452  ('HBTR')
 *     u32  write_idx (next free entry index, wraps at MAX_ENTRIES)
 *     u32  count     (total entries written, never wraps — for ordering)
 *     u32  reserved
 *
 *   entries (each 16 bytes, ring buffer of 256 entries = 4 KB):
 *     char tag[8]   (e.g. "BOOT   ", "OPNEW  ", "CTOR   ")
 *     u32  value1
 *     u32  value2
 *
 *   Total: 16 + 16*256 = 4112 bytes.
 *
 * After a crash, run `tools/dump_trace.sh` to fetch + decode.
 *
 * IMPORTANT: hb_trace_init() does NOT clear the buffer — it only
 * sets the magic if missing. Apps that want a fresh trace should
 * call hb_trace_reset(). Apps that want to inherit from a prior
 * session (to see what crashed last time) call hb_trace_init().
 */

#include "hb_sdk.h"

/* Far from both the standard app load VA (0x0867f8e4) and the load
   stub region (0x09100000 + ~5 KB). 0x09120000 is a safe scratch
   address used by no SDK code or stub binary. */
#define HB_TRACE_BASE    0x09120000u
#define HB_TRACE_MAGIC   0x48425452u   /* 'HBTR' */
#define HB_TRACE_MAX     256

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t write_idx;
    uint32_t count;
    uint32_t reserved;
} hb_trace_hdr_t;

typedef struct __attribute__((packed)) {
    char     tag[8];
    uint32_t v1;
    uint32_t v2;
} hb_trace_entry_t;

static hb_trace_hdr_t *hdr(void)
{
    return (hb_trace_hdr_t *)HB_TRACE_BASE;
}

static hb_trace_entry_t *entries(void)
{
    return (hb_trace_entry_t *)(HB_TRACE_BASE + sizeof(hb_trace_hdr_t));
}

void hb_trace_reset(void)
{
    hb_trace_hdr_t *h = hdr();
    h->magic     = HB_TRACE_MAGIC;
    h->write_idx = 0;
    h->count     = 0;
    h->reserved  = 0;
    /* don't bother zeroing the entries — old data is fine, we
       overwrite via write_idx and count tells the reader how
       many are valid */
}

void hb_trace_init(void)
{
    hb_trace_hdr_t *h = hdr();
    if (h->magic != HB_TRACE_MAGIC) {
        hb_trace_reset();
    }
}

/* Copy up to 8 chars of tag, NUL-pad. */
static void copy_tag(char *dst, const char *src)
{
    int i = 0;
    while (i < 8 && src[i]) { dst[i] = src[i]; i++; }
    while (i < 8) { dst[i] = ' '; i++; }
}

void hb_trace_log(const char *tag, uint32_t v1, uint32_t v2)
{
    hb_trace_hdr_t *h = hdr();
    if (h->magic != HB_TRACE_MAGIC) hb_trace_reset();

    uint32_t idx = h->write_idx;
    if (idx >= HB_TRACE_MAX) idx = 0;
    hb_trace_entry_t *e = &entries()[idx];
    copy_tag(e->tag, tag ? tag : "?");
    e->v1 = v1;
    e->v2 = v2;

    h->write_idx = (idx + 1) % HB_TRACE_MAX;
    h->count++;
}
