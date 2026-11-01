/* One resident-owned arena, adapted from the tested Angry Birds port.
 * Size-binned free lists and coalescing keep asset-read allocations bounded. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "nano_alloc.h"

#ifdef AB_ALLOC_TEST
extern void *port_test_aux_block;
extern uint32_t hb_os_heap_free(void);
extern void *hb_os_alloc(uint32_t n);
extern void hb_os_free(void *p);
#define HB_APP_AUX_BLOCK port_test_aux_block
#define malloc ab_test_malloc
#define calloc ab_test_calloc
#define realloc ab_test_realloc
#define free ab_test_free
#define memalign ab_test_memalign
#else
#include "hb_sdk.h"
#include "hb_heap.h"
#endif

#ifndef PORT_ARENA_SIZE
#define PORT_ARENA_SIZE 0x80000u
#endif
#define PORT_ARENA_MIN 0x80000u
#define GL_RESERVE     0x400000u
#define BIN_COUNT 28
#define NONE UINT32_MAX
#define USED 1u
#define MIN_SPLIT 64u

/* All blocks are 16-byte multiples. Neighbour sizes permit constant-time
 * coalescing; free-list links are arena offsets so this header is always 16 bytes. */
typedef struct { uint32_t size_flags, prev_size, prev_free, next_free; } ArenaBlk;
typedef struct { void *base; uint32_t size, pad[2]; } AllocHdr;
static uint8_t *sArena;
static uint32_t sArenaSize, sHeapFreeAtInit, sBins[BIN_COUNT];
static PortAllocStats sAllocStats;

void port_alloc_stats_reset(void) { memset(&sAllocStats, 0, sizeof(sAllocStats)); }
PortAllocStats port_alloc_stats(void) { return sAllocStats; }
uint32_t port_arena_size(void) { return sArenaSize; }
uint32_t port_heap_free_at_arena_init(void) { return sHeapFreeAtInit; }

static uint32_t block_size(const ArenaBlk *b) { return b->size_flags & ~15u; }
static uint32_t offset(const ArenaBlk *b) { return (uint32_t)((const uint8_t *)b - sArena); }
static ArenaBlk *block_at(uint32_t off) { return (ArenaBlk *)(sArena + off); }
static unsigned bin_for(uint32_t size) { return 27u - (unsigned)__builtin_clz(size); }

static void insert_free(ArenaBlk *b)
{
    unsigned bin = bin_for(block_size(b));
    b->prev_free = NONE;
    b->next_free = sBins[bin];
    if (b->next_free != NONE) block_at(b->next_free)->prev_free = offset(b);
    sBins[bin] = offset(b);
}

static void remove_free(ArenaBlk *b)
{
    if (b->prev_free == NONE) sBins[bin_for(block_size(b))] = b->next_free;
    else block_at(b->prev_free)->next_free = b->next_free;
    if (b->next_free != NONE) block_at(b->next_free)->prev_free = b->prev_free;
}

static void arena_init(void)
{
    if (sArena) return;
    if (HB_APP_AUX_BLOCK) { hb_os_free(HB_APP_AUX_BLOCK); HB_APP_AUX_BLOCK = NULL; }
    uint32_t size = PORT_ARENA_SIZE, freeb = hb_os_heap_free();
    if (freeb > GL_RESERVE + PORT_ARENA_MIN && freeb - GL_RESERVE < size)
        size = (freeb - GL_RESERVE) & ~0xFFFFu;
    else if (freeb <= GL_RESERVE + PORT_ARENA_MIN)
        size = PORT_ARENA_MIN;
    while (!(sArena = hb_os_alloc(size)) && size > 0x100000u) size -= 0x80000u;
    if (!sArena) return;
    sArenaSize = size & ~15u;
    sHeapFreeAtInit = freeb;
    HB_APP_AUX_BLOCK = sArena; /* resident frees the entire arena on app unload */
    for (unsigned i = 0; i < BIN_COUNT; i++) sBins[i] = NONE;
    ArenaBlk *b = block_at(0);
    b->size_flags = sArenaSize;
    b->prev_size = 0;
    insert_free(b);
}

static void fix_next_prev(ArenaBlk *b)
{
    uint32_t next = offset(b) + block_size(b);
    if (next < sArenaSize) block_at(next)->prev_size = block_size(b);
}

/* b is not on a free list yet. Merge both neighbours before inserting it. */
static void release_block(ArenaBlk *b)
{
    b->size_flags &= ~USED;
    uint32_t next = offset(b) + block_size(b);
    if (next < sArenaSize && !(block_at(next)->size_flags & USED)) {
        ArenaBlk *n = block_at(next);
        remove_free(n);
        b->size_flags += block_size(n);
    }
    if (b->prev_size) {
        ArenaBlk *p = block_at(offset(b) - b->prev_size);
        if (!(p->size_flags & USED)) {
            remove_free(p);
            p->size_flags += block_size(b);
            b = p;
        }
    }
    fix_next_prev(b);
    insert_free(b);
}

