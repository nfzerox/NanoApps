/*
 * hb_sdk.h — Experimental homebrew SDK for iPod nano 7 (N7G), retailos 1.1.2.
 *
 * Design philosophy:
 *   - Apps are flat-binary blobs uploaded via SCSI and exec'd, or chainloaded
 *     using launcher.
 *   - SDK = a small set of .c files compiled into the app binary, NOT
 *     a shared library (no dynamic linking on the device).
 *   - APIs are thin wrappers over hardware MMIO, OS-state polling, or OS API
 *     so the SDK adds minimal code size.
 *   - Apps include this header + link against sdk source files.
 * Skeleton of an app:
 *
 *     #include "hb_sdk.h"
 *
 *     HB_APP_ENTRY(payload_entry) {
 *         hb_fill_screen(HB_BLUE);
 *         hb_draw_uint(40, 200, 1234567, HB_WHITE, HB_BLUE);
 *     }
 */

#ifndef HB_SDK_H_
#define HB_SDK_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- Screen geometry ---- */

#define HB_SCREEN_W   240
#define HB_SCREEN_H   432

/* ---- Color ---- */

typedef uint32_t hb_color_t;   /* 0x00RRGGBB, alpha unused */

#define HB_RGB(r, g, b) \
    ((((uint32_t)(r)) << 16) | (((uint32_t)(g)) << 8) | (uint32_t)(b))

#define HB_BLACK     HB_RGB(0x00, 0x00, 0x00)
#define HB_WHITE     HB_RGB(0xFF, 0xFF, 0xFF)
#define HB_RED       HB_RGB(0xFF, 0x00, 0x00)
#define HB_GREEN     HB_RGB(0x00, 0xFF, 0x00)
#define HB_BLUE      HB_RGB(0x00, 0x00, 0xFF)
#define HB_YELLOW    HB_RGB(0xFF, 0xFF, 0x00)
#define HB_CYAN      HB_RGB(0x00, 0xFF, 0xFF)
#define HB_MAGENTA   HB_RGB(0xFF, 0x00, 0xFF)

/* ---- Geometry types ---- */

typedef struct { int16_t x, y; } hb_point_t;
typedef struct { int16_t x, y, w, h; } hb_rect_t;

/* ---- Display (hb_mipi.c) ---- */

/* Draw a filled rectangle directly to the LCD via MIPI MMIO.
   Bounds are clipped externally (no bounds check here for perf). */
void hb_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, hb_color_t color);

/* Full-screen fill. */
void hb_fill_screen(hb_color_t color);

/* ---- Text rendering (hb_text.c) ---- */

/* Scale factor for the built-in 8x8 font.
   Each digit becomes 8*HB_TEXT_SCALE pixels wide/tall. */
#define HB_TEXT_SCALE  4
#define HB_DIGIT_W     (8 * HB_TEXT_SCALE)
#define HB_DIGIT_H     (8 * HB_TEXT_SCALE)

/* Render a single digit (0..9). Higher digits clamp to glyph 0. */
void hb_draw_digit(int16_t x, int16_t y, uint8_t digit,
                   hb_color_t fg, hb_color_t bg);

/* Render a 32-bit hex value as 8 hex digits (no '0x' prefix).
   Total width = 8 * HB_DIGIT_W = 256 px at SCALE=4 (too wide for 240px screen).
   Split via two hb_draw_hex16 calls or reduce scale to fit. */
void hb_draw_hex32(int16_t x, int16_t y, uint32_t v,
                   hb_color_t fg, hb_color_t bg);

/* Render 16-bit hex value as 4 hex digits. Width = 4 * HB_DIGIT_W = 128 px. */
void hb_draw_hex16(int16_t x, int16_t y, uint16_t v,
                   hb_color_t fg, hb_color_t bg);

/* Render an unsigned decimal number, fixed-width.
   - x, y: top-left of leftmost digit
   - n: value to render
   - n_digits: number of digit columns to draw (left-padded with zeros)
   Automatically clamps n to fit in n_digits via subtraction (no libc div). */
