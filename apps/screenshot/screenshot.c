/*
 * screenshot — capture + browse system screenshots.
 *
 * Capture: the resident hooks Home-triple-click and calls the OS screenshot
 * handler (writes /screenshotNNNN.bmp). This app only owns the TOGGLE for that
 * hook (a RAM byte the resident reads) and a browser for the saved BMPs.
 *
 * Two views:
 *   - Grid: a header with the mode selector + a refresh button, then a
 *     left-aligned scrollable grid of thumbnails (newest first), decoded
 *     incrementally so the UI stays live. The scroll position is remembered
 *     across a viewer round-trip.
 *   - Viewer: one screenshot shown full-screen, unscaled; any tap returns to
 *     the grid.
 *
 * Images: screenshots are full-screen BMPs (~240x432, 24/32-bit). We decode
 * straight to RGB565 at the needed size (thumb or fit-to-screen) so nothing
 * full-resolution-times-4 lives in the LVGL pool. The whole BMP file is read
 * into a temp buffer, decoded, then the temp is freed — peak is one file plus
 * one output image. If a file won't fit, that thumbnail is skipped.
 */
#include "hb_sdk.h"
#include "hb_sysfx.h"
#include "hb_heap.h"
#include "lvgl/lvgl.h"
#include "hb_lv_surface.h"
#include "hb_shared.h"

/* Both toggles live in the resident's shared block (hb_shared.h):
 * screenshot_mode drives Home-triple-click, statusbar_demo makes the status bar
 * read 9:41 with a full battery, for pretty shots. */

#define MAX_SHOTS   64
#define THUMB_W     66
#define THUMB_H     118          /* ~240:432 portrait aspect */
#define GRID_COLS   3

#define RGB565(r,g,b) ((uint16_t)(((((uint32_t)(r)) & 0xF8u) << 8) | \
                                  (((((uint32_t)(g)) & 0xFCu)) << 3) | \
                                  (((uint32_t)(b)) >> 3)))

typedef enum { VIEW_GRID, VIEW_IMAGE, VIEW_COLORS, VIEW_PLAYER,
               VIEW_SETTINGS, VIEW_ACTION } view_t;

static view_t   s_view = VIEW_GRID;
static int      s_scr_w, s_scr_h;
static lv_obj_t *s_colors_view;
static lv_obj_t *s_settings_view;
static lv_obj_t *s_action_view;

#define HDR_ORANGE 0xff9f0a

static char     s_names[MAX_SHOTS][24];   /* "screenshotNNNN.bmp" / "recNNNN.rec" */
static uint8_t  s_is_rec[MAX_SHOTS];      /* 1 = a .rec screen recording           */
static int      s_n_shots = 0;

/* ---- .rec playback (delta+RLE frame stream; see sdk/hb_record.c) ---- */
#define REC_HEADER   32u
#define REC_MAX      (12u * 1024u * 1024u)   /* refuse pathologically large files */
#define REC_PLAY_MS  33                       /* ~30 fps playback                  */
static lv_obj_t *s_player_view, *s_play_canvas;
static uint8_t  *s_rec;                       /* whole .rec file (OS heap)         */
static uint16_t *s_recon;                     /* RGB565 reconstruct buffer (canvas)*/
static uint32_t  s_rec_len, s_rec_pos;
static int       s_rec_w, s_rec_h, s_rec_bpp; /* stored bytes/pixel: 4 XRGB | 2 565 */
static uint32_t  s_play_next_ms;

static lv_obj_t *s_screen;
static lv_obj_t *s_grid_view;
static lv_obj_t *s_image_view;

/* Grid thumbnails: incremental decode driven by the per-frame callback. Each
 * decoded thumb keeps its own RGB565 buffer alive while the grid is shown. */
static lv_obj_t *s_grid_box;
static uint16_t *s_thumb_buf[MAX_SHOTS];
static int       s_thumb_next;            /* next index to decode (0..s_n_shots) */
static int32_t   s_grid_scroll_y;         /* remembered across a viewer round-trip */
static int       s_grid_restore;          /* re-apply s_grid_scroll_y while decoding */

