/*
 * sudoku — 9x9 sudoku with a small built-in puzzle bank.
 *
 * Board is drawn on a single 234x234 lv_canvas (one widget instead of
 * 81). Tap a cell to select it (highlight); below the board is a
 * 1-9 + erase keypad — tap a digit to place it in the selected cell.
 * Given cells (from the puzzle) are bold-black and can't be edited;
 * user-entered cells are blue. Mistakes (placement that violates
 * row/col/box) shown in red.
 *
 * Puzzles stored as 81-char strings: 1-9 for given digits, 0 for
 * blanks. Rotated through on each "New game".
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define BOARD_PX     234
#define CELL_PX      26                /* 26 * 9 = 234 */
#define HEADER_H     30
#define KEYPAD_H     120
#define CANVAS_BYTES (BOARD_PX * BOARD_PX * 4)
/* In the app's own arena: the old fixed 0x092E0000 lies inside the OS heap
 * and was overwriting live objects (and sometimes this app's own code). */
static uint8_t s_canvas_buf[BOARD_PX * BOARD_PX * 4] __attribute__((aligned(4)));

/* board[r][c]: 0 = empty, 1..9 = digit. `given[r][c]` true if from
   the puzzle (immutable). */
static uint8_t s_board[9][9];
static bool    s_given[9][9];
static int     s_sel_r = -1, s_sel_c = -1;

/* Built-in puzzles. Each is 81 chars, row-major. 0 = blank. */
static const char *s_puzzles[] = {
    /* easy */
    "530070000600195000098000060800060003400803001700020006060000280000419005000080079",
    /* medium */
    "000260701680070090190004500820100040004602900050003028009300074040050036703018000",
    /* hard */
    "000000907000420180000705026100904000050000040000507009920108000034059000507000000",
    /* very hard */
    "030000000000000000005400602701002000000060000000300508209006700000000000000000040",
};
#define N_PUZZLES  (int)(sizeof s_puzzles / sizeof s_puzzles[0])
static int s_puzzle_idx = 0;

static lv_obj_t *s_canvas;
static lv_obj_t *s_status_lbl;
static lv_obj_t *s_keys[10];          /* 1..9 + erase */

/* ---- canvas helpers ---- */

static uint32_t pack(uint32_t hex)
{
    uint8_t r=(hex>>16)&0xFF, g=(hex>>8)&0xFF, b=hex&0xFF;
    return ((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
}

static void canvas_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    uint32_t *buf = (uint32_t *)s_canvas_buf;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= BOARD_PX) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= BOARD_PX) continue;
            buf[yy * BOARD_PX + xx] = color;
        }
    }
}

/* ---- validity ---- */

static bool placement_ok(int r, int c, int n)
{
    if (n == 0) return true;
    for (int i = 0; i < 9; i++) {
        if (i != c && s_board[r][i] == n) return false;
        if (i != r && s_board[i][c] == n) return false;
    }
    int br = (r / 3) * 3, bc = (c / 3) * 3;
    for (int rr = br; rr < br + 3; rr++) for (int cc = bc; cc < bc + 3; cc++) {
        if (rr == r && cc == c) continue;
        if (s_board[rr][cc] == n) return false;
    }
    return true;
}

static bool is_solved(void)
{
    for (int r = 0; r < 9; r++) for (int c = 0; c < 9; c++) {
        int n = s_board[r][c];
        if (n == 0) return false;
        if (!placement_ok(r, c, n)) return false;
    }
    return true;
}

/* ---- render ---- */

static void render(void)
{
    /* Light background, dark grid lines. */
    canvas_fill_rect(0, 0, BOARD_PX, BOARD_PX, pack(0xf5f5f0));
    /* Selected cell highlight */
    if (s_sel_r >= 0 && s_sel_c >= 0) {
        canvas_fill_rect(s_sel_c * CELL_PX, s_sel_r * CELL_PX,
                         CELL_PX, CELL_PX, pack(0xffdf80));
    }
    /* Same-digit highlight for the selected cell's digit */
    if (s_sel_r >= 0 && s_sel_c >= 0 && s_board[s_sel_r][s_sel_c] != 0) {
        int n = s_board[s_sel_r][s_sel_c];
        for (int r = 0; r < 9; r++) for (int c = 0; c < 9; c++) {
            if (s_board[r][c] == n && (r != s_sel_r || c != s_sel_c)) {
                canvas_fill_rect(c * CELL_PX, r * CELL_PX,
                                 CELL_PX, CELL_PX, pack(0xf2e9c4));
            }
        }
    }
    /* Grid lines: thin gray, plus thick black at 3-cell boundaries. */
    for (int i = 0; i <= 9; i++) {
        bool thick = (i % 3 == 0);
        int t = thick ? 2 : 1;
        canvas_fill_rect(0, i * CELL_PX - (i == 9 ? t : 0),
                         BOARD_PX, t, pack(thick ? 0x111111 : 0x999999));
        canvas_fill_rect(i * CELL_PX - (i == 9 ? t : 0), 0,
                         t, BOARD_PX, pack(thick ? 0x111111 : 0x999999));
    }
    /* Digits via canvas label */
    lv_layer_t layer; lv_canvas_init_layer(s_canvas, &layer);
    for (int r = 0; r < 9; r++) for (int c = 0; c < 9; c++) {
        int n = s_board[r][c];
        if (n == 0) continue;
        char buf[2] = { (char)('0' + n), 0 };
        lv_draw_label_dsc_t dsc; lv_draw_label_dsc_init(&dsc);
        if (s_given[r][c]) {
            dsc.color = lv_color_hex(0x111111);
        } else if (!placement_ok(r, c, n)) {
            dsc.color = lv_color_hex(hb_color_danger());
        } else {
            dsc.color = lv_color_hex(0x1a73e8);
        }
        dsc.font = &lv_font_montserrat_20;
        dsc.text = buf;
        dsc.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t a;
        a.x1 = c * CELL_PX + 1; a.y1 = r * CELL_PX + 1;
        a.x2 = c * CELL_PX + CELL_PX - 1; a.y2 = r * CELL_PX + CELL_PX - 1;
        lv_draw_label(&layer, &dsc, &a);
    }
    lv_canvas_finish_layer(s_canvas, &layer);
    lv_obj_invalidate(s_canvas);
}

