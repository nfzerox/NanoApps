/*
 * paint — finger-paint with line interpolation, color palette,
 * bucket fill, and quit button.
 *
 * Press either volume button (or tap the red X) to exit cleanly back
 * to the Connected screen.
 *
 * Layout (240x432):
 *   y =   0..31   top bar: color indicator (120) + bucket F (60) + quit X (60)
 *   y =  32..391  canvas
 *   y = 392..431  palette: 6 swatches @ 40px wide
 */

#include "hb_sdk.h"

/* ---- Layout ---- */
#define TOP_BAR_H    32
#define PALETTE_H    40
#define CANVAS_Y0    TOP_BAR_H
#define CANVAS_Y1    (HB_SCREEN_H - PALETTE_H)
#define CANVAS_H     (CANVAS_Y1 - CANVAS_Y0)

#define QUIT_BTN_W   60
#define QUIT_BTN_X   (HB_SCREEN_W - QUIT_BTN_W)

#define BUCKET_BTN_W 60
#define BUCKET_BTN_X (QUIT_BTN_X - BUCKET_BTN_W)

#define COLOR_IND_W  BUCKET_BTN_X

#define PALETTE_COLS  6
#define PALETTE_COL_W (HB_SCREEN_W / PALETTE_COLS)  /* 40 */
#define PALETTE_Y0    CANVAS_Y1

/* ---- Palette colors ---- */
static const hb_color_t palette[PALETTE_COLS] = {
    HB_BLACK,
    HB_WHITE,
    HB_RED,
    HB_GREEN,
    HB_BLUE,
    HB_YELLOW,
};

/* ---- Helpers ---- */
static int16_t abs16(int16_t v) { return v < 0 ? -v : v; }

