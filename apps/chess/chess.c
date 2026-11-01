/*
 * chess — minimalist chess with vs-AI and vs-Human modes.
 *
 * Board: 8x8 canvas, 26-px cells (208 total, centered in 240).
 *
 * Tap a piece (your color) to select; legal destinations are
 * highlighted; tap one to move. Game ends when a king is captured
 * (no formal checkmate detection in v1 — simpler and still playable).
 *
 * AI: depth-3 minimax with material + simple positional scoring.
 * Castling, en passant, and promotion-choice are deferred — pawns
 * reaching the back rank auto-promote to queens.
 *
 * Mode toggle in the header switches between vs-AI (you play white)
 * and vs-Human (alternating taps from one device).
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_glyph.h"
#include "lvgl/lvgl.h"

#define CELL_PX  26
#define BOARD_W  (8 * CELL_PX)     /* 208 */
#define BOARD_H  BOARD_W
#define HEADER_H 36
/* In the app's own arena: the old fixed 0x092E0000 lies inside the OS heap
 * and was overwriting live objects (and sometimes this app's own code). */
static uint8_t s_canvas_buf[BOARD_W * BOARD_H * 4] __attribute__((aligned(4)));

/* Piece codes. Positive = white, negative = black. */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

static int8_t s_board[8][8];
static int    s_turn = 1;            /* 1 = white, -1 = black */
static int    s_sel_r = -1, s_sel_c = -1;
static int    s_winner = 0;          /* 0 ongoing; 1=white; -1=black */
typedef enum { MODE_AI, MODE_HUMAN } mode_t;
static mode_t s_mode = MODE_AI;

#define MAX_MOVES 64
typedef struct { int r, c; } pos_t;
static pos_t s_legal[MAX_MOVES];
static int   s_n_legal = 0;

static lv_obj_t *s_canvas;
static lv_obj_t *s_status_lbl;
static lv_obj_t *s_mode_btn_lbl;

/* ---- canvas helpers ---- */

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

/* Piece type (1..6) -> its pre-rendered A8 glyph bitmap (sdk/generated/hb_glyphs). */
static const lv_image_dsc_t *piece_img(int a)
{
    switch (a) {
        case PAWN:   return &hb_glyph_pawn;
        case KNIGHT: return &hb_glyph_knight;
        case BISHOP: return &hb_glyph_bishop;
        case ROOK:   return &hb_glyph_rook;
        case QUEEN:  return &hb_glyph_queen;
        case KING:   return &hb_glyph_king;
    }
    return NULL;
}

/* ---- moves ---- */

static bool in_board(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }
static int  sign_of(int p) { return p > 0 ? 1 : (p < 0 ? -1 : 0); }

static void add_move(int r, int c) { if (s_n_legal < MAX_MOVES) { s_legal[s_n_legal].r = r; s_legal[s_n_legal].c = c; s_n_legal++; } }

/* Generate legal moves for piece at (r, c). Doesn't fully check
   for self-checks (king-into-check tolerated) — good enough for a
   casual game; the AI's eval punishes hanging the king anyway. */
