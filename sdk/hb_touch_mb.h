/*
 * hb_touch_mb.h — resident-latched event-path touch mailbox (single source of truth).
 *
 * The resident's touch-posting entry hook writes [magic, x, y, pressed] to the
 * mailbox in its shared block (hb_shared->touch, hb_shared.h) on every touch
 * event; surface apps read it so a tap is never missed (polling the transient
 * OS touch list only caught ~1/4 of taps). It used to sit at fixed 0x09135f40,
 * which is live OS heap.
 */
#ifndef HB_TOUCH_MB_H
#define HB_TOUCH_MB_H

#include <stdint.h>

#define HB_TOUCH_MB_MAGIC  0x54504e54u   /* 'TNPT' — resident has written it    */

typedef struct {
    int valid;   /* 1 if the resident has latched the mailbox (else poll)       */
    int down;    /* finger down                                                 */
    int x, y;    /* screen coordinates (only meaningful when valid)             */
} hb_touch_state_t;

/* Read the latched mailbox into *st. Returns st->valid (1 = armed, 0 = caller
 * should fall back to polling the OS touch list). */
int hb_touch_mb_read(hb_touch_state_t *st);

#endif /* HB_TOUCH_MB_H */
