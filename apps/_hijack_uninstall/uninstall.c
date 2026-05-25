/*
 * uninstall.c — restore original handler from /Apps/.hijack_backup.
 *
 * Reverses what _hijack_install did. Reboot also reverses it (RAM
 * patches don't persist), so this exists mainly for in-session
 * iteration / testing without rebooting.
 */

#include "hb_sdk.h"

#define HANDLE_PODCASTS_ADDR  0x0844032cu
#define BACKUP_PATH           "/Apps/.hijack_backup"
#define BG                    HB_RGB(0x00, 0x00, 0x40)
#define OK                    HB_GREEN
#define ERR                   HB_RED

HB_APP_ENTRY(payload_entry)
{
    hb_fill_screen(BG);
    hb_draw_str(8, 4, "HIJACK UNDO", 2, HB_YELLOW, BG);

    int16_t y = 50;
    uint32_t sz = hb_fs_size(BACKUP_PATH);
    if (sz != 12) {
        hb_draw_str(8, y, "backup missing", 2, ERR, BG);
        goto wait;
    }

    uint8_t orig[12];
    if (hb_fs_read(BACKUP_PATH, orig, 12) != 12) {
        hb_draw_str(8, y, "read FAIL", 2, ERR, BG);
        goto wait;
    }

    volatile uint8_t *target = (volatile uint8_t *)HANDLE_PODCASTS_ADDR;
    for (int i = 0; i < 12; i++) target[i] = orig[i];

    /* I-cache nudge */
    {
        volatile uint32_t sum = 0;
        volatile uint32_t *flush = (volatile uint32_t *)HANDLE_PODCASTS_ADDR;
        for (int i = 0; i < 64; i++) sum += flush[i];
        (void)sum;
    }

    hb_draw_str(8, y, "RESTORED", 2, OK, BG);
    y += 30;
    hb_draw_str(8, y, "Podcasts works again", 1, HB_WHITE, BG);

wait:
    hb_draw_str(8, HB_SCREEN_H - 20, "vol exits", 1, HB_WHITE, BG);
    for (uint32_t f = 0; f < 5000000; f++) {
        if (hb_button_pressed(HB_BTN_VOL_UP) ||
            hb_button_pressed(HB_BTN_VOL_DOWN)) return;
        for (volatile uint32_t i = 0; i < 1000; i++) { }
    }
}
