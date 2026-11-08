/*
 * hb_shared.h — state the resident shares with the running app.
 *
 * There is no safe fixed DRAM on the untethered Nano 7: 0x0910xxxx-0x094xxxxx
 * is all live OS heap. So the resident owns one hb_shared_t inside its own
 * heap-loaded image and hands its address to every app entry (the 5th
 * payload_entry argument). The SDK reaches it through the global `hb_shared`,
 * which each image sets at entry; it is NULL until then (every user checks).
 *
 * Bump HB_SHARED_VERSION when the layout changes. Surface runtimes require an
 * exact version match, so a field that old apps can safely ignore is appended
 * with its own magic instead (touch_snap below) and the version stays put.
 */
#ifndef HB_SHARED_H
#define HB_SHARED_H

#include <stdint.h>

#define HB_SHARED_MAGIC    0x48534248u   /* 'HBSH' */
#define HB_SHARED_VERSION  2u

/* Every OS finger queue's newest sample, copied by the resident at each touch
 * event before the OS drains the queues (polling from an app frame races that
 * drain). finger[q] = { id, status, x, y }; status HB_TOUCH_SNAP_EMPTY when the
 * queue held nothing readable. seq is bumped after each copy. Check magic first:
 * residents before this field existed leave whatever follows hb_shared there. */
#define HB_TOUCH_SNAP_MAGIC  0x504e5354u   /* 'TSNP' */
#define HB_TOUCH_SNAP_EMPTY  0xff
typedef struct {
    uint32_t magic;                       /* HB_TOUCH_SNAP_MAGIC                 */
    volatile uint32_t seq;
    volatile int32_t finger[8][4];        /* id, status, x, y                    */
} hb_touch_snap_t;

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
    /* Appended (version unchanged): multi-finger snapshot, see hb_touch_snap_t. */
    hb_touch_snap_t touch_snap;
} hb_shared_t;

/* Set by the image's entry (resident: its own block; app: the entry argument). */
extern hb_shared_t *hb_shared;

/* Surface-app entry: the resident calls entry(0, fb, w, h, shared). */
typedef void *(*hb_surface_entry_t)(int op, void *fb, int w, int h, hb_shared_t *sh);

#endif /* HB_SHARED_H */
