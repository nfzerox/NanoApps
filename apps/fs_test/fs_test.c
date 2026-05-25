/*
 * fs_test — check a few known filesystem paths and show YES/NO.
 */

#include "hb_sdk.h"

#define BG     HB_BLACK
#define OK     HB_GREEN
#define ERR    HB_RED
#define TBD    HB_RGB(0x60, 0x60, 0x60)

static const char *paths[] = {
    "/",
    "/iPod_Control",
    "/iPod_Control/Device",
    "/iPod_Control/Music",
    "/no/such/dir",
    "/screenshot0000.bmp",
};
#define N_PATHS (sizeof(paths) / sizeof(paths[0]))

HB_APP_ENTRY(payload_entry)
{
    hb_fill_screen(BG);
    hb_draw_str(8, 0, "FS TEST", 2, HB_YELLOW, BG);

    for (uint32_t i = 0; i < N_PATHS; i++) {
        int16_t y = 30 + i * 32;
        hb_draw_str(8, y, paths[i], 1, HB_WHITE, BG);

        bool exists = hb_fs_exists(paths[i]);
        hb_draw_str(180, y, exists ? "YES" : "no", 1,
                    exists ? OK : ERR, BG);
    }

    /* Hold a while so user can read results, then either vol button exits. */
    for (uint32_t frame = 0; frame < 5000000; frame++) {
        if (hb_button_pressed(HB_BTN_VOL_UP) ||
            hb_button_pressed(HB_BTN_VOL_DOWN)) {
            return;
        }
        for (volatile uint32_t i = 0; i < 5000; i++) { }
    }
}