static void gen_moves(int r, int c, pos_t *out, int *n_out)
{
    int saved_n = s_n_legal;
    pos_t *saved_list = NULL;
    if (out != s_legal) {
        s_n_legal = 0;       /* reuse s_legal as temp */
    } else {
        s_n_legal = 0;
    }
    (void)saved_n; (void)saved_list;

    int p = s_board[r][c];
    if (p == 0) { *n_out = 0; return; }
    int sgn = sign_of(p);
    int abp = p < 0 ? -p : p;

    if (abp == PAWN) {
        int dir = (p > 0) ? -1 : 1;     /* white moves toward r=0 */
        int start_r = (p > 0) ? 6 : 1;
        /* one step */
        int nr = r + dir;
        if (in_board(nr, c) && s_board[nr][c] == 0) {
            add_move(nr, c);
            /* two steps from start row */
            int nr2 = r + 2 * dir;
            if (r == start_r && in_board(nr2, c) && s_board[nr2][c] == 0) add_move(nr2, c);
        }
        /* captures */
        for (int dc = -1; dc <= 1; dc += 2) {
            int tc = c + dc;
            if (in_board(nr, tc) && s_board[nr][tc] != 0 && sign_of(s_board[nr][tc]) != sgn) {
                add_move(nr, tc);
            }
        }
    } else if (abp == KNIGHT) {
        static const int kn[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for (int i = 0; i < 8; i++) {
            int nr = r + kn[i][0], nc = c + kn[i][1];
            if (!in_board(nr, nc)) continue;
            if (s_board[nr][nc] == 0 || sign_of(s_board[nr][nc]) != sgn) add_move(nr, nc);
        }
    } else if (abp == BISHOP || abp == ROOK || abp == QUEEN) {
        /* clockwise, so even = diagonals (bishop), odd = straight lines (rook) */
        static const int dirs8[8][2] = {{-1,-1},{-1,0},{-1,1},{0,1},{1,1},{1,0},{1,-1},{0,-1}};
        int start = (abp == BISHOP) ? 0 : (abp == ROOK ? 1 : 0);
        int step  = (abp == BISHOP) ? 2 : (abp == ROOK ? 2 : 1);
        for (int i = start; i < 8; i += step) {
            int dr = dirs8[i][0], dc = dirs8[i][1];
            for (int s = 1; s < 8; s++) {
                int nr = r + s * dr, nc = c + s * dc;
                if (!in_board(nr, nc)) break;
                if (s_board[nr][nc] == 0) { add_move(nr, nc); continue; }
                if (sign_of(s_board[nr][nc]) != sgn) add_move(nr, nc);
                break;
            }
        }
    } else if (abp == KING) {
        for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = r + dr, nc = c + dc;
            if (!in_board(nr, nc)) continue;
            if (s_board[nr][nc] == 0 || sign_of(s_board[nr][nc]) != sgn) add_move(nr, nc);
        }
    }
    *n_out = s_n_legal;
}

/* ---- AI ---- */

static const int s_material[7] = { 0, 100, 320, 330, 500, 900, 20000 };

static int evaluate(void)
{
    int s = 0;
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
        int p = s_board[r][c];
        if (!p) continue;
        int v = s_material[p < 0 ? -p : p];
        s += (p > 0 ? v : -v);
        /* Small center bonus for minor pieces. */
        if ((p == KNIGHT || p == BISHOP || p == -KNIGHT || p == -BISHOP) &&
            r >= 2 && r <= 5 && c >= 2 && c <= 5) s += (p > 0 ? 10 : -10);
    }
    return s;
}

/* Move = from(r,c) → to(r,c). The AI iterates all (from, to) pairs
   for its color and recurses. */
static int negamax(int depth, int color, int alpha, int beta)
{
    if (depth == 0) return color * evaluate();
    int best = -1000000;
    pos_t moves[MAX_MOVES]; int nm;
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
        int p = s_board[r][c];
        if (p == 0 || sign_of(p) != color) continue;
        gen_moves(r, c, moves, &nm);
        /* We modified s_legal; copy out so recursion doesn't trash it. */
        pos_t local[MAX_MOVES];
        for (int i = 0; i < nm; i++) local[i] = s_legal[i];
        for (int i = 0; i < nm; i++) {
            int tr = local[i].r, tc = local[i].c;
            int captured = s_board[tr][tc];
            /* If we'd capture the opponent's king, prune fast. */
            if (captured == -color * KING) return 30000 - depth;
            int saved_from = s_board[r][c];
            s_board[tr][tc] = s_board[r][c];
            s_board[r][c] = 0;
            /* Auto-promote pawn to queen on back rank. */
            if (saved_from == color * PAWN && (tr == 0 || tr == 7)) {
                s_board[tr][tc] = (int8_t)(color * QUEEN);
            }
            int s = -negamax(depth - 1, -color, -beta, -alpha);
            s_board[r][c] = saved_from;
            s_board[tr][tc] = (int8_t)captured;
            if (s > best) best = s;
            if (best > alpha) alpha = best;
            if (alpha >= beta) return best;
        }
    }
    return best == -1000000 ? color * evaluate() : best;
}

