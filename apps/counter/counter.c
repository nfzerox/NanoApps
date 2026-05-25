/*
 * counter — minimal SDK example. Draws a static 7-digit number on a
 * blue background.
 */

#include "hb_sdk.h"

HB_APP_ENTRY(payload_entry)
{
    /* Crash-log breadcrumb — if launcher loaded us successfully,
       this gets recorded in the trace buffer at 0x09120000 (which
       survives reboot). */
    hb_trace_log("CNT_ENT", 0xfeed, 0);

    hb_fill_screen(HB_BLUE);

    int16_t x = (HB_SCREEN_W - 7 * HB_DIGIT_W) / 2;
    int16_t y = (HB_SCREEN_H - HB_DIGIT_H) / 2;
    hb_draw_uint(x, y, 1234567, 7, HB_WHITE, HB_BLUE);

    hb_trace_log("CNT_END", 0, 0);
}
