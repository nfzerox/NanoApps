/*
 * freecell — Microsoft-style FreeCell solitaire.
 *
 * Layout: top row holds 4 freecells + 4 foundations; below them are
 * 8 tableau columns fanned downward. Tap a face-up card to attempt
 * auto-move: foundation if legal, else first compatible tableau
 * column, else first empty freecell. Tap an empty freecell or
 * foundation to clear selection. Stock is unused (FreeCell deals
 * everything to the tableau).
 *
 * Compact card layout sized for 240-wide screen.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_glyph.h"
#include "lvgl/lvgl.h"

#define SUIT_BLACK(s) ((s) == 0 || (s) == 3)
#define SUIT_RED(s)   ((s) == 1 || (s) == 2)

typedef uint8_t card_t;
#define CARD(r, s) ((card_t)((r) * 4 + (s)))
#define RANK(c)    ((int)((c) / 4))
#define SUIT(c)    ((int)((c) % 4))
#define CARD_NONE  0xFF

static const char *s_rank_name[13] = { "A","2","3","4","5","6","7","8","9","10","J","Q","K" };
static const char *s_suit_glyph[4] = { "C","D","H","S" };
static const uint32_t s_suit_color[4] = { 0x111111, 0xe63946, 0xe63946, 0x111111 };

typedef struct {
    card_t cards[20];
    int n;
} stack_t;

static stack_t s_freecells[4];
static stack_t s_foundations[4];
static stack_t s_tableau[8];

#define CANVAS_W 240
#define CANVAS_H 392
/* In the app's own arena: the old fixed 0x092E0000 lies inside the OS heap
 * and was overwriting live objects (and sometimes this app's own code). */
static uint8_t s_canvas_buf[CANVAS_W * CANVAS_H * 4] __attribute__((aligned(4)));
static lv_obj_t *s_canvas;
static lv_obj_t *s_status_lbl;

#define CARD_W 28
#define CARD_H 36
#define FAN_Y  12
#define TOP_Y  6
#define TAB_Y  56

static uint32_t s_rng;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

static void stack_clear(stack_t *s) { s->n = 0; }
static void stack_push(stack_t *s, card_t c) { s->cards[s->n++] = c; }
static card_t stack_top(const stack_t *s) { return s->n ? s->cards[s->n-1] : CARD_NONE; }

static void deal(void)
{
    card_t deck[52];
    for (int i = 0; i < 52; i++) deck[i] = (card_t)i;
    for (int i = 51; i > 0; i--) {
        int j = (int)(rnd() % (uint32_t)(i + 1));
        card_t t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    for (int i = 0; i < 4; i++) { stack_clear(&s_freecells[i]); stack_clear(&s_foundations[i]); }
    for (int i = 0; i < 8; i++) stack_clear(&s_tableau[i]);
    int idx = 0;
    /* First 4 columns get 7 cards, last 4 get 6. */
    for (int c = 0; c < 8; c++) {
        int n = (c < 4) ? 7 : 6;
        for (int r = 0; r < n; r++) stack_push(&s_tableau[c], deck[idx++]);
    }
}

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
        if (yy < 0 || yy >= CANVAS_H) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= CANVAS_W) continue;
            buf[yy * CANVAS_W + xx] = color;
        }
    }
}
static void cborder(int x, int y, int w, int h, uint32_t color)
{
    cfill(x, y, w, 1, color);
    cfill(x, y + h - 1, w, 1, color);
    cfill(x, y, 1, h, color);
    cfill(x + w - 1, y, 1, h, color);
}

static void draw_empty(int x, int y, const char *sym)
{
    cfill(x, y, CARD_W, CARD_H, pack(0x0e3e2c));
    cborder(x, y, CARD_W, CARD_H, pack(0x355c47));
    if (sym && sym[0]) {
        lv_layer_t layer; lv_canvas_init_layer(s_canvas, &layer);
        lv_draw_label_dsc_t dsc; lv_draw_label_dsc_init(&dsc);
        dsc.color = lv_color_hex(0x355c47);
        dsc.font = &lv_font_montserrat_14;
        dsc.text = sym;
        dsc.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t a = { x, y + 10, x + CARD_W, y + CARD_H - 4 };
        lv_draw_label(&layer, &dsc, &a);
        lv_canvas_finish_layer(s_canvas, &layer);
    }
}

static void draw_card(int x, int y, card_t c)
{
    cfill(x, y, CARD_W, CARD_H, pack(0xf5f5f0));
    cborder(x, y, CARD_W, CARD_H, pack(0x333333));
    int r = RANK(c), s = SUIT(c);
    char rbuf[3];
    if (r == 9) { rbuf[0] = '1'; rbuf[1] = '0'; rbuf[2] = 0; }
    else        { rbuf[0] = s_rank_name[r][0]; rbuf[1] = 0; }
    lv_layer_t layer; lv_canvas_init_layer(s_canvas, &layer);
    lv_draw_label_dsc_t dsc; lv_draw_label_dsc_init(&dsc);
    dsc.color = lv_color_hex(s_suit_color[s]);
    dsc.font = &lv_font_montserrat_14;
    dsc.text = rbuf;
    dsc.align = LV_TEXT_ALIGN_LEFT;
    lv_area_t a = { x + 2, y + 1, x + CARD_W - 2, y + 14 };
    lv_draw_label(&layer, &dsc, &a);
    const lv_image_dsc_t *si = hb_glyph_suit(s);
    if (si) hb_glyph_draw(&layer, si, x + CARD_W - 7, y + CARD_H - 8, 12,
                          lv_color_hex(s_suit_color[s]), false, lv_color_black());
    lv_canvas_finish_layer(s_canvas, &layer);
}

