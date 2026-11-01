/*
 * minesweeper — classic minesweeper on a canvas.
 *
 * 12 cols x 16 rows of 18-px cells (216 x 288 px playfield), 24 mines.
 * Single LVGL canvas instead of one-widget-per-cell — 192 cells would
 * blow the heap (see feedback_lvgl_widget_budget).
 *
 * Input: short tap reveals a cell; long-press (>= 400 ms) flags or
 * unflags it. Revealed cells display the count of adjacent mines (or
 * blank for 0); revealing a 0-count cell flood-fills outward through
 * neighboring 0-cells. Game ends on mine reveal (loss) or when all
 * non-mine cells are revealed (win).
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_glyph.h"
#include "lvgl/lvgl.h"

#define COLS         12
#define ROWS         16
#define CELL_PX      18
#define BOARD_PX_W   (COLS * CELL_PX)
#define BOARD_PX_H   (ROWS * CELL_PX)
#define N_MINES      24
#define HEADER_H     48

#define CANVAS_BYTES     (BOARD_PX_W * BOARD_PX_H * 4)
/* In the app's own arena: the old fixed 0x092E0000 lies inside the OS heap
 * and was overwriting live objects (and sometimes this app's own code). */
static uint8_t s_canvas_buf[BOARD_PX_W * BOARD_PX_H * 4] __attribute__((aligned(4)));

/* Each cell: low 4 bits = adjacent mine count (or 0xF for mine);
   higher bits = flags (revealed, flagged). */
#define C_COUNT_MASK  0x0F
#define C_IS_MINE     0x0F
#define C_REVEALED    0x10
#define C_FLAGGED     0x20

static uint8_t s_board[ROWS][COLS];
static int     s_n_revealed = 0;
static int     s_n_flagged  = 0;
static bool    s_dead = false;
static bool    s_won  = false;

static lv_obj_t *s_canvas;
static lv_obj_t *s_status_lbl;
static lv_obj_t *s_overlay;
static lv_obj_t *s_overlay_lbl;

/* Long-press detection */
static int      s_press_x, s_press_y;
static uint32_t s_press_t_ms;
static bool     s_pressing;
static bool     s_long_handled;

static uint32_t s_rng;
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

static void itoa_u(uint32_t v, char *out)
{
    char b[12]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    int k = 0; while (i) out[k++] = b[--i];
    out[k] = 0;
}

static uint32_t pack(uint32_t hex)
{
    uint8_t r=(hex>>16)&0xFF, g=(hex>>8)&0xFF, b=hex&0xFF;
    return ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)b;
}

static void canvas_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    uint32_t *buf = (uint32_t *)s_canvas_buf;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= BOARD_PX_H) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= BOARD_PX_W) continue;
            buf[yy * BOARD_PX_W + xx] = color;
        }
    }
}

/* Number colors (1..8). */
static const uint32_t s_num_color[9] = {
    0x000000, 0x1a73e8, 0x2bb673, 0xe63946,
    0x6c2bd9, 0xa64500, 0x008080, 0x000000, 0x555555,
};