/* b has been removed from the free lists, or is already allocated. */
static void resize_block(ArenaBlk *b, uint32_t need)
{
    uint32_t available = block_size(b);
    if (available - need >= MIN_SPLIT) {
        b->size_flags = need | USED;
        ArenaBlk *tail = block_at(offset(b) + need);
        tail->size_flags = available - need;
        tail->prev_size = need;
        release_block(tail);
    } else {
        b->size_flags = available | USED;
        fix_next_prev(b);
    }
}

static void *arena_alloc(uint32_t n)
{
    sAllocStats.allocations++;
    arena_init();
    if (!sArena || n > sArenaSize - sizeof(ArenaBlk) - 15u) {
        sAllocStats.failures++;
        return NULL;
    }
    uint32_t need = ((n + 15u) & ~15u) + sizeof(ArenaBlk);
    for (unsigned bin = bin_for(need); bin < BIN_COUNT; bin++) {
        for (uint32_t off = sBins[bin]; off != NONE; off = block_at(off)->next_free) {
            ArenaBlk *b = block_at(off);
            sAllocStats.probes++;
            if (block_size(b) < need) continue;
            remove_free(b);
            resize_block(b, need);
            return b + 1;
        }
    }
    sAllocStats.failures++;
    return NULL;
}

uint32_t port_arena_largest_free(void)
{
    uint32_t best = 0;
    for (unsigned bin = 0; sArena && bin < BIN_COUNT; bin++)
        for (uint32_t off = sBins[bin]; off != NONE; off = block_at(off)->next_free)
            if (block_size(block_at(off)) > best) best = block_size(block_at(off));
    return best;
}

static int in_arena(const void *p)
{
    return sArena && (uintptr_t)p >= (uintptr_t)sArena && (uintptr_t)p < (uintptr_t)sArena + sArenaSize;
}

void *memalign(size_t align, size_t size)
{
    if (align < 16) align = 16;
    if ((align & (align - 1)) || align > UINT32_MAX - sizeof(AllocHdr) ||
        size > UINT32_MAX - align - sizeof(AllocHdr)) return NULL;
    uint8_t *raw = arena_alloc((uint32_t)(size + align - 1u + sizeof(AllocHdr)));
    if (!raw) {
#ifndef AB_ALLOC_TEST
        printf("OOM: %u bytes\n", (unsigned)size);
#endif
        return NULL;
    }
    uintptr_t p = ((uintptr_t)raw + sizeof(AllocHdr) + align - 1) & ~(uintptr_t)(align - 1);
    AllocHdr *h = (AllocHdr *)p - 1;
    h->base = raw;
    h->size = (uint32_t)size;
    return (void *)p;
}

void *malloc(size_t size) { return memalign(16, size); }
void free(void *p)
{
    if (!p) return;
    void *base = ((AllocHdr *)p - 1)->base;
    if (in_arena(base)) release_block((ArenaBlk *)base - 1);
    else hb_os_free(base);
}

void *calloc(size_t n, size_t size)
{
    if (size && n > UINT32_MAX / size) return NULL;
    void *p = malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void *realloc(void *p, size_t size)
{
    if (!p) return malloc(size);
    if (!size) { free(p); return NULL; }
    AllocHdr *h = (AllocHdr *)p - 1;
    if (in_arena(h->base)) {
        ArenaBlk *b = (ArenaBlk *)h->base - 1;
        uint32_t prefix = (uint32_t)((uint8_t *)p - (uint8_t *)b);
        if (size > UINT32_MAX - prefix - 15u) return NULL;
        uint32_t need = (prefix + (uint32_t)size + 15u) & ~15u;
        uint32_t next = offset(b) + block_size(b);
        if (need > block_size(b) && next < sArenaSize && !(block_at(next)->size_flags & USED) &&
            need <= block_size(b) + block_size(block_at(next))) {
            ArenaBlk *n = block_at(next);
            remove_free(n);
            b->size_flags = (block_size(b) + block_size(n)) | USED;
        }
        if (need <= block_size(b)) {
            resize_block(b, need);
            h->size = (uint32_t)size;
            return p;
        }
    }
    void *q = malloc(size);
    if (q) {
        uint32_t copy = h->size < size ? h->size : (uint32_t)size;
        memcpy(q, p, copy);
        sAllocStats.copied += copy;
        free(p);
    }
    return q;
}