/* Viewer state. */
static lv_obj_t *s_view_canvas;
static uint16_t *s_view_buf;
static int       s_view_idx;

static void build_grid(void);
static void build_image(int idx);
static void build_player(int idx);
static void build_settings(void);
static void build_action(void);
static void build_colors(void);
static void free_thumbs(void);
static void close_grid(void);
static void refresh_cb(lv_event_t *e);

/* ---------------- filesystem: enumerate /screenshotNNNN.bmp -------------- */

static bool is_shot_name(const char *n)
{
    int len = 0;
    while (n[len]) len++;
    if (len < 5) return false;
    /* prefix "screenshot", suffix ".bmp" */
    const char *pre = "screenshot";
    int i = 0;
    for (; pre[i]; i++) if (n[i] != pre[i]) return false;
    return n[len-4] == '.' && (n[len-3]|0x20) == 'b' &&
           (n[len-2]|0x20) == 'm' && (n[len-1]|0x20) == 'p';
}

/* "recNNNN.rec" — a screen recording. */
static bool is_rec_name(const char *n)
{
    int len = 0;
    while (n[len]) len++;
    if (len < 5) return false;
    const char *pre = "rec";
    int i = 0;
    for (; pre[i]; i++) if (n[i] != pre[i]) return false;
    return n[len-4] == '.' && (n[len-3]|0x20) == 'r' &&
           (n[len-2]|0x20) == 'e' && (n[len-1]|0x20) == 'c';
}

static void copy_name(char *dst, const char *src)
{
    int i = 0;
    for (; src[i] && i < 23; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* newest-first ordering: names are zero-padded, so reverse-lexicographic = newest. */
static int name_cmp(const char *a, const char *b)
{
    int i = 0;
    for (; a[i] && b[i]; i++) {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    }
    if (a[i]) return 1;
    if (b[i]) return -1;
    return 0;
}

static void scan_shots(void)
{
    hb_dir_t d;
    char name[64];
    bool is_dir;
    s_n_shots = 0;
    if (!hb_fs_dir_open(&d, "/", false))
        return;
    while (s_n_shots < MAX_SHOTS && hb_fs_dir_next(&d, name, sizeof name, &is_dir)) {
        if (is_dir) continue;
        if (is_shot_name(name))      s_is_rec[s_n_shots] = 0;
        else if (is_rec_name(name))  s_is_rec[s_n_shots] = 1;
        else continue;
        copy_name(s_names[s_n_shots], name);
        s_n_shots++;
    }
    hb_fs_dir_close(&d);
    /* insertion sort, newest (largest name) first; carry the type flag along */
    for (int i = 1; i < s_n_shots; i++) {
        char cur[24]; uint8_t curt = s_is_rec[i];
        copy_name(cur, s_names[i]);
        int j = i;
        while (j > 0 && name_cmp(s_names[j-1], cur) < 0) {
            copy_name(s_names[j], s_names[j-1]); s_is_rec[j] = s_is_rec[j-1];
            j--;
        }
        copy_name(s_names[j], cur); s_is_rec[j] = curt;
    }
}

/* ---------------- BMP decode (24/32-bit, scaled to fit a box) ------------ */

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1]<<8));
}

/* Decode the BMP at /<name> into a freshly lv_malloc'd RGB565 buffer scaled
 * (nearest-neighbour) to fit within maxw x maxh preserving aspect. On success
 * returns the buffer and writes the output size to *ow,*oh. NULL on any error
 * (missing/oversized file, unsupported format, allocation failure). */