void hb_draw_uint(int16_t x, int16_t y, uint32_t n, uint8_t n_digits,
                  hb_color_t fg, hb_color_t bg);

/* ---- ASCII text rendering (hb_font.c) ---- */

/* Draw one 8x8 ASCII glyph at integer pixel scale (1 = 8x8, 2 = 16x16,
   etc). Chars outside 0x20..0x7F render as space. */
void hb_draw_char(int16_t x, int16_t y, char c, uint8_t scale,
                  hb_color_t fg, hb_color_t bg);

/* Draw a null-terminated string starting at (x, y), left-to-right with
   no kerning. Advances 8*scale pixels per glyph. */
void hb_draw_str(int16_t x, int16_t y, const char *s, uint8_t scale,
                 hb_color_t fg, hb_color_t bg);

/* ---- Buttons (hb_button.c) ---- */

/* Available buttons on N7G.
     - Vol up/down: direct SoC GPIO at PDAT5 bits 0/1 (0x3CF000A4).
     - Home/Power/Play-Pause: PMU button-state registers. The polled
       reader can race with OS-initiated transactions; if a button read
       fails, treat as not-pressed (return false). */
typedef enum {
    HB_BTN_VOL_UP     = 0,
    HB_BTN_VOL_DOWN   = 1,
    HB_BTN_HOME       = 2,
    HB_BTN_POWER      = 3,
    HB_BTN_PLAY_PAUSE = 4,
} hb_button_t;

/* Returns true if the given button is currently held down. */
bool hb_button_pressed(hb_button_t btn);

/* ---- Screenshots (hb_screenshot.c) ---- */

/* Trigger the OS screenshot handler. Writes /screenshotNNNN.bmp at the
   filesystem root when the iPod OS filesystem is mounted. This captures the
   whole OS compositor, including the homebrew app's own view. */
bool hb_screenshot_take(void);

/* ---- Touch (hb_touch.c) ---- */

/* One observed touch event. */
typedef struct {
    bool valid;          /* did anything change since last poll? */
    uint32_t time_lo;    /* low 32 bits of OS uptime in us */
    int16_t x, y;
    uint8_t status;      /* observed touch phase: 0=down, 1=move, 2=up */
    uint32_t touch_id;   /* monotonic id for each stroke */
} hb_touch_t;

/* Initialize touch subsystem (snapshots the current state).
   Call once before hb_touch_poll. */
void hb_touch_init(void);

/* Returns true if a new touch state-change was observed since last
   poll. Fills *out with details (including x, y coords) if non-NULL.
   Does NOT block. */
bool hb_touch_poll(hb_touch_t *out);

/* Returns true if a touch is currently in the list (held finger).
   Fills *out with current X, Y if non-NULL. */
bool hb_touch_get_coords(hb_touch_t *out);

/* Pop the oldest queued touch sample. Must be called to keep the list
   draining; the OS queue is small and drops events once full. Returns
   true if a node was popped, false if the queue was already empty. */
bool hb_touch_pop_front(void);

/* Pop nodes until the list has at most 1 entry, so the most recent
   touch becomes the front. Avoids the singleton race with a concurrent
   driver push. Returns the number of nodes popped. */
uint32_t hb_touch_drain_to_one(void);

/* ---- Multitouch ---- */

/* Inspect all available finger slots, returning the count of active
   fingers and filling out[0..count-1] with their coords. Each entry's
   touch_id is the finger slot index. */
#define HB_MAX_FINGERS 8
int hb_touch_poll_multi(hb_touch_t out[HB_MAX_FINGERS]);

/* Drain-to-one across all 8 finger lists. Call once per frame before
   hb_touch_poll_multi to ensure each list shows its newest sample. */
void hb_touch_drain_all(void);

/* ---- Filesystem (hb_fs.c) ----

   PRECONDITION: iPod must be on the home screen with the user-facing
   filesystem mounted. Run `start eject` once. While in "Connected"
   mode, the volume is unmounted and every call below returns false. */

