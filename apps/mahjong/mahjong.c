/*
 * mahjong — single-layer pair-matching solitaire.
 *
 * 8 cols × 6 rows of tiles (48 tiles, 24 unique pairs). Each tile
 * shows a symbol from a small set; tap two tiles with the same
 * symbol AND at least one free side (left or right) to remove
 * them as a pair. Clear the board to win.
 *
 * The classic stacked-layer mahjong turtle layout is deferred — a
 * single-layer "free-side" rule still gives the core matching feel
 * on a tiny screen, and fits comfortably in the LVGL widget budget
 * via a single canvas.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_glyph.h"
#include "lvgl/lvgl.h"

#define COLS 8
#define ROWS 6
#define N_TILES (COLS * ROWS)         /* 48 */
#define N_PAIRS (N_TILES / 2)         /* 24 */

#define CELL_PX 28
#define BOARD_W (COLS * CELL_PX)      /* 224 */
#define BOARD_H (ROWS * CELL_PX)      /* 168 */
#define HEADER_H 40

/* In the app's own arena: the old fixed 0x092E0000 lies inside the OS heap
 * and was overwriting live objects (and sometimes this app's own code). */
static uint8_t s_canvas_buf[BOARD_W * BOARD_H * 4] __attribute__((aligned(4)));

/* Each tile: -1 = removed, else 0..23 = pair id. */
static int8_t s_board[ROWS][COLS];
static int    s_sel_r = -1, s_sel_c = -1;
static int    s_remaining = N_TILES;

static lv_obj_t *s_canvas;
static lv_obj_t *s_status_lbl;

/* Pair artwork: 24 distinct pre-rendered icons (sdk/generated/hb_glyphs) so each
   matched pair is easy to tell apart, recolored by group via s_pair_color. */
static const lv_image_dsc_t *const s_pair_img[N_PAIRS] = {
    &hb_glyph_star, &hb_glyph_moon, &hb_glyph_sun, &hb_glyph_bolt, &hb_glyph_fire,
    &hb_glyph_leaf, &hb_glyph_droplet, &hb_glyph_snowflake, &hb_glyph_crown,
    &hb_glyph_gem, &hb_glyph_bell, &hb_glyph_anchor, &hb_glyph_feather,
    &hb_glyph_cube, &hb_glyph_key, &hb_glyph_bug, &hb_glyph_ghost, &hb_glyph_paw,
    &hb_glyph_fish, &hb_glyph_frog, &hb_glyph_dragon, &hb_glyph_dove,
    &hb_glyph_cat, &hb_glyph_tree,
};
static const uint32_t s_pair_color[N_PAIRS] = {
    0xe63946, 0xe63946, 0xe63946, 0xe63946, 0xe63946, 0xe63946, 0xe63946, 0xe63946, 0xe63946,
    0x14213d, 0x14213d, 0x14213d, 0x14213d,
    0xa663cc, 0xa663cc, 0xa663cc, 0xa663cc, 0xa663cc,
    0x2a9d8f, 0x2a9d8f, 0x2a9d8f, 0x2a9d8f,
    0xf77f00, 0xf77f00,
};

static uint32_t s_rng;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

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

static void deal(void)
{
    /* Build a GUARANTEED-SOLVABLE board. A random fill often leaves no matching
       pair among the only tiles that are free at the start (the two edge columns),
       so the player is stuck immediately. Instead, assign symbols in a valid
       removal order: repeatedly take TWO cells that are currently free, give them a
       fresh pair symbol, and mark them gone. Both cells of a pair are free at the
       same moment here, so they're matchable when the player reaches them — and the
       first pair lands on the open edges, so there's always a legal first move. */
    bool present[ROWS][COLS];
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
        present[r][c] = true;
        s_board[r][c] = -1;
    }
    int placed = 0;            /* pairs assigned so far */
    int left = N_TILES;
    while (left > 0) {
        /* free = still present AND a horizontal neighbour is missing/an edge */
        int fr[N_TILES][2], nf = 0;
        for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
            if (!present[r][c]) continue;
            bool lo = (c == 0)        || !present[r][c - 1];
            bool ro = (c == COLS - 1) || !present[r][c + 1];
            if (lo || ro) { fr[nf][0] = r; fr[nf][1] = c; nf++; }
        }
        /* nf >= 2 whenever left >= 2 (every present run exposes an end), so this
           loop never spins. */
        int a = (int)(rnd() % (uint32_t)nf);
        int b; do { b = (int)(rnd() % (uint32_t)nf); } while (b == a);
        int8_t pid = (int8_t)(placed % N_PAIRS);
        s_board[fr[a][0]][fr[a][1]] = pid;  present[fr[a][0]][fr[a][1]] = false;
        s_board[fr[b][0]][fr[b][1]] = pid;  present[fr[b][0]][fr[b][1]] = false;
        placed++;
        left -= 2;
    }
    s_remaining = N_TILES;
    s_sel_r = s_sel_c = -1;
}

