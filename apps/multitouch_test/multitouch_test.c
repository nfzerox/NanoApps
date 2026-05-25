/*
 * multitouch_test — paint a colored dot per finger.
 *
 * hb_touch_poll_multi reads all available finger slots each frame and
 * reports active fingers.
 * Each finger gets its own color so we can see how many are simultaneous.
 *
 * Exit on Vol Up.
 */

#include "hb_sdk.h"

static const hb_color_t FINGER_COLORS[HB_MAX_FINGERS] = {
    HB_RGB(0xFF, 0x40, 0x40),  /* 0 red    */
    HB_RGB(0x40, 0xFF, 0x40),  /* 1 green  */
    HB_RGB(0x40, 0x40, 0xFF),  /* 2 blue   */
    HB_RGB(0xFF, 0xFF, 0x40),  /* 3 yellow */
    HB_RGB(0xFF, 0x40, 0xFF),  /* 4 magenta*/
    HB_RGB(0x40, 0xFF, 0xFF),  /* 5 cyan   */
    HB_RGB(0xFF, 0xA0, 0x40),  /* 6 orange */
    HB_RGB(0xA0, 0xFF, 0xA0),  /* 7 mint   */
};

HB_APP_ENTRY(payload_entry)
{
    hb_fill_screen(HB_BLACK);
    hb_draw_uint(8, 4, 0, 1, HB_WHITE, HB_BLACK);

    uint32_t last_n = 99;
    for (uint32_t frame = 0; frame < 2000000; frame++) {
        if (hb_button_pressed(HB_BTN_VOL_UP)) break;

        hb_touch_drain_all();

        hb_touch_t fingers[HB_MAX_FINGERS];
        int n = hb_touch_poll_multi(fingers);

        if ((uint32_t)n != last_n) {
            hb_fill_rect(0, 0, HB_SCREEN_W, 40, HB_BLACK);
            hb_draw_uint(8, 4, (uint32_t)n, 1, HB_WHITE, HB_BLACK);
            last_n = (uint32_t)n;
        }

        for (int i = 0; i < n; i++) {
            int16_t x = fingers[i].x;
            int16_t y = fingers[i].y;
            int16_t r = 14;
            int16_t x0 = x - r, y0 = y - r;
            if (x0 < 0) x0 = 0;
            if (y0 < 40) y0 = 40;
            if (x0 + 2*r >= HB_SCREEN_W) x0 = HB_SCREEN_W - 2*r;
            if (y0 + 2*r >= HB_SCREEN_H) y0 = HB_SCREEN_H - 2*r;
            hb_color_t c = FINGER_COLORS[fingers[i].touch_id & 7];
            hb_fill_rect(x0, y0, 2*r, 2*r, c);
        }

        for (volatile uint32_t i = 0; i < 3000; i++) { }
    }
}