/* True if `path` exists on the main iPod filesystem. */
bool hb_fs_exists(const char *path);

/* Create/truncate `path` and write `size` bytes from `data`. Returns
   true on success. Path is relative to the volume root, e.g.
   "/iPod_Control/myfile.txt". Caches data, sets EOF, flushes before
   returning, so the data is durable on disk. */
bool hb_fs_write(const char *path, const void *data, uint32_t size);

/* Write several buffers to one file, in order, with a single open (e.g. a
   recording ring split across multiple allocations). */
bool hb_fs_write_parts(const char *path, void *const *ptrs, const uint32_t *lens, int nparts);

/* Streaming writer: open a file, append to it many times, then close. One stream
   open at a time. Lets a large output (screen recording) be flushed buffer-by-
   buffer instead of held whole in RAM. */
bool hb_fs_stream_open(const char *path);
bool hb_fs_stream_write(const void *data, uint32_t len);
bool hb_fs_stream_close(void);

/* Read up to `max_size` bytes from `path` into `buf`. Returns the
   number of bytes actually read (0 if file is missing or read failed). */
uint32_t hb_fs_read(const char *path, void *buf, uint32_t max_size);

/* Directory iteration. Opaque storage; pass the same hb_dir_t through
   open / next / close. Caller owns the buffer.

   Usage:
       hb_dir_t d;
       char name[128];
       bool is_dir;
       hb_fs_dir_open(&d, "/iPod_Control", false);
       while (hb_fs_dir_next(&d, name, sizeof name, &is_dir)) {
           // name is the path relative to the start dir
       }
       hb_fs_dir_close(&d);

   `recursive`=false stops at the immediate children of `path`.
   `recursive`=true also descends into sub-directories. */
typedef struct { uint8_t opaque[0x50]; } hb_dir_t;

bool hb_fs_dir_open (hb_dir_t *iter, const char *path, bool recursive);
bool hb_fs_dir_next (hb_dir_t *iter, char *out_name, int out_size,
                     bool *out_is_dir);
void hb_fs_dir_close(hb_dir_t *iter);
/* Same as hb_fs_dir_open but on an explicit volume id (0=Main, 4=Resources). */
bool hb_fs_dir_open_at(hb_dir_t *iter, const char *path, bool recursive,
                       int volume_id);

/* Delete a file. Returns true if the file no longer exists after the
   call (success or already-missing). Will fail to delete a non-empty
   directory — use hb_fs_rmdir_recursive for that. */
bool hb_fs_remove(const char *path);

/* Create a directory, including any missing intermediate directories
   (mkdir -p semantics). Returns true on success or if the dir already
   existed. */
bool hb_fs_mkdir(const char *path);

/* Get file size in bytes. Returns 0 if the file doesn't exist or stat
   fails. (Zero-byte files indistinguishable from missing — use
   hb_fs_exists if you need to disambiguate.) */
uint32_t hb_fs_size(const char *path);

/* True if `path` exists AND is a directory. */
bool hb_fs_is_dir(const char *path);

/* Remove an empty directory. Use hb_fs_rmrf for non-empty trees. */
bool hb_fs_rmdir(const char *path);

/* Recursive remove — file OR directory. Returns true on full success.
   Use for cleaning up app data folders, /Apps/Foo.app etc. */
bool hb_fs_rmrf(const char *path);

/* Smart unlink — works on either files or empty dirs (uses
   is_dir + remove vs. rmdir). */
bool hb_fs_unlink(const char *path);

/* Set the FAT attribute byte on a file/dir. Common bits:
   0x01 is read only, 0x02 is hidden, 0x04 is system, 0x10 is directory.
   Returns true if the call succeeded. */
bool hb_fs_set_attr(const char *path, uint8_t attr_byte);