static void freecell_xy(int i, int *x, int *y)   { *x = i * (CARD_W + 1); *y = TOP_Y; }
static void foundation_xy(int i, int *x, int *y) { *x = 240 - 4 * (CARD_W + 1) + i * (CARD_W + 1); *y = TOP_Y; }
static void tableau_xy(int col, int slot, int *x, int *y)
{
    *x = col * (CARD_W + 2) + 2;
    *y = TAB_Y + slot * FAN_Y;
}

static void render(void)
{
    cfill(0, 0, CANVAS_W, CANVAS_H, pack(0x0a3a26));
    for (int i = 0; i < 4; i++) {
        int x, y;
        freecell_xy(i, &x, &y);
        if (s_freecells[i].n == 0) draw_empty(x, y, "F");
        else                       draw_card(x, y, stack_top(&s_freecells[i]));
        foundation_xy(i, &x, &y);
        if (s_foundations[i].n == 0) draw_empty(x, y, s_suit_glyph[i]);
        else                         draw_card(x, y, stack_top(&s_foundations[i]));
    }
    for (int col = 0; col < 8; col++) {
        stack_t *st = &s_tableau[col];
        if (st->n == 0) {
            int x, y; tableau_xy(col, 0, &x, &y);
            draw_empty(x, y, "");
            continue;
        }
        for (int i = 0; i < st->n; i++) {
            int x, y; tableau_xy(col, i, &x, &y);
            draw_card(x, y, st->cards[i]);
        }
    }
    lv_obj_invalidate(s_canvas);
}

/* ---- move logic ---- */

static bool can_drop_foundation(card_t c, const stack_t *f)
{
    int suit = SUIT(c), rank = RANK(c);
    if (f->n == 0) return rank == 0;
    card_t top = stack_top(f);
    return SUIT(top) == suit && RANK(top) + 1 == rank;
}
static bool can_drop_tableau(card_t c, const stack_t *t)
{
    int rank = RANK(c), suit = SUIT(c);
    if (t->n == 0) return true;
    card_t top = stack_top(t);
    if (RANK(top) != rank + 1) return false;
    return SUIT_RED(suit) != SUIT_RED(SUIT(top));
}

/* Try to move the top card of `src` to first legal slot:
   foundation > tableau > freecell. Returns true if moved. */
static bool try_auto_move(stack_t *src)
{
    if (src->n == 0) return false;
    card_t top = stack_top(src);
    /* Foundation */
    for (int i = 0; i < 4; i++) {
        if (can_drop_foundation(top, &s_foundations[i])) {
            stack_push(&s_foundations[i], top);
            src->n--;
            return true;
        }
    }
    /* Tableau (non-self) */
    for (int i = 0; i < 8; i++) {
        if (&s_tableau[i] == src) continue;
        if (can_drop_tableau(top, &s_tableau[i])) {
            stack_push(&s_tableau[i], top);
            src->n--;
            return true;
        }
    }
    /* Freecell */
    for (int i = 0; i < 4; i++) {
        if (s_freecells[i].n == 0 && src != &s_freecells[i]) {
            stack_push(&s_freecells[i], top);
            src->n--;
            return true;
        }
    }
    return false;
}

/* ---- input ---- */

static void on_canvas_press(lv_event_t *e)
{
    (void)e;
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    lv_area_t area; lv_obj_get_coords(s_canvas, &area);
    int cx = p.x - area.x1, cy = p.y - area.y1;
    if (cx < 0 || cy < 0 || cx >= CANVAS_W || cy >= CANVAS_H) return;

    /* Top row: freecells or foundations? */
    if (cy < TAB_Y - 4) {
        int fc_x_end = 4 * (CARD_W + 1);
        if (cx < fc_x_end) {
            int i = cx / (CARD_W + 1);
            if (i >= 0 && i < 4 && s_freecells[i].n > 0) try_auto_move(&s_freecells[i]);
        } else {
            int fnd_start = 240 - 4 * (CARD_W + 1);
            if (cx >= fnd_start) {
                int i = (cx - fnd_start) / (CARD_W + 1);
                if (i >= 0 && i < 4 && s_foundations[i].n > 0) try_auto_move(&s_foundations[i]);
            }
        }
    } else {
        /* Tableau column */
        int col = (cx - 2) / (CARD_W + 2);
        if (col >= 0 && col < 8) try_auto_move(&s_tableau[col]);
    }
    render();
}

static void on_new(lv_event_t *e) { (void)e; deal(); render(); }

static bool is_won(void)
{
    int total = 0;
    for (int i = 0; i < 4; i++) total += s_foundations[i].n;
    return total == 52;
}

static void refresh_status(void)
{
    if (is_won()) lv_label_set_text(s_status_lbl, "You win!");
    else          lv_label_set_text(s_status_lbl, "");
}

static void on_frame(void) { refresh_status(); }

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    s_rng = hb_time_uptime_us() | 1u;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a3a26), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_LEFT, 8, -6);

    lv_obj_t *btn_new = lv_button_create(scr);
    lv_obj_set_size(btn_new, 104, 30);
    lv_obj_align(btn_new, LV_ALIGN_BOTTOM_RIGHT, -8, -4);
    lv_obj_set_style_bg_color(btn_new, lv_color_hex(hb_color_surface()), 0);
    lv_obj_add_event_cb(btn_new, on_new, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(btn_new);
    lv_label_set_text(nl, "New game");
    lv_obj_set_style_text_color(nl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(nl);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, CANVAS_W, CANVAS_H,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(s_canvas, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_canvas_press, LV_EVENT_PRESSED, NULL);

    deal();
    render();
    hb_lv_set_frame_cb(on_frame);
}
