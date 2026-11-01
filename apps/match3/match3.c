/*
 * match3 — Bejeweled-style match-three puzzle.
 *
 * 6x8 grid of 40 px gem cells (6 gem types). Each gem type has both a
 * distinct color AND a distinct shape (gem/star/leaf/droplet/moon/
 * heart bitmap), so runs are readable even for colorblind players.
 * Tap a gem to select it; tap an adjacent gem to swap. If the swap
 * creates a run of 3+ in a row or column, the matched gems clear, gems
 * above fall down, and new random gems drop in from the top. Otherwise
 * the swap reverts.
 *
 * Single canvas render (no per-gem widgets) to fit the LVGL widget
 * budget; gem shapes are A8 bitmaps drawn via hb_glyph_draw.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_glyph.h"
#include "lvgl/lvgl.h"

#define COLS         6
#define ROWS         8
#define CELL_PX      40
#define BOARD_W      (COLS * CELL_PX)   /* 240 */
#define BOARD_H      (ROWS * CELL_PX)   /* 320 */
#define N_COLORS     6
#define HEADER_H     40

/* In the app's own arena: the old fixed 0x092E0000 lies inside the OS heap
 * and was overwriting live objects (and sometimes this app's own code). */
static uint8_t s_canvas_buf[BOARD_W * BOARD_H * 4] __attribute__((aligned(4)));

static const uint32_t s_gem_color[N_COLORS] = {
    0xe63946, 0xfcbf49, 0x2a9d8f, 0x457b9d, 0xa663cc, 0xff6bd6,
};

/* A distinct shape per gem type (sdk/generated/hb_glyphs), recolored to the
   gem color at draw time, so a run reads by silhouette as well as color. */
static const lv_image_dsc_t *const s_gem_img[N_COLORS] = {
    &hb_glyph_gem, &hb_glyph_star, &hb_glyph_leaf,
    &hb_glyph_droplet, &hb_glyph_moon, &hb_glyph_heart,
};

static int8_t s_board[ROWS][COLS];     /* -1 = empty, 0..5 = color */
static int    s_sel_r = -1, s_sel_c = -1;
static int    s_score = 0;
static int    s_best  = 0;

static lv_obj_t *s_canvas;
static lv_obj_t *s_score_lbl;

#define BEST_PATH  "/Data/Match3/best.txt"
#define DATA_DIR   "/Data/Match3"

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

/* ---- match detection + cascade ---- */

static bool find_matches(uint8_t marks[ROWS][COLS])
{
    bool any = false;
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) marks[r][c] = 0;
    /* Horizontal runs of 3+ */
    for (int r = 0; r < ROWS; r++) {
        int run = 1;
        for (int c = 1; c <= COLS; c++) {
            if (c < COLS && s_board[r][c] >= 0 && s_board[r][c] == s_board[r][c-1]) {
                run++;
            } else {
                if (run >= 3) {
                    for (int k = 0; k < run; k++) marks[r][c-1-k] = 1;
                    any = true;
                }
                run = 1;
            }
        }
    }
    /* Vertical runs of 3+ */
    for (int c = 0; c < COLS; c++) {
        int run = 1;
        for (int r = 1; r <= ROWS; r++) {
            if (r < ROWS && s_board[r][c] >= 0 && s_board[r][c] == s_board[r-1][c]) {
                run++;
            } else {
                if (run >= 3) {
                    for (int k = 0; k < run; k++) marks[r-1-k][c] = 1;
                    any = true;
                }
                run = 1;
            }
        }
    }
    return any;
}

static void collapse(void)
{
    for (int c = 0; c < COLS; c++) {
        /* Drop existing gems. */
        int wr = ROWS - 1;
        for (int r = ROWS - 1; r >= 0; r--) {
            if (s_board[r][c] >= 0) s_board[wr--][c] = s_board[r][c];
        }
        while (wr >= 0) s_board[wr--][c] = -1;
        /* Fill empty cells with new random gems. */
        for (int r = 0; r < ROWS; r++) {
            if (s_board[r][c] < 0) s_board[r][c] = (int8_t)(rnd() % N_COLORS);
        }
    }
}

static int resolve_cascades(void)
{
    int total_cleared = 0;
    int chain = 0;
    static uint8_t marks[ROWS][COLS];
    while (find_matches(marks)) {
        chain++;
        int cleared = 0;
        for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
            if (marks[r][c]) { s_board[r][c] = -1; cleared++; }
        }
        total_cleared += cleared;
        /* Chain bonus: each subsequent cascade scores 2x. */
        s_score += cleared * 10 * chain;
        collapse();
    }
    return total_cleared;
}

static void initial_fill(void)
{
    /* Fill randomly, then resolve any starting matches so the board
       begins with no in-place matches (cleared cleared gives points
       on first draw, undesirable — instead, generate without
       starting matches). */
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
        int tries = 0;
        do {
            s_board[r][c] = (int8_t)(rnd() % N_COLORS);
            tries++;
        } while (tries < 20 && (
            (c >= 2 && s_board[r][c] == s_board[r][c-1] && s_board[r][c] == s_board[r][c-2]) ||
            (r >= 2 && s_board[r][c] == s_board[r-1][c] && s_board[r][c] == s_board[r-2][c])
        ));
    }
}

/* ---- render ---- */

