/*
 * hb_kb.c — on-screen QWERTY keyboard for touch input.
 *
 * 4 rows of 10 cells. Apps decide the position + cell size. Tap
 * coordinates are mapped to characters; special slots use the
 * HB_KB_DEL / HB_KB_SAVE / HB_KB_SPACE markers.
 *
 * Designed to compose with hb_ui_poll — use hb_kb_hit() on a HB_UI_TAP
 * event.
 */

#include "hb_sdk.h"

static const char *k_qwerty_layout[HB_KB_ROWS] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl,",
    "\x01zxcvbnm\x03\x02",   /* DEL, ..., SPACE, SAVE */
};
static const char *k_qwerty_labels[HB_KB_ROWS] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl,",
    "<zxcvbnm_>",            /* '<' = del marker, '_' = space, '>' = save */
};

hb_kb_t hb_kb_qwerty(int16_t x, int16_t y, int16_t cell_w, int16_t cell_h)
{
    hb_kb_t kb;
    kb.x = x;
    kb.y = y;
    kb.cell_w = cell_w;
    kb.cell_h = cell_h;
    for (int r = 0; r < HB_KB_ROWS; r++) {
        kb.layout[r] = k_qwerty_layout[r];
        kb.labels[r] = k_qwerty_labels[r];
    }
    kb.bg_key     = HB_RGB(0x30, 0x30, 0x50);
    kb.bg_special = HB_RGB(0x60, 0x00, 0x60);
    kb.fg         = HB_WHITE;
    return kb;
}

/* For a single-character cell, compute the (lx, ly) for centered
   16x16 label (scale-2 single char). */
static inline void center_label(int16_t cx, int16_t cy,
                                int16_t cw, int16_t ch,
                                int16_t *lx, int16_t *ly)
{
    *lx = cx + (cw - 16) / 2;
    *ly = cy + (ch - 16) / 2;
}

void hb_kb_draw(const hb_kb_t *kb)
{
    /* Background wipe. */
    hb_fill_rect(kb->x, kb->y,
                 kb->cell_w * HB_KB_COLS, kb->cell_h * HB_KB_ROWS,
                 HB_BLACK);

    for (int r = 0; r < HB_KB_ROWS; r++) {
        for (int c = 0; c < HB_KB_COLS; c++) {
            int16_t cx = kb->x + c * kb->cell_w;
            int16_t cy = kb->y + r * kb->cell_h;

            char ch = kb->layout[r][c];
            hb_color_t bg = kb->bg_key;
            if (ch == HB_KB_DEL || ch == HB_KB_SAVE) bg = kb->bg_special;

            hb_fill_rect(cx + 1, cy + 1,
                         kb->cell_w - 2, kb->cell_h - 2, bg);

            char label = kb->labels[r][c];
            char buf[2] = { label, '\0' };
            int16_t lx, ly;
            center_label(cx, cy, kb->cell_w, kb->cell_h, &lx, &ly);
            hb_draw_str(lx, ly, buf, 2, kb->fg, bg);
        }
    }
}

/* Linear scan — avoids __aeabi_uidivmod from freestanding builds. */
static int row_col_from_xy(const hb_kb_t *kb,
                           int16_t tx, int16_t ty,
                           int *out_r, int *out_c)
{
    if (tx < kb->x || ty < kb->y) return 0;
    int r = 0, c = 0;
    int16_t dy = ty - kb->y;
    while (r < HB_KB_ROWS && dy >= kb->cell_h) {
        dy -= kb->cell_h; r++;
    }
    if (r >= HB_KB_ROWS) return 0;
    int16_t dx = tx - kb->x;
    while (c < HB_KB_COLS && dx >= kb->cell_w) {
        dx -= kb->cell_w; c++;
    }
    if (c >= HB_KB_COLS) return 0;
    *out_r = r; *out_c = c;
    return 1;
}

char hb_kb_hit(const hb_kb_t *kb, int16_t tx, int16_t ty)
{
    int r, c;
    if (!row_col_from_xy(kb, tx, ty, &r, &c)) return 0;
    return kb->layout[r][c];
}
