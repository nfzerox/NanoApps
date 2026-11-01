/*
 * main_host.c — bring the shared UI up on a Linux desktop, at 240x432.
 *
 * The window is forced to the iPod's exact panel size. That is the whole
 * point: a host build at some other resolution would let you tune a layout
 * that does not exist on the device.
 *
 * Backends, chosen with --backend or ENTRAIN_BACKEND:
 *   sdl       a 240x432 window. What you develop against.
 *   fbdev     /dev/fb0, for a small panel over tinydrm's fbdev shim.
 *   drm       /dev/dri/card0 directly.
 *   headless  renders into memory and writes BMPs. No display needed, which
 *             is what makes it possible to check the layout from a shell.
 *
 *   ./build/entrain-host                    # SDL window
 *   ./build/entrain-host --shot shots/      # render every screen to shots/
 *   ./build/entrain-host --backend fbdev
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "../ui.h"
#include "../engine.h"
#include "../platform/sys.h"
#include "../platform/audio.h"

#ifdef __linux__
#include <fcntl.h>
#include <errno.h>
#endif

#define W EN_SCREEN_W
#define H EN_SCREEN_H

/* Provided only by the N31 Linux backend. Declared weak at file scope — a
   block-scope weak attribute is quietly ignored by some GCC versions — so the
   desktop build, which links a different backend, still resolves to NULL. */
void en_audio_linux_set_device(int card, int device) __attribute__((weak));
void en_audio_linux_force_null(int on) __attribute__((weak));

/* ---- headless display ---------------------------------------------------- */

static uint8_t s_fb[W * H * 4];      /* XRGB8888, matching the device */

static void headless_flush(lv_display_t *d, const lv_area_t *area,
                           uint8_t *px_map)
{
    (void)area;
    /* DIRECT render mode hands us the whole framebuffer every time. */
    memcpy(s_fb, px_map, sizeof s_fb);
    lv_display_flush_ready(d);
}

static lv_display_t *headless_create(void)
{
    static uint8_t buf[W * H * 4];
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(d, buf, NULL, sizeof buf,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, headless_flush);
    return d;
}

/* Bottom-up 24-bit BMP. Enough to eyeball a layout, and it needs no libraries;
   Makefile.host converts them to PNG with Pillow, which ./start already
   installs. */
static int write_bmp(const char *path)
{
    const int row = W * 3;
    const int pad = (4 - (row % 4)) % 4;
    const int img = (row + pad) * H;
    const int total = 54 + img;

    uint8_t h[54];
    memset(h, 0, sizeof h);
    h[0] = 'B'; h[1] = 'M';
    h[2] = (uint8_t)total; h[3] = (uint8_t)(total >> 8);
    h[4] = (uint8_t)(total >> 16); h[5] = (uint8_t)(total >> 24);
    h[10] = 54;
    h[14] = 40;
    h[18] = (uint8_t)W; h[19] = (uint8_t)(W >> 8);
    h[22] = (uint8_t)H; h[23] = (uint8_t)(H >> 8);
    h[26] = 1;
    h[28] = 24;
    h[34] = (uint8_t)img; h[35] = (uint8_t)(img >> 8);
    h[36] = (uint8_t)(img >> 16); h[37] = (uint8_t)(img >> 24);

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    fwrite(h, 1, sizeof h, f);

    uint8_t zero[4] = { 0, 0, 0, 0 };
    for (int y = H - 1; y >= 0; y--) {
        for (int x = 0; x < W; x++) {
            const uint8_t *p = &s_fb[(y * W + x) * 4];   /* B G R X */
            fwrite(p, 1, 3, f);
        }
        if (pad) fwrite(zero, 1, (size_t)pad, f);
    }
    fclose(f);
    return 1;
}

#include "backlight.h"
#include "touch.h"

/* ---- physical buttons over evdev ------------------------------------------
 *
 * The N31 kernel exposes n31-buttons and the PMIC buttons as keyboards, and
 * there is no touchscreen device at all yet, so this is the only way to drive
 * the UI on the device.
 *
 * The event struct is declared by hand rather than pulled from <linux/input.h>
 * on purpose. musl uses a 64-bit time_t, so its struct timeval is 16 bytes,
 * while the kernel writes input_event with 32-bit time fields on arm32. Using
 * the system struct would misparse every event by eight bytes — silently, and
 * in a way that looks like the buttons simply do not work. */

#ifdef __linux__

