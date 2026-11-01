/*
 * connect4 — drop discs into a 7-col x 6-row board; first to 4-in-
 * a-row (horizontal, vertical, or diagonal) wins.
 *
 * Player is yellow (goes first). AI is red — a fixed-depth 5-ply
 * minimax with simple positional scoring (center column favored,
 * 3-in-a-row threats heavily weighted). Plays reasonably well
 * without being unbeatable.
 *
 * Tap a column to drop a disc into it.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define COLS    7
#define ROWS    6
#define CELL_PX 30
#define BOARD_W (COLS * CELL_PX)   /* 210 */
#define BOARD_H (ROWS * CELL_PX)   /* 180 */
#define HEADER_H 60

#define EMPTY 0
#define HUMAN 1
#define AI    2

/* In the app's own arena: the old fixed 0x092E0000 lies inside the OS heap
 * and was overwriting live objects (and sometimes this app's own code). */
static uint8_t s_canvas_buf[BOARD_W * BOARD_H * 4] __attribute__((aligned(4)));

static int8_t s_board[ROWS][COLS];
static int    s_winner = 0;     /* 0 = ongoing, 1 = human, 2 = AI, 3 = draw */

static lv_obj_t *s_canvas;
static lv_obj_t *s_status_lbl;

static uint32_t pack(uint32_t hex)
{
    uint8_t r=(hex>>16)&0xFF, g=(hex>>8)&0xFF, b=hex&0xFF;
    return ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)b;
}
static void cfill(int x, int y, int w, int h, uint32_t color)
{
    uint32_t *buf = (uint32_t *)s_canvas_buf;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= BOARD_H) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= BOARD_W) continue;
            buf[yy * BOARD_W + xx] = color;
        }
    }
}
static void cdisc(int cx, int cy, int r, uint32_t color)
{
    int r2 = r * r;
    uint32_t *buf = (uint32_t *)s_canvas_buf;
    for (int dy = -r; dy <= r; dy++) {
        int yy = cy + dy;
        if (yy < 0 || yy >= BOARD_H) continue;
        for (int dx = -r; dx <= r; dx++) {
            int xx = cx + dx;
            if (xx < 0 || xx >= BOARD_W) continue;
            if (dx*dx + dy*dy <= r2) buf[yy * BOARD_W + xx] = color;
        }
    }
}

static int check_win(const int8_t b[ROWS][COLS], int *out_who)
{
    /* Horizontal, vertical, two diagonals. */
    for (int r = 0; r < ROWS; r++) for (int c = 0; c <= COLS - 4; c++) {
        int v = b[r][c]; if (!v) continue;
        if (v == b[r][c+1] && v == b[r][c+2] && v == b[r][c+3]) {
            if (out_who) *out_who = v; return v;
        }
    }
    for (int r = 0; r <= ROWS - 4; r++) for (int c = 0; c < COLS; c++) {
        int v = b[r][c]; if (!v) continue;
        if (v == b[r+1][c] && v == b[r+2][c] && v == b[r+3][c]) {
            if (out_who) *out_who = v; return v;
        }
    }
    for (int r = 0; r <= ROWS - 4; r++) for (int c = 0; c <= COLS - 4; c++) {
        int v = b[r][c]; if (!v) continue;
        if (v == b[r+1][c+1] && v == b[r+2][c+2] && v == b[r+3][c+3]) {
            if (out_who) *out_who = v; return v;
        }
    }
    for (int r = 3; r < ROWS; r++) for (int c = 0; c <= COLS - 4; c++) {
        int v = b[r][c]; if (!v) continue;
        if (v == b[r-1][c+1] && v == b[r-2][c+2] && v == b[r-3][c+3]) {
            if (out_who) *out_who = v; return v;
        }
    }
    int any_empty = 0;
    for (int c = 0; c < COLS; c++) if (b[0][c] == 0) { any_empty = 1; break; }
    if (!any_empty) return 3;
    return 0;
}

/* Returns the row a disc would land in column c, or -1 if column full. */
static int drop_row(const int8_t b[ROWS][COLS], int c)
{
    for (int r = ROWS - 1; r >= 0; r--) if (b[r][c] == 0) return r;
    return -1;
}

/* Score a board from AI's perspective. Larger = better for AI. */
static int score_window(int a, int b, int c, int d, int who)
{
    int oc = 0, sc = 0, ec = 0;
    int vals[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) {
        if (vals[i] == who)            sc++;
        else if (vals[i] == 0)         ec++;
        else                           oc++;
    }
    if (oc > 0 && sc > 0) return 0;     /* mixed → no advantage either way */
    if (sc == 4) return 100;
    if (sc == 3 && ec == 1) return 5;
    if (sc == 2 && ec == 2) return 2;
    if (oc == 3 && ec == 1) return -4;  /* block opponent's 3 */
    return 0;
}

