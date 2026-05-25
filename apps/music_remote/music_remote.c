/*
 * music_remote — touchscreen remote for OS music playback.
 *
 * Uses the media helpers. Queue music in the stock Music app
 * first, then launch this app to control the active player.
 *
 * Buttons:
 *   PLAY, PAUSE, TOGGLE, PREV, NEXT, NOWPLAY
 */

#include "hb_sdk.h"

typedef struct {
    int16_t x, y, w, h;
    hb_color_t bg;
    const char *label;
    uint8_t action;
} btn_t;

enum {
    ACT_PLAY = 1,
    ACT_PAUSE,
    ACT_TOGGLE,
    ACT_PREV,
    ACT_NEXT,
    ACT_NOWPLAY,
};

static void draw_btn(const btn_t *b, int pressed)
{
    hb_color_t bg = pressed ? HB_WHITE : b->bg;
    hb_color_t fg = pressed ? HB_BLACK : HB_WHITE;
    hb_fill_rect(b->x, b->y, b->w, b->h, bg);
    int tw = hb_helvetica_width(b->label);
    int tx = b->x + (b->w - tw) / 2;
    int ty = b->y + (b->h - hb_helvetica_line_height()) / 2;
    hb_helvetica_draw(tx, ty, b->label, fg, bg);
}

static int hit(const btn_t *b, int16_t x, int16_t y)
{
    return (x >= b->x && x < b->x + b->w &&
            y >= b->y && y < b->y + b->h);
}

static void draw_status(void)
{
    hb_fill_rect(0, 374, HB_SCREEN_W, 34, HB_BLACK);
    int state = hb_media_state();
    const char *msg = "state ?";
    if (state == 0) msg = "playing";
    else if (state == 1) msg = "paused";
    else if (state == 2) msg = "stopped";
    hb_helvetica_draw(8, 382, msg, HB_YELLOW, HB_BLACK);
}

static void run_action(uint8_t action)
{
    if (action == ACT_PLAY) {
        hb_media_set_paused(false);
    } else if (action == ACT_PAUSE) {
        hb_media_set_paused(true);
    } else if (action == ACT_TOGGLE) {
        hb_media_set_paused(hb_media_state() == 0);
    } else if (action == ACT_PREV) {
        hb_media_prev();
    } else if (action == ACT_NEXT) {
        hb_media_next();
    } else if (action == ACT_NOWPLAY) {
        hb_media_show_now_playing();
    }
}

HB_APP_ENTRY(payload_entry)
{
    hb_ui_init();
    hb_fill_screen(HB_BLACK);
    hb_helvetica_draw(4, 6, "music remote", HB_CYAN, HB_BLACK);

    btn_t btns[] = {
        { 8,   40, 110, 56, HB_RGB(0x20,0x60,0x20), "PLAY",    ACT_PLAY },
        { 122, 40, 110, 56, HB_RGB(0x60,0x40,0x20), "PAUSE",   ACT_PAUSE },
        { 8,  104, 224, 56, HB_RGB(0x20,0x40,0x70), "TOGGLE",  ACT_TOGGLE },
        { 8,  168, 110, 56, HB_RGB(0x40,0x40,0x60), "PREV",    ACT_PREV },
        { 122,168, 110, 56, HB_RGB(0x40,0x40,0x60), "NEXT",    ACT_NEXT },
        { 8,  232, 224, 56, HB_RGB(0x20,0x30,0x70), "NOWPLAY", ACT_NOWPLAY },
    };
    int nb = (int)(sizeof(btns)/sizeof(btns[0]));
    for (int i = 0; i < nb; i++) draw_btn(&btns[i], 0);
    draw_status();

    int last_pressed = -1;

    for (uint32_t frame = 0; frame < 9000000; frame++) {
        int16_t tx = 0, ty = 0;
        hb_ui_event_t e = hb_ui_poll(&tx, &ty);
        if (e == HB_UI_EXIT) break;
        if (e == HB_UI_TAP) {
            int now_pressed = -1;
            for (int i = 0; i < nb; i++) {
                if (hit(&btns[i], tx, ty)) {
                    now_pressed = i;
                    break;
                }
            }

            if (now_pressed >= 0) draw_btn(&btns[now_pressed], 1);
            if (last_pressed >= 0 && last_pressed != now_pressed) {
                draw_btn(&btns[last_pressed], 0);
            }
            last_pressed = now_pressed;
            if (now_pressed >= 0) {
                run_action(btns[now_pressed].action);
                draw_status();
                for (volatile uint32_t i = 0; i < 120000; i++) { }
                draw_btn(&btns[now_pressed], 0);
                last_pressed = -1;
            }
        }

        hb_ui_pace();
    }
    hb_ui_done();
}
