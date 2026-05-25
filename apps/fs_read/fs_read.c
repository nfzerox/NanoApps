/*
 * fs_read — write a known string then read it back, in one session,
 * so we control both ends and can verify the round trip.
 *
 * Workflow:
 *   tools/eject.sh     (once, iPod -> home screen, FS mounted)
 *   deploy fs_read     (writes, then reads, then displays)
 *
 * Uses hb_fs_write + hb_fs_read SDK functions.
 */

#include "hb_sdk.h"

#define BG     HB_BLACK
#define OK     HB_GREEN
#define ERR    HB_RED

#define FILE_PATH "/iPod_Control/hb_rw.txt"
static const char k_payload[] = "HOMEBREW_RW_TEST_0123456789";

HB_APP_ENTRY(payload_entry)
{
    hb_fill_screen(BG);
    hb_draw_str(8, 0, "FS READ", 2, HB_YELLOW, BG);

    int16_t y = 40;

    /* Write known string. */
    uint32_t want = sizeof(k_payload) - 1;
    bool wrote = hb_fs_write(FILE_PATH, k_payload, want);
    hb_draw_str(8, y, "write:", 1, HB_WHITE, BG);
    hb_draw_str(64, y, wrote ? "OK" : "FAIL", 1,
                wrote ? OK : ERR, BG);
    hb_draw_uint(120, y, want, 4, HB_WHITE, BG);
    y += 16;

    /* Read it back. */
    char buf[64];
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    uint32_t n = hb_fs_read(FILE_PATH, buf, sizeof(buf) - 1);

    hb_draw_str(8, y, "bytes:", 1, HB_WHITE, BG);
    hb_draw_uint(64, y, n, 4, n == want ? OK : ERR, BG);
    y += 24;

    /* Display read content as a single line (replace non-printables
       with '.'). */
    if (n > 0) {
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        buf[n] = '\0';
        for (uint32_t i = 0; i < n; i++) {
            if ((uint8_t)buf[i] < 0x20 || (uint8_t)buf[i] >= 0x7F) {
                buf[i] = '.';
            }
        }
        hb_draw_str(8, y, "content:", 1, HB_WHITE, BG);
        y += 16;
        hb_draw_str(8, y, buf, 1, HB_CYAN, BG);
        y += 24;

        /* Compare to what we wrote. */
        bool match = (n == want);
        for (uint32_t i = 0; i < n && match; i++) {
            char expected = k_payload[i];
            if ((uint8_t)expected < 0x20 || (uint8_t)expected >= 0x7F) {
                expected = '.';
            }
            if (buf[i] != expected) match = false;
        }
        hb_draw_str(8, y, "match:", 1, HB_WHITE, BG);
        hb_draw_str(64, y, match ? "YES" : "no", 1,
                    match ? OK : ERR, BG);
    } else {
        hb_draw_str(8, y, "READ FAILED", 1, ERR, BG);
    }

    hb_draw_str(8, HB_SCREEN_H - 32, "VOL TO EXIT", 1, HB_WHITE, BG);
    for (uint32_t frame = 0; frame < 2000000; frame++) {
        if (hb_button_pressed(HB_BTN_VOL_UP) ||
            hb_button_pressed(HB_BTN_VOL_DOWN)) return;
        for (volatile uint32_t i = 0; i < 5000; i++) { }
    }
}