struct en_input_event {
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

#define EN_EV_KEY 0x01

/* One slot per /dev/input/eventN, indexed by N, so a device that disappears
   and comes back at the same number refills its own slot. */
#define EN_MAX_INPUT_FDS 16
static int s_input_fd[EN_MAX_INPUT_FDS];
static int s_trace_keys;
static uint32_t s_last_scan_ms;

/* How often to look for devices that have appeared since the last scan. Cheap:
   a handful of open() calls on a machine with three input nodes. */
#define EN_INPUT_RESCAN_MS 2000

static void input_scan(int first_time)
{
    int total = 0, opened = 0;
    for (int i = 0; i < EN_MAX_INPUT_FDS; i++) {
        if (s_input_fd[i] >= 0) { total++; continue; }
        char path[64];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            s_input_fd[i] = fd;
            total++;
            opened++;
            if (!first_time) printf("input: event%d appeared\n", i);
        }
    }
    if (first_time)  printf("entrain-host: %d input device(s)\n", total);
    else if (opened) printf("input: %d device(s) open\n", total);
    s_last_scan_ms = en_sys_millis();
}

static void input_open_all(void)
{
    for (int i = 0; i < EN_MAX_INPUT_FDS; i++) s_input_fd[i] = -1;
    input_scan(1);
}

/* Key codes, decoded from the capability bitmaps in
   /proc/bus/input/devices rather than assumed:

     n31-buttons       KEY=1010 0 1c0000 0 0 0  ->  114 115 116 164 172
     n31-pmic-buttons  KEY=1010 0 100000 0 0 0  ->  116 164 172

   POWER is deliberately not mapped; that one belongs to the system. */
#define EN_KEY_ESC        1
#define EN_KEY_VOLUMEDOWN 114
#define EN_KEY_VOLUMEUP   115
#define EN_KEY_POWER      116
#define EN_KEY_BACKKEY    158
#define EN_KEY_PLAYPAUSE  164
#define EN_KEY_HOMEPAGE   172
#define EN_KEY_NEXTSONG   163
#define EN_KEY_PREVSONG   165

/* Both input nodes declare PLAYPAUSE and HOMEPAGE, so one physical press can
   surface on both and be handled twice. Anything closer together than this is
   the same press. */
#define EN_KEY_DEBOUNCE_MS 250

static int s_home_cycles;

/*
 * The touch panel.
 *
 * Looked for again on every input rescan rather than once at startup, for the
 * reason the launcher had to learn: an app started at boot can easily be up
 * before the touchscreen driver has registered its node, and a single miss
 * then means no touch for the whole session. The rescan already exists for
 * the buttons, so this costs one more pass over the same directory.
 */
static lv_display_t *s_touch_disp;
static lv_indev_t   *s_touch_indev;

static void touch_try(void)
{
    const char *tp;

    if (s_touch_indev || !s_touch_disp)
        return;

    tp = n31_touch_find();
    if (!tp)
        return;

    s_touch_indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, tp);
    if (!s_touch_indev) {
        printf("entrain: %s would not open as a pointer\n", tp);
        return;
    }
    lv_indev_set_display(s_touch_indev, s_touch_disp);
    printf("entrain: touch on %s\n", tp);
    fflush(stdout);
}

/* True if this code arrived too soon after the last one to be a new press. */
static int debounced(uint16_t code)
{
    static uint16_t last_code;
    static uint32_t last_ms;
    uint32_t now = en_sys_millis();

    if (code == last_code && now - last_ms < EN_KEY_DEBOUNCE_MS) return 1;
    last_code = code;
    last_ms = now;
    return 0;
}