/* ---- Audio (hb_audio.c) ----

   Play a WAV file via. Volume is 0..0x7fff (use 0x7fff for max). Returns 
   true if the audio pthread was spawned.

   IMPORTANT: this spawns a pthread to do the actual loadFile + play.
   The pthread only gets CPU once your main app yields it — i.e. either
   return from your app entry, or block on a long sleep. A tight UI
   loop after this call will STARVE the pthread and no sound plays.

   Single-shot only: the descriptor is a static buffer. Don't call
   again until the previous sound has finished.

   Known WAV paths on the iPod resource volume:
       Resources/Sounds/shake.wav        (shake-to-shuffle sound)
       Resources/Sounds/volumebeep.wav   (volume HUD chime) */
bool hb_audio_play_wav(const char *path, uint32_t volume_0_to_7fff);
/* Same but loads from the main iPod filesystem. */
bool hb_audio_play_wav_main(const char *path, uint32_t volume_0_to_7fff);

/* Audio playback split into 4 steps. Caller MUST interleave a
   scale-3 hb_draw_str (or equivalent MIPI activity) between each
   step or the device reboots. Empirically: pure CPU delay, small
   fill_rects, and a single wiggle within the SDK were all insufficient.
   The exact mechanism isn't fully understood — probably the OS audio
   task only wakes on certain MIPI / dispatch events.

   Volume_id: 0=main filesystem, 4=resource filesystem.
   desc must be a >= 0x78 byte buffer (static or pthread stack).
   Caller must be in a pthread with >= 24 KB stack (loadFile needs ~21 KB).

   Canonical pattern:
       uint8_t desc[0x80];
       hb_audio_ctor(desc);             hb_draw_str(... "step1" ...);
       hb_audio_loadfile(desc, p, 4);   hb_draw_str(... "step2" ...);
       hb_audio_setfields(desc, 0x7fff); hb_draw_str(... "step3" ...);
       hb_audio_play_now(desc);
*/
void hb_audio_ctor    (void *desc);
int  hb_audio_loadfile(void *desc, const char *path, int volume_id);
void hb_audio_setfields(void *desc, uint32_t volume_0_to_7fff);
bool hb_audio_play_now (void *desc);

/* ---- Brightness (hb_brightness.c) ----

   Set LCD backlight level via. Two interfaces:
     hb_brightness_set_percent(0..100) — uses the same piecewise
       mapping the OS uses (0..40% -> MIN..DEFAULT, 40..100% ->
       DEFAULT..MAX) with N7G board values.
     hb_brightness_set(u16 raw_level) — direct raw value pass-through.
       Usable range ~0x3200..0xED80; below MIN clamps, level=0 does
       NOT actually power the backlight off.

   hb_brightness_get() returns the last value we set. The OS does not
   expose a stable "read current backlight" API. */
void     hb_brightness_set        (uint16_t raw_level);
void     hb_brightness_set_percent(int percent_0_to_100);
uint16_t hb_brightness_get        (void);
/* True backlight power on/off. The level=0 setter clamps to the
   minimum visible level and stays lit; this fn disables the driver. */
void     hb_brightness_power      (bool on);

/* ---- Display wake lock (hb_lv_surface.c, LVGL surface apps) ----

   hb_wake_lock(true) keeps the screen lit while your app runs — no auto-dim /
   screen-off. hb_wake_lock(false) restores normal behaviour. Call once at app
   entry; it self-manages a 10 s timer that resets the OS idle clock like a touch
   (the OS's dim-permission screen flag can't gate a homebrew-pushed controller). Use it
   for anything that should stay visible without touch input: benchmarks,
   slideshows, now-playing, a clock. Released automatically on app switch. */
void     hb_wake_lock             (bool on);


void     hb_lv_set_frame_cb       (void (*cb)(void));