static uint16_t *decode_fit(const char *name, int maxw, int maxh, int *ow, int *oh)
{
    char path[32];
    path[0] = '/';
    copy_name(path + 1, name);

    uint32_t fsize = hb_fs_size(path);
    if (fsize < 54u || fsize > 700u * 1024u)
        return NULL;

    uint8_t *file = (uint8_t *)lv_malloc(fsize);
    if (!file) return NULL;
    uint32_t got = hb_fs_read(path, file, fsize);
    if (got < 54u || file[0] != 'B' || file[1] != 'M') { lv_free(file); return NULL; }

    uint32_t dataoff = rd_le32(file + 10);
    int32_t  w  = (int32_t)rd_le32(file + 18);
    int32_t  hh = (int32_t)rd_le32(file + 22);
    uint16_t bpp = rd_le16(file + 28);
    int topdown = 0;
    if (hh < 0) { topdown = 1; hh = -hh; }
    int h = (int)hh;
    if (w <= 0 || h <= 0 || (bpp != 24 && bpp != 32) || dataoff >= fsize) {
        lv_free(file); return NULL;
    }
    uint32_t bytespp = bpp / 8u;
    uint32_t rowstride = ((((uint32_t)w * bpp) + 31u) / 32u) * 4u;
    if (dataoff + rowstride * (uint32_t)h > fsize) { lv_free(file); return NULL; }

    /* fit-preserving-aspect output size */
    int dw = maxw, dh = (int)((int64_t)maxw * h / w);
    if (dh > maxh) { dh = maxh; dw = (int)((int64_t)maxh * w / h); }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    uint16_t *out = (uint16_t *)lv_malloc((uint32_t)dw * (uint32_t)dh * 2u);
    if (!out) { lv_free(file); return NULL; }

    for (int dy = 0; dy < dh; dy++) {
        int sy = (int)((int64_t)dy * h / dh);
        int srow = topdown ? sy : (h - 1 - sy);
        const uint8_t *rp = file + dataoff + (uint32_t)srow * rowstride;
        uint16_t *orow = out + (uint32_t)dy * dw;
        for (int dx = 0; dx < dw; dx++) {
            int sx = (int)((int64_t)dx * w / dw);
            const uint8_t *px = rp + (uint32_t)sx * bytespp;
            orow[dx] = RGB565(px[2], px[1], px[0]);   /* BMP is BGR(A) */
        }
    }
    lv_free(file);
    *ow = dw; *oh = dh;
    return out;
}

/* ---------------- toggle ------------------------------------------------- */

/* The resident reads this byte on Home-triple-click: 0 = nothing (Accessibility),
 * 1 = take a screenshot, 2 = start/stop a screen recording. */
static uint8_t mode_get(void) { uint8_t m = hb_shared->screenshot_mode; return m <= 2u ? m : 0u; }
static void    mode_set(uint8_t m) { hb_shared->screenshot_mode = m; }

static const char *mode_name(uint8_t m)
{
    return m == 1u ? "Screenshot" : m == 2u ? "Record" : "Off";
}

/* Settings screen: open from the grid; drill into the action picker / colors;
 * back returns to the grid. (close_grid handles the grid teardown.) */
static void settings_open_cb(lv_event_t *e) { (void)e; close_grid(); build_settings(); }
static void settings_back_cb(lv_event_t *e)
{ (void)e; if (s_settings_view) { lv_obj_delete(s_settings_view); s_settings_view = NULL; } build_grid(); }
static void action_open_cb(lv_event_t *e)
{ (void)e; if (s_settings_view) { lv_obj_delete(s_settings_view); s_settings_view = NULL; } build_action(); }
static void colors_open_cb(lv_event_t *e)
{ (void)e; if (s_settings_view) { lv_obj_delete(s_settings_view); s_settings_view = NULL; } build_colors(); }

/* Battery-100% override (for pretty screenshots); the resident's battery-index
 * hook reads this byte. */
static void battery_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    hb_shared->statusbar_demo = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1u : 0u;
}

/* Device colors set (index order matches the device-color field). */
static const char *const s_color_names[] = {
    "Space Gray", "Silver", "Blue", "Green", "Yellow", "Pink",
    "Purple", "Product Red", "Space Gray (2015)", "Gold", "Pink (2015)", "Blue (2015)",
};
#define N_COLORS ((int)(sizeof s_color_names / sizeof s_color_names[0]))

