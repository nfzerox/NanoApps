/*
 * solitaire — Klondike. Canvas-rendered board (single widget) with
 * tap-to-auto-move semantics.
 *
 * Layout:
 *   Top row: 4 foundations (left) + stock + waste (right).
 *   Bottom:  7 tableau columns, cards fanned down within each.
 *
 * Tap a face-up card and the game tries to move it (and any cards
 * stacked on top of it) — first to a foundation (if a single card and
 * legal), otherwise to a legal tableau column. If no move is possible
 * the card just stays. Tap the stock to flip the next card to the
 * waste; tap an empty stock to recycle the waste back into the stock.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_glyph.h"
#include "lvgl/lvgl.h"

/* Suits: 0=clubs, 1=diamonds, 2=hearts, 3=spades. */
#define SUIT_BLACK(s) ((s) == 0 || (s) == 3)
#define SUIT_RED(s)   ((s) == 1 || (s) == 2)

/* Cards encoded as (rank * 4 + suit): rank 0..12 = A,2..10,J,Q,K. */
typedef uint8_t card_t;
#define CARD(r, s) ((card_t)((r) * 4 + (s)))
#define RANK(c)    ((int)((c) / 4))
#define SUIT(c)    ((int)((c) % 4))
#define CARD_NONE  0xFF

static const char *s_rank_name[13] = {
    "A","2","3","4","5","6","7","8","9","10","J","Q","K"
};
static const char *s_suit_glyph[4] = { "C","D","H","S" };
static const uint32_t s_suit_color[4] = {
    0x111111, 0xe63946, 0xe63946, 0x111111
};

/* Stacks. Tableau columns hold both face-down and face-up cards.
   `face_up_from` is the index of the first face-up card in the
   stack (cards below it are face-down). */
typedef struct {
    card_t cards[24];
    int    n;
    int    face_up_from;
} stack_t;

static stack_t s_foundations[4];      /* one per suit */
static stack_t s_tableau[7];
static stack_t s_stock;
static stack_t s_waste;

/* Canvas */
#define CANVAS_W  240
#define CANVAS_H  392
/* In the app's own arena: the old fixed 0x092E0000 lies inside the OS heap
 * and was overwriting live objects (and sometimes this app's own code). */
static uint8_t s_canvas_buf[CANVAS_W * CANVAS_H * 4] __attribute__((aligned(4)));

static lv_obj_t *s_canvas;
static lv_obj_t *s_status_lbl;

/* Card layout */
#define CARD_W 30
#define CARD_H 40
#define TOP_Y  8
#define TAB_Y  60
#define FAN_Y  14    /* vertical offset between fanned cards in tableau */
#define TAB_X_GAP 2

/* PRNG */
static uint32_t s_rng;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

/* ---- stack ops ---- */

static void stack_clear(stack_t *s) { s->n = 0; s->face_up_from = 0; }
static void stack_push(stack_t *s, card_t c) { s->cards[s->n++] = c; }
static card_t stack_pop(stack_t *s) { return s->cards[--s->n]; }
static card_t stack_top(const stack_t *s) { return s->n ? s->cards[s->n-1] : CARD_NONE; }

/* ---- deal ---- */

