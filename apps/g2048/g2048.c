/*
 * 2048 — sliding-tile puzzle.
 *
 * 4x4 grid of power-of-two tiles. Swipe in a cardinal direction to
 * slide all tiles that way; any pair of equal tiles that collide
 * merges into one tile of double value. A new "2" (90%) or "4"
 * (10%) tile spawns on a random empty cell after every move that
 * actually moved or merged anything. Game ends when no move would
 * change the board.
 *
 * UI: top score + best display, 4x4 grid of colored tiles below.
 */

#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define N         4
#define TILE_PX   52
#define GAP_PX    6
#define BOARD_PX  (N * TILE_PX + (N + 1) * GAP_PX)   /* 4*52 + 5*6 = 238 */

#define BEST_PATH "/Data/g2048/best.txt"
#define DATA_DIR  "/Data/g2048"

static int s_board[N][N];      /* exponent: 1=>2, 2=>4, ... 0=>empty */
static int s_score;
static int s_best;

static lv_obj_t *s_score_lbl;
static lv_obj_t *s_best_lbl;
static lv_obj_t *s_board_obj;
static lv_obj_t *s_tile_objs [N][N];
static lv_obj_t *s_tile_labels[N][N];
static lv_obj_t *s_overlay;
static lv_obj_t *s_overlay_lbl;

/* Touch state for swipe gesture. */
static int  s_press_x, s_press_y;
static bool s_pressing;

/* ---- PRNG ---- */
static uint32_t s_rng;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/* ---- string + persistence helpers ---- */

static void itoa_u(uint32_t v, char *out)
{
    char b[12]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    int k = 0; while (i) out[k++] = b[--i];
    out[k] = 0;
}

static void load_best(void)
{
    char buf[16];
    uint32_t n = hb_fs_read(BEST_PATH, buf, sizeof buf - 1);
    if (n == 0) return;
    buf[n] = 0;
    int v = 0;
    for (uint32_t i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) {
        v = v * 10 + (buf[i] - '0');
    }
    s_best = v;
}

static void save_best(void)
{
    char buf[16]; itoa_u((uint32_t)s_best, buf);
    int n = 0; while (buf[n]) n++;
    hb_fs_write(BEST_PATH, buf, (uint32_t)n);
}

/* ---- board mechanics ---- */

/* Track the most recent spawn / merge cell coords so render() can
   add a brief pulse highlight as visual feedback. Set to (-1,-1)
   when nothing to pulse. */
static int s_pulse_r = -1, s_pulse_c = -1;

static void spawn_tile(void)
{
    int empties[N * N][2]; int n = 0;
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++)
        if (s_board[r][c] == 0) { empties[n][0] = r; empties[n][1] = c; n++; }
    if (n == 0) return;
    int pick = (int)(rnd() % (uint32_t)n);
    int r = empties[pick][0], c = empties[pick][1];
    s_board[r][c] = ((rnd() % 10) == 0) ? 2 : 1;   /* 10% chance of 4 (exp=2) */
    s_pulse_r = r;
    s_pulse_c = c;
}

static void clear_board(void)
{
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) s_board[r][c] = 0;
    s_score = 0;
}

/* Slide a single 4-element row to the LEFT in-place. Returns true if
   anything changed (movement OR merge). Adds merge values to s_score. */
static bool slide_row_left(int *row)
{
    int compact[N]; int cn = 0;
    for (int i = 0; i < N; i++) if (row[i]) compact[cn++] = row[i];
    int out[N] = { 0 };
    int oi = 0;
    int i = 0;
    while (i < cn) {
        if (i + 1 < cn && compact[i] == compact[i + 1]) {
            out[oi] = compact[i] + 1;       /* merged: 2^(e+1) */
            s_score += 1 << out[oi];
            oi++; i += 2;
        } else {
            out[oi++] = compact[i++];
        }
    }
    bool changed = false;
    for (int j = 0; j < N; j++) {
        if (out[j] != row[j]) changed = true;
        row[j] = out[j];
    }
    return changed;
}