static int score_board(const int8_t b[ROWS][COLS], int who)
{
    int s = 0;
    /* center bonus */
    for (int r = 0; r < ROWS; r++) if (b[r][3] == who) s += 3;
    for (int r = 0; r < ROWS; r++) for (int c = 0; c <= COLS - 4; c++)
        s += score_window(b[r][c], b[r][c+1], b[r][c+2], b[r][c+3], who);
    for (int r = 0; r <= ROWS - 4; r++) for (int c = 0; c < COLS; c++)
        s += score_window(b[r][c], b[r+1][c], b[r+2][c], b[r+3][c], who);
    for (int r = 0; r <= ROWS - 4; r++) for (int c = 0; c <= COLS - 4; c++)
        s += score_window(b[r][c], b[r+1][c+1], b[r+2][c+2], b[r+3][c+3], who);
    for (int r = 3; r < ROWS; r++) for (int c = 0; c <= COLS - 4; c++)
        s += score_window(b[r][c], b[r-1][c+1], b[r-2][c+2], b[r-3][c+3], who);
    return s;
}

/* Minimax with alpha-beta. Depth-limited (5 ply). */
static int minimax(int8_t b[ROWS][COLS], int depth, int alpha, int beta, int player)
{
    int who = 0; int w = check_win(b, &who);
    if (w == AI)    return 1000000 - (5 - depth);
    if (w == HUMAN) return -1000000 + (5 - depth);
    if (w == 3 || depth == 0) return score_board(b, AI) - score_board(b, HUMAN);
    if (player == AI) {
        int v = -2000000;
        for (int c = 0; c < COLS; c++) {
            int r = drop_row(b, c);
            if (r < 0) continue;
            b[r][c] = (int8_t)AI;
            int s = minimax(b, depth - 1, alpha, beta, HUMAN);
            b[r][c] = 0;
            if (s > v) v = s;
            if (v > alpha) alpha = v;
            if (alpha >= beta) break;
        }
        return v;
    } else {
        int v = 2000000;
        for (int c = 0; c < COLS; c++) {
            int r = drop_row(b, c);
            if (r < 0) continue;
            b[r][c] = (int8_t)HUMAN;
            int s = minimax(b, depth - 1, alpha, beta, AI);
            b[r][c] = 0;
            if (s < v) v = s;
            if (v < beta) beta = v;
            if (alpha >= beta) break;
        }
        return v;
    }
}

static int best_ai_col(void)
{
    int8_t copy[ROWS][COLS];
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) copy[r][c] = s_board[r][c];
    int best_s = -2000000, best_c = -1;
    /* Center-out search order so on equal scores we prefer middle. */
    static const int order[7] = { 3, 2, 4, 1, 5, 0, 6 };
    for (int oi = 0; oi < 7; oi++) {
        int c = order[oi];
        int r = drop_row(copy, c);
        if (r < 0) continue;
        copy[r][c] = (int8_t)AI;
        int s = minimax(copy, 4, -2000000, 2000000, HUMAN);
        copy[r][c] = 0;
        if (s > best_s) { best_s = s; best_c = c; }
    }
    return best_c;
}

static void render(void)
{
    cfill(0, 0, BOARD_W, BOARD_H, pack(0x14213d));
    int r2 = (CELL_PX / 2) - 2;
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
        int cx = c * CELL_PX + CELL_PX / 2;
        int cy = r * CELL_PX + CELL_PX / 2;
        cdisc(cx, cy, r2, pack(0x0a0e1a));
        int v = s_board[r][c];
        if (v == HUMAN) cdisc(cx, cy, r2 - 2, pack(0xfcbf49));
        else if (v == AI) cdisc(cx, cy, r2 - 2, pack(0xe63946));
    }
    lv_obj_invalidate(s_canvas);
}

static void refresh_status(void)
{
    if (s_winner == HUMAN) lv_label_set_text(s_status_lbl, "You win!  Tap to play again");
    else if (s_winner == AI) lv_label_set_text(s_status_lbl, "AI wins.  Tap to play again");
    else if (s_winner == 3)  lv_label_set_text(s_status_lbl, "Draw.  Tap to play again");
    else lv_label_set_text(s_status_lbl, "Tap a column to drop");
}

static void reset_round(void)
{
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) s_board[r][c] = 0;
    s_winner = 0;
}

static void on_press(lv_event_t *e)
{
    (void)e;
    if (s_winner) { reset_round(); render(); refresh_status(); return; }
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    lv_area_t area; lv_obj_get_coords(s_canvas, &area);
    int cx = p.x - area.x1;
    if (cx < 0 || cx >= BOARD_W) return;
    int col = cx / CELL_PX;
    int row = drop_row(s_board, col);
    if (row < 0) return;
    s_board[row][col] = HUMAN;
    int who = 0;
    if (check_win(s_board, &who)) { s_winner = who; render(); refresh_status(); return; }
    /* AI move */
    int ac = best_ai_col();
    if (ac >= 0) {
        int ar = drop_row(s_board, ac);
        if (ar >= 0) s_board[ar][ac] = AI;
        if (check_win(s_board, &who)) s_winner = who;
    }
    render();
    refresh_status();
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Connect Four");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, BOARD_W, BOARD_H,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_press, LV_EVENT_PRESSED, NULL);

    reset_round();
    render();
    refresh_status();

}
