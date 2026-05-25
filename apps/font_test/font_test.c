/*
 * font_test — compare default 8x8 bitmap font vs bundled Helvetica
 * bitmap font (rendered from system Helvetica.ttf by sdk/tools).
 *
 * Whole app in 64 KB pthread so we have room for the font bitmap data.
 */

#include "hb_sdk.h"

#define PTHREAD_CREATE_ADDR (0x080226f8u | 1u)
typedef int (*pthread_create_t)(uint32_t *thread, void *attr,
                                void *(*start)(void *), void *arg);

static void *app_main(void *arg)
{
    (void)arg;
    hb_ui_init();
    hb_fill_screen(HB_BLACK);

    /* Top: default 8x8 font, scale 1 + 2 */
    hb_draw_str(4, 4,  "Default 8x8 font", 1, HB_YELLOW, HB_BLACK);
    hb_draw_str(4, 18, "Scale 2 ABC abc 123", 2, HB_YELLOW, HB_BLACK);

    /* Below: Helvetica via bundled bitmap */
    int y = 60;
    hb_helvetica_draw(4, y, "Helvetica bundled bitmap",
                      HB_CYAN, HB_BLACK);
    y += hb_helvetica_line_height() + 4;

    hb_helvetica_draw(4, y, "The quick brown fox", HB_WHITE, HB_BLACK);
    y += hb_helvetica_line_height() + 4;

    hb_helvetica_draw(4, y, "jumps over the lazy dog!",
                      HB_WHITE, HB_BLACK);
    y += hb_helvetica_line_height() + 4;

    hb_helvetica_draw(4, y, "0123456789 ABCDEF",
                      HB_GREEN, HB_BLACK);
    y += hb_helvetica_line_height() + 4;

    hb_helvetica_draw(4, y, "iPod nano 7 (N7G)",
                      HB_MAGENTA, HB_BLACK);
    y += hb_helvetica_line_height() + 10;

    /* Mixed line */
    int16_t pen = 4;
    pen = hb_helvetica_draw(pen, y, "Mixed: ", HB_WHITE, HB_BLACK);
    pen = hb_helvetica_draw(pen, y, "RED ",    HB_RED,   HB_BLACK);
    pen = hb_helvetica_draw(pen, y, "GREEN ",  HB_GREEN, HB_BLACK);
    pen = hb_helvetica_draw(pen, y, "BLUE",    HB_BLUE,  HB_BLACK);

    hb_draw_str(4, HB_SCREEN_H - 14, "HOME / VOL = exit", 1,
                HB_RGB(0x80,0x80,0x80), HB_BLACK);

    /* Idle until exit gesture */
    for (uint32_t frame = 0; frame < 20000000; frame++) {
        int16_t tx, ty;
        hb_ui_event_t e = hb_ui_poll(&tx, &ty);
        if (e == HB_UI_EXIT) break;
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
