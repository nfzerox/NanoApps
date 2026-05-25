/*
 * fs_demo — check a few common filesystem paths using the SDK wrapper.
 *
 * Deploy AFTER running `tools/eject.sh` once (iPod on home screen,
 * FS mounted).
 */

#include "hb_sdk.h"

#define BG    HB_BLACK
#define OK    HB_GREEN
#define ERR   HB_RED

static const char *const PATHS[] = {
    "/iPod_Control",
    "/iPod_Control/Device",
    "/iPod_Control/Device/Preferences",
    "/iPod_Control/Device/GrapeFirmware.bin",
    "/iPod_Control/iTunes",
    "/iPod_Control/Speakable",
    "/iPod_Control/Logs",
    "/iPod_Control/Tones",
    "/DCIM",
    "/Recordings",
    "/no/such/path",
};
#define N_PATHS (sizeof(PATHS) / sizeof(PATHS[0]))

HB_APP_ENTRY(payload_entry)
{
    hb_fill_screen(BG);
    hb_draw_str(8, 0, "FS DEMO", 2, HB_YELLOW, BG);

    for (uint32_t i = 0; i < N_PATHS; i++) {
        int16_t y = 30 + i * 24;
        hb_draw_str(4, y, PATHS[i], 1, HB_RGB(0x80, 0x80, 0x80), BG);
        hb_draw_str(190, y, "...", 1, HB_RGB(0x80, 0x80, 0x80), BG);
    }

    for (uint32_t i = 0; i < N_PATHS; i++) {
        bool exists = hb_fs_exists(PATHS[i]);
        int16_t y = 30 + i * 24;
        hb_fill_rect(186, y, HB_SCREEN_W - 186, 16, BG);
        hb_draw_str(190, y, exists ? "YES" : "no", 1,
                    exists ? OK : ERR, BG);
    }

    /* Done. Linger so user can read, vol-up exits. */
    hb_draw_str(8, HB_SCREEN_H - 32, "DONE - VOL TO EXIT", 1, HB_WHITE, BG);
    for (uint32_t frame = 0; frame < 1000000; frame++) {
        if (hb_button_pressed(HB_BTN_VOL_UP) ||
            hb_button_pressed(HB_BTN_VOL_DOWN)) return;
        for (volatile uint32_t i = 0; i < 5000; i++) { }
    }
}
