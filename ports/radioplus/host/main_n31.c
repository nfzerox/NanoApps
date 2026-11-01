/*
 * main_n31.c — Radio+ on the N31 Linux port.
 *
 * Framebuffer out, evdev in, and a loop. Everything interesting is elsewhere:
 * the driver owns the tuner, a thread owns the capture, and the screens read a
 * model that this file never touches.
 *
 * Two things it does own, because nothing else can:
 *
 *   Shutdown. The capture thread holds a PCM device and the tuner holds power,
 *   and neither is released by exiting. A radio left drawing current after the
 *   app is gone is a real fault on a battery device, so the signal handlers
 *   are wired before anything is opened.
 *
 *   Pacing. LVGL is asked to run at a steady rate rather than as fast as it
 *   can. There is nothing on these screens that moves faster than a few times
 *   a second, and spinning would cost battery to redraw a frequency that has
 *   not changed.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "../ui.h"
#include "../model.h"
#include "../platform/tuner.h"
#include "../platform/capture.h"
#include "../platform/player.h"
#include "display.h"
#include "touch.h"

#define FRAME_MS 33          /* about 30 Hz, which is more than enough */

static volatile sig_atomic_t s_quit;

static uint32_t rp_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

static uint32_t millis(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + (uint32_t)(t.tv_nsec / 1000000));
}

static void on_signal(int sig)
{
    (void)sig;
    s_quit = 1;
}

/*
 * Buttons.
 *
 * There is no touchscreen on this device - /proc/bus/input/devices lists
 * n31-buttons, an accelerometer and n31-pmic-buttons and nothing else - so an
 * interface that can only be tapped cannot be driven at all.
 *
 * Read straight from evdev rather than through an LVGL keypad group: the
 * screens are positioned rather than laid out in a focus order, and inventing
 * one purely so a generic focus mechanism has something to walk would be more
 * machinery than the four actions actually wanted.
 */
#define KEY_VOLUMEDOWN   114
#define KEY_VOLUMEUP     115
#define KEY_MENU         139
#define KEY_HOMEPAGE     172
#define KEY_BACK         158
#define KEY_NEXTSONG     163
#define KEY_PLAYPAUSE    164
#define KEY_PREVIOUSSONG 165

#define MAX_KEY_FDS 4
static int s_key_fd[MAX_KEY_FDS];
static int s_key_fds;

/* struct input_event, without pulling in linux/input.h - it collides with some
   libc headers on this toolchain. Layout is time, type, code, value. */
