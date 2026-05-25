/*
 * hb_touch.c — touch input via polling OS state globals.
 *
 * Strategy: watch the OS touch timestamp and inspect the current touch
 * sample queue. When the timestamp changes, report the latest sample.
 */

#include "hb_sdk.h"

/* Confirmed via SCSI diff: low 32 bits of touch event time, in µs. */
#define HB_TOUCH_TIMESTAMP_ADDR  0x08909080u

/* First-finger touch queue. Each queue head is 0x18 bytes. */
#define HB_TOUCH_LIST_HEAD       0x089a5298u

static uint32_t s_last_ts = 0;
static int s_inited = 0;

void hb_touch_init(void)
{
    volatile uint32_t *ts = (volatile uint32_t *)HB_TOUCH_TIMESTAMP_ADDR;
    s_last_ts = *ts;
    s_inited = 1;
}

/* Read current touch coords from the first-finger queue if any.
   Empty list check: head[+0x14] != 0.

   Returns true and fills *out if a touch was readable. False if empty. */
bool hb_touch_get_coords(hb_touch_t *out)
{
    volatile uint32_t *head = (volatile uint32_t *)HB_TOUCH_LIST_HEAD;
    uint32_t size = head[5];  /* +0x14 */
    if (size == 0) return false;

    uint32_t end_node = head[4];  /* +0x10 = ptr to end_node */
    if (end_node < 0x08000000u || end_node >= 0x10000000u) return false;
    uint32_t first_node = *(volatile uint32_t *)end_node;
    if (first_node < 0x08000000u || first_node >= 0x10000000u) return false;

    volatile int32_t *value = (volatile int32_t *)(first_node + 8);
    volatile uint8_t *vb    = (volatile uint8_t *)(first_node + 8);
    if (out) {
        out->valid = true;
        out->time_lo = *(volatile uint32_t *)HB_TOUCH_TIMESTAMP_ADDR;
        out->x = (int16_t)value[2];  /* value+8  */
        out->y = (int16_t)value[3];  /* value+12 */
        out->status   = vb[4];       /* value+4 = touch status byte */
        out->touch_id = (uint32_t)value[0]; /* value+0 = touch ID */
    }
    return true;
}

bool hb_touch_poll(hb_touch_t *out)
{
    if (!s_inited) hb_touch_init();

    volatile uint32_t *ts = (volatile uint32_t *)HB_TOUCH_TIMESTAMP_ADDR;
    uint32_t cur = *ts;
    if (cur == s_last_ts) return false;
    s_last_ts = cur;
    return hb_touch_get_coords(out);
}

/* Pop the front node off the first-finger queue without taking the
   list mutex:
     - end = head[+0x10]; N = end->next (== first real node)
     - N->prev->next = N->next; N->next->prev = N->prev   (unlink)
     - head[+0x14]--                                       (size--)
     - N->next = head[+4]; head[+4] = N                    (push to freelist)
   The OS uses an intrusive freelist at head[+4] (singly-linked via +0).

*/
bool hb_touch_pop_front(void)
{
    volatile uint32_t *head = (volatile uint32_t *)HB_TOUCH_LIST_HEAD;
    uint32_t end_node = head[4];          /* head[+0x10] */
    if (end_node < 0x08000000u || end_node >= 0x10000000u) return false;

    uint32_t node = *(volatile uint32_t *)(end_node + 0);  /* end->next */
    if (node == end_node) return false;   /* empty (sentinel loop) */
    if (node < 0x08000000u || node >= 0x10000000u) return false;

    uint32_t next = *(volatile uint32_t *)(node + 0);  /* N->next */
    uint32_t prev = *(volatile uint32_t *)(node + 4);  /* N->prev (= end_node) */

    /* Unlink N. */
    *(volatile uint32_t *)(prev + 0) = next;
    *(volatile uint32_t *)(next + 4) = prev;

    /* size-- */
    head[5] = head[5] - 1;

    /* Push N to freelist at head[+4] (singly-linked via offset 0). */
    *(volatile uint32_t *)(node + 0) = head[1];
    head[1] = node;

    return true;
}

/* Loop pop_front until size <= 1, so the most recent touch is in
   front. Avoids the singleton race where a concurrent driver push and
   our pop both mutate end_node and clobber each other. Always leaves a
   node in the list. Returns number of nodes popped. */
uint32_t hb_touch_drain_to_one(void)
{
    volatile uint32_t *head = (volatile uint32_t *)HB_TOUCH_LIST_HEAD;
    uint32_t popped = 0;
    while (head[5] >= 2) {
        if (!hb_touch_pop_front()) break;
        popped++;
    }
    return popped;
}

/* ---- Multitouch ----
 *
 * The OS hardware driver maintains one small queue per finger. Each
 * queue has the same layout as the first-finger queue.
 *
 * We mirror hb_touch_drain_to_one + hb_touch_get_coords across all 8
 * heads, returning the count of active fingers.
 */

#define HB_TOUCH_LIST_BASE   0x089a5298u
#define HB_TOUCH_LIST_STRIDE 0x18u

static bool ht_pop_front_at(volatile uint32_t *head)
{
    uint32_t end_node = head[4];
    if (end_node < 0x08000000u || end_node >= 0x10000000u) return false;
    uint32_t node = *(volatile uint32_t *)(end_node + 0);
    if (node == end_node) return false;
    if (node < 0x08000000u || node >= 0x10000000u) return false;

    uint32_t next = *(volatile uint32_t *)(node + 0);
    uint32_t prev = *(volatile uint32_t *)(node + 4);
    *(volatile uint32_t *)(prev + 0) = next;
    *(volatile uint32_t *)(next + 4) = prev;
    head[5] = head[5] - 1;
    *(volatile uint32_t *)(node + 0) = head[1];
    head[1] = node;
    return true;
}

static bool ht_get_coords_at(volatile uint32_t *head, hb_touch_t *out,
                             uint32_t finger_idx)
{
    uint32_t size = head[5];
    if (size == 0) return false;
    uint32_t end_node = head[4];
    if (end_node < 0x08000000u || end_node >= 0x10000000u) return false;
    uint32_t first_node = *(volatile uint32_t *)end_node;
    if (first_node < 0x08000000u || first_node >= 0x10000000u) return false;

    volatile int32_t *value = (volatile int32_t *)(first_node + 8);
    volatile uint8_t *vb    = (volatile uint8_t *)(first_node + 8);
    if (out) {
        out->valid = true;
        out->time_lo = *(volatile uint32_t *)HB_TOUCH_TIMESTAMP_ADDR;
        out->x = (int16_t)value[2];
        out->y = (int16_t)value[3];
        out->status = vb[4];
        out->touch_id = finger_idx;
    }
    return true;
}

void hb_touch_drain_all(void)
{
    for (uint32_t i = 0; i < HB_MAX_FINGERS; i++) {
        volatile uint32_t *head =
            (volatile uint32_t *)(HB_TOUCH_LIST_BASE + i * HB_TOUCH_LIST_STRIDE);
        while (head[5] >= 2) {
            if (!ht_pop_front_at(head)) break;
        }
    }
}

int hb_touch_poll_multi(hb_touch_t out[HB_MAX_FINGERS])
{
    int n = 0;
    for (uint32_t i = 0; i < HB_MAX_FINGERS; i++) {
        volatile uint32_t *head =
            (volatile uint32_t *)(HB_TOUCH_LIST_BASE + i * HB_TOUCH_LIST_STRIDE);
        if (ht_get_coords_at(head, &out[n], i)) {
            n++;
        }
    }
    return n;
}