static void input_poll(void)
{
    struct en_input_event ev;

    /* Pick up anything that has appeared since the last look. The PMIC buttons
       were re-registered twice inside one session while their driver was being
       worked on; without this the app goes on polling a node the kernel has
       already torn down, and every press after that lands nowhere. */
    if (en_sys_millis() - s_last_scan_ms >= EN_INPUT_RESCAN_MS) {
        input_scan(0);
        touch_try();
    }

    for (int i = 0; i < EN_MAX_INPUT_FDS; i++) {
        if (s_input_fd[i] < 0) continue;

        for (;;) {
            ssize_t n = read(s_input_fd[i], &ev, sizeof ev);
            if (n != (ssize_t)sizeof ev) {
                /* Anything but "nothing to read" means the node has gone.
                   Drop it; the next scan reopens it if it comes back. */
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    printf("input: event%d went away (errno %d)\n", i, errno);
                    close(s_input_fd[i]);
                    s_input_fd[i] = -1;
                }
                break;
            }
            if (ev.type != EN_EV_KEY || ev.value != 1) continue;   /* press only */
            if (s_trace_keys)
                printf("key %u down (event%d)\n", ev.code, i);
            if (debounced(ev.code)) continue;

            switch (ev.code) {
            case EN_KEY_VOLUMEUP:   en_ui_key(EN_KEY_VOL_UP);     break;
            case EN_KEY_VOLUMEDOWN: en_ui_key(EN_KEY_VOL_DOWN);   break;
            case EN_KEY_PLAYPAUSE:
            case EN_KEY_NEXTSONG:   en_ui_key(EN_KEY_PLAY_PAUSE); break;
            /*
             * POWER is the launcher's.
             *
             * This briefly toggled the backlight here, which was better than
             * the button doing nothing but still wrong: every app would need
             * its own copy, they would disagree, and two of them acting on
             * the same press - the app and the launcher, which reads the same
             * evdev nodes - fights over the panel. The launcher is the one
             * process that is always running and always holds the backlight,
             * so sleep lives there and apps do not think about it.
             */

            case EN_KEY_HOMEPAGE:
                /*
                 * HOME leaves, the way it does everywhere else here.
                 *
                 * It used to be a third way of saying Back, which meant that
                 * from the top screen it did nothing and there was no way out
                 * of the app but killing it. The launcher is what HOME is
                 * for; Back is still on its own button below.
                 */
                if (s_home_cycles)
                    en_ui_key(EN_KEY_NEXT_PROGRAM);
                else
                    en_sys_request_exit();
                break;

            case EN_KEY_BACKKEY:
            case EN_KEY_ESC:
                en_ui_key(EN_KEY_BACK);
                break;
            default: break;
            }
        }
    }
}

#else
static int s_home_cycles;
static void input_open_all(void) {}
static void input_poll(void) {}
#endif

/* ---- main ---------------------------------------------------------------- */

static void pump(int frames)
{
    for (int i = 0; i < frames; i++) {
        en_ui_tick();
        lv_timer_handler();
        usleep(8000);
    }
}

static void usage(void)
{
    printf("entrain-host — the Entrain UI at %dx%d\n\n"
           "  --backend sdl|fbdev|drm|headless\n"
           "  --shot DIR     render every screen to DIR/NN-name.bmp and exit\n"
           "  --fbdev PATH   framebuffer device (default /dev/fb0)\n"
           "  --drm PATH     DRM card (default /dev/dri/card0)\n"
           "  --evdev PATH   touch device for fbdev/drm backends\n"
           "  --first-run    show the headphone notice\n"
           "  --buttons      drive the UI from /dev/input (no touchscreen yet)\n"
           "  --home-cycles  home steps through the programs instead of going back\n"
           "  --trace-keys   print every key code, to find out what the buttons send\n"
           "  --demo N       walk through every screen, N seconds each\n"
           "  --screen NAME  start on a named screen (library, now-playing, ...)\n"
           "  --tab N        Library tab: 0 presets, 1 programs, 2 custom\n"
           "  --exit-after N quit after N seconds, so a test run cleans up\n"
           "  --preset N     start playing preset N\n"
           "  --program N    start playing program N\n"
           "  --card C [D]   ALSA card and device (default 0 0)\n"
           "  --null-audio   keep time but open no PCM device\n", W, H);
}