/* ---- Settings (hb_prefs.c) ----

   Read/write the OS preference store. Keys are dotted strings like
   "General.Brightness", "General.BacklightTimer", "Playback.VolumeLimit".
   Writes appear to persist immediately — no separate flush slot is called,
   as far as we traced.

   Caveat: settings the OS doesn't already know about will be created
   but won't appear in the Settings UI unless the OS also registers
   them. For experimenting with persistent app state, prefer
   writing to file on disk instead. */
int32_t hb_settings_get_int(const char *key, int32_t default_v);
void    hb_settings_set_int(const char *key, int32_t value);
void    hb_settings_set_str(const char *key, const char *value);

/* Convenience getters/setters for known keys. BacklightTimer is seconds,
   0 = always-on. */
int32_t hb_settings_get_brightness(void);
void    hb_settings_set_brightness(int32_t level_1_to_3);
int32_t hb_settings_get_backlight_timer(void);
void    hb_settings_set_backlight_timer(int32_t seconds);

/* ---- Uptime (hb_time.c) ----

   Free-running monotonic uptime from the SoC microsecond counter. Treat
   values as nominal ms, fine for LVGL animations and any relative timing
   inside one app run. For wall-clock dates / time, use hb_rtc_read instead. */
uint32_t hb_time_uptime_us(void);
uint32_t hb_time_uptime_ms(void);

/* ---- I-cache maintenance (hb_reloc.c) ----

   Invalidate the instruction cache for [addr, addr + size). Call this
   after writing new code bytes into a region you're about to jump to
   — otherwise the CPU may execute stale cache lines from whatever
   code used to live there.

   The SCSI exec command only invalidates a small head window, so
   apps that load other apps (or load shaders / patches) must call
   this themselves. HB_APP_ENTRY invokes it for the app's own .text. */
void hb_icache_invalidate(uint32_t addr, uint32_t size);

/* ---- RTC / Time (hb_rtc.c) ----

   Read current date/time from the OS clock service. */
typedef struct {
    uint8_t  seconds;     /* +0  0-59 */
    uint8_t  minutes;     /* +1  0-59 */
    uint8_t  hours;       /* +2  0-23 */
    uint8_t  day_of_month;/* +3  1-31 */
    uint8_t  month;       /* +4  1-12 */
    uint8_t  _pad;        /* +5  unused byte */
    uint16_t year;        /* +6  16-bit */
    uint8_t  weekday;     /* +8  last byte */
} hb_rtc_time_t;

void hb_rtc_read(hb_rtc_time_t *out);

/* Set the device clock. Validates Year ∈ [2000,2099], Month ∈ [1,12],
   Day ∈ [1,31], Hour ∈ [0,23], Min/Sec ∈ [0,59]. Returns true on
   success. Preserves timezone/DST flags from existing RTC state. */
bool hb_rtc_set(uint16_t year, uint8_t month, uint8_t day,
                uint8_t hour, uint8_t minute, uint8_t second);

/* ---- Accelerometer (hb_accel.c) ----

   Returns the most recent 3-axis sample produced by the OS
   accelerometer task.

   Axis convention (held in portrait orientation, screen toward you):
     +X = right, +Y = up, +Z = toward you (out of screen)
   Stationary "screen up" reads roughly (0, 0, +1000) milli-g. */
void hb_accel_read_raw(int32_t out_xyz[3]);

/* Same as above but converted to milli-g. 1g = 1000. */
void hb_accel_read_milli_g(int32_t out_xyz[3]);

/* ---- Crash-log trace channel (hb_trace.c) ----

   RAM at 0x09100C00 survives N7G's kernel-panic + reboot cycle.
   Use this to leave breadcrumbs from apps that might crash; after the
   reboot, host reads the buffer via SCSI (`tools/dump_trace.sh`) to
   see what we did last.

   Usage:
     hb_trace_init();                  // claim buffer if not magic-stamped
     hb_trace_log("BOOT",   0,  0);    // breadcrumb at any point
     hb_trace_log("OP_NEW", buf_addr, size);
     hb_trace_log("CTOR",   ret_addr, 0);
     // if we crash anywhere here, the trace tells us where */
