/*
 * brightness_test — 4 buttons to set backlight to OFF / LOW / MED / MAX.
 * Calls hb_brightness_set with raw u16 level values.
 *
 * Whole app runs in a 64 KB pthread so we can later mix with audio etc.
 */

#include "hb_sdk.h"

#define PTHREAD_CREATE_ADDR (0x080226f8u | 1u)
typedef int (*pthread_create_t)(uint32_t *thread, void *attr,
                                void *(*start)(void *), void *arg);

#define BW 70
#define BH 60
#define ROW1_Y 60
#define ROW2_Y 130

static void redraw(const char *status, hb_color_t fg)
{
    hb_fill_screen(HB_BLACK);
    hb_draw_str(4, 0, "BRIGHTNESS", 2, HB_YELLOW, HB_BLACK);
    hb_ui_button_draw(  4, ROW1_Y, BW, BH, "0%",
                       HB_RGB(0x20,0x20,0x20), HB_WHITE);
    hb_ui_button_draw( 84, ROW1_Y, BW, BH, "25%",
                       HB_RGB(0x30,0x30,0x00), HB_WHITE);
    hb_ui_button_draw(164, ROW1_Y, BW, BH, "50%",
                       HB_RGB(0x60,0x60,0x00), HB_WHITE);
    hb_ui_button_draw( 84, ROW2_Y, BW, BH, "100%",
                       HB_RGB(0xa0,0xa0,0x00), HB_BLACK);
    /* True backlight power toggle. */
    hb_ui_button_draw(164, ROW2_Y, BW, BH, "PWR",
                       HB_RGB(0x40,0x00,0x00), HB_WHITE);
    hb_ui_button_draw(  4, ROW2_Y, BW, BH, "ON",
                       HB_RGB(0x00,0x60,0x00), HB_WHITE);
    hb_draw_str(4, 210, status, 1, fg, HB_BLACK);
    hb_draw_str(4, HB_SCREEN_H - 14, "HOME / VOL = exit", 1,
                HB_RGB(0x80,0x80,0x80), HB_BLACK);
}

static void *app_main(void *arg)
{
    (void)arg;
    hb_ui_init();
    redraw("tap level button", HB_WHITE);

    for (uint32_t frame = 0; frame < 20000000; frame++) {
        int16_t tx, ty;
        hb_ui_event_t e = hb_ui_poll(&tx, &ty);
        if (e == HB_UI_EXIT) break;
        if (e == HB_UI_TAP) {
            if (hb_ui_button_hit(tx, ty, 164, ROW2_Y, BW, BH)) {
                hb_brightness_power(false);
                redraw("backlight POWER OFF", HB_RED);
                continue;
            }
            if (hb_ui_button_hit(tx, ty, 4, ROW2_Y, BW, BH)) {
                hb_brightness_power(true);
                redraw("backlight POWER ON", HB_GREEN);
                continue;
            }
            int p = -1;
            if      (hb_ui_button_hit(tx, ty,  4, ROW1_Y, BW, BH)) p = 0;
            else if (hb_ui_button_hit(tx, ty, 84, ROW1_Y, BW, BH)) p = 25;
            else if (hb_ui_button_hit(tx, ty,164, ROW1_Y, BW, BH)) p = 50;
            else if (hb_ui_button_hit(tx, ty, 84, ROW2_Y, BW, BH)) p = 100;
            if (p >= 0) {
                hb_brightness_set_percent(p);
                /* show "pct=N raw=0xXXXX" */
                char buf[40] = "pct=";
                int bi = 4;
                int t = p, tens = 0;
                while (t >= 10) { t -= 10; tens++; }
                if (tens) buf[bi++] = '0' + tens;
                buf[bi++] = '0' + t;
                buf[bi++] = ' '; buf[bi++] = 'r'; buf[bi++] = 'a';
                buf[bi++] = 'w'; buf[bi++] = '='; buf[bi++] = '0';
                buf[bi++] = 'x';
                uint16_t raw = hb_brightness_get();
                static const char hex[] = "0123456789ABCDEF";
                for (int i = 3; i >= 0; i--)
                    buf[bi++] = hex[(raw >> (i*4)) & 0xF];
                buf[bi] = 0;
                redraw(buf, HB_GREEN);
            }
        }
        hb_ui_pace();
    }

    hb_ui_done();
    return (void *)0;
}

HB_APP_ENTRY(payload_entry)
{
    static uint32_t attr[16];
    for (int i = 0; i < 16; i++) attr[i] = 0;
    attr[0] = 0x50544841u;
    attr[2] = 2;
    attr[4] = 0x10000;
    attr[6] = 1;
    attr[7] = 1;
    attr[8] = 1;
    attr[9] = 0;
    uint32_t tid = 0;
    ((pthread_create_t)PTHREAD_CREATE_ADDR)(&tid, attr, app_main, (void *)0);
}
