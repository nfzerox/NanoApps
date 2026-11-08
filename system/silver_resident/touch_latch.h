/* touch_latch.h: what the resident latches into the touch mailbox from the OS
 * first-finger queue. From nano7kit by Shimku (MIT).
 *
 * The queue at 0x089a5298 is a circular list behind a sentinel node (head[4]),
 * oldest sample first: hb_touch_pop_front (sdk/hb_touch.c) takes the front and
 * hb_touch_drain_to_one leaves "the most recent touch". The touch-posting entry
 * we hook runs before the OS drains it, so more than one sample can be waiting.
 * The resident used to latch only the FRONT sample. With a backlog that is
 * wrong: an old lift in front of a new press latched "up" while the finger was
 * down, and the position lagged; a game reading a new swipe saw it as a lift.
 *
 * Now: down if any queued sample is down, at the newest such sample; up (at the
 * newest sample) only if every queued sample has lifted.
 * A tap whose press and lift are both queued still latches down first, as it did
 * with the front sample, so no tap is lost. The walk reads at most 16 nodes and
 * never writes to the queue.
 *
 * Returns 1 with *x, *y, *pressed set; 0 if the queue is empty (released);
 * -1 if nothing could be read (leave the mailbox as it is). */
#ifndef HB_TOUCH_LATCH_H
#define HB_TOUCH_LATCH_H
#include <stdint.h>
#include "hb_shared.h"

#ifndef HB_TL_RAM
#define HB_TL_RAM(a) ((a) >= 0x08000000u && (a) < 0x10000000u)
#endif
#define HB_TL_MAX_NODES 16u

typedef uintptr_t hb_tl_word;

static inline int hb_tl_pick(volatile hb_tl_word *head, int32_t *x, int32_t *y,
                             int32_t *pressed)
{
    hb_tl_word end, node;
    uint32_t n = 0;
    int got = 0, down = 0;
    if (head[5] == 0)
        return 0;
    end = head[4];
    if (!HB_TL_RAM(end))
        return -1;
    for (node = ((volatile hb_tl_word *)end)[0];
         node != end && HB_TL_RAM(node) && n < HB_TL_MAX_NODES;
         node = ((volatile hb_tl_word *)node)[0], n++) {
        volatile uint8_t *v = (volatile uint8_t *)(((volatile hb_tl_word *)node) + 2); /* node + 8 */
        int is_down = (v[4] != 1);                                     /* status 1: the touch ended */
        if (is_down || !down) {
            *x = (int32_t)(int16_t)((volatile int32_t *)v)[2];
            *y = (int32_t)(int16_t)((volatile int32_t *)v)[3];
        }
        if (is_down)
            down = 1;
        got = 1;
    }
    if (!got)
        return -1;
    *pressed = down;
    return 1;
}

/* The newest sample of one finger queue, for hb_shared_t.touch_snap: out =
 * id, status, x, y; status HB_TOUCH_SNAP_EMPTY when the queue holds nothing
 * readable. Read at the same moment as the latch; never writes to the queue. */
static inline void hb_tl_newest(volatile hb_tl_word *head, volatile int32_t *out)
{
    hb_tl_word end, node, last = 0;
    uint32_t n = 0;
    if (head[5] != 0 && HB_TL_RAM(head[4])) {
        end = head[4];
        for (node = ((volatile hb_tl_word *)end)[0];
             node != end && HB_TL_RAM(node) && n < HB_TL_MAX_NODES;
             node = ((volatile hb_tl_word *)node)[0], n++)
            last = node;
    }
    if (!last) {
        out[1] = (int32_t)HB_TOUCH_SNAP_EMPTY;
        return;
    }
    {
        volatile int32_t *v = (volatile int32_t *)(((volatile hb_tl_word *)last) + 2); /* node + 8 */
        out[0] = v[0];
        out[2] = (int32_t)(int16_t)v[2];
        out[3] = (int32_t)(int16_t)v[3];
        out[1] = (int32_t)((volatile uint8_t *)v)[4];
    }
}

#endif