struct rp_input_event {
    long     sec;
    long     usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

static void open_keys(void)
{
    for (int i = 0; i < 12 && s_key_fds < MAX_KEY_FDS; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/event%d", i);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        /* Ask which EVENT TYPES the device reports, not which key codes.
           EVIOCGBIT(EV_KEY) returns the key bitmap, and reading only its first
           word tests keys 0 to 31 - which these devices do not have. Their keys
           are volume and transport, at 114 and above, so a device full of
           exactly the keys wanted looked like a device with none. */
        unsigned long types = 0;
        int req = (int)(0x80000000u | ((sizeof types) << 16) | ('E' << 8)
                        | 0x20);              /* EVIOCGBIT(0, ...) */
        if (ioctl(fd, req, &types) >= 0 && (types & (1u << 1))) {   /* EV_KEY */
            s_key_fd[s_key_fds++] = fd;
            printf("radioplus: keys on %s\n", path);
            continue;
        }
        close(fd);
    }
    if (!s_key_fds) printf("radioplus: no key input found\n");
}

/* One step along the swipe sequence from wherever we are, wrapping, so one
   button reaches every screen rather than stopping at the end. A screen that
   is not in the sequence - the register editors - starts from the beginning. */
static int swipe_step(int delta)
{
    const int n = rp_ui_swipe_count();
    if (n <= 0) return 0;

    int at = 0;
    for (int i = 0; i < n; i++) {
        if (rp_ui_swipe_at(i) == rp_ui_current()) { at = i; break; }
    }
    at = (at + delta) % n;
    if (at < 0) at += n;
    return at;
}

static void pump_keys(void)
{
    struct rp_input_event ev;

    for (int i = 0; i < s_key_fds; i++) {
        while (read(s_key_fd[i], &ev, sizeof ev) == (ssize_t)sizeof ev) {
            if (ev.type != 1) continue;                  /* EV_KEY only */

            /*
             * Presses, and repeats for the volume alone.
             *
             * evdev sends 1 for a press and 2 for each autorepeat. Every other
             * key here does something a repeat should not do twenty times a
             * second - paging through screens, starting a recording - so they
             * take presses only. Volume is the one control where holding the
             * button down and watching the level move is the expected way to
             * use it.
             */
            {
                bool vol = (ev.code == KEY_VOLUMEUP
                            || ev.code == KEY_VOLUMEDOWN);
                if (ev.value != 1 && !(vol && ev.value == 2)) continue;
            }

            switch (ev.code) {
            /*
             * The two buttons on this device that are labelled for a job.
             *
             * They used to page through the screens, because until now there
             * was nothing on this hardware for them to turn down: the tuner
             * has no volume register, and the codec's own was not reachable
             * from here. There is one now - a softvol per output, applied in
             * the plugin the player writes to - so they do what they say.
             *
             * Paging is still on the transport keys beside them, and on the
             * swipe, so nothing has been taken away.
             */
            case KEY_VOLUMEUP:
                rp_act_nudge_volume(+4);
                break;
            case KEY_VOLUMEDOWN:
                rp_act_nudge_volume(-4);
                break;

            /*
             * Step through the swipe sequence, not through the enum.
             *
             * These used to add one to rp_ui_current() and wrap at a fixed
             * count. That stopped being the same thing when two screens became
             * optional: the sequence is built at run time now, so walking the
             * enum would land on a screen that is turned off and skip the
             * dots' idea of where you are.
             */
            case KEY_NEXTSONG:
                rp_ui_show(rp_ui_swipe_at(swipe_step(+1)));
                break;
            case KEY_PREVIOUSSONG:
                rp_ui_show(rp_ui_swipe_at(swipe_step(-1)));
                break;
            case KEY_PLAYPAUSE:
                /* Exactly what the middle transport button does, so the key and
                   the screen never disagree about what it means. */
                if (rp_model.play_file || rp_model.behind_ms)
                    rp_act_pause_toggle();
                else
                    rp_act_record_toggle();
                break;
            case KEY_HOMEPAGE:
                /*
                 * HOME leaves, the way it does in every other app here.
                 *
                 * It was not handled at all, so there was no way out of Radio+
                 * except killing it from a console - and the launcher
                 * deliberately does not take HOME away from an app that says
                 * it owns its own keys. Back is still Back, on its own button.
                 */
                s_quit = 1;
                break;
            case KEY_MENU:
            case KEY_BACK:
                rp_ui_show(RP_SCREEN_NOW);
                break;
            default:
                break;
            }
        }
    }
}

#if LV_USE_EVDEV
static lv_display_t *s_touch_disp;
static lv_indev_t   *s_touch_indev;
static const char   *s_touch_want;      /* --input, when one was given */
static uint32_t      s_touch_next_try;
static uint32_t      s_touch_since;

#define TOUCH_RETRY_MS   1000u
#define TOUCH_GIVE_UP_MS 60000u
#endif

/*
 * The touch panel, through the search every other app here uses.
 *
 * This had its own copy, and the copy was weaker in the way that matters on
 * this device: it looked at event0 through event11 and matched on the name
 * alone. The launcher's looks at event0 through event31 and asks each device
 * what axes it reports before believing its name.
 *
 * That difference is not academic. The panel's node does not survive an app
 * here - it goes away and comes back, and it does not have to come back with
 * the same number - so a search that stops at eleven finds nothing as soon as
 * the numbering has drifted past it, which is exactly the state Radio+ starts
 * in when it is launched after another app. Touch worked on a fresh boot and
 * not afterwards, and the swipe between pages went with it, because a gesture
 * needs a pointer like everything else does.
 *
 * One implementation, one behaviour. n31_touch_find is in the launcher's
 * touch.c, compiled in here the way display.c already is.
 */
static const char *find_touch(void)
{
    return n31_touch_find();
}

#if LV_USE_EVDEV
/*
 * Attach the panel if it is there, and keep asking if it is not.
 *
 * Cheap - a handful of opens and an ioctl each - and it runs once a second
 * for the first minute only. After that a device with no panel stops paying
 * for one.
 */
static void touch_retry(void)
{
    const char *tp;

    if (s_touch_indev || !s_touch_disp)
        return;

    tp = s_touch_want ? s_touch_want : find_touch();
    if (!tp) {
        s_touch_next_try = rp_now_ms() + TOUCH_RETRY_MS;
        return;
    }

    s_touch_indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, tp);
    if (!s_touch_indev) {
        /* Named on the command line and unopenable is a mistake worth
           reporting; found by searching and unopenable is worth another go. */
        if (s_touch_want) {
            printf("radioplus: %s would not open as a pointer\n", tp);
            s_touch_want = NULL;
        }
        s_touch_next_try = rp_now_ms() + TOUCH_RETRY_MS;
        return;
    }

