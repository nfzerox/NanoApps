/*
 * launcher.c — homebrew app launcher.
 *
 * Layout (240×432 portrait):
 *   - Header  (top, 40 px)   : "Homebrew" + page indicator
 *   - Grid    (middle)        : 2×3 tiles, 120×120 each = 240×360 total
 *   - Footer  (bottom, 32 px) : [<] prev | tile-count | next [>]
 *
 * Draws on state changes and uses hb_ui_poll for touch queue draining
 * and tap debounce.
 */

#include "hb_sdk.h"

#define HEADER_H        40
#define TILE_W          120
#define TILE_H          120
#define COLS            2
#define ROWS            3
#define APPS_PER_PAGE   (COLS * ROWS)
#define MAX_APPS        64
#define FOOTER_H        32

#define BG_COLOR        HB_RGB(0x10, 0x10, 0x18)
#define HDR_COLOR       HB_RGB(0x20, 0x20, 0x30)
#define TILE_COLOR      HB_RGB(0x30, 0x40, 0x60)
#define TILE_BORDER     HB_RGB(0x60, 0x80, 0xa0)
#define TXT_COLOR       HB_WHITE
#define EMPTY_BG        HB_RGB(0x18, 0x18, 0x20)
#define BTN_BG          HB_RGB(0x40, 0x60, 0x90)
#define BTN_DISABLED    HB_RGB(0x20, 0x28, 0x38)

static int g_n_apps;
static int g_page;
static int g_total_pages;
static hb_app_info_t g_apps[MAX_APPS];

/* Subtract-based digit. */
static int dec_digit(int *n, int divisor)
{
    int d = 0;
    while (*n >= divisor) { *n -= divisor; d++; }
    return d;
}

static void draw_header(void)
{
    hb_fill_rect(0, 0, HB_SCREEN_W, HEADER_H, HDR_COLOR);
    hb_draw_str(8, 8, "Homebrew", 3, TXT_COLOR, HDR_COLOR);

    /* "p X/Y" at scale 2 — readable. */
    char buf[10] = "p _/_";
    buf[2] = '0' + (g_page + 1);
    buf[4] = '0' + g_total_pages;
    hb_draw_str(160, 12, buf, 2, TXT_COLOR, HDR_COLOR);
}

static void draw_tile(int col, int row, const char *label, bool empty)
{
    int x = col * TILE_W;
    int y = HEADER_H + row * TILE_H;
    if (empty) {
        hb_fill_rect(x, y, TILE_W, TILE_H, EMPTY_BG);
        return;
    }
    hb_fill_rect(x, y, TILE_W, TILE_H, TILE_BORDER);
    hb_fill_rect(x + 2, y + 2, TILE_W - 4, TILE_H - 4, TILE_COLOR);

    char trunc[8];
    int li = 0;
    while (label[li] && li < 7) { trunc[li] = label[li]; li++; }
    trunc[li] = 0;
    int w = li * 16;
    int tx = x + (TILE_W - w) / 2;
    int ty = y + (TILE_H - 16) / 2;
    hb_draw_str(tx, ty, trunc, 2, TXT_COLOR, TILE_COLOR);
}

static void draw_footer(void)
{
    int fy = HEADER_H + ROWS * TILE_H;   /* = 40 + 360 = 400 */
    hb_fill_rect(0, fy, HB_SCREEN_W, FOOTER_H, HDR_COLOR);

    /* "<" prev button — left third (80 px) */
    hb_color_t lc = (g_page > 0) ? BTN_BG : BTN_DISABLED;
    hb_fill_rect(2, fy + 2, 76, FOOTER_H - 4, lc);
    hb_draw_str(32, fy + 8, "<", 2, TXT_COLOR, lc);

    /* Tile-count middle (apps shown / total) */
    char buf[12] = "X/YY";
    int n = g_n_apps;
    int h = dec_digit(&n, 100);
    int t = dec_digit(&n, 10);
    int o = n;
    /* "page apps" */
    int start = g_page * APPS_PER_PAGE;
    int end_idx = start + APPS_PER_PAGE;
    if (end_idx > g_n_apps) end_idx = g_n_apps;
    int shown = end_idx - start;
    buf[0] = '0' + shown;
    buf[1] = '/';
    if (h) { buf[2] = '0' + h; buf[3] = '0' + t; buf[4] = '0' + o; buf[5] = 0; }
    else if (t) { buf[2] = '0' + t; buf[3] = '0' + o; buf[4] = 0; }
    else { buf[2] = '0' + o; buf[3] = 0; }
    hb_draw_str(100, fy + 8, buf, 2, TXT_COLOR, HDR_COLOR);

    /* ">" next button — right third */
    hb_color_t rc = (g_page + 1 < g_total_pages) ? BTN_BG : BTN_DISABLED;
    hb_fill_rect(HB_SCREEN_W - 78, fy + 2, 76, FOOTER_H - 4, rc);
    hb_draw_str(HB_SCREEN_W - 50, fy + 8, ">", 2, TXT_COLOR, rc);
}

static void paint(void)
{
    hb_fill_screen(BG_COLOR);
    draw_header();
    int start = g_page * APPS_PER_PAGE;
    for (int slot = 0; slot < APPS_PER_PAGE; slot++) {
        int idx = start + slot;
        int col = slot % COLS;
        int row = slot / COLS;
        if (idx >= g_n_apps) {
            draw_tile(col, row, "", true);
        } else {
            draw_tile(col, row, g_apps[idx].label, false);
        }
    }
    draw_footer();
}