static const char *color_name(int idx)
{
    if (idx < 0 || idx >= N_COLORS) return "Default";
    return s_color_names[idx];
}

/* ---------------- grid view --------------------------------------------- */

static void thumb_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_n_shots && s_is_rec[idx]) build_player(idx);
    else                                              build_image(idx);
}

/* Re-apply the remembered scroll position. The grid grows as thumbnails decode,
 * so LVGL clamps an early scroll request to the current (small) content height;
 * we re-apply each tick until decoding finishes so we land on the saved row. */
static void grid_apply_scroll(void)
{
    if (s_grid_restore && s_grid_box)
        lv_obj_scroll_to_y(s_grid_box, s_grid_scroll_y, LV_ANIM_OFF);
}

/* Decode one thumbnail per frame; create its canvas; stop when done. */
static void grid_frame_cb(void)
{
    if (s_view != VIEW_GRID || !s_grid_box) return;
    if (s_thumb_next >= s_n_shots) {
        grid_apply_scroll();
        s_grid_restore = 0;
        hb_lv_set_frame_cb(0);
        return;
    }

    int i = s_thumb_next++;

    lv_obj_t *cell = lv_button_create(s_grid_box);
    lv_obj_set_size(cell, THUMB_W + 6, THUMB_H + 6);
    lv_obj_set_style_pad_all(cell, 2, 0);
    lv_obj_set_style_radius(cell, 4, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x202020), 0);
    lv_obj_add_event_cb(cell, thumb_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    if (s_is_rec[i]) {
        /* recording: a dark tile with a play badge (decoding a multi-MB .rec for a
         * thumbnail would be far too costly; tap to play it full-screen). */
        lv_obj_t *play = lv_label_create(cell);
        lv_label_set_text(play, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(play, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(play, &lv_font_montserrat_28, 0);
        lv_obj_center(play);
    } else {
        int tw = 0, th = 0;
        uint16_t *buf = decode_fit(s_names[i], THUMB_W, THUMB_H, &tw, &th);
        if (buf) {
            s_thumb_buf[i] = buf;
            lv_obj_t *cv = lv_canvas_create(cell);
            lv_canvas_set_buffer(cv, buf, tw, th, LV_COLOR_FORMAT_RGB565);
            lv_obj_center(cv);
        }
    }

    grid_apply_scroll();           /* keep nudging toward the saved row as we grow */
}

static void build_grid(void)
{
    s_view = VIEW_GRID;
    s_thumb_next = 0;
    s_grid_restore = 1;           /* re-apply s_grid_scroll_y as thumbs decode */
    for (int i = 0; i < MAX_SHOTS; i++) s_thumb_buf[i] = NULL;

    s_grid_view = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_grid_view);
    lv_obj_set_size(s_grid_view, s_scr_w, s_scr_h);
    lv_obj_set_style_radius(s_grid_view, 0, 0);
    lv_obj_set_flex_flow(s_grid_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(s_grid_view, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_grid_view, LV_OPA_COVER, 0);

    /* header: "Screenshot" (orange) + Settings + Refresh */
    lv_obj_t *hdr = lv_obj_create(s_grid_view);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, s_scr_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(hdr, 8, 0);
    lv_obj_set_style_pad_column(hdr, 6, 0);

    lv_obj_t *cap = lv_label_create(hdr);
    lv_label_set_text(cap, "Screenshot");
    lv_obj_set_style_text_color(cap, lv_color_hex(HDR_ORANGE), 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_20, 0);
    lv_obj_set_flex_grow(cap, 1);

    lv_obj_t *settings = lv_button_create(hdr);
    lv_obj_set_size(settings, 36, 36);
    lv_obj_set_style_pad_all(settings, 0, 0);
    lv_obj_add_event_cb(settings, settings_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gl = lv_label_create(settings);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_center(gl);

    lv_obj_t *refresh = lv_button_create(hdr);
    lv_obj_set_size(refresh, 36, 36);
    lv_obj_set_style_pad_all(refresh, 0, 0);
    lv_obj_add_event_cb(refresh, refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(refresh);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_center(rl);

    if (s_n_shots == 0) {
        lv_obj_t *empty = lv_label_create(s_grid_view);
        lv_label_set_text(empty, "No screenshots yet");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x909090), 0);
        lv_obj_set_width(empty, s_scr_w - 24);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    /* scrollable thumbnail grid */
    s_grid_box = lv_obj_create(s_grid_view);
    lv_obj_remove_style_all(s_grid_box);
    lv_obj_set_width(s_grid_box, s_scr_w);
    lv_obj_set_flex_grow(s_grid_box, 1);
    lv_obj_set_flex_flow(s_grid_box, LV_FLEX_FLOW_ROW_WRAP);
    /* left-align: partial rows (1-2 thumbs) start at the left edge, not centred */
    lv_obj_set_flex_align(s_grid_box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(s_grid_box, 6, 0);
    lv_obj_set_style_pad_row(s_grid_box, 6, 0);
    lv_obj_set_style_pad_column(s_grid_box, 6, 0);
    lv_obj_set_scroll_dir(s_grid_box, LV_DIR_VER);

    hb_lv_set_frame_cb(grid_frame_cb);     /* decode thumbs incrementally */
}

static void free_thumbs(void)
{
    hb_lv_set_frame_cb(0);
    for (int i = 0; i < MAX_SHOTS; i++) {
        if (s_thumb_buf[i]) { lv_free(s_thumb_buf[i]); s_thumb_buf[i] = NULL; }
    }
    s_grid_box = NULL;
}

static void close_grid(void)
{
    if (s_grid_box) s_grid_scroll_y = lv_obj_get_scroll_y(s_grid_box);  /* remember position */
    free_thumbs();
    if (s_grid_view) { lv_obj_delete(s_grid_view); s_grid_view = NULL; }
}

/* Refresh: re-scan the filesystem and rebuild the grid from the top. */
static void refresh_cb(lv_event_t *e)
{
    (void)e;
    close_grid();
    s_grid_scroll_y = 0;
    scan_shots();
    build_grid();
}

/* ---------------- image viewer ------------------------------------------ */

static void free_view_buf(void)
{
    if (s_view_buf) { lv_free(s_view_buf); s_view_buf = NULL; }
    s_view_canvas = NULL;
}

static void viewer_load(int idx)
{
    if (idx < 0) idx = s_n_shots - 1;
    if (idx >= s_n_shots) idx = 0;
    s_view_idx = idx;

    free_view_buf();
    int iw = 0, ih = 0;
    /* full screen, unscaled: a full-screen capture (240x432) lands 1:1 here */
    s_view_buf = decode_fit(s_names[idx], s_scr_w, s_scr_h, &iw, &ih);
    if (!s_view_buf) return;

    s_view_canvas = lv_canvas_create(s_image_view);
    lv_canvas_set_buffer(s_view_canvas, s_view_buf, iw, ih, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_view_canvas);
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    free_view_buf();
    if (s_image_view) { lv_obj_delete(s_image_view); s_image_view = NULL; }
    build_grid();
}

static void build_image(int idx)
{
    close_grid();
    s_view = VIEW_IMAGE;

    s_image_view = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_image_view);
    lv_obj_set_size(s_image_view, s_scr_w, s_scr_h);
    lv_obj_set_style_radius(s_image_view, 0, 0);
    lv_obj_set_style_bg_color(s_image_view, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_image_view, LV_OPA_COVER, 0);
    /* any tap anywhere returns to the grid */
    lv_obj_add_flag(s_image_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_image_view, back_cb, LV_EVENT_CLICKED, NULL);

    viewer_load(idx);
}

/* ---------------- recording player -------------------------------------- */

/* Decode the next frame of the loaded .rec into s_recon (RGB565), advancing
 * s_rec_pos. Returns 1 if a frame was produced, 0 at EOF (caller loops). Mirrors
 * sdk/hb_record.c's encoder + tools/recsplit.py: per-frame [u32 oplen][ops];
 * ops are u16 tokens over w*h pixels — bit15=1 COPY `cnt` literals (bpp bytes
 * each), bit15=0 SKIP `cnt` pixels (keep prior). Everything bounds-checked so a
 * truncated/corrupt file can't read out of range. */
static int rec_decode_next(void)
{
    uint32_t end, oplen;
    int n = s_rec_w * s_rec_h, px = 0;
    if (s_rec_pos + 4u > s_rec_len) return 0;
    oplen = rd_le32(s_rec + s_rec_pos); s_rec_pos += 4u;
    if (oplen == 0u || s_rec_pos + oplen > s_rec_len) return 0;
    end = s_rec_pos + oplen;
    while (s_rec_pos + 2u <= end && px < n) {
        uint16_t tok = rd_le16(s_rec + s_rec_pos); s_rec_pos += 2u;
        int cnt = tok & 0x7fff;
        if (tok & 0x8000) {                         /* COPY literals */
            for (int k = 0; k < cnt && px < n; k++) {
                if (s_rec_bpp == 4) {
                    uint32_t v;
                    if (s_rec_pos + 4u > end) { px = n; break; }
                    v = rd_le32(s_rec + s_rec_pos); s_rec_pos += 4u;
                    s_recon[px++] = RGB565((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
                } else {
                    if (s_rec_pos + 2u > end) { px = n; break; }
                    s_recon[px++] = rd_le16(s_rec + s_rec_pos); s_rec_pos += 2u;
                }
            }
        } else {                                    /* SKIP: keep s_recon */
            px += cnt;
        }
    }
    s_rec_pos = end;
    return 1;
}

static void rec_rewind(void)
{
    int n = s_rec_w * s_rec_h;
    s_rec_pos = REC_HEADER;
    for (int i = 0; i < n; i++) s_recon[i] = 0;   /* frame 0 diffs against black */
}

static void player_free(void)
{
    hb_lv_set_frame_cb(0);
    if (s_rec)   { hb_os_free(s_rec);   s_rec = 0; }
    if (s_recon) { hb_os_free(s_recon); s_recon = 0; }
    s_play_canvas = NULL;
}

/* time-throttled to ~30 fps regardless of the LVGL refresh rate; loops at EOF */
static void player_frame_cb(void)
{
    if (s_view != VIEW_PLAYER || !s_play_canvas) return;
    uint32_t now = lv_tick_get();
    if ((int32_t)(now - s_play_next_ms) < 0) return;
    s_play_next_ms = now + REC_PLAY_MS;
    if (!rec_decode_next()) { rec_rewind(); rec_decode_next(); }
    lv_obj_invalidate(s_play_canvas);
}

static void player_back_cb(lv_event_t *e)
{
    (void)e;
    player_free();
    if (s_player_view) { lv_obj_delete(s_player_view); s_player_view = NULL; }
    build_grid();
}

/* Show a centred message in a fresh full-screen view, tap to dismiss to grid. */
static void player_message(const char *msg)
{
    s_player_view = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_player_view);
    lv_obj_set_size(s_player_view, s_scr_w, s_scr_h);
    lv_obj_set_style_radius(s_player_view, 0, 0);
    lv_obj_set_style_bg_color(s_player_view, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_player_view, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_player_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_player_view, player_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(s_player_view);
    lv_obj_set_width(l, s_scr_w - 32);                       /* fit width, wrap   */
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, msg);
    lv_obj_set_style_text_color(l, lv_color_hex(0x909090), 0);
    lv_obj_center(l);
}

static void build_player(int idx)
{
    char path[32];
    uint32_t fsize;
    close_grid();
    s_view = VIEW_PLAYER;

    path[0] = '/'; copy_name(path + 1, s_names[idx]);
    fsize = hb_fs_size(path);
    if (fsize < REC_HEADER || fsize > REC_MAX) { player_message("Transfer to a computer to view."); return; }
    s_rec = (uint8_t *)hb_os_alloc(fsize);
    if (!s_rec) { player_message("Transfer to a computer to view."); return; }
    s_rec_len = hb_fs_read(path, s_rec, fsize);
    if (s_rec_len < REC_HEADER || s_rec[0] != 'H' || s_rec[1] != 'B' || s_rec[2] != 'R' ||
        s_rec[3] != 'E' || s_rec[4] != 'C') { player_free(); player_message("Not a recording."); return; }
    s_rec_w   = (int)rd_le32(s_rec + 8);
    s_rec_h   = (int)rd_le32(s_rec + 12);
    s_rec_bpp = (int)rd_le32(s_rec + 24);
    if (s_rec_bpp != 4) s_rec_bpp = 2;                          /* legacy RGB565 */
    if (s_rec_w <= 0 || s_rec_h <= 0 || s_rec_w > 512 || s_rec_h > 512) {
        player_free(); player_message("Bad recording."); return;
    }
    s_recon = (uint16_t *)hb_os_alloc((uint32_t)s_rec_w * (uint32_t)s_rec_h * 2u);
    if (!s_recon) { player_free(); player_message("Transfer to a computer to view."); return; }
    rec_rewind();

    s_player_view = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_player_view);
    lv_obj_set_size(s_player_view, s_scr_w, s_scr_h);
    lv_obj_set_style_radius(s_player_view, 0, 0);
    lv_obj_set_style_bg_color(s_player_view, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_player_view, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_player_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_player_view, player_back_cb, LV_EVENT_CLICKED, NULL);

    s_play_canvas = lv_canvas_create(s_player_view);
    lv_canvas_set_buffer(s_play_canvas, s_recon, s_rec_w, s_rec_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_play_canvas);

    s_play_next_ms = lv_tick_get();
    hb_lv_set_frame_cb(player_frame_cb);
}

/* ---------------- device-color picker ----------------------------------- */

/* highlight the active row so the live OS color is obvious */
static void colors_mark(lv_obj_t *list, int sel)
{
    uint32_t n = lv_obj_get_child_count(list), i;
    for (i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(list, i);
        int idx = (int)(intptr_t)lv_obj_get_user_data(row);
        lv_obj_set_style_bg_color(row, lv_color_hex(idx == sel ? 0x303060 : 0x202020), 0);
    }
}

static void color_pick_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0) hb_devcolor_reset(); else hb_devcolor_set(idx);
    colors_mark(lv_obj_get_parent(row), hb_devcolor_get());  /* re-read what stuck */
}

static void colors_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_colors_view) { lv_obj_delete(s_colors_view); s_colors_view = NULL; }
    build_settings();
}

