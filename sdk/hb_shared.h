/*
 * hb_shared.h — state the resident shares with the running app.
 *
 * There is no safe fixed DRAM on the untethered Nano 7: 0x0910xxxx-0x094xxxxx
 * is all live OS heap. So the resident owns one hb_shared_t inside its own
 * heap-loaded image and hands its address to every app entry (the 5th
 * payload_entry argument). The SDK reaches it through the global `hb_shared`,
 * which each image sets at entry; it is NULL until then (every user checks).
 *
 * Bump HB_SHARED_VERSION when the layout changes.
 */
#ifndef HB_SHARED_H
#define HB_SHARED_H

#include <stdint.h>

#define HB_SHARED_MAGIC    0x48534248u   /* 'HBSH' */
#define HB_SHARED_VERSION  2u

typedef struct hb_shared {
    uint32_t magic;                       /* HB_SHARED_MAGIC                     */
    uint32_t version;                     /* HB_SHARED_VERSION                   */
    /* Resident-latched touch mailbox [magic, x, y, pressed] (see hb_touch_mb.h). */
    volatile int32_t touch[4];
    /* One app-owned hb_os_alloc block the resident frees on unload (hb_heap.h). */
    void *volatile aux_block;
    /* Screenshot app toggles: 0 off / 1 screenshot / 2 record on triple-click
     * Home; demo status bar (9:41, full battery). */
    volatile uint8_t screenshot_mode;
    volatile uint8_t statusbar_demo;
    uint8_t  pad[2];
    /* The resident's trace ring logger (hb_trace_log forwards here). */
    void (*trace)(const char *tag, uint32_t v1, uint32_t v2);
    /* Stop asynchronous work; return zero while the OS still owns app memory.
     * The resident must not unload or reinitialize the image until this returns 1. */
    int (*prepare_unload)(void);
} hb_shared_t;

/* Set by the image's entry (resident: its own block; app: the entry argument). */
extern hb_shared_t *hb_shared;

/* Surface-app entry: the resident calls entry(0, fb, w, h, shared). */
typedef void *(*hb_surface_entry_t)(int op, void *fb, int w, int h, hb_shared_t *sh);

#endif /* HB_SHARED_H */
