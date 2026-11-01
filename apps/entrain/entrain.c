/*
 * entrain.c — the iPod entry point.
 *
 * Deliberately thin. Every screen lives in ui.c, which the host build compiles
 * unchanged; if the widgets lived here, the desktop target could only ever
 * approximate the layout, and then it would not be worth having.
 *
 * This file is the device's half of the contract: bring the UI up, keep the
 * screen awake, map the physical buttons, and make sure audio never outlives
 * the app.
 */

#include "hb_sdk.h"
#include "hb_lv_surface.h"
#include "lvgl/lvgl.h"

#include "ui.h"
#include "platform/sys.h"

/* Physical buttons are polled rather than delivered, so they need their own
   edge detection. Home doubles as "leave" — the resident owns the actual
   teardown, but we get to hear about the press first and stop the audio. */
typedef struct {
    hb_button_t btn;
    en_key_t    key;
    bool        was_down;
} btn_map_t;

static btn_map_t s_buttons[] = {
    { HB_BTN_VOL_UP,     EN_KEY_VOL_UP,     false },
    { HB_BTN_VOL_DOWN,   EN_KEY_VOL_DOWN,   false },
    { HB_BTN_PLAY_PAUSE, EN_KEY_PLAY_PAUSE, false },
    { HB_BTN_HOME,       EN_KEY_BACK,       false },
};

#define N_BUTTONS ((int)(sizeof s_buttons / sizeof s_buttons[0]))

static bool s_shut_down;

/* RetailOS acts on the play/pause button itself, before any homebrew app sees
   it: pressing play while Entrain is open starts the Music app underneath us.
   We cannot stop the OS from handling the button, but we can undo what it did.
   hb_media talks to the same system player, so if it has started, pause it.
   Entrain owns the audio output while it is on screen — two things playing at
   once is never what the user meant. */
static uint32_t s_media_check_ms;

static void suppress_os_media(void)
{
    uint32_t now = hb_time_uptime_ms();
    if (now - s_media_check_ms < 250) return;    /* 4 Hz is plenty */
    s_media_check_ms = now;

    if (hb_media_state() == 0)                   /* 0 = playing */
        hb_media_set_paused(true);
}

static void poll_buttons(void)
{
    for (int i = 0; i < N_BUTTONS; i++) {
        bool down = hb_button_pressed(s_buttons[i].btn);
        if (down && !s_buttons[i].was_down)
            en_ui_key(s_buttons[i].key);
        s_buttons[i].was_down = down;
    }
}

/* The runtime calls this once per OS timer tick, before lv_timer_handler. */
static void frame(void)
{
    if (s_shut_down) return;

    poll_buttons();
    suppress_os_media();
    en_ui_tick();

    /* Home from the Library asks to leave. The resident performs the actual
       exit; our job is to be silent before it does, because a loop still
       playing after the app is gone is the worst thing this app could do. */
    if (en_sys_exit_requested()) {
        en_ui_shutdown();
        s_shut_down = true;
    }
}

HB_APP_ENTRY(payload_entry)
{
    /* Programs run for three quarters of an hour with no touches. Without
       this the OS would dim and sleep partway through every one of them. */
    hb_wake_lock(true);

    en_ui_init();
    hb_lv_set_frame_cb(frame);
}
