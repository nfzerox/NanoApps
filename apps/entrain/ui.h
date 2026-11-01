/*
 * ui.h — the shared LVGL interface.
 *
 * This file is identical on both targets. entrain.c brings it up on the iPod,
 * host/main_host.c brings it up on a 240x432 desktop window, and neither one
 * contains a single widget: if the UI lived in the device entry point, the
 * host build could only ever approximate it, and the point of having a host
 * build is that the layout you tune there is the layout the device renders.
 */

#ifndef ENTRAIN_UI_H
#define ENTRAIN_UI_H

#include <stdint.h>
#include <stdbool.h>

/* The device's panel. The host window is forced to match. */
#define EN_SCREEN_W 240
#define EN_SCREEN_H 432

/* Build every screen and show the right one. Call after LVGL is initialised
   and a display exists. */
void en_ui_init(void);

/* Once per frame, before lv_timer_handler. Drives the engine, the readouts,
   the ring pulse and the screen-blank timer. */
void en_ui_tick(void);

/* Physical buttons, mapped by the platform. */
typedef enum {
    EN_KEY_VOL_UP = 0,
    EN_KEY_VOL_DOWN,
    EN_KEY_PLAY_PAUSE,
    EN_KEY_BACK,         /* home / escape */
    /* Start the next program and show it. Exists so a device with no working
       touchscreen can still get at more than whatever launched first; the
       N31 Linux port maps its home button to this. */
    EN_KEY_NEXT_PROGRAM
} en_key_t;

void en_ui_key(en_key_t key);

/* Tear down cleanly: stop audio, save preferences. */
void en_ui_shutdown(void);

/* Screens, exposed so the host screenshot mode can walk through them. */
typedef enum {
    EN_SCREEN_LIBRARY = 0,
    EN_SCREEN_NOW,
    EN_SCREEN_TUNE,
    EN_SCREEN_TIMER,
    EN_SCREEN_SETTINGS,
    EN_SCREEN_FIRSTRUN,
    EN_SCREEN_COUNT
} en_screen_t;

void        en_ui_goto(en_screen_t screen, bool animate);
en_screen_t en_ui_current(void);
const char *en_ui_screen_name(en_screen_t screen);

/* Force the first-run headphone notice on or off, for screenshots and tests. */
void en_ui_set_first_run(bool first_run);

/* Open one of the Library's shelves: 0 Presets, 1 Programs, 2 Custom,
   3 Suites, or -1 for the root list. The first three numbers are what they
   were when these were tabs, which is why Suites is 3 rather than 2.

   Exists because the N31 Linux port has no working touchscreen yet, so there
   is otherwise no way to reach a shelf to look at it - and because the
   screenshot mode has to walk them. */
void en_ui_set_tab(int tab);

/* Suppress the blank-while-playing timer. Useful while bringing a device up:
   a screen that goes dark after thirty seconds is indistinguishable from an
   app that has crashed, and on a port with no working touchscreen there may be
   nothing able to wake it again. */
void en_ui_set_blanking(bool enabled);

#endif /* ENTRAIN_UI_H */