static void ai_play(int color)
{
    int best_s = -1000000;
    int bf_r = -1, bf_c = -1, bt_r = -1, bt_c = -1;
    pos_t moves[MAX_MOVES]; int nm;
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
        int p = s_board[r][c];
        if (p == 0 || sign_of(p) != color) continue;
        gen_moves(r, c, moves, &nm);
        pos_t local[MAX_MOVES];
        for (int i = 0; i < nm; i++) local[i] = s_legal[i];
        for (int i = 0; i < nm; i++) {
            int tr = local[i].r, tc = local[i].c;
            int captured = s_board[tr][tc];
            int saved_from = s_board[r][c];
            s_board[tr][tc] = s_board[r][c]; s_board[r][c] = 0;
            if (saved_from == color * PAWN && (tr == 0 || tr == 7))
                s_board[tr][tc] = (int8_t)(color * QUEEN);
            int s = -negamax(2, -color, -1000000, 1000000);
            s_board[r][c] = saved_from;
            s_board[tr][tc] = (int8_t)captured;
            if (s > best_s) {
                best_s = s; bf_r = r; bf_c = c; bt_r = tr; bt_c = tc;
            }
        }
    }
    if (bf_r < 0) { s_winner = -color; return; }
    int captured = s_board[bt_r][bt_c];
    if (captured == -color * KING) s_winner = color;
    int saved_from = s_board[bf_r][bf_c];
    s_board[bt_r][bt_c] = s_board[bf_r][bf_c];
    s_board[bf_r][bf_c] = 0;
    if (saved_from == color * PAWN && (bt_r == 0 || bt_r == 7))
        s_board[bt_r][bt_c] = (int8_t)(color * QUEEN);
}

/* ---- setup + render ---- */

static void reset_board(void)
{
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) s_board[r][c] = 0;
    static const int back[8] = { ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK };
    for (int c = 0; c < 8; c++) {
        s_board[0][c] = (int8_t)(-back[c]);
        s_board[1][c] = -PAWN;
        s_board[6][c] = PAWN;
        s_board[7][c] = (int8_t)back[c];
    }
    s_turn = 1; s_sel_r = s_sel_c = -1; s_winner = 0;
}

static void render(void)
{
    /* Cells */
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
        bool light = ((r + c) & 1) == 0;
        uint32_t bg = light ? 0xf0d9b5 : 0xb58863;
        if (s_sel_r == r && s_sel_c == c) bg = 0xffe066;
        cfill(c * CELL_PX, r * CELL_PX, CELL_PX, CELL_PX, pack(bg));
    }
    /* Legal-move highlights */
    for (int i = 0; i < s_n_legal; i++) {
        int r = s_legal[i].r, c = s_legal[i].c;
        cfill(c * CELL_PX + 4, r * CELL_PX + 4, CELL_PX - 8, CELL_PX - 8,
              pack(0x4ec25c));
    }
    /* Piece glyphs: pre-rendered A8 bitmaps, recolored by side with an
       opposite-tone outline halo so they read on both square colors. */
    lv_layer_t layer; lv_canvas_init_layer(s_canvas, &layer);
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
        int p = s_board[r][c];
        if (!p) continue;
        const lv_image_dsc_t *img = piece_img(p < 0 ? -p : p);
        if (!img) continue;
        bool white = p > 0;
        hb_glyph_draw(&layer, img,
                      c * CELL_PX + CELL_PX / 2, r * CELL_PX + CELL_PX / 2, CELL_PX - 3,
                      lv_color_hex(white ? 0xffffff : 0x111111),
                      true, lv_color_hex(white ? 0x1a1a1a : 0xf2f2f2));
    }
    lv_canvas_finish_layer(s_canvas, &layer);
    lv_obj_invalidate(s_canvas);
}

static void refresh_status(void)
{
    if (s_winner == 1)       lv_label_set_text(s_status_lbl, "White wins. New game →");
    else if (s_winner == -1) lv_label_set_text(s_status_lbl, "Black wins. New game →");
    else lv_label_set_text(s_status_lbl, s_turn == 1 ? "White to move" : "Black to move");
}

/* ---- input ---- */