static void render(void)
{
    /* Cell backgrounds first (direct buffer writes), then composite the gem
       shapes on top through a single canvas layer. */
    cfill(0, 0, BOARD_W, BOARD_H, pack(0x0a0e1a));
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
        int x = c * CELL_PX, y = r * CELL_PX;
        uint32_t bg = pack(0x111522);
        if (s_sel_r == r && s_sel_c == c) bg = pack(0x4a5060);
        cfill(x + 1, y + 1, CELL_PX - 2, CELL_PX - 2, bg);
    }
    lv_layer_t layer; lv_canvas_init_layer(s_canvas, &layer);
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
        int8_t v = s_board[r][c];
        if (v < 0) continue;
        int x = c * CELL_PX, y = r * CELL_PX;
        hb_glyph_draw(&layer, s_gem_img[(int)v], x + CELL_PX / 2, y + CELL_PX / 2,
                      CELL_PX - 6, lv_color_hex(s_gem_color[(int)v]),
                      true, lv_color_hex(0x05070d));
    }
    lv_canvas_finish_layer(s_canvas, &layer);
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

static void refresh_score(void)
{
    char buf[32]; int k = 0;
    const char *p = "Score: "; while (*p) buf[k++] = *p++;
    char nb[12]; itoa_u((uint32_t)s_score, nb);
    for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    if (s_best > 0) {
        p = "  Best "; while (*p) buf[k++] = *p++;
        itoa_u((uint32_t)s_best, nb);
        for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    }
    buf[k] = 0;
    lv_label_set_text(s_score_lbl, buf);
}

static void load_best(void)
{
    char b[12];
    uint32_t n = hb_fs_read(BEST_PATH, b, sizeof b - 1);
    if (n == 0) return;
    b[n] = 0;
    int v = 0;
    for (uint32_t i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++) v = v * 10 + (b[i] - '0');
    s_best = v;
}
static void save_best(void)
{
    char b[12]; itoa_u((uint32_t)s_best, b);
    int n = 0; while (b[n]) n++;
    hb_fs_write(BEST_PATH, b, (uint32_t)n);
}

/* ---- input ---- */

/* When a swap fails to create a match we want the user to see the
   swap briefly before it reverts, so the input feels responsive
   instead of silently swallowed. A pending-revert state runs the
   revert from the per-frame callback after a short delay. */
static int      s_pending_a_r = -1, s_pending_a_c, s_pending_b_r, s_pending_b_c;
static uint32_t s_pending_at_ms;

static void on_canvas_press(lv_event_t *e)
{
    (void)e;
    if (s_pending_a_r >= 0) return;          /* swap-revert in flight */
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    lv_area_t area; lv_obj_get_coords(s_canvas, &area);
    int cx = p.x - area.x1, cy = p.y - area.y1;
    if (cx < 0 || cy < 0 || cx >= BOARD_W || cy >= BOARD_H) return;
    int c = cx / CELL_PX, r = cy / CELL_PX;
    if (s_sel_r < 0) {
        s_sel_r = r; s_sel_c = c;
    } else {
        int dr = r - s_sel_r, dc = c - s_sel_c;
        int adr = dr < 0 ? -dr : dr, adc = dc < 0 ? -dc : dc;
        if (adr + adc == 1) {
            int ar = s_sel_r, ac = s_sel_c, br = r, bc = c;
            int8_t tmp = s_board[br][bc]; s_board[br][bc] = s_board[ar][ac]; s_board[ar][ac] = tmp;
            static uint8_t marks[ROWS][COLS];
            if (find_matches(marks)) {
                resolve_cascades();
                if (s_score > s_best) { s_best = s_score; save_best(); }
            } else {
                /* Schedule a visible revert: keep the swap on screen
                   for 250 ms so the user can see what they did, then
                   swap back from on_frame. */
                s_pending_a_r = ar; s_pending_a_c = ac;
                s_pending_b_r = br; s_pending_b_c = bc;
                s_pending_at_ms = hb_time_uptime_ms();
            }
            s_sel_r = -1; s_sel_c = -1;
        } else if (r == s_sel_r && c == s_sel_c) {
            s_sel_r = -1; s_sel_c = -1;
        } else {
            s_sel_r = r; s_sel_c = c;
        }
    }
    render();
    refresh_score();
}

static void on_frame(void)
{
    if (s_pending_a_r < 0) return;
    uint32_t now = hb_time_uptime_ms();
    if ((uint32_t)(now - s_pending_at_ms) < 250u) return;
    /* Revert the swap. */
    int8_t tmp = s_board[s_pending_b_r][s_pending_b_c];
    s_board[s_pending_b_r][s_pending_b_c] = s_board[s_pending_a_r][s_pending_a_c];
    s_board[s_pending_a_r][s_pending_a_c] = tmp;
    s_pending_a_r = -1;
    render();
}

static void on_new(lv_event_t *e)
{
    (void)e;
    s_score = 0;
    s_sel_r = s_sel_c = -1;
    initial_fill();
    render();
    refresh_score();
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
    lv_obj_set_style_text_font(s_score_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_score_lbl, LV_ALIGN_TOP_LEFT, 8, 8);

    lv_obj_t *btn_new = lv_button_create(scr);
    lv_obj_set_size(btn_new, 60, 28);
    lv_obj_align(btn_new, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_set_style_bg_color(btn_new, lv_color_hex(hb_color_surface()), 0);
    lv_obj_add_event_cb(btn_new, on_new, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(btn_new);
    lv_label_set_text(nl, "New");
    lv_obj_set_style_text_color(nl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(nl);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, BOARD_W, BOARD_H,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(s_canvas, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_canvas_press, LV_EVENT_PRESSED, NULL);

    initial_fill();
    render();
    refresh_score();

    hb_lv_set_frame_cb(on_frame);
}
