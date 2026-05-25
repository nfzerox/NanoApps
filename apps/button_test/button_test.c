/*
 * button_test — show live state of each physical N7G button.
 *
 * Each row is a button. The label is white when idle, green when held.
 * Press either volume button to exit.
 */

#include "hb_sdk.h"

#define BG_COLOR    HB_BLACK
#define IDLE_COLOR  HB_WHITE
#define PRESS_COLOR HB_GREEN
#define TBD_COLOR   HB_RGB(0x60, 0x60, 0x60)

#define LABEL_SCALE 3       /* 24x24 glyphs */
#define LABEL_H     (8 * LABEL_SCALE)
#define ROW_H       48
#define LABEL_X     12

typedef struct {
    const char *label;
    int         is_wired;   /* 0 = no I2C SDK yet, 1 = readable */
    hb_button_t btn_id;     /* meaningful when is_wired = 1 */
} btn_row_t;

static const btn_row_t rows[] = {
    { "VOL UP",     1, HB_BTN_VOL_UP     },
    { "VOL DOWN",   1, HB_BTN_VOL_DOWN   },
    { "HOME",       1, HB_BTN_HOME       },
    { "POWER",      1, HB_BTN_POWER      },
    { "PLAY/PAUSE", 1, HB_BTN_PLAY_PAUSE },
};
#define N_ROWS (sizeof(rows) / sizeof(rows[0]))

static void draw_row(int row, bool pressed)
{
    int16_t y = 16 + row * ROW_H;
    hb_color_t fg;
    if (!rows[row].is_wired) fg = TBD_COLOR;
    else if (pressed)        fg = PRESS_COLOR;
    else                     fg = IDLE_COLOR;

    /* Wipe the row's label area, then redraw text. */
    hb_fill_rect(0, y, HB_SCREEN_W, LABEL_H, BG_COLOR);
    hb_draw_str(LABEL_X, y, rows[row].label, LABEL_SCALE, fg, BG_COLOR);
}

HB_APP_ENTRY(payload_entry)
{
    hb_fill_screen(BG_COLOR);

    /* Header. */
    hb_draw_str(8, 0, "BUTTON TEST", 2, HB_YELLOW, BG_COLOR);

    /* Initial draw of all rows. */
    bool last_pressed[N_ROWS] = { 0 };
    for (uint32_t i = 0; i < N_ROWS; i++) {
        draw_row(i, false);
    }

    /* Hint at the bottom. */
    hb_draw_str(8, HB_SCREEN_H - 32, "BOTH VOL = EXIT", 2,
                HB_RGB(0x80, 0x80, 0x80), BG_COLOR);

    uint32_t both_held_frames = 0;

    for (uint32_t frame = 0; frame < 20000000; frame++) {
        bool vu = hb_button_pressed(HB_BTN_VOL_UP);
        bool vd = hb_button_pressed(HB_BTN_VOL_DOWN);
        bool ho = hb_button_pressed(HB_BTN_HOME);
        bool po = hb_button_pressed(HB_BTN_POWER);
        bool pp = hb_button_pressed(HB_BTN_PLAY_PAUSE);

        bool cur[N_ROWS] = { vu, vd, ho, po, pp };

        /* Only redraw rows whose state changed (text rendering is
           expensive enough that constant redraw flickers). */
        for (uint32_t i = 0; i < N_ROWS; i++) {
            if (!rows[i].is_wired) continue;
            if (cur[i] != last_pressed[i]) {
                draw_row(i, cur[i]);
                last_pressed[i] = cur[i];
            }
        }

        /* Exit only when BOTH volume buttons are held for ~50ms,
           so individual button tests don't quit the app early. */
        if (vu && vd) {
            if (++both_held_frames > 200) return;
        } else {
            both_held_frames = 0;
        }

        /* Pace */
        for (volatile uint32_t i = 0; i < 5000; i++) { }
    }
}