int main(int argc, char **argv)
{
    const char *backend = getenv("ENTRAIN_BACKEND");
    const char *shot_dir = NULL;
    const char *fbdev_path = "/dev/fb0";
    const char *drm_path = "/dev/dri/card0";
    const char *evdev_path = getenv("ENTRAIN_EVDEV");
    int first_run = 0;
    int use_buttons = 0;
    int demo_seconds = 0;
    int start_program = -1;
    int start_preset = -1;
    int start_tab = -1;
    int exit_after = 0;
    int no_blank = 0;
    const char *start_screen = NULL;

    for (int i = 1; i < argc; i++) {
        int has = i + 1 < argc;
        if (!strcmp(argv[i], "--backend") && has) backend = argv[++i];
        else if (!strcmp(argv[i], "--shot") && has) shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--fbdev") && has) fbdev_path = argv[++i];
        else if (!strcmp(argv[i], "--drm") && has) drm_path = argv[++i];
        else if (!strcmp(argv[i], "--evdev") && has) evdev_path = argv[++i];
        else if (!strcmp(argv[i], "--first-run")) first_run = 1;
        else if (!strcmp(argv[i], "--buttons")) use_buttons = 1;
        else if (!strcmp(argv[i], "--home-cycles")) {
            use_buttons = 1;
            s_home_cycles = 1;
        }
        else if (!strcmp(argv[i], "--trace-keys")) {
            use_buttons = 1;
#ifdef __linux__
            s_trace_keys = 1;
#endif
        }
        else if (!strcmp(argv[i], "--demo") && has) demo_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--program") && has) start_program = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--preset") && has) start_preset = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tab") && has) start_tab = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screen") && has) start_screen = argv[++i];
        else if (!strcmp(argv[i], "--exit-after") && has) exit_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-blank")) no_blank = 1;
        else if (!strcmp(argv[i], "--null-audio")) {
            if (en_audio_linux_force_null) en_audio_linux_force_null(1);
        }
        else if (!strcmp(argv[i], "--card") && has) {
            int c = atoi(argv[++i]);
            int d = (i + 1 < argc && argv[i + 1][0] != '-') ? atoi(argv[++i]) : 0;
            if (en_audio_linux_set_device) en_audio_linux_set_device(c, d);
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (shot_dir && !backend) backend = "headless";
#if LV_USE_LINUX_DRM
    /*
     * On the device, DRM unless told otherwise.
     *
     * This defaulted to sdl, which is right on a workstation and is not a
     * thing that exists on an iPod - the N31 build was only ever reached with
     * an explicit --backend. Now the device build picks the fast path by
     * itself and the desktop build is unchanged, because LV_USE_LINUX_DRM is
     * only 1 in the N31 config.
     */
    if (!backend) backend = "drm";
#endif
    if (!backend) backend = "sdl";

    /* Line-buffer stdout. Redirected to a file it would otherwise be block
       buffered, and a diagnostic that only appears once 4 KB has accumulated
       is indistinguishable from one that never fired. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    lv_init();
    lv_tick_set_cb(en_sys_millis);

    lv_display_t *disp = NULL;

    if (!strcmp(backend, "headless")) {
        disp = headless_create();
    }
#if LV_USE_SDL
    else if (!strcmp(backend, "sdl")) {
        disp = lv_sdl_window_create(W, H);
        lv_sdl_mouse_create();
    }
#endif
#if LV_USE_LINUX_FBDEV
    else if (!strcmp(backend, "fbdev")) {
        disp = lv_linux_fbdev_create();
        if (disp) lv_linux_fbdev_set_file(disp, fbdev_path);
    }
#endif
#if LV_USE_LINUX_DRM
    else if (!strcmp(backend, "drm")) {
        disp = lv_linux_drm_create();
        if (disp && lv_linux_drm_set_file(disp, drm_path, -1) != LV_RESULT_OK) {
            /*
             * Opened the node and could not drive it. Delete rather than
             * leak: LVGL closes the descriptor on LV_EVENT_DELETE, and
             * holding it would keep DRM master and stop the fbdev fallback
             * from ever reaching the panel.
             */
            lv_display_delete(disp);
            disp = NULL;
        }
#if LV_USE_LINUX_FBDEV
        if (!disp) {
            fprintf(stderr, "entrain: %s would not drive the panel; "
                            "falling back to %s\n", drm_path, fbdev_path);
            disp = lv_linux_fbdev_create();
            if (disp) lv_linux_fbdev_set_file(disp, fbdev_path);
        }
#endif
    }
#endif
    else {
        fprintf(stderr, "backend '%s' is not compiled in\n", backend);
        return 1;
    }

    if (!disp) {
        fprintf(stderr, "could not open the %s backend\n", backend);
        return 1;
    }

    /* Force the device's geometry regardless of what the panel reports, so a
       larger fbdev still shows exactly the iPod's layout. */
    lv_display_set_resolution(disp, W, H);

#if LV_USE_EVDEV
    if (evdev_path && strcmp(backend, "sdl") && strcmp(backend, "headless")) {
        lv_indev_t *in = lv_evdev_create(LV_INDEV_TYPE_POINTER, evdev_path);
        if (!in) fprintf(stderr, "warning: no touch on %s\n", evdev_path);
    }