    lv_indev_set_display(s_touch_indev, s_touch_disp);
    s_touch_next_try = 0;
    printf("radioplus: touch on %s\n", tp);
    fflush(stdout);
}

/* Called from the frame loop; does nothing once the panel is in hand. */
static void touch_tick(void)
{
    uint32_t now;

    if (s_touch_indev || !s_touch_next_try)
        return;
    now = rp_now_ms();
    if (now < s_touch_next_try)
        return;
    if (!s_touch_since)
        s_touch_since = now;
    if (now - s_touch_since > TOUCH_GIVE_UP_MS) {
        s_touch_next_try = 0;
        printf("radioplus: no touch panel; buttons only\n");
        fflush(stdout);
        return;
    }
    touch_retry();
}
#else
static void touch_retry(void) {}
static void touch_tick(void) {}
#endif

static void usage(const char *me)
{
    printf("usage: %s [--fb /dev/fb0] [--input /dev/input/eventN]\n"
           "\n"
           "  RADIOPLUS_HOME  where presets and recordings live\n"
           "                  (default /tmp/radioplus)\n"
           "\n"
           "FM rides on hci0, so bring Bluetooth up first with n31-bt-up.\n",
           me);
}

int main(int argc, char **argv)
{
    const char *fb = "/dev/fb0";
    const char *input = NULL;

    for (int i = 1; i < argc; i++) {
        int has = i + 1 < argc;
        if (!strcmp(argv[i], "--fb") && has) fb = argv[++i];
        else if (!strcmp(argv[i], "--input") && has) input = argv[++i];
        else { usage(argv[0]); return 0; }
    }

    /* Wired before anything is opened, so an interrupt during start-up still
       releases whatever had been taken by then. */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    lv_init();
    lv_tick_set_cb(millis);

    /* DRM if the driver is there, fbdev if it is not - see display.h. */
    lv_display_t *disp = n31_display_create(fb, RP_SCREEN_W, RP_SCREEN_H, NULL);
    if (!disp) {
        fprintf(stderr, "radioplus: no display at all\n");
        return 1;
    }
    printf("radioplus: display %s\n", n31_display_describe());

#if LV_USE_EVDEV
    s_touch_since = 0;
    /*
     * The panel, looked for again until it is found.
     *
     * Asked once, at startup, this got the answer "no panel" whenever Radio+
     * started before the touchscreen driver had registered its node - and
     * kept that answer for the whole session. That is not hypothetical here:
     * the node does not survive the previous app, so an app started straight
     * after another one is asking at exactly the wrong moment. See
     * touch_retry() in the main loop.
     */
    s_touch_want = input;
    s_touch_disp = disp;
    touch_retry();
    if (!s_touch_indev)
        printf("radioplus: no touch panel yet, still looking\n");
#endif

    open_keys();

    /*
     * Something on the screen before anything else.
     *
     * Until this was here the panel kept whatever the launcher had left on it
     * until the first real screen existed, and a device that looks like it did
     * not start is indistinguishable from one that did not.
     *
     * The first rp_model_refresh is quick now - it reads the settings and the
     * presets, then asks the hardware once and returns whatever it finds. The
     * waiting, if there is any, happens further down where the buttons still
     * work.
     */
    rp_ui_boot("starting");

    static const rp_progress_t prog = { rp_ui_boot, rp_ui_boot_failed };
    rp_model_set_progress(&prog);

    rp_model_refresh();

    /*
     * The interface is built before the wait, not after it.
     *
     * Nothing in rp_ui_init needs hardware - it draws presets and recordings,
     * both of which came off the filesystem during bring-up - so there is no
     * reason to make the listener wait for a radio before they can have the
     * screens. Building first means the buttons do something, every screen is
     * ready the instant a driver turns up, and the boot screen becomes a cover
     * over a working interface rather than a substitute for one.
     */
    rp_ui_init();

    /*
     * Then wait on top of it for whatever has not arrived.
     *
     * The drivers this needs are loaded by scripts running alongside this
     * process - the sound modules a few seconds into boot, hci0 raised by
     * another - so which of us gets there first is a race, and losing it used
     * to be permanent: one attempt, at the wrong instant, and "no audio
     * hardware" and "no Bluetooth" latched into a model that never asked
     * again.
     *
     * The platform says what it is still waiting for and decides when to give
     * up saying it. Three things end this loop: everything arrives, the
     * platform's deadline passes, or the listener presses something and goes
     * to a screen of their own - which is why pump_keys is in here. A wait
     * that ignores the buttons is a device that has hung, and this one can
     * legitimately last twenty seconds.
     *
     * Whatever ends it, the retry inside rp_model_refresh keeps going, so a
     * tuner that appears a minute from now still works.
     */
    if (rp_model_waiting()) rp_ui_boot_show();

    for (const char *what;
         !s_quit && rp_ui_booting() && (what = rp_model_waiting()) != NULL; ) {
        pump_keys();
        touch_tick();
        rp_ui_boot(what);
        usleep(120000);
        rp_model_refresh();
        rp_ui_tick();
    }

    /* Nothing more to report; the real screens say the rest themselves. */
    rp_model_set_progress(NULL);
    if (rp_ui_booting()) rp_ui_show(RP_SCREEN_NOW);

    printf("radioplus: %s\n", rp_model.backend ? rp_model.backend : "?");
    printf("radioplus: %s\n",
           rp_model.capture_backend ? rp_model.capture_backend : "?");
    if (!rp_model.tuner_ok && rp_model.tuner_note)
        printf("radioplus: %s\n", rp_model.tuner_note);

    while (!s_quit) {
        pump_keys();
        touch_tick();
        rp_model_refresh();
        rp_ui_tick();

        uint32_t wait = lv_timer_handler();
        if (wait > FRAME_MS) wait = FRAME_MS;   /* also covers no-timer */
        usleep(wait * 1000u);
    }

    /* The whole reason the signal handlers exist. A capture thread still
       holding a PCM device, or a tuner still drawing power, outlives the
       process otherwise. */
    printf("\nradioplus: stopping\n");
    en_cap_record_stop();
    en_play_stop();
    en_cap_stop();
    en_tuner_shutdown();
    return 0;
}
