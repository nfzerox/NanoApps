/*
 * hb_heap.h — the OS main heap (OS memory allocator).
 *
 * Homebrew shares the live OS heap. Two ways in:
 *   - the heap allocator (0x0842d444) panics when the allocation fails.
 *     A too-large request reboots the device. Never probe with it.
 *   - The nothrow allocator (0x0842b440) underneath returns NULL
 *     on failure. hb_os_alloc() wraps it, so you can request optimistically and
 *     handle NULL.
 *
 * Use sparingly and free promptly — holding megabytes starves the OS (compositor,
 * etc.). The free-size queries are pure reads (no allocation), safe to call any
 * time, e.g. to size a buffer before grabbing it.
 */
#ifndef HB_HEAP_H
#define HB_HEAP_H

#include <stdint.h>

/* Total free bytes in the OS heap right now (the free-size query). Pure read. */
uint32_t hb_os_heap_free(void);

/* Largest single CONTIGUOUS free block (the largest-free-block query) — the real
 * ceiling for one allocation, which fragmentation can hold well below the total.
 * Pure read. */
uint32_t hb_os_heap_largest(void);

/* Allocate `size` bytes from the OS heap. Returns NULL on failure (NOT a panic,
 * unlike the heap allocator). The usable region is guaranteed to lie within physical
 * SDRAM (0x08000000..0x0A000000). Free with hb_os_free(). */
void *hb_os_alloc(uint32_t size);

/* Free a block returned by hb_os_alloc (the matched free entry). */
void hb_os_free(void *p);

/* One app-owned hb_os_alloc block that the resident frees when it unloads the
 * app (app switch / relaunch after quit) and clears at boot. For buffers too big
 * for .bss that would otherwise leak: LVGL apps get no quit callback, and their
 * .bss (holding the pointer) is re-zeroed on every launch. Free any previous
 * block before storing a new one. Never use a fixed DRAM address as a buffer —
 * 0x0910xxxx-0x094xxxxx is live OS heap. The slot lives in the resident's
 * shared block (hb_shared.h), set before any app code runs. */
#include "hb_shared.h"
#define HB_APP_AUX_BLOCK      (hb_shared->aux_block)

#endif /* HB_HEAP_H */