/* Apply slide in any direction by symmetry. */
static bool move_dir(int dx, int dy)
{
    bool any = false;
    if (dx == -1) {           /* left */
        for (int r = 0; r < N; r++) if (slide_row_left(s_board[r])) any = true;
    } else if (dx == 1) {     /* right — reverse, slide left, reverse */
        for (int r = 0; r < N; r++) {
            int rev[N];
            for (int c = 0; c < N; c++) rev[c] = s_board[r][N-1-c];
            if (slide_row_left(rev)) any = true;
            for (int c = 0; c < N; c++) s_board[r][N-1-c] = rev[c];
        }
    } else if (dy == -1) {    /* up */
        for (int c = 0; c < N; c++) {
            int col[N];
            for (int r = 0; r < N; r++) col[r] = s_board[r][c];
            if (slide_row_left(col)) any = true;
            for (int r = 0; r < N; r++) s_board[r][c] = col[r];
        }
    } else if (dy == 1) {     /* down */
        for (int c = 0; c < N; c++) {
            int col[N];
            for (int r = 0; r < N; r++) col[r] = s_board[N-1-r][c];
            if (slide_row_left(col)) any = true;
            for (int r = 0; r < N; r++) s_board[N-1-r][c] = col[r];
        }
    }
    return any;
}

/* True if any move is possible. */
static bool has_moves(void)
{
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) {
        if (s_board[r][c] == 0) return true;
        if (c + 1 < N && s_board[r][c] == s_board[r][c+1]) return true;
        if (r + 1 < N && s_board[r][c] == s_board[r+1][c]) return true;
    }
    return false;
}

/* ---- rendering ---- */

/* Tile color palette per exponent. Higher tiles are richer / warmer. */
static const uint32_t s_tile_bg[] = {
    0x222531, 0xeeeed5, 0xeee0c8, 0xf2b179,
    0xf59563, 0xf67c5f, 0xf65e3b, 0xedcf72,
    0xedcc61, 0xedc850, 0xedc53f, 0xedc22e,
    0x3c3a32,
};
static const uint32_t s_tile_fg[] = {
    0x777777, 0x776e65, 0x776e65, 0xfaf8ef,
    0xfaf8ef, 0xfaf8ef, 0xfaf8ef, 0xfaf8ef,
    0xfaf8ef, 0xfaf8ef, 0xfaf8ef, 0xfaf8ef,
    0xfaf8ef,
};
#define N_TILE_BG (int)(sizeof s_tile_bg / sizeof s_tile_bg[0])

static int32_t s_anim_size_value;
static void anim_size_cb(void *obj, int32_t v)
{
    /* Resize centered around the cell origin. */
    lv_obj_t *t = (lv_obj_t *)obj;
    lv_obj_set_size(t, v, v);
    /* Re-center inside the GAP+TILE_PX cell. We stored the cell index
       in the obj's user_data so we know which cell to re-pos. */
    int idx = (int)(uintptr_t)lv_obj_get_user_data(t);
    int r = idx / N, c = idx % N;
    int cx = GAP_PX + c * (TILE_PX + GAP_PX) + TILE_PX / 2;
    int cy = GAP_PX + r * (TILE_PX + GAP_PX) + TILE_PX / 2;
    lv_obj_set_pos(t, cx - v / 2, cy - v / 2);
}

static void refresh_tile(int r, int c)
{
    lv_obj_t *t = s_tile_objs[r][c];
    lv_obj_t *l = s_tile_labels[r][c];
    int e = s_board[r][c];
    int idx = (e >= N_TILE_BG) ? N_TILE_BG - 1 : e;
    lv_obj_set_style_bg_color(t, lv_color_hex(s_tile_bg[idx]), 0);
    /* Stash cell index so the size-animation callback can re-center. */
    lv_obj_set_user_data(t, (void *)(uintptr_t)(r * N + c));
    /* If this is the just-spawned cell, animate the tile growing
       from a small size to full TILE_PX. */
    if (r == s_pulse_r && c == s_pulse_c && e != 0) {
        s_anim_size_value = 8;
        lv_obj_set_size(t, 8, 8);
        anim_size_cb(t, 8);
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, t);
        lv_anim_set_exec_cb(&a, anim_size_cb);
        lv_anim_set_values(&a, 8, TILE_PX);
        lv_anim_set_duration(&a, 140);
        lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
        lv_anim_start(&a);
    } else {
        lv_obj_set_size(t, TILE_PX, TILE_PX);
        anim_size_cb(t, TILE_PX);
    }
    if (e == 0) {
        lv_label_set_text(l, "");
    } else {
        char buf[8]; itoa_u((uint32_t)(1u << e), buf);
        lv_label_set_text(l, buf);
        lv_obj_set_style_text_color(l, lv_color_hex(s_tile_fg[idx]), 0);
        /* Smaller font for 4-digit tiles so they fit. */
        int v = 1 << e;
        const lv_font_t *f =
            (v >= 1000) ? &lv_font_montserrat_20 :
            (v >= 100)  ? &lv_font_montserrat_24 :
                          &lv_font_montserrat_28;
        lv_obj_set_style_text_font(l, f, 0);
    }
}