static void deal(void)
{
    card_t deck[52];
    for (int i = 0; i < 52; i++) deck[i] = (card_t)i;
    /* Fisher-Yates shuffle */
    for (int i = 51; i > 0; i--) {
        int j = (int)(rnd() % (uint32_t)(i + 1));
        card_t t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    for (int i = 0; i < 4; i++) stack_clear(&s_foundations[i]);
    for (int i = 0; i < 7; i++) stack_clear(&s_tableau[i]);
    stack_clear(&s_stock);
    stack_clear(&s_waste);

    /* Tableau: col i gets i+1 cards; only the top one is face-up. */
    int idx = 0;
    for (int c = 0; c < 7; c++) {
        for (int r = 0; r <= c; r++) stack_push(&s_tableau[c], deck[idx++]);
        s_tableau[c].face_up_from = s_tableau[c].n - 1;
    }
    /* Remainder → stock (all face-down). */
    while (idx < 52) stack_push(&s_stock, deck[idx++]);
    s_stock.face_up_from = s_stock.n;     /* nothing face-up in stock */
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

static void draw_card_back(int x, int y)
{
    cfill(x, y, CARD_W, CARD_H, pack(0x14213d));
    cborder(x, y, CARD_W, CARD_H, pack(0xfca311));
    /* Subtle hatched look — diagonal lines. */
    for (int yy = y + 3; yy < y + CARD_H - 3; yy += 4) {
        cfill(x + 3, yy, CARD_W - 6, 1, pack(0x1d3557));
    }
}

static void draw_card_face(int x, int y, card_t c, bool selected)
{
    uint32_t bg = selected ? pack(0xfcbf49) : pack(0xf5f5f0);
    cfill(x, y, CARD_W, CARD_H, bg);
    cborder(x, y, CARD_W, CARD_H, pack(0x333333));
    /* Rank top-left, suit bottom-right. */
    lv_layer_t layer; lv_canvas_init_layer(s_canvas, &layer);
    int r = RANK(c), s = SUIT(c);
    char rbuf[3];
    if (r == 9) { rbuf[0] = '1'; rbuf[1] = '0'; rbuf[2] = 0; }
    else        { rbuf[0] = s_rank_name[r][0]; rbuf[1] = 0; }
    lv_draw_label_dsc_t dsc; lv_draw_label_dsc_init(&dsc);
    dsc.color = lv_color_hex(s_suit_color[s]);
    dsc.font  = &lv_font_montserrat_14;
    dsc.text  = rbuf;
    dsc.align = LV_TEXT_ALIGN_LEFT;
    lv_area_t a = { x + 2, y + 1, x + CARD_W - 2, y + 16 };
    lv_draw_label(&layer, &dsc, &a);
    const lv_image_dsc_t *si = hb_glyph_suit(s);
    if (si) hb_glyph_draw(&layer, si, x + CARD_W - 8, y + CARD_H - 9, 13,
                          lv_color_hex(s_suit_color[s]), false, lv_color_black());
    /* Big center rank — uses montserrat_20 since 28 doesn't fit. */
    dsc.text  = rbuf;
    dsc.font  = &lv_font_montserrat_20;
    dsc.align = LV_TEXT_ALIGN_CENTER;
    a.x1 = x; a.y1 = y + 14; a.x2 = x + CARD_W; a.y2 = y + 34;
    lv_draw_label(&layer, &dsc, &a);
    lv_canvas_finish_layer(s_canvas, &layer);
}

static void draw_empty_slot(int x, int y, const char *symbol)
{
    cfill(x, y, CARD_W, CARD_H, pack(0x0e3e2c));
    cborder(x, y, CARD_W, CARD_H, pack(0x355c47));
    if (symbol && symbol[0]) {
        lv_layer_t layer; lv_canvas_init_layer(s_canvas, &layer);
        lv_draw_label_dsc_t dsc; lv_draw_label_dsc_init(&dsc);
        dsc.color = lv_color_hex(0x355c47);
        dsc.font  = &lv_font_montserrat_14;
        dsc.text  = symbol;
        dsc.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t a = { x, y + 12, x + CARD_W, y + 28 };
        lv_draw_label(&layer, &dsc, &a);
        lv_canvas_finish_layer(s_canvas, &layer);
    }
}

/* Coords of the various card slots. */
static void foundation_xy(int i, int *out_x, int *out_y) { *out_x = i * 32; *out_y = TOP_Y; }
static void stock_xy(int *out_x, int *out_y)             { *out_x = 240 - 2 * (CARD_W + 2); *out_y = TOP_Y; }
static void waste_xy(int *out_x, int *out_y)             { *out_x = 240 - 1 * (CARD_W + 2); *out_y = TOP_Y; }
static void tableau_xy(int col, int slot, int *out_x, int *out_y)
{
    *out_x = col * (CARD_W + TAB_X_GAP) + 2;
    *out_y = TAB_Y + slot * FAN_Y;
}

/* ---- render ---- */

/* Selection: track which stack we tapped from. -1 if none. */
static int s_sel_kind = -1;     /* 0=tableau, 1=waste, 2=foundation */
static int s_sel_col  = -1;
static int s_sel_slot = -1;     /* tableau row index */

static void render(void)
{
    cfill(0, 0, CANVAS_W, CANVAS_H, pack(0x0a3a26));    /* green felt */

    /* Foundations */
    for (int i = 0; i < 4; i++) {
        int x, y; foundation_xy(i, &x, &y);
        if (s_foundations[i].n == 0) draw_empty_slot(x, y, s_suit_glyph[i]);
        else draw_card_face(x, y, stack_top(&s_foundations[i]),
            s_sel_kind == 2 && s_sel_col == i);
    }
    /* Stock + waste */
    int sx, sy; stock_xy(&sx, &sy);
    if (s_stock.n == 0) draw_empty_slot(sx, sy, "<>");
    else draw_card_back(sx, sy);
    int wx, wy; waste_xy(&wx, &wy);
    if (s_waste.n == 0) draw_empty_slot(wx, wy, "");
    else draw_card_face(wx, wy, stack_top(&s_waste),
        s_sel_kind == 1);
    /* Tableau */
    for (int col = 0; col < 7; col++) {
        stack_t *st = &s_tableau[col];
        if (st->n == 0) {
            int x, y; tableau_xy(col, 0, &x, &y);
            draw_empty_slot(x, y, "");
            continue;
        }
        for (int i = 0; i < st->n; i++) {
            int x, y; tableau_xy(col, i, &x, &y);
            if (i < st->face_up_from) {
                draw_card_back(x, y);
            } else {
                bool sel = (s_sel_kind == 0 && s_sel_col == col && i >= s_sel_slot);
                draw_card_face(x, y, st->cards[i], sel);
            }
        }
    }
    lv_obj_invalidate(s_canvas);
}

/* ---- move logic ---- */

static bool can_drop_on_foundation(card_t c, const stack_t *f)
{
    int suit = SUIT(c), rank = RANK(c);
    if (f->n == 0) return rank == 0;       /* only Ace on empty */
    card_t top = stack_top(f);
    return SUIT(top) == suit && RANK(top) + 1 == rank;
}

static bool can_drop_on_tableau(card_t bottom_of_moving, const stack_t *t)
{
    int rank = RANK(bottom_of_moving), suit = SUIT(bottom_of_moving);
    if (t->n == 0) return rank == 12;      /* only K on empty */
    card_t top = stack_top(t);
    if (RANK(top) != rank + 1) return false;
    /* Must alternate colors. */
    if (SUIT_RED(suit) == SUIT_RED(SUIT(top))) return false;
    return true;
}

/* Move cards [from_idx..n-1] from src to dst. dst gets `cards[from..]`. */
static void move_run(stack_t *src, int from_idx, stack_t *dst)
{
    int n_to_move = src->n - from_idx;
    for (int i = 0; i < n_to_move; i++) {
        stack_push(dst, src->cards[from_idx + i]);
    }
    src->n = from_idx;
    if (src->face_up_from > src->n) src->face_up_from = src->n;
    /* If we exposed a face-down card, flip it. */
    if (src->n > 0 && src->face_up_from >= src->n) {
        src->face_up_from = src->n - 1;
    }
}

/* Try to auto-place the run starting at (kind, col, slot). Returns
   true if a move was made. */
static bool try_auto_move(int kind, int col, int slot)
{
    stack_t *src = NULL;
    int from_idx = 0;
    if (kind == 0) {
        src = &s_tableau[col];
        from_idx = slot;
    } else if (kind == 1) {
        src = &s_waste;
        from_idx = src->n - 1;
    } else if (kind == 2) {
        src = &s_foundations[col];
        from_idx = src->n - 1;
    } else return false;
    if (from_idx >= src->n) return false;
    int n_run = src->n - from_idx;

    card_t bottom = src->cards[from_idx];

    /* Foundation: only single cards. */
    if (n_run == 1) {
        for (int i = 0; i < 4; i++) {
            if (can_drop_on_foundation(bottom, &s_foundations[i])) {
                move_run(src, from_idx, &s_foundations[i]);
                return true;
            }
        }
    }
    /* Tableau */
    for (int i = 0; i < 7; i++) {
        if (&s_tableau[i] == src) continue;
        if (can_drop_on_tableau(bottom, &s_tableau[i])) {
            move_run(src, from_idx, &s_tableau[i]);
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

    /* Stock click: cycle. */
    int sx, sy; stock_xy(&sx, &sy);
    if (cx >= sx && cx < sx + CARD_W && cy >= sy && cy < sy + CARD_H) {
        if (s_stock.n == 0) {
            /* Recycle waste back into stock. */
            while (s_waste.n > 0) stack_push(&s_stock, stack_pop(&s_waste));
        } else {
            stack_push(&s_waste, stack_pop(&s_stock));
        }
        s_sel_kind = -1;
        render();
        return;
    }
    /* Waste */
    int wx, wy; waste_xy(&wx, &wy);
    if (cx >= wx && cx < wx + CARD_W && cy >= wy && cy < wy + CARD_H && s_waste.n > 0) {
        try_auto_move(1, 0, 0);
        s_sel_kind = -1;
        render();
        return;
    }
    /* Foundations */
    for (int i = 0; i < 4; i++) {
        int fx, fy; foundation_xy(i, &fx, &fy);
        if (cx >= fx && cx < fx + CARD_W && cy >= fy && cy < fy + CARD_H && s_foundations[i].n > 0) {
            try_auto_move(2, i, 0);
            s_sel_kind = -1;
            render();
            return;
        }
    }
    /* Tableau: identify column + which row was tapped. */
    if (cy >= TAB_Y) {
        int col = (cx - 2) / (CARD_W + TAB_X_GAP);
        if (col >= 0 && col < 7) {
            stack_t *st = &s_tableau[col];
            int row_in_col = (cy - TAB_Y) / FAN_Y;
            if (row_in_col >= st->n) row_in_col = st->n - 1;
            if (row_in_col < 0) row_in_col = 0;
            /* Only allow grabbing face-up cards. */
            if (st->n > 0 && row_in_col >= st->face_up_from) {
                try_auto_move(0, col, row_in_col);
            }
            s_sel_kind = -1;
            render();
            return;
        }
    }
}

static void on_new_game(lv_event_t *e)
{
    (void)e;
    deal();
    s_sel_kind = -1;
    render();
}

static bool is_won(void)
{
    int total = 0;
    for (int i = 0; i < 4; i++) total += s_foundations[i].n;
    return total == 52;
}

static void refresh_status(void)
{
    if (is_won()) {
        lv_label_set_text(s_status_lbl, "You win!");
    } else {
        char buf[24]; int k = 0;
        const char *p = "Stock: "; while (*p) buf[k++] = *p++;
        char nb[8]; int v = s_stock.n + s_waste.n, ti = 0;
        if (v == 0) nb[ti++] = '0';
        while (v) { nb[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) buf[k++] = nb[--ti];
        buf[k] = 0;
        lv_label_set_text(s_status_lbl, buf);
    }
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
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_LEFT, 8, -8);

    lv_obj_t *btn_new = lv_button_create(scr);
    lv_obj_set_size(btn_new, 104, 30);
    lv_obj_align(btn_new, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
    lv_obj_set_style_bg_color(btn_new, lv_color_hex(hb_color_surface()), 0);
    lv_obj_add_event_cb(btn_new, on_new_game, LV_EVENT_CLICKED, NULL);
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