static bool side_free(int r, int c)
{
    /* "Free" means the left OR right side is open (no tile or
       removed). The board edge counts as open. */
    if (c == 0 || s_board[r][c-1] < 0) return true;
    if (c == COLS - 1 || s_board[r][c+1] < 0) return true;
    return false;
}

static void render(void)
{
    cfill(0, 0, BOARD_W, BOARD_H, pack(0x0a3a26));
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
        int x = c * CELL_PX, y = r * CELL_PX;
        int8_t v = s_board[r][c];
        if (v < 0) continue;       /* removed */
        uint32_t bg = pack(0xf5f5f0);
        if (s_sel_r == r && s_sel_c == c) bg = pack(0xffdf80);
        if (!side_free(r, c)) bg = pack(0xc8d3df);  /* locked-in tile */
        cfill(x + 1, y + 1, CELL_PX - 2, CELL_PX - 2, bg);
        cfill(x, y, CELL_PX, 1, pack(0x333333));
        cfill(x, y + CELL_PX - 1, CELL_PX, 1, pack(0x333333));
        cfill(x, y, 1, CELL_PX, pack(0x333333));
        cfill(x + CELL_PX - 1, y, 1, CELL_PX, pack(0x333333));
        lv_layer_t layer; lv_canvas_init_layer(s_canvas, &layer);
        hb_glyph_draw(&layer, s_pair_img[(int)v], x + CELL_PX / 2, y + CELL_PX / 2,
                      CELL_PX - 8, lv_color_hex(s_pair_color[(int)v]),
                      false, lv_color_black());
        lv_canvas_finish_layer(s_canvas, &layer);
    }
    lv_obj_invalidate(s_canvas);
}

static void itoa_u(uint32_t v, char *out)
{
    char b[12]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    int k = 0; while (i) out[k++] = b[--i];
    out[k] = 0;
}

static void refresh_status(void)
{
    if (s_remaining == 0) { lv_label_set_text(s_status_lbl, "You win!"); return; }
    char buf[24]; int k = 0;
    const char *p = "Remaining: "; while (*p) buf[k++] = *p++;
    char nb[12]; itoa_u((uint32_t)s_remaining, nb);
    for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    buf[k] = 0;
    lv_label_set_text(s_status_lbl, buf);
}

static void on_canvas_press(lv_event_t *e)
{
    (void)e;
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    lv_area_t area; lv_obj_get_coords(s_canvas, &area);
    int cx = p.x - area.x1, cy = p.y - area.y1;
    if (cx < 0 || cy < 0 || cx >= BOARD_W || cy >= BOARD_H) return;
    int c = cx / CELL_PX, r = cy / CELL_PX;
    if (s_board[r][c] < 0) return;
    if (!side_free(r, c)) return;
    if (s_sel_r < 0) { s_sel_r = r; s_sel_c = c; render(); return; }
    if (s_sel_r == r && s_sel_c == c) { s_sel_r = s_sel_c = -1; render(); return; }
    if (s_board[r][c] == s_board[s_sel_r][s_sel_c]) {
        s_board[r][c] = -1;
        s_board[s_sel_r][s_sel_c] = -1;
        s_remaining -= 2;
    }
    s_sel_r = s_sel_c = -1;
    render();
    refresh_status();
}

static void on_new(lv_event_t *e)
{
    (void)e; deal(); render(); refresh_status();
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    s_rng = hb_time_uptime_us() | 1u;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a3a26), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Mahjong");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 6);

    lv_obj_t *btn_new = lv_button_create(scr);
    lv_obj_set_size(btn_new, 70, 28);
    lv_obj_align(btn_new, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_bg_color(btn_new, lv_color_hex(hb_color_surface()), 0);
    lv_obj_add_event_cb(btn_new, on_new, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(btn_new);
    lv_label_set_text(nl, "New");
    lv_obj_set_style_text_color(nl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(nl);

    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, BOARD_W, BOARD_H,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(s_canvas, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_canvas_press, LV_EVENT_PRESSED, NULL);

    deal();
    render();
    refresh_status();
}