static void draw_cell(int r, int c)
{
    int x = c * CELL_PX;
    int y = r * CELL_PX;
    uint8_t v = s_board[r][c];
    uint32_t bg;
    if (v & C_REVEALED) {
        if ((v & C_COUNT_MASK) == C_IS_MINE) bg = pack(0xe63946);
        else                                 bg = pack(0xc0c0c0);
    } else if (v & C_FLAGGED) {
        bg = pack(0x6b7280);
    } else {
        bg = pack(0x4a5060);
    }
    canvas_fill_rect(x, y, CELL_PX, CELL_PX, pack(0x111522));
    canvas_fill_rect(x + 1, y + 1, CELL_PX - 2, CELL_PX - 2, bg);

    if ((v & C_REVEALED) && (v & C_COUNT_MASK) != 0 &&
        (v & C_COUNT_MASK) != C_IS_MINE) {
        /* Numbered cell — draw the count via canvas label */
        int n = v & C_COUNT_MASK;
        char buf[2] = { (char)('0' + n), 0 };
        lv_layer_t layer; lv_canvas_init_layer(s_canvas, &layer);
        lv_draw_label_dsc_t dsc; lv_draw_label_dsc_init(&dsc);
        dsc.color = lv_color_hex(s_num_color[n]);
        dsc.font  = &lv_font_montserrat_14;
        dsc.text  = buf;
        dsc.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t a;
        a.x1 = x + 1; a.y1 = y + 1;
        a.x2 = x + CELL_PX - 1; a.y2 = y + CELL_PX - 1;
        lv_draw_label(&layer, &dsc, &a);
        lv_canvas_finish_layer(s_canvas, &layer);
    }
    if (!(v & C_REVEALED) && (v & C_FLAGGED)) {
        /* Flag glyph */
        lv_layer_t layer; lv_canvas_init_layer(s_canvas, &layer);
        hb_glyph_draw(&layer, &hb_glyph_flag, x + CELL_PX / 2, y + CELL_PX / 2,
                      CELL_PX - 4, lv_color_hex(hb_color_danger()), false, lv_color_black());
        lv_canvas_finish_layer(s_canvas, &layer);
    }
    if ((v & C_REVEALED) && (v & C_COUNT_MASK) == C_IS_MINE) {
        lv_layer_t layer; lv_canvas_init_layer(s_canvas, &layer);
        hb_glyph_draw(&layer, &hb_glyph_bomb, x + CELL_PX / 2, y + CELL_PX / 2,
                      CELL_PX - 4, lv_color_hex(0x000000), false, lv_color_black());
        lv_canvas_finish_layer(s_canvas, &layer);
    }
}

static void draw_all(void)
{
    canvas_fill_rect(0, 0, BOARD_PX_W, BOARD_PX_H, pack(0x111522));
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) draw_cell(r, c);
    lv_obj_invalidate(s_canvas);
}

static void refresh_status(void)
{
    char buf[32]; int k = 0;
    const char *p = LV_SYMBOL_BELL " "; while (*p) buf[k++] = *p++;
    char nb[12]; itoa_u((uint32_t)(N_MINES - s_n_flagged), nb);
    for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    p = "  Found "; while (*p) buf[k++] = *p++;
    itoa_u((uint32_t)s_n_revealed, nb);
    for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    buf[k] = 0;
    lv_label_set_text(s_status_lbl, buf);
}

static void show_overlay(const char *text)
{
    lv_label_set_text(s_overlay_lbl, text);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void seed_board(int avoid_r, int avoid_c)
{
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) s_board[r][c] = 0;
    int placed = 0;
    while (placed < N_MINES) {
        int r = (int)(rnd() % ROWS);
        int c = (int)(rnd() % COLS);
        if (r == avoid_r && c == avoid_c) continue;     /* first-tap safety */
        if ((s_board[r][c] & C_COUNT_MASK) == C_IS_MINE) continue;
        s_board[r][c] = C_IS_MINE;
        placed++;
    }
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
        if ((s_board[r][c] & C_COUNT_MASK) == C_IS_MINE) continue;
        int n = 0;
        for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = r + dr, nc = c + dc;
            if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) continue;
            if ((s_board[nr][nc] & C_COUNT_MASK) == C_IS_MINE) n++;
        }
        s_board[r][c] = (uint8_t)n;
    }
}

/* Flood-fill reveal starting at (r,c). Iterative to avoid deep stack. */
static void flood_reveal(int r0, int c0)
{
    static int stack_r[ROWS * COLS];
    static int stack_c[ROWS * COLS];
    int sp = 0;
    stack_r[sp] = r0; stack_c[sp] = c0; sp++;
    while (sp > 0) {
        sp--;
        int r = stack_r[sp];
        int c = stack_c[sp];
        if (r < 0 || r >= ROWS || c < 0 || c >= COLS) continue;
        uint8_t v = s_board[r][c];
        if (v & C_REVEALED) continue;
        if (v & C_FLAGGED) continue;
        if ((v & C_COUNT_MASK) == C_IS_MINE) continue;
        s_board[r][c] |= C_REVEALED;
        s_n_revealed++;
        if ((v & C_COUNT_MASK) == 0) {
            for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                stack_r[sp] = r + dr; stack_c[sp] = c + dc; sp++;
            }
        }
    }
}

static void check_win(void)
{
    if (s_n_revealed >= ROWS * COLS - N_MINES) {
        s_won = true;
        show_overlay("You win!\nTap to retry");
    }
}