void hb_trace_init(void);
void hb_trace_reset(void);
void hb_trace_log(const char *tag, uint32_t v1, uint32_t v2);

/* ---- Image / bitmap (hb_image.c) ----

   For homebrew, ship pre-decoded bitmaps (BMP is convenient) or roll
   your own per-app decoder.

   Performance note: the current blit is a per-pixel fill_rect loop —
   O(N) MIPI long-writes. Fine for icons (≤ 64×64) but slow for
   full-screen. A dedicated streaming blitter is on the TODO list. */
/* Push a raw 24bpp RGB888 or 16bpp RGB565 region to the LCD.
   Implemented via MIPI long-write (~500 px per packet) — fast enough
   to push the full 240×432 screen in one frame. The LVGL adapter
   uses the RGB565 variant for its flush callback. */
void hb_blit_rgb24 (int16_t x, int16_t y, int16_t w, int16_t h,
                    const uint8_t  *src);
void hb_blit_rgb565(int16_t x, int16_t y, int16_t w, int16_t h,
                    const uint16_t *src);

/* Older names — same as hb_blit_rgb24/565. Kept for app-side compat. */
void hb_mipi_blit_rgb888(int16_t x, int16_t y, int16_t w, int16_t h,
                         const uint8_t *src);
void hb_mipi_blit_rgb565(int16_t x, int16_t y, int16_t w, int16_t h,
                         const uint16_t *src);
/* XRGB8888 in, RGB888 out to MIPI. Matches LVGL's LV_COLOR_DEPTH=32
   pixel layout — used by the LVGL adapter's flush callback. */
void hb_mipi_blit_xrgb8888(int16_t x, int16_t y, int16_t w, int16_t h,
                           const uint32_t *src);

/* Block until the LCM enters vblank (tearing-effect signal on GPIO
   1.6 transitions HIGH → LOW). Pair with a blit so the pixel push
   happens during vblank — eliminates tearing visible when LVGL
   updates land mid-frame. ~25 ms timeout so we don't wedge if the
   panel ever stops generating vsync. */
void hb_display_wait_vsync(void);

/* Read an uncompressed Windows BMP file (24bpp or 32bpp) into
   out_pixels as 24bpp RGB. Caller must size out_pixels >= w*h*3.
   Returns true on success. */
bool hb_bmp_load_to(const char *path, void *out_pixels,
                    uint32_t out_capacity,
                    int16_t *out_w, int16_t *out_h, int *out_bpp);

/* ---- Screen orientation (hb_orientation.c) ----

   Read / write the OS screen rotation. Reads/writes the
   General.UIRotation setting and calls the Silver rotation handler
   to apply it (compositor re-renders). Homebrew apps drawing
   directly to MIPI see the raw 240x432 framebuffer regardless of
   rotation — transform coordinates yourself if you want to follow
   the OS orientation. */
int32_t hb_orientation_get(void);              /* 0/90/180/270 */
void    hb_orientation_set(int32_t degrees);   /* writes + applies */
void    hb_orientation_apply_from_accel(void); /* trigger auto-rotate */

/* ---- Media player direct control (hb_media.c) ----

   Talks to the OS media player. This is best for simple queries and
   experimental low-level control. */
int  hb_media_state(void);            /* 0=playing, 1=paused, 2=stopped, -1=no player */
bool hb_media_has_session(void);      /* true if a track is queued (playing OR paused) */
void hb_media_set_paused(bool paused);/* true = pause, false = resume */
void hb_media_toggle(void);           /* play/pause toggle */
void hb_media_next(void);
void hb_media_prev(void);

/* ---- Battery (hb_battery.c) ----

   Read OS PMU cache fields. Pieced the layout together by diffing the cache
   plugged vs unplugged and a few firmware call sites — best-effort; some fields
   in that block we never identified. */