/* Hit-test region codes returned by hit() */
enum { HIT_NONE = -1, HIT_PREV = -2, HIT_NEXT = -3 };

static int hit(int16_t tx, int16_t ty)
{
    int fy = HEADER_H + ROWS * TILE_H;
    if (ty >= fy) {
        if (tx < HB_SCREEN_W / 3) return HIT_PREV;
        if (tx >= 2 * HB_SCREEN_W / 3) return HIT_NEXT;
        return HIT_NONE;
    }
    if (ty < HEADER_H) return HIT_NONE;
    int col = (tx >= TILE_W) ? 1 : 0;
    int yy  = ty - HEADER_H;
    int row;
    if      (yy < TILE_H)     row = 0;
    else if (yy < 2 * TILE_H) row = 1;
    else if (yy < 3 * TILE_H) row = 2;
    else return HIT_NONE;
    return row * COLS + col;
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_reset();
    hb_trace_log("LAU_BOOT", 0, 0);

    g_n_apps = hb_app_scan(g_apps, MAX_APPS);
    hb_trace_log("LAU_SCAN", (uint32_t)g_n_apps, 0);

    /* Log the size of each scanned app to the trace so we can map
       an index to an app by file size (Counter=0x4d0, Clock=0xcea,
       etc.). Useful for picking --autotap-idx without guessing. */
    for (int i = 0; i < g_n_apps && i < 16; i++) {
        uint32_t sz = hb_fs_size(g_apps[i].exec);
        hb_trace_log("APP_IDX", (uint32_t)i, sz);
    }

    g_page = 0;
    g_total_pages = (g_n_apps + APPS_PER_PAGE - 1) / APPS_PER_PAGE;
    if (g_total_pages == 0) g_total_pages = 1;

    /* Sanity check — does /Apps/.load_stub exist? */
    uint32_t stub_size = hb_fs_size("/Apps/.load_stub");
    hb_trace_log("STUB_SZ", stub_size, 0);

    if (g_n_apps == 0) {
        hb_fill_screen(BG_COLOR);
        hb_draw_str(20, 100, "No apps found", 2, TXT_COLOR, BG_COLOR);
        hb_draw_str(20, 140, "in /Apps/", 2, TXT_COLOR, BG_COLOR);
        for (uint32_t i = 0; i < 5000000; i++) {
            if (hb_button_pressed(HB_BTN_VOL_UP)) return;
            for (volatile int j = 0; j < 200; j++) {}
        }
        return;
    }

    int launching = -1;
    bool dirty = true;

#ifdef HB_AUTO_TAP
    /* Automation mode: after AUTO_TAP_FRAMES the launcher behaves as
       if the user tapped tile 0 (or AUTO_TAP_IDX). For use by
       tools/trace_crash.sh — no human interaction needed. */
    uint32_t auto_tap_at = HB_AUTO_TAP;   /* frame to fire the synthetic tap */
#endif

    hb_ui_init();

    for (uint32_t frame = 0; ; frame++) {
        if (dirty) {
            paint();
            dirty = false;
        }

        if (launching >= 0) {
            hb_trace_log("LAU_TAP", (uint32_t)launching, 0);
            hb_fill_rect(0, HEADER_H, HB_SCREEN_W, TILE_H * ROWS,
                         HB_RGB(0xff, 0x80, 0x00));
            hb_draw_str(20, HEADER_H + 80, "Launching...", 2,
                        HB_BLACK, HB_RGB(0xff, 0x80, 0x00));
            for (volatile int i = 0; i < 200000; i++) {}
            hb_trace_log("LAU_CALL", (uint32_t)(uintptr_t)g_apps[launching].exec, 0);
            hb_ui_done();
            if (hb_app_load_and_exec(g_apps[launching].exec)) {
                hb_trace_log("LAU_EXIT", 0, 0);
                return;
            }
            hb_ui_init();
            hb_trace_log("LAU_RTN", 0xfa11ed, 0);
            launching = -1;
            paint();
            hb_draw_str(8, HEADER_H + 80, "load FAILED", 2, HB_RED, BG_COLOR);
            for (volatile int i = 0; i < 3000000; i++) {}
            dirty = true;
            continue;
        }

#ifdef HB_AUTO_TAP
        if (frame == auto_tap_at && launching < 0 && g_n_apps > 0) {
            int idx = 0;
#ifdef HB_AUTO_TAP_IDX
            idx = HB_AUTO_TAP_IDX;
#endif
            if (idx < g_n_apps) launching = idx;
            hb_trace_log("AUTOTAP", (uint32_t)idx, frame);
        }
#endif

        int16_t tx, ty;
        hb_ui_event_t e = hb_ui_poll(&tx, &ty);
        if (e == HB_UI_EXIT) {
            hb_ui_done();
            return;
        }
        if (e == HB_UI_TAP) {
            int r = hit(tx, ty);
            if (r == HIT_PREV) {
                if (g_page > 0) {
                    g_page--;
                    dirty = true;
                }
            } else if (r == HIT_NEXT) {
                if (g_page + 1 < g_total_pages) {
                    g_page++;
                    dirty = true;
                }
            } else if (r >= 0) {
                int idx = g_page * APPS_PER_PAGE + r;
                if (idx < g_n_apps) launching = idx;
            }
        }

        hb_ui_pace();
    }
}