static lv_obj_t *color_row(lv_obj_t *parent, const char *txt, int idx)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_user_data(row, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(row, color_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    return row;
}

static void build_colors(void)
{
    s_view = VIEW_COLORS;

    s_colors_view = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_colors_view);
    lv_obj_set_size(s_colors_view, s_scr_w, s_scr_h);
    lv_obj_set_style_radius(s_colors_view, 0, 0);
    lv_obj_set_flex_flow(s_colors_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(s_colors_view, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_colors_view, LV_OPA_COVER, 0);

    /* header: back + title */
    lv_obj_t *hdr = lv_obj_create(s_colors_view);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, s_scr_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(hdr, 8, 0);
    lv_obj_set_style_pad_column(hdr, 8, 0);

    lv_obj_t *back = lv_button_create(hdr);
    lv_obj_set_height(back, 32);
    lv_obj_add_event_cb(back, colors_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "Device color");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);

    /* scrollable list: Default + the colors */
    lv_obj_t *list = lv_obj_create(s_colors_view);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, s_scr_w);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    color_row(list, "Default (this device)", -1);
    for (int i = 0; i < N_COLORS; i++)
        color_row(list, color_name(i), i);

    colors_mark(list, hb_devcolor_get());
}

/* ---------------- settings screen --------------------------------------- */