uint32_t hb_battery_voltage_mv     (void);   /* ~3500-4200 mV Li-poly */
uint32_t hb_battery_level_0_to_15  (void);   /* OS-computed 0..15 scale */
uint32_t hb_battery_charger_state  (void);   /* observed charger state, 0..6 */
bool     hb_battery_is_charging    (void);   /* charge state 5 or 6 */

/* Diagnostic — raw struct word at 0x0890c070 + (word_offset * 4). */
uint32_t hb_battery_cache_read(int word_offset);

/* ---- Paged list (hb_paged_list.c) ----

   Trivial state holder for "page N of M" lists. No drawing — caller
   renders. Compute helpers handle page-count math, clamping, and the
   index range for the current page.

   Usage:
       hb_paged_list_t l;
       hb_paged_list_init(&l, total_items, 8);   // 8 per page
       int from = hb_paged_list_first(&l);
       int to   = hb_paged_list_last(&l);        // exclusive
       for (int i = from; i < to; i++) draw_row(i, items[i]);

       // On NEXT / PREV button:
       hb_paged_list_next(&l);
       hb_paged_list_prev(&l);  */
typedef struct {
    int total;
    int page_size;
    int current_page;
} hb_paged_list_t;

void hb_paged_list_init      (hb_paged_list_t *l, int total, int page_size);
int  hb_paged_list_page_count(const hb_paged_list_t *l);
int  hb_paged_list_first     (const hb_paged_list_t *l);
int  hb_paged_list_last      (const hb_paged_list_t *l);
void hb_paged_list_next      (hb_paged_list_t *l);
void hb_paged_list_prev      (hb_paged_list_t *l);
void hb_paged_list_set_total (hb_paged_list_t *l, int total);

/* ---- Touch UI (hb_ui.c) ----

   Primitives for building a tap-driven UI on top of the touch layer.
   Convention: touch for in-app interaction, vol button for clean exit.

   Workflow:
       hb_ui_init();
       hb_fill_screen(BG);
       hb_ui_button_draw(...);   // for each button
       while (1) {
           int16_t x, y;
           hb_ui_event_t e = hb_ui_poll(&x, &y);
           if (e == HB_UI_EXIT) break;
           if (e == HB_UI_TAP) {
               if (hb_ui_button_hit(x, y, B1_X, B1_Y, B1_W, B1_H)) ...
           }
           hb_ui_pace();
       }
       hb_ui_done();
*/

typedef enum {
    HB_UI_NONE = 0,
    HB_UI_TAP,
    /* User requested app exit. Fired on either:
       - vol up OR vol down press
       - home button press */
    HB_UI_EXIT,
} hb_ui_event_t;
/* Back-compat alias for code that hasn't renamed yet. */
#define HB_UI_VOL_EXIT HB_UI_EXIT

/* Suppress OS-side touch dispatch so taps on our overlay don't also
   launch apps underneath. Restore on hb_ui_done. */
void hb_ui_init(void);
void hb_ui_done(void);

/* Draw a labeled button. Label is centered, rendered at text scale 2. */
void hb_ui_button_draw(int16_t x, int16_t y, int16_t w, int16_t h,
                       const char *label, hb_color_t bg, hb_color_t fg);

/* True if (tx, ty) lands inside the button rect. */
bool hb_ui_button_hit(int16_t tx, int16_t ty,
                      int16_t btn_x, int16_t btn_y,
                      int16_t btn_w, int16_t btn_h);

/* Poll for events. Returns HB_UI_TAP with coords on a new finger-down,
   HB_UI_EXIT when user wants to leave (vol or home), HB_UI_NONE otherwise.
   Debounce in SDK ignores held fingers (one tap = one event). */
hb_ui_event_t hb_ui_poll(int16_t *out_x, int16_t *out_y);

/* Standard pacing between hb_ui_poll calls (~tens of µs). */
void hb_ui_pace(void);