static void refresh_status(void)
{
    if (is_solved()) {
        lv_label_set_text(s_status_lbl, "Solved! Tap New for another");
    } else {
        char buf[40]; int k = 0;
        const char *p = "Puzzle "; while (*p) buf[k++] = *p++;
        buf[k++] = '0' + (s_puzzle_idx + 1);
        p = " of "; while (*p) buf[k++] = *p++;
        buf[k++] = '0' + N_PUZZLES;
        buf[k] = 0;
        lv_label_set_text(s_status_lbl, buf);
    }
}

/* ---- input ---- */

static void on_canvas_press(lv_event_t *e)
{
    (void)e;
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    lv_area_t area; lv_obj_get_coords(s_canvas, &area);
    int cx = p.x - area.x1;
    int cy = p.y - area.y1;
    if (cx < 0 || cy < 0 || cx >= BOARD_PX || cy >= BOARD_PX) return;
    s_sel_r = cy / CELL_PX;
    s_sel_c = cx / CELL_PX;
    render();
}

static void on_key(lv_event_t *e)
{
    int n = (int)(uintptr_t)lv_event_get_user_data(e);   /* 0..9 (0 = erase) */
    if (s_sel_r < 0 || s_sel_c < 0) return;
    if (s_given[s_sel_r][s_sel_c]) return;
    s_board[s_sel_r][s_sel_c] = (uint8_t)n;
    render();
    refresh_status();
}

static void load_puzzle(int idx)
{
    const char *p = s_puzzles[idx];
    for (int i = 0; i < 81; i++) {
        int r = i / 9, c = i % 9;
        char ch = p[i];
        int n = (ch >= '1' && ch <= '9') ? (ch - '0') : 0;
        s_board[r][c] = (uint8_t)n;
        s_given[r][c] = (n != 0);
    }
    s_sel_r = s_sel_c = -1;
    render();
    refresh_status();
}

static void on_new(lv_event_t *e)
{
    (void)e;
    s_puzzle_idx = (s_puzzle_idx + 1) % N_PUZZLES;
    load_puzzle(s_puzzle_idx);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Header */
    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_LEFT, 8, 8);

    lv_obj_t *btn_new = lv_button_create(scr);
    lv_obj_set_size(btn_new, 60, 24);
    lv_obj_align(btn_new, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_set_style_bg_color(btn_new, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(btn_new, on_new, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(btn_new);
    lv_label_set_text(nl, "New");
    lv_obj_set_style_text_color(nl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(nl);

    /* Canvas */
    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, BOARD_PX, BOARD_PX,
                         LV_COLOR_FORMAT_XRGB8888);
    /* The default object style adds padding + a border; without these resets the
       canvas content area is inset and the widget draws a frame around the 234px
       board, which reads as stray grid lines "outside" it. */
    lv_obj_set_size(s_canvas, BOARD_PX, BOARD_PX);
    lv_obj_set_style_pad_all(s_canvas, 0, 0);
    lv_obj_set_style_border_width(s_canvas, 0, 0);
    lv_obj_set_style_outline_width(s_canvas, 0, 0);
    lv_obj_set_style_radius(s_canvas, 0, 0);
    lv_obj_align(s_canvas, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_canvas_press, LV_EVENT_PRESSED, NULL);

    /* Keypad: 1-9 + erase */
    int kp_y = HEADER_H + BOARD_PX + 8;
    int btn_w = 44, btn_h = 36, gap = 4;
    int row_w = 5 * btn_w + 4 * gap;
    int start_x = (240 - row_w) / 2;
    for (int i = 0; i < 10; i++) {
        int col = i % 5;
        int row = i / 5;
        int x = start_x + col * (btn_w + gap);
        int y = kp_y + row * (btn_h + gap);
        lv_obj_t *b = lv_button_create(scr);
        lv_obj_set_size(b, btn_w, btn_h);
        lv_obj_set_pos(b, x, y);
        if (i == 9) {
            lv_obj_set_style_bg_color(b, lv_color_hex(hb_color_text_dim()), 0);
            lv_obj_add_event_cb(b, on_key, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)0);   /* erase = 0 */
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, LV_SYMBOL_BACKSPACE);
            lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
            lv_obj_center(l);
        } else {
            int n = i + 1;
            lv_obj_set_style_bg_color(b, lv_color_hex(hb_color_surface()), 0);
            lv_obj_add_event_cb(b, on_key, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)n);
            lv_obj_t *l = lv_label_create(b);
            char buf[2] = { (char)('0' + n), 0 };
            lv_label_set_text(l, buf);
            lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
            lv_obj_center(l);
        }
        s_keys[i] = b;
    }

    load_puzzle(0);
}