/* shared back-header used by the settings + sub-pickers */
static void settings_header(lv_obj_t *parent, const char *title, lv_event_cb_t back_cb)
{
    lv_obj_t *hdr = lv_obj_create(parent);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, s_scr_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(hdr, 8, 0);
    lv_obj_set_style_pad_column(hdr, 8, 0);
    lv_obj_t *back = lv_button_create(hdr);
    lv_obj_set_height(back, 32);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
}

/* a tappable settings row: label (grows) + value + chevron → drill-in */
static void settings_drill(lv_obj_t *parent, const char *label, const char *value,
                           lv_event_cb_t cb)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, 12, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x202020), 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, lv_color_hex(0x9090FF), 0);
    lv_obj_t *a = lv_label_create(row);
    lv_label_set_text(a, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(a, lv_color_hex(0x808080), 0);
}

static void build_settings(void)
{
    s_view = VIEW_SETTINGS;
    s_settings_view = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_settings_view);
    lv_obj_set_size(s_settings_view, s_scr_w, s_scr_h);
    lv_obj_set_style_radius(s_settings_view, 0, 0);
    lv_obj_set_flex_flow(s_settings_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(s_settings_view, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_settings_view, LV_OPA_COVER, 0);

    settings_header(s_settings_view, "Settings", settings_back_cb);

    lv_obj_t *list = lv_obj_create(s_settings_view);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, s_scr_w);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    settings_drill(list, "Triple-click Home", mode_name(mode_get()), action_open_cb);
    settings_drill(list, "Device color", color_name(hb_devcolor_get()), colors_open_cb);

    /* demo status-bar toggle */
    lv_obj_t *brow = lv_obj_create(list);
    lv_obj_remove_style_all(brow);
    lv_obj_set_width(brow, lv_pct(100));
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(brow, 8, 0);
    lv_obj_set_style_pad_hor(brow, 12, 0);
    lv_obj_set_style_radius(brow, 6, 0);
    lv_obj_set_style_bg_color(brow, lv_color_hex(0x202020), 0);
    lv_obj_t *bl = lv_label_create(brow);
    lv_label_set_text(bl, "Demo status bar (9:41)");
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_flex_grow(bl, 1);
    lv_obj_t *bsw = lv_switch_create(brow);
    if (hb_shared->statusbar_demo) lv_obj_add_state(bsw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(bsw, battery_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ---------------- triple-click action picker ---------------------------- */

static void action_back_cb(lv_event_t *e)
{ (void)e; if (s_action_view) { lv_obj_delete(s_action_view); s_action_view = NULL; } build_settings(); }

static void action_mark(lv_obj_t *list, int sel)
{
    uint32_t n = lv_obj_get_child_count(list), i;
    for (i = 0; i < n; i++)
        lv_obj_set_style_bg_color(lv_obj_get_child(list, i),
                                  lv_color_hex((int)i == sel ? 0x303060 : 0x202020), 0);
}

static void action_pick_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    mode_set((uint8_t)idx);
    action_mark(lv_obj_get_parent(row), idx);
}

static void build_action(void)
{
    s_view = VIEW_ACTION;
    s_action_view = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_action_view);
    lv_obj_set_size(s_action_view, s_scr_w, s_scr_h);
    lv_obj_set_style_radius(s_action_view, 0, 0);
    lv_obj_set_flex_flow(s_action_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(s_action_view, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_action_view, LV_OPA_COVER, 0);

    settings_header(s_action_view, "Triple-click Home", action_back_cb);

    lv_obj_t *list = lv_obj_create(s_action_view);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, s_scr_w);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 6, 0);

    static const char *const opts[3] = { "Off", "Screenshot", "Record" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = lv_button_create(list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_ver(row, 12, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_add_event_cb(row, action_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, opts[i]);
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    }
    action_mark(list, mode_get());
}

/* ---------------- entry -------------------------------------------------- */

void lv_app_main(void)
{
    lv_display_t *disp = lv_display_get_default();
    s_scr_w = lv_display_get_horizontal_resolution(disp);
    s_scr_h = lv_display_get_vertical_resolution(disp);

    s_screen = lv_screen_active();
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    scan_shots();
    build_grid();
}