/* ---- On-screen keyboard (hb_kb.c) ----

   4-row 10-column QWERTY keyboard. Apps decide position + cell size.
   Bottom row has special-key slots (delete, space, save) returned as
   distinct character markers so the app can dispatch them.

   Usage:
       hb_kb_t kb = hb_kb_qwerty(0, 250, 24, 45);
       hb_kb_draw(&kb);
       ...
       if (e == HB_UI_TAP) {
           char ch = hb_kb_hit(&kb, tx, ty);
           if (ch == HB_KB_DEL)  ...
           else if (ch == HB_KB_SAVE) ...
           else if (ch != 0)     append ch to buffer;
       }
*/

#define HB_KB_ROWS  4
#define HB_KB_COLS  10

/* Special-key markers inside the layout strings. */
#define HB_KB_DEL    '\x01'
#define HB_KB_SAVE   '\x02'
#define HB_KB_SPACE  '\x03'

typedef struct {
    int16_t x, y;
    int16_t cell_w, cell_h;
    const char *layout[HB_KB_ROWS];   /* HB_KB_COLS chars per row, markers OK */
    const char *labels[HB_KB_ROWS];   /* what to render on each key */
    hb_color_t bg_key;
    hb_color_t bg_special;
    hb_color_t fg;
} hb_kb_t;

/* Build a standard QWERTY keyboard at the given position + cell size. */
hb_kb_t hb_kb_qwerty(int16_t x, int16_t y, int16_t cell_w, int16_t cell_h);

/* Render the keyboard. Call once after layout/position changes. */
void hb_kb_draw(const hb_kb_t *kb);

/* Return the character or special marker at (tx, ty), or 0 if the
   point isn't inside any cell. */
char hb_kb_hit(const hb_kb_t *kb, int16_t tx, int16_t ty);

/* ---- App entry-point boilerplate ---- */

/* Use this macro for the app's entry function. It pins the function
   into .text.entry so the linker places it first at LINK_VA (the
   address the SCSI handler calls).

   The macro emits a small trampoline that zeros .bss (now parked in
   a NOLOAD section at BSS_VA = 0x09200000 — see hb_app.ld) and then
   calls the user-supplied body. Moving .bss out of the loaded binary
   keeps SCSI uploads small and stops large apps from overwriting
   tens of KB of OS .text with zeros. */
#ifdef HB_LV_SURFACE_APP
/* Relocatable LVGL Silver-app build (LV_SURFACE=1): the hb_lv_surface runtime
 * owns the entry/loop; the app body becomes lv_app_main(), which the runtime
 * calls after LVGL is up (lv_init + display + indev already done). The app
 * builds its UI there and, if it needs a per-frame tick, registers it with
 * hb_lv_set_frame_cb(). The whole app (code + LVGL + in-.bss pool) is one
 * relocatable image the resident loads into an operator-new arena. */
void hb_lv_set_frame_cb(void (*cb)(void));
#define HB_APP_ENTRY(name) void lv_app_main(void)
#else
#define HB_APP_ENTRY(name) \
    static void _hb_user_##name(void); \
    __attribute__((section(".text.entry"), used, noinline)) \
    void name(void) { \
        extern unsigned int __bss_start__[], __bss_end__[]; \
        extern unsigned char __text_start__[], __text_end__[]; \
        /* Flush instruction cache for our own .text via SVC 70 — the \
           SCSI exec command may only invalidate a small window, which \
           is fine for the original tiny apps but leaves stale lines \
           when an LVGL-sized binary lands in the middle of OS .text. */ \
        register unsigned int r12 __asm__("r12") = 7; \
        register unsigned int r0  __asm__("r0")  = (unsigned int)__text_start__; \
        register unsigned int r1  __asm__("r1")  = \
            (unsigned int)(__text_end__ - __text_start__); \
        __asm__ volatile("svc #70" : "+r"(r12), "+r"(r0), "+r"(r1) :: "memory"); \
        for (unsigned int *_p = __bss_start__; _p < __bss_end__; _p++) *_p = 0; \
        _hb_user_##name(); \
    } \
    static void _hb_user_##name(void)
#endif

#endif /* HB_SDK_H_ */