#endif

    if (first_run) en_ui_set_first_run(true);
    if (use_buttons) input_open_all();
    en_ui_init();

    if (no_blank) en_ui_set_blanking(false);
    if (start_tab >= 0) en_ui_set_tab(start_tab);
    if (start_preset >= 0)  en_engine_play_preset(start_preset);
    if (start_program >= 0) en_engine_play_program(start_program);
    if (start_preset >= 0 || start_program >= 0) en_ui_goto(EN_SCREEN_NOW, false);
    if (start_screen) {
        for (int sc = 0; sc < EN_SCREEN_COUNT; sc++) {
            if (!strcmp(start_screen, en_ui_screen_name((en_screen_t)sc))) {
                en_ui_goto((en_screen_t)sc, false);
                break;
            }
        }
    }

    printf("entrain-host: %s backend, %dx%d, audio via %s\n",
           backend, W, H, en_audio_backend_name());
    fflush(stdout);

    /* ---- screenshot mode ---- */
    if (shot_dir) {
        /* Give the screens something real to show. A Now Playing shot of an
           idle player would tell you nothing about the layout. */
        en_engine_play_program(0);
        pump(60);

        static const en_screen_t order[] = {
            EN_SCREEN_FIRSTRUN, EN_SCREEN_LIBRARY, EN_SCREEN_NOW,
            EN_SCREEN_TUNE, EN_SCREEN_TIMER, EN_SCREEN_SETTINGS
        };
        for (unsigned i = 0; i < sizeof order / sizeof order[0]; i++) {
            en_ui_goto(order[i], false);
            pump(30);
            lv_refr_now(disp);

            char path[512];
            snprintf(path, sizeof path, "%s/%s.bmp", shot_dir,
                     en_ui_screen_name(order[i]));
            if (write_bmp(path)) printf("  %s\n", path);
        }

        /* And one shot per Library shelf. The shot above is the root list;
           these are what is on the shelves, which is where the app's content
           actually lives. This block used to render a second Library view and
           then never write it out, so it produced nothing at all. */
        static const struct { int tab; const char *name; } SHELVES[] = {
            { 0, "library-presets" },
            { 1, "library-programs" },
            { 3, "library-suites" },
            { 4, "library-frequencies" },
        };
        for (unsigned i = 0; i < sizeof SHELVES / sizeof SHELVES[0]; i++) {
            en_ui_goto(EN_SCREEN_LIBRARY, false);
            en_ui_set_tab(SHELVES[i].tab);
            pump(20);
            lv_refr_now(disp);

            char path[512];
            snprintf(path, sizeof path, "%s/%s.bmp", shot_dir, SHELVES[i].name);
            if (write_bmp(path)) printf("  %s\n", path);
        }

        /* One level deeper on the frequency shelf, which is the only shelf
           that has one: initials, then the sets under an initial. A shot of
           the initials alone would not show what a set row looks like. */
        en_ui_set_tab(4);
        en_ui_key(EN_KEY_VOL_DOWN);      /* off the digits, onto A */
        en_ui_key(EN_KEY_PLAY_PAUSE);    /* open it */
        pump(20);
        lv_refr_now(disp);
        {
            char path[512];
            snprintf(path, sizeof path, "%s/library-frequency-sets.bmp",
                     shot_dir);
            if (write_bmp(path)) printf("  %s\n", path);
        }

        /* Leave it on the root, so the library shot above is what a rerun
           reproduces rather than whichever shelf happened to be last. */
        en_ui_set_tab(-1);

        en_ui_shutdown();
        return 0;
    }

    /* ---- interactive ---- */
    static const en_screen_t WALK[] = {
        EN_SCREEN_LIBRARY, EN_SCREEN_NOW, EN_SCREEN_TUNE,
        EN_SCREEN_TIMER, EN_SCREEN_SETTINGS
    };
    uint32_t next_walk = en_sys_millis() + (uint32_t)demo_seconds * 1000u;
    unsigned walk_i = 0;

    uint32_t deadline = exit_after > 0
                      ? en_sys_millis() + (uint32_t)exit_after * 1000u : 0;

    while (!en_sys_exit_requested()) {
        if (deadline && en_sys_millis() >= deadline) {
            printf("exit-after %ds reached\n", exit_after);
            break;
        }
        if (use_buttons) input_poll();

        /* With no touchscreen on this port, a timed walk is the only way to
           see every screen. It is a diagnostic, not a feature. */
        if (demo_seconds > 0 && en_sys_millis() >= next_walk) {
            next_walk = en_sys_millis() + (uint32_t)demo_seconds * 1000u;
            walk_i = (walk_i + 1) % (sizeof WALK / sizeof WALK[0]);
            en_ui_goto(WALK[walk_i], true);
            printf("screen: %s\n", en_ui_screen_name(WALK[walk_i]));
            fflush(stdout);
        }

        en_ui_tick();
        uint32_t wait = lv_timer_handler();
        if (wait == LV_NO_TIMER_READY) wait = 8;
        if (wait > 16) wait = 16;
        usleep(wait * 1000);
    }

    en_ui_shutdown();
    return 0;
}
