/*
 * hb_ui.c — minimal touch-UI primitives for homebrew apps.
 *
 * Provides:
 *  - hb_ui_init / hb_ui_done : suppress OS-side touch dispatch so
 *    taps on our overlay don't also launch the apps underneath
 *    (Clock / Settings etc).
 *  - hb_ui_button_draw / hb_ui_button_hit : button rendering + hit test.
 *  - hb_ui_poll : poll touch with built-in debounce + vol-button exit.
 *
 * Convention (per memory: feedback-app-input-pattern):
 *   touch for in-app interaction, vol button for clean exit only.
 *
 * Must be used in FS mode (after tools/eject.sh) only if the app
 * wants to coexist with the home-screen OS UI. In DMIA mode the OS
 * doesn't dispatch taps so hb_ui_init's touch suppression is a no-op
 * but harmless.
 */

#include "hb_sdk.h"

#define G_TOUCH_AVAILABLE_ADDR 0x08909069u

static uint8_t s_saved_touch_avail;
static bool    s_initialized = false;

void hb_ui_init(void)
{
    volatile uint8_t *p = (volatile uint8_t *)G_TOUCH_AVAILABLE_ADDR;
    s_saved_touch_avail = *p;
    *p = 0;
    s_initialized = true;
}

void hb_ui_done(void)
{
    if (!s_initialized) return;
    volatile uint8_t *p = (volatile uint8_t *)G_TOUCH_AVAILABLE_ADDR;
    *p = s_saved_touch_avail;
    s_initialized = false;
}

static int16_t strlen_i16(const char *s)
{
    int16_t n = 0;
    while (*s++) n++;
    return n;
}

void hb_ui_button_draw(int16_t x, int16_t y, int16_t w, int16_t h,
                       const char *label, hb_color_t bg, hb_color_t fg)
{
    hb_fill_rect(x, y, w, h, bg);
    int16_t n = strlen_i16(label);
    /* Scale 2 -> each char is 16px wide. */
    int16_t lx = x + (w - 16 * n) / 2;
    int16_t ly = y + (h - 16) / 2;
    if (lx < x) lx = x;
    if (ly < y) ly = y;
    hb_draw_str(lx, ly, label, 2, fg, bg);
}

bool hb_ui_button_hit(int16_t tx, int16_t ty,
                      int16_t btn_x, int16_t btn_y,
                      int16_t btn_w, int16_t btn_h)
{
    return (tx >= btn_x && tx < btn_x + btn_w &&
            ty >= btn_y && ty < btn_y + btn_h);
}

/* Debounce + last-touch tracking. Single global instance —
   one app, one polling loop. */
static int16_t  s_prev_x = -9999, s_prev_y = -9999;
static uint32_t s_cooldown = 0;
static uint32_t s_home_exit_count = 0;

hb_ui_event_t hb_ui_poll(int16_t *out_x, int16_t *out_y)
{
    if (hb_screenshot_poll_chord()) {
        s_home_exit_count = 0;
        return HB_UI_NONE;
    }

    /* Exit gestures: vol = backwards-compat;
       home = primary.
       Home waits briefly so Home+Power can be recognized as the
       screenshot chord instead of immediately exiting the app. */
    if (hb_button_pressed(HB_BTN_VOL_UP)  ||
        hb_button_pressed(HB_BTN_VOL_DOWN)) {
        return HB_UI_EXIT;
    }
    if (hb_button_pressed(HB_BTN_HOME) &&
        hb_button_pressed(HB_BTN_POWER)) {
        s_home_exit_count = 0;
        return HB_UI_NONE;
    }
    if (hb_button_pressed(HB_BTN_HOME)) {
        s_home_exit_count++;
        if (s_home_exit_count > 500) return HB_UI_EXIT;
        return HB_UI_NONE;
    }
    s_home_exit_count = 0;

    hb_touch_drain_to_one();

    hb_touch_t t;
    bool got = hb_touch_get_coords(&t);
    bool changed = got && (t.x != s_prev_x || t.y != s_prev_y);

    if (s_cooldown > 0) s_cooldown--;

    if (changed && s_cooldown == 0) {
        s_prev_x = t.x;
        s_prev_y = t.y;
        /* ~300ms cooldown: prevents a held finger from firing
           the same tap N times back-to-back. */
        s_cooldown = 1500;
        if (out_x) *out_x = t.x;
        if (out_y) *out_y = t.y;
        return HB_UI_TAP;
    }
    return HB_UI_NONE;
}

void hb_ui_pace(void)
{
    /* Standard pace step. Apps call this once per loop iter. */
    for (volatile uint32_t i = 0; i < 3000; i++) { }
}