static bool s_first_tap = true;

static void reveal(int r, int c)
{
    if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
    uint8_t v = s_board[r][c];
    if (v & (C_REVEALED | C_FLAGGED)) return;
    if (s_first_tap) {
        seed_board(r, c);
        s_first_tap = false;
    }
    if ((s_board[r][c] & C_COUNT_MASK) == C_IS_MINE) {
        s_board[r][c] |= C_REVEALED;
        s_dead = true;
        /* Reveal all mines */
        for (int rr = 0; rr < ROWS; rr++) for (int cc = 0; cc < COLS; cc++) {
            if ((s_board[rr][cc] & C_COUNT_MASK) == C_IS_MINE) {
                s_board[rr][cc] |= C_REVEALED;
            }
        }
        show_overlay("Boom!\nTap to retry");
    } else {
        flood_reveal(r, c);
        check_win();
    }
    draw_all();
    refresh_status();
}

static void toggle_flag(int r, int c)
{
    if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
    if (s_board[r][c] & C_REVEALED) return;
    if (s_board[r][c] & C_FLAGGED) {
        s_board[r][c] &= ~C_FLAGGED;
        s_n_flagged--;
    } else {
        s_board[r][c] |= C_FLAGGED;
        s_n_flagged++;
    }
    draw_cell(r, c);
    lv_obj_invalidate(s_canvas);
    refresh_status();
}

static void reset_game(void)
{
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) s_board[r][c] = 0;
    s_n_revealed = 0;
    s_n_flagged = 0;
    s_dead = false;
    s_won = false;
    s_first_tap = true;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    draw_all();
    refresh_status();
}

/* ---- input ---- */

static void on_press(lv_event_t *e)
{
    (void)e;
    if (s_dead || s_won) {
        reset_game();
        return;
    }
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    s_press_x = p.x;
    s_press_y = p.y;
    s_press_t_ms = hb_time_uptime_ms();
    s_pressing = true;
    s_long_handled = false;
}

static void on_release(lv_event_t *e)
{
    (void)e;
    if (!s_pressing) return;
    s_pressing = false;
    if (s_long_handled) return;            /* long-press already fired */
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    int dx = p.x - s_press_x;
    int dy = p.y - s_press_y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (adx > 12 || ady > 12) return;      /* moved → ignore */
    lv_area_t area; lv_obj_get_coords(s_canvas, &area);
    int cx = p.x - area.x1;
    int cy = p.y - area.y1;
    if (cx < 0 || cy < 0 || cx >= BOARD_PX_W || cy >= BOARD_PX_H) return;
    int c = cx / CELL_PX, r = cy / CELL_PX;
    reveal(r, c);
}

/* Frame tick: watch for long-press to fire while the finger is still
   down. We don't have PRESS_LOST or HOLD events wired cleanly, so we
   poll the indev state and timestamp here. */
static void on_frame(void)
{
    if (!s_pressing || s_long_handled) return;
    uint32_t now = hb_time_uptime_ms();
    if ((uint32_t)(now - s_press_t_ms) < 400u) return;
    /* Long press fired — treat as flag */
    s_long_handled = true;
    lv_area_t area; lv_obj_get_coords(s_canvas, &area);
    int cx = s_press_x - area.x1;
    int cy = s_press_y - area.y1;
    if (cx < 0 || cy < 0 || cx >= BOARD_PX_W || cy >= BOARD_PX_H) return;
    int c = cx / CELL_PX, r = cy / CELL_PX;
    toggle_flag(r, c);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    s_rng = hb_time_uptime_us() | 1u;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Minesweeper");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, LV_SYMBOL_BELL " 24  Found 0");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 28);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, BOARD_PX_W, BOARD_PX_H,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(s_canvas, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_press,   LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_canvas, on_release, LV_EVENT_RELEASED, NULL);

    s_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_overlay, 200, 130);
    lv_obj_center(s_overlay);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_border_width(s_overlay, 2, 0);
    lv_obj_set_style_radius(s_overlay, 12, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_overlay_lbl = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(s_overlay_lbl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_overlay_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_overlay_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_overlay_lbl);

    reset_game();
    hb_lv_set_frame_cb(on_frame);
}
