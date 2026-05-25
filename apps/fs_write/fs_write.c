/*
 * fs_write — create a text file on the iPod using the SDK's hb_fs API.
 *
 */

#include "hb_sdk.h"

#define BG       HB_BLACK
#define OK       HB_GREEN
#define ERR      HB_RED

#define FILE_PATH "/iPod_Control/homebrew_hello.txt"
static const char k_message[] = "hello from n7g homebrew filesystem!\n";

HB_APP_ENTRY(payload_entry)
{
    hb_fill_screen(BG);
    hb_draw_str(8, 0, "FS WRITE", 2, HB_YELLOW, BG);

    int16_t y = 50;

    bool wrote = hb_fs_write(FILE_PATH, k_message, sizeof(k_message) - 1);
    hb_draw_str(8, y, "write:", 1, HB_WHITE, BG);
    hb_draw_str(56, y, wrote ? "OK" : "FAIL", 1,
                wrote ? OK : ERR, BG);
    y += 20;

    bool exists = hb_fs_exists(FILE_PATH);
    hb_draw_str(8, y, "exists:", 1, HB_WHITE, BG);
    hb_draw_str(64, y, exists ? "YES" : "no", 1,
                exists ? OK : ERR, BG);
    y += 20;

    hb_draw_str(8, HB_SCREEN_H - 32, "VOL TO EXIT", 1, HB_WHITE, BG);
    for (uint32_t frame = 0; frame < 2000000; frame++) {
        if (hb_button_pressed(HB_BTN_VOL_UP) ||
            hb_button_pressed(HB_BTN_VOL_DOWN)) return;
        for (volatile uint32_t i = 0; i < 5000; i++) { }
    }
}
