/* Snapshot OS heap metadata after the CFF bootstrap/loader, before NanoApps
 * registration. This is a diagnostic resident, not a fix for the heap overlap.
 * It makes no writes into the investigated heap window except its own loaded
 * image and the SDK file cache that the bootstrap already uses.
 */
#include "hb_sdk.h"
#include "hb_silver_uitask.h"

#define HEAP_OBJECT_CELL 0x08909134u
#define RAM_LO           0x08000000u
#define RAM_HI           0x0a000000u
#define MAX_REGIONS      20u
#define MAX_SAMPLES      10u
#define MAX_BLOCKS       500000u
#define DIAG_PATH        "/Library/heapdiag.bin"
#define ACTION_SLOT      (HB_SILVER_HOME_VTABLE + HB_SILVER_HOME_DISPATCH_SLOT)
#define RAWMSG_SLOT      (HB_SILVER_HOME_VTABLE + HB_SILVER_HOME_RAWMSG_SLOT)

typedef struct { uint32_t start, size; } region_t;
typedef struct {
    uint32_t target, region, header, end, word0, word1, found;
} sample_t;
typedef struct {
    uint32_t magic, version, heap_object, region_count, status;
    uint32_t bad_address, bad_word0, blocks_walked, sample_count;
    region_t regions[MAX_REGIONS];
    sample_t samples[MAX_SAMPLES];
} snapshot_t;

/* The sample list is ordered by address; the decoder treats flags as raw bits
 * until the allocator's exact flag semantics have been established. */
static const uint32_t targets[MAX_SAMPLES] = {
    0x09100000u, 0x09118000u, 0x0911a000u, 0x09120000u,
    0x09130000u, 0x09135b00u, 0x09137000u, 0x09138000u,
    0x09140000u, 0x09170000u,
};

static void capture(snapshot_t *s)
{
    uint32_t i, j, heap;
    uint32_t *words = (uint32_t *)s;
    for (i = 0; i < sizeof(*s) / 4u; ++i) words[i] = 0;
    s->magic = 0x31445048u; /* HPD1, little endian */
    s->version = 1;
    s->sample_count = MAX_SAMPLES;
    for (i = 0; i < MAX_SAMPLES; ++i) {
        s->samples[i].target = targets[i];
        s->samples[i].region = 0xffffffffu;
    }

    heap = *(volatile const uint32_t *)HEAP_OBJECT_CELL;
    s->heap_object = heap;
    if (heap < RAM_LO || heap > RAM_HI - 0xa8u || (heap & 3u)) {
        s->status = 1; /* invalid heap object */
        return;
    }
    s->region_count = *(volatile const uint32_t *)(heap + 0xa4u);
    if (s->region_count == 0 || s->region_count > MAX_REGIONS) {
        s->status = 2; /* invalid region count */
        return;
    }

    for (i = 0; i < s->region_count; ++i) {
        uint32_t start = *(volatile const uint32_t *)(heap + 4u + 8u * i);
        uint32_t size = *(volatile const uint32_t *)(heap + 8u + 8u * i);
        uint32_t end, hdr;
        s->regions[i].start = start;
        s->regions[i].size = size;
        if (start < RAM_LO || start >= RAM_HI || (start & 3u) ||
            size < 8u || size > RAM_HI - start) {
            s->status = 3; /* invalid region bounds */
            s->bad_address = start;
            return;
        }
        end = start + size;
        if (start > targets[MAX_SAMPLES - 1] || end <= targets[0])
            continue;
        for (hdr = start; hdr <= end - 8u && hdr <= targets[MAX_SAMPLES - 1]; ) {
            uint32_t raw0 = *(volatile const uint32_t *)hdr;
            uint32_t raw1 = *(volatile const uint32_t *)(hdr + 4u);
            uint32_t block_size = raw0 & ~3u;
            uint32_t next;
            if (block_size == 0u) {
                for (j = 0; j < MAX_SAMPLES; ++j) {
                    if (!s->samples[j].found && s->samples[j].target >= hdr &&
                        s->samples[j].target < end) {
                        s->status = 6; /* end marker before a target */
                        s->bad_address = hdr;
                        break;
                    }
                }
                break;
            }
            if (block_size < 8u || block_size > end - hdr ||
                ++s->blocks_walked > MAX_BLOCKS) {
                s->status = s->blocks_walked > MAX_BLOCKS ? 5u : 4u;
                s->bad_address = hdr;
                s->bad_word0 = raw0;
                return;
            }
            next = hdr + block_size;
            for (j = 0; j < MAX_SAMPLES; ++j) {
                sample_t *p = &s->samples[j];
                if (!p->found && p->target >= hdr && p->target < next) {
                    p->region = i;
                    p->header = hdr;
                    p->end = next;
                    p->word0 = raw0;
                    p->word1 = raw1;
                    p->found = 1;
                }
            }
            hdr = next;
        }
    }
}

typedef int (*dispatch_t)(void *, const char *, void *);

__attribute__((used, noinline))
static int diag_dispatch(void *self, const char *name, void *msg)
{
    return ((dispatch_t)HB_SILVER_HOME_DISPATCH_ORIG)(self, name, msg);
}

__attribute__((used, noinline))
static int diag_rawmsg(void *self, void *msg)
{
    char event[64];
    int result = hb_silver_rawmsg(self, msg, "sbitem.hb.", ".clicked",
                                  event, sizeof event);
    return result == HB_SILVER_RAWMSG_CLAIMED ? 1 : result;
}

HB_APP_ENTRY(payload_entry)
{
    snapshot_t snapshot;
    capture(&snapshot);

    /* The loader restores both stock slots immediately before entry. Arm the
     * raw gate before file I/O so persisted homebrew icons stay inert. */
    if (*(volatile uint32_t *)ACTION_SLOT == HB_SILVER_HOME_DISPATCH_ORIG &&
        *(volatile uint32_t *)RAWMSG_SLOT == HB_SILVER_HOME_RAWMSG_ORIG) {
        hb_silver_uitask_hook(HB_SILVER_HOME_VTABLE,
                              HB_SILVER_HOME_RAWMSG_SLOT, (void *)&diag_rawmsg);
        hb_silver_uitask_hook(HB_SILVER_HOME_VTABLE,
                              HB_SILVER_HOME_DISPATCH_SLOT, (void *)&diag_dispatch);
    }
    hb_fs_write(DIAG_PATH, &snapshot, sizeof snapshot);
}