/* Bresenham line drawing 3x3 brush, clipped to canvas. */
static void draw_line(int16_t x0, int16_t y0,
                      int16_t x1, int16_t y1,
                      hb_color_t c)
{
    int16_t dx =  abs16(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int16_t dy = -abs16(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy;

    for (;;) {
        int16_t bx = x0 - 1, by = y0 - 1;
        if (bx < 0) bx = 0;
        if (by < CANVAS_Y0) by = CANVAS_Y0;
        if (bx > HB_SCREEN_W - 3) bx = HB_SCREEN_W - 3;
        if (by > CANVAS_Y1 - 3) by = CANVAS_Y1 - 3;
        hb_fill_rect(bx, by, 3, 3, c);

        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Draw an X made of two diagonal stripes centered at (cx, cy),
   each arm extending r pixels with a 3px-thick brush. */
static void draw_x(int16_t cx, int16_t cy, int16_t r, hb_color_t fg)
{
    for (int16_t i = -r; i <= r; i++) {
        hb_fill_rect(cx + i - 1, cy + i - 1, 3, 3, fg);
        hb_fill_rect(cx + i - 1, cy - i - 1, 3, 3, fg);
    }
}

static void draw_top_bar(hb_color_t current)
{
    /* Color indicator on the left */
    hb_fill_rect(0, 0, COLOR_IND_W, TOP_BAR_H, current);

    /* Bucket button: black bg with white 'F' (glyph 15) for Fill */
    hb_fill_rect(BUCKET_BTN_X, 0, BUCKET_BTN_W, TOP_BAR_H, HB_BLACK);
    int16_t fx = BUCKET_BTN_X + (BUCKET_BTN_W - HB_DIGIT_W) / 2;
    hb_draw_digit(fx, 0, 15, HB_WHITE, HB_BLACK);

    /* Quit button: red bg with white X */
    hb_fill_rect(QUIT_BTN_X, 0, QUIT_BTN_W, TOP_BAR_H, HB_RED);
    draw_x(QUIT_BTN_X + QUIT_BTN_W / 2, TOP_BAR_H / 2, 10, HB_WHITE);
}

static void draw_palette(void)
{
    for (int i = 0; i < PALETTE_COLS; i++) {
        hb_fill_rect(i * PALETTE_COL_W, PALETTE_Y0,
                     PALETTE_COL_W, PALETTE_H, palette[i]);
    }
    /* Thin white separators for the dark-on-dark swatches */
    for (int i = 1; i < PALETTE_COLS; i++) {
        hb_fill_rect(i * PALETTE_COL_W - 1, PALETTE_Y0, 1, PALETTE_H, HB_WHITE);
    }
}

static void clear_canvas(hb_color_t c)
{
    hb_fill_rect(0, CANVAS_Y0, HB_SCREEN_W, CANVAS_H, c);
}

HB_APP_ENTRY(payload_entry)
{
    hb_color_t current_color = HB_BLACK;
    hb_color_t canvas_bg     = HB_WHITE;

    /* Initial screen */
    hb_fill_screen(HB_WHITE);
    clear_canvas(canvas_bg);
    draw_top_bar(current_color);
    draw_palette();

    int16_t last_x = -1, last_y = -1;
    int16_t prev_t_x = -9999, prev_t_y = -9999;
    uint32_t prev_touch_id = 0xFFFFFFFFu;
    uint32_t frames_since_change = 0;
    (void)prev_touch_id;  /* silence unused if touch_id is always 0 */

    for (uint32_t frame = 0; frame < 5000000; frame++) {
        /* Either volume button exits cleanly to the Connected screen. */
        if (hb_button_pressed(HB_BTN_VOL_UP) ||
            hb_button_pressed(HB_BTN_VOL_DOWN)) {
            return;
        }
        hb_screenshot_poll_chord();

        hb_touch_drain_to_one();

        hb_touch_t t;
        bool got = hb_touch_get_coords(&t);
        bool changed = got && (t.x != prev_t_x || t.y != prev_t_y);

        if (changed) {
            frames_since_change = 0;
            prev_t_x = t.x;
            prev_t_y = t.y;

            /* New touchID = new stroke. Reset last so we don't connect
               the previous stroke's end-point to this stroke's start. */
            if (t.touch_id != prev_touch_id) {
                last_x = -1;
                last_y = -1;
                prev_touch_id = t.touch_id;
            }

            /* Also reset on a big jump (>60 px Manhattan), as a safety
               net for when status/id signals aren't sufficient. */
            if (last_x >= 0 &&
                (abs16(t.x - last_x) + abs16(t.y - last_y) > 60)) {
                last_x = -1;
                last_y = -1;
            }

            if (t.y < CANVAS_Y0) {
                /* Top bar: quit (right), bucket (middle), indicator (left) */
                if (t.x >= QUIT_BTN_X) {
                    return;
                } else if (t.x >= BUCKET_BTN_X) {
                    canvas_bg = current_color;
                    clear_canvas(canvas_bg);
                }
                last_x = -1; last_y = -1;
            } else if (t.y >= CANVAS_Y1) {
                /* Palette */
                int col = t.x / PALETTE_COL_W;
                if (col < 0) col = 0;
                if (col >= PALETTE_COLS) col = PALETTE_COLS - 1;
                current_color = palette[col];
                draw_top_bar(current_color);
                last_x = -1; last_y = -1;
            } else {
                /* Canvas: continue line from last, or seed a dot */
                if (last_x >= 0 && last_y >= 0) {
                    draw_line(last_x, last_y, t.x, t.y, current_color);
                } else {
                    hb_fill_rect(t.x - 1, t.y - 1, 3, 3, current_color);
                }
                last_x = t.x;
                last_y = t.y;
            }
        } else {
            /* No new coord — count "finger up" frames so the next
               tap starts a fresh stroke instead of connecting from
               wherever we left off. */
            /* Reset stroke after the OS clearly stopped updating
               (~75ms). Our poll loop runs much faster than the
               touch sampling rate (~16ms), so a small threshold
               here would falsely cut mid-drag strokes. */
            frames_since_change++;
            if (frames_since_change > 5000) {
                last_x = -1;
                last_y = -1;
            }
        }

        /* Pace */
        for (volatile uint32_t i = 0; i < 3000; i++) { }
    }
}