static void on_canvas_press(lv_event_t *e)
{
    (void)e;
    if (s_winner) { reset_board(); s_n_legal = 0; render(); refresh_status(); return; }
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    lv_area_t area; lv_obj_get_coords(s_canvas, &area);
    int cx = p.x - area.x1, cy = p.y - area.y1;
    if (cx < 0 || cy < 0 || cx >= BOARD_W || cy >= BOARD_H) return;
    int c = cx / CELL_PX, r = cy / CELL_PX;
    /* In vs-AI mode, human is white and only plays on s_turn == 1. */
    if (s_mode == MODE_AI && s_turn != 1) return;
    int p_at = s_board[r][c];
    if (s_sel_r < 0) {
        if (p_at == 0) return;
        if (sign_of(p_at) != s_turn) return;
        s_sel_r = r; s_sel_c = c;
        gen_moves(r, c, s_legal, &s_n_legal);
        render();
        return;
    }
    /* Already had a selection — is (r,c) a legal destination? */
    bool legal = false;
    for (int i = 0; i < s_n_legal; i++) {
        if (s_legal[i].r == r && s_legal[i].c == c) { legal = true; break; }
    }
    if (!legal) {
        if (p_at != 0 && sign_of(p_at) == s_turn) {
            s_sel_r = r; s_sel_c = c;
            gen_moves(r, c, s_legal, &s_n_legal);
        } else {
            s_sel_r = s_sel_c = -1;
            s_n_legal = 0;
        }
        render();
        return;
    }
    /* Make the move. */
    int from_r = s_sel_r, from_c = s_sel_c;
    int captured = s_board[r][c];
    if (captured == s_turn * KING * -1) s_winner = s_turn;
    int piece = s_board[from_r][from_c];
    s_board[r][c] = (int8_t)piece;
    s_board[from_r][from_c] = 0;
    if (piece == s_turn * PAWN && (r == 0 || r == 7)) {
        s_board[r][c] = (int8_t)(s_turn * QUEEN);
    }
    s_sel_r = s_sel_c = -1;
    s_n_legal = 0;
    if (!s_winner) s_turn = -s_turn;
    render();
    refresh_status();
    /* If AI mode and it's the AI's turn, let it play immediately. */
    if (s_mode == MODE_AI && !s_winner && s_turn == -1) {
        ai_play(-1);
        s_turn = 1;
        render();
        refresh_status();
    }
}

static void on_mode_toggle(lv_event_t *e)
{
    (void)e;
    s_mode = (s_mode == MODE_AI) ? MODE_HUMAN : MODE_AI;
    lv_label_set_text(s_mode_btn_lbl, s_mode == MODE_AI ? "vs AI" : "vs Human");
    reset_board();
    s_n_legal = 0;
    render();
    refresh_status();
}

static void on_new(lv_event_t *e)
{
    (void)e;
    reset_board();
    s_n_legal = 0;
    render();
    refresh_status();
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "White to move");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_LEFT, 6, 6);

    /* Controls on their own row along the bottom: vs-AI toggle on the left,
       New on the right (wider, with padding). */
    lv_obj_t *mode_btn = lv_button_create(scr);
    lv_obj_set_size(mode_btn, 100, 32);
    lv_obj_align(mode_btn, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_set_style_bg_color(mode_btn, lv_color_hex(hb_color_surface()), 0);
    lv_obj_add_event_cb(mode_btn, on_mode_toggle, LV_EVENT_CLICKED, NULL);
    s_mode_btn_lbl = lv_label_create(mode_btn);
    lv_label_set_text(s_mode_btn_lbl, "vs AI");
    lv_obj_set_style_text_color(s_mode_btn_lbl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(s_mode_btn_lbl);

    lv_obj_t *new_btn = lv_button_create(scr);
    lv_obj_set_size(new_btn, 100, 32);
    lv_obj_align(new_btn, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_set_style_bg_color(new_btn, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(new_btn, on_new, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(new_btn);
    lv_label_set_text(nl, "New");
    lv_obj_set_style_text_color(nl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(nl);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, BOARD_W, BOARD_H,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_canvas_press, LV_EVENT_PRESSED, NULL);

    reset_board();
    render();
    refresh_status();

}
