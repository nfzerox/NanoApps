/*
 * fs_gui — tap-driven FS test using the hb_ui SDK.
 *   tap WRITE button -> write a counter-stamped payload
 *   tap READ  button -> read it back, display content
 *   either vol button -> exit
 *
 * Deploy after `tools/eject.sh` so the filesystem is mounted.
 */

#include "hb_sdk.h"

#define BG       HB_BLACK
#define OK       HB_GREEN
#define ERR      HB_RED
#define BTN_BG   HB_RGB(0x40, 0x00, 0x80)
#define BTN_FG   HB_WHITE

#define BTN_X    10
#define BTN_W    220
#define BTN_H    60
#define WRITE_Y  60
#define READ_Y   140

#define STATUS_X       8
#define STATUS_Y       220
#define STATUS_W       (HB_SCREEN_W - 16)
#define STATUS_H       80
#define STATUS_LINE_H  16

#define FILE_PATH "/iPod_Control/hb_gui.txt"

static void status(const char *label, hb_color_t label_fg, const char *body)
{
    hb_fill_rect(STATUS_X - 2, STATUS_Y - 2, STATUS_W + 4, STATUS_H + 4, BG);
    hb_draw_str(STATUS_X, STATUS_Y, label, 1, label_fg, BG);
    if (body) {
        hb_draw_str(STATUS_X, STATUS_Y + STATUS_LINE_H, body, 1, HB_CYAN, BG);
    }
}

/* 6-digit decimal subtract-based itoa (no libgcc divmod). */
static void itoa6(char *dst, uint32_t v)
{
    static const uint32_t pow10[6] = { 100000, 10000, 1000, 100, 10, 1 };
    for (int i = 0; i < 6; i++) {
        char d = '0';
        while (v >= pow10[i]) { v -= pow10[i]; d++; }
        dst[i] = d;
    }
    dst[6] = '\0';
}

HB_APP_ENTRY(payload_entry)
{
    hb_ui_init();

    hb_fill_screen(BG);
    hb_draw_str(8, 0, "FS GUI", 2, HB_YELLOW, BG);
    hb_ui_button_draw(BTN_X, WRITE_Y, BTN_W, BTN_H, "WRITE", BTN_BG, BTN_FG);
    hb_ui_button_draw(BTN_X, READ_Y,  BTN_W, BTN_H, "READ",  BTN_BG, BTN_FG);
    status("tap a button", HB_WHITE, 0);

    uint32_t write_counter = 0;

    for (uint32_t frame = 0; frame < 5000000; frame++) {
        int16_t tx, ty;
        hb_ui_event_t e = hb_ui_poll(&tx, &ty);
        if (e == HB_UI_VOL_EXIT) break;

        if (e == HB_UI_TAP) {
            if (hb_ui_button_hit(tx, ty, BTN_X, WRITE_Y, BTN_W, BTN_H)) {
                char payload[32];
                const char *prefix = "homebrew_write_";
                int idx = 0;
                while (prefix[idx]) { payload[idx] = prefix[idx]; idx++; }
                itoa6(&payload[idx], write_counter);
                idx += 6;
                payload[idx++] = '\n';
                payload[idx] = '\0';

                bool ok = hb_fs_write(FILE_PATH, payload, idx);
                if (ok) {
                    payload[idx - 1] = '\0'; /* drop trailing \n */
                    status("WRITE OK", OK, payload);
                    write_counter++;
                } else {
                    status("WRITE FAIL", ERR, 0);
                }
            } else if (hb_ui_button_hit(tx, ty, BTN_X, READ_Y, BTN_W, BTN_H)) {
                static char rbuf[64];
                for (uint32_t i = 0; i < sizeof(rbuf); i++) rbuf[i] = 0;
                uint32_t n = hb_fs_read(FILE_PATH, rbuf, sizeof(rbuf) - 1);
                if (n == 0) {
                    status("READ FAIL (no file?)", ERR, 0);
                } else {
                    if (n >= sizeof(rbuf)) n = sizeof(rbuf) - 1;
                    rbuf[n] = '\0';
                    for (uint32_t i = 0; i < n; i++) {
                        if ((uint8_t)rbuf[i] < 0x20 ||
                            (uint8_t)rbuf[i] >= 0x7F) rbuf[i] = '.';
                    }
                    status("READ OK", OK, rbuf);
                }
            }
        }

        hb_ui_pace();
    }

    hb_ui_done();
}