static void refresh_all(void)
{
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) refresh_tile(r, c);
    char buf[24]; int k = 0;
    const char *p = "Score: "; while (*p) buf[k++] = *p++;
    char nb[12]; itoa_u((uint32_t)s_score, nb);
    for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    buf[k] = 0;
    lv_label_set_text(s_score_lbl, buf);

    k = 0; p = "Best: "; while (*p) buf[k++] = *p++;
    itoa_u((uint32_t)s_best, nb);
    for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    buf[k] = 0;
    lv_label_set_text(s_best_lbl, buf);
}

static void show_game_over(void)
{
    char buf[40]; int k = 0;
    const char *p = "Game Over\nScore: "; while (*p) buf[k++] = *p++;
    char nb[12]; itoa_u((uint32_t)s_score, nb);
    for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    p = "\nTap to retry"; while (*p) buf[k++] = *p++;
    buf[k] = 0;
    lv_label_set_text(s_overlay_lbl, buf);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ---- input ---- */

static void start_game(void)
{
    clear_board();
    spawn_tile();
    spawn_tile();
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    refresh_all();
}

static void on_board_press(lv_event_t *e)
{
    (void)e;
    if (!lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN)) {
        start_game();
        return;
    }
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    s_press_x = p.x; s_press_y = p.y;
    s_pressing = true;
}

static void on_board_release(lv_event_t *e)
{
    (void)e;
    if (!s_pressing) return;
    s_pressing = false;
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    int dx = p.x - s_press_x;
    int dy = p.y - s_press_y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (adx < 20 && ady < 20) return;     /* too small */
    int mx = 0, my = 0;
    if (adx > ady) mx = dx > 0 ? 1 : -1;
    else           my = dy > 0 ? 1 : -1;

    bool moved = move_dir(mx, my);
    if (moved) {
        spawn_tile();
        if (s_score > s_best) { s_best = s_score; save_best(); }
        refresh_all();
        if (!has_moves()) show_game_over();
    }
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    load_best();

    s_rng = hb_time_uptime_us() | 1u;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_score_lbl = lv_label_create(scr);
    lv_label_set_text(s_score_lbl, "Score: 0");
    lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_score_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(s_score_lbl, LV_ALIGN_TOP_LEFT, 10, 14);

    s_best_lbl = lv_label_create(scr);
    lv_label_set_text(s_best_lbl, "Best: 0");
    lv_obj_set_style_text_color(s_best_lbl, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(s_best_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(s_best_lbl, LV_ALIGN_TOP_RIGHT, -10, 14);

    /* Board container — itself the swipe target. */
    s_board_obj = lv_obj_create(scr);
    lv_obj_set_size(s_board_obj, BOARD_PX, BOARD_PX);
    lv_obj_align(s_board_obj, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_color(s_board_obj, lv_color_hex(0x1a1a23), 0);
    lv_obj_set_style_radius(s_board_obj, 10, 0);
    lv_obj_set_style_border_width(s_board_obj, 0, 0);
    lv_obj_set_style_pad_all(s_board_obj, 0, 0);
    lv_obj_clear_flag(s_board_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_board_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_board_obj, on_board_press,   LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_board_obj, on_board_release, LV_EVENT_RELEASED, NULL);

    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) {
        lv_obj_t *t = lv_obj_create(s_board_obj);
        lv_obj_set_size(t, TILE_PX, TILE_PX);
        lv_obj_set_pos(t,
            GAP_PX + c * (TILE_PX + GAP_PX),
            GAP_PX + r * (TILE_PX + GAP_PX));
        lv_obj_set_style_radius(t, 6, 0);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_set_style_shadow_width(t, 0, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
        s_tile_objs[r][c] = t;

        lv_obj_t *l = lv_label_create(t);
        lv_label_set_text(l, "");
        lv_obj_set_style_text_color(l, lv_color_hex(0x776e65), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_center(l);
        s_tile_labels[r][c] = l;
    }

    /* Game-over overlay. */
    s_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_overlay, 200, 140);
    lv_obj_center(s_overlay);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_border_width(s_overlay, 2, 0);
    lv_obj_set_style_radius(s_overlay, 12, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_overlay_lbl = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(s_overlay_lbl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_overlay_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_overlay_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_overlay_lbl);

    start_game();
}
