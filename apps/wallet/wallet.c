/*
 * wallet — Passbook-style colorful cards for the things in your
 * wallet that aren't your actual wallet: membership numbers, gift
 * card codes, loyalty IDs, etc.
 *
 * Each pass has a title (e.g. "Costco"), a subtitle (e.g. "Member
 * #12345"), a numeric / alphanumeric code (the thing you actually
 * need to display at the cashier), and a color. The library shows
 * them as a vertical scroll of gradient cards. Tap a card to open
 * the big "show this to the cashier" detail view with the code
 * rendered in a large monospace-ish font.
 *
 * QR / barcode rendering is a future addition — v1 displays the code
 * as plain text. The user can drop a pre-rendered QR PNG/BMP into a
 * future per-pass directory and the detail view will pick it up
 * (tracked as a follow-up).
 *
 * Storage: /Data/Wallet/items.txt — one pass per line:
 *   title<TAB>subtitle<TAB>code<TAB>color_hex<NL>
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_t9.h"
#include "hb_swipe_row.h"
#include "lvgl/lvgl.h"

#define MAX_PASSES   32
#define MAX_FIELD    64
#define DATA_DIR     "/Data/Wallet"
#define ITEMS_PATH   "/Data/Wallet/items.txt"
#define FILE_BUF_SZ  (MAX_PASSES * (MAX_FIELD * 3 + 16))

typedef struct {
    char title[MAX_FIELD];
    char subtitle[MAX_FIELD];
    char code[MAX_FIELD];
    uint32_t color;
} pass_t;

static pass_t s_passes[MAX_PASSES];
static int    s_n_passes = 0;
static int    s_edit_idx = -1;     /* -1 = adding new */

typedef enum {
    VIEW_LIBRARY,
    VIEW_DETAIL,
    VIEW_EDITOR,
    VIEW_FIELD_INPUT,
} view_t;
static view_t s_view = VIEW_LIBRARY;

static lv_obj_t *s_scr;
static lv_obj_t *s_library_view;
static lv_obj_t *s_detail_view;
static lv_obj_t *s_editor_view;
static lv_obj_t *s_field_view;

static lv_obj_t *s_list_box;
static lv_obj_t *s_d_card;
static lv_obj_t *s_d_qr;
static lv_obj_t *s_d_barcode;
static lv_obj_t *s_d_title;
static lv_obj_t *s_d_subtitle;
static lv_obj_t *s_d_code;

static lv_obj_t *s_e_title_label;
static lv_obj_t *s_e_subtitle_label;
static lv_obj_t *s_e_code_label;
static lv_obj_t *s_e_color_chip;
static lv_obj_t *s_f_title_label;
static lv_obj_t *s_f_ta;

/* Which field the field-input view is editing. */
typedef enum { F_TITLE, F_SUBTITLE, F_CODE } field_id_t;
static field_id_t s_editing_field;

/* Editor scratch (mirrors a pass_t but is the WIP for the current
   add/edit session). */
static pass_t s_editor_buf;

/* ---- helpers ---- */

static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void s_copy(char *dst, const char *src, int dst_sz)
{
    int i = 0; while (src[i] && i < dst_sz - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void s_clear(char *dst, int dst_sz) { for (int i = 0; i < dst_sz; i++) dst[i] = 0; }

static int parse_hex(const char *s)
{
    int v = 0;
    while (*s) {
        char c = *s++;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else continue;
        v = v * 16 + d;
    }
    return v;
}

static void hex_to_str(uint32_t v, char *out)
{
    const char *hex = "0123456789abcdef";
    for (int i = 5; i >= 0; i--) {
        out[i] = hex[v & 0xF];
        v >>= 4;
    }
    out[6] = 0;
}

/* ---- persistence ---- */

static void load_passes(void)
{
    s_n_passes = 0;
    static char buf[FILE_BUF_SZ];
    uint32_t n = hb_fs_read(ITEMS_PATH, buf, sizeof buf - 1);
    if (n == 0) return;
    buf[n] = 0;

    uint32_t i = 0;
    while (i < n && s_n_passes < MAX_PASSES) {
        pass_t *p = &s_passes[s_n_passes];
        s_clear(p->title, MAX_FIELD);
        s_clear(p->subtitle, MAX_FIELD);
        s_clear(p->code, MAX_FIELD);
        p->color = 0x457b9d;
        /* Read title until \t */
        int k = 0;
        while (i < n && buf[i] != '\t' && buf[i] != '\n' && k < MAX_FIELD - 1) p->title[k++] = buf[i++];
        if (i < n && buf[i] == '\t') i++;
        /* subtitle */
        k = 0;
        while (i < n && buf[i] != '\t' && buf[i] != '\n' && k < MAX_FIELD - 1) p->subtitle[k++] = buf[i++];
        if (i < n && buf[i] == '\t') i++;
        /* code */
        k = 0;
        while (i < n && buf[i] != '\t' && buf[i] != '\n' && k < MAX_FIELD - 1) p->code[k++] = buf[i++];
        if (i < n && buf[i] == '\t') i++;
        /* color hex */
        char chex[12]; int ck = 0;
        while (i < n && buf[i] != '\n' && ck < 11) chex[ck++] = buf[i++];
        chex[ck] = 0;
        if (ck > 0) p->color = (uint32_t)parse_hex(chex);
        if (i < n && buf[i] == '\n') i++;
        if (p->title[0]) s_n_passes++;
    }
}

static void save_passes(void)
{
    static char buf[FILE_BUF_SZ];
    uint32_t pos = 0;
    for (int i = 0; i < s_n_passes; i++) {
        pass_t *p = &s_passes[i];
        for (int j = 0; p->title[j] && pos < sizeof buf - 2; j++) buf[pos++] = p->title[j];
        buf[pos++] = '\t';
        for (int j = 0; p->subtitle[j] && pos < sizeof buf - 2; j++) buf[pos++] = p->subtitle[j];
        buf[pos++] = '\t';
        for (int j = 0; p->code[j] && pos < sizeof buf - 2; j++) buf[pos++] = p->code[j];
        buf[pos++] = '\t';
        char hex[8]; hex_to_str(p->color, hex);
        for (int j = 0; hex[j] && pos < sizeof buf - 2; j++) buf[pos++] = hex[j];
        buf[pos++] = '\n';
    }
    hb_fs_write(ITEMS_PATH, buf, pos);
}

/* ---- forward decls ---- */
static void open_library(void);
static void open_detail(int idx);
static void open_editor(int idx);
static void open_field(field_id_t fid);

/* ---- library callbacks ---- */

static void on_row_tap(void *cookie) { open_detail((int)(uintptr_t)cookie); }
static void on_row_delete(void *cookie)
{
    int idx = (int)(uintptr_t)cookie;
    if (idx < 0 || idx >= s_n_passes) return;
    for (int i = idx; i < s_n_passes - 1; i++) s_passes[i] = s_passes[i+1];
    s_n_passes--;
    save_passes();
    open_library();
}

static void on_new(lv_event_t *e) { (void)e; open_editor(-1); }
static void on_back_to_library(lv_event_t *e) { (void)e; open_library(); }

static void on_color_cycle(lv_event_t *e)
{
    (void)e;
    static const uint32_t palette[] = {
        0xe63946, 0xf77f00, 0xfcbf49, 0x2a9d8f,
        0x457b9d, 0xa663cc, 0x3a86ff, 0xff006e,
    };
    int n_pal = (int)(sizeof palette / sizeof palette[0]);
    int cur = 0;
    for (int i = 0; i < n_pal; i++)
        if (palette[i] == s_editor_buf.color) { cur = i; break; }
    s_editor_buf.color = palette[(cur + 1) % n_pal];
    lv_obj_set_style_bg_color(s_e_color_chip,
        lv_color_hex(s_editor_buf.color), 0);
}

static void on_save_editor(lv_event_t *e)
{
    (void)e;
    if (s_editor_buf.title[0] == 0) { open_library(); return; }
    if (s_edit_idx >= 0 && s_edit_idx < s_n_passes) {
        s_passes[s_edit_idx] = s_editor_buf;
    } else if (s_n_passes < MAX_PASSES) {
        s_passes[s_n_passes++] = s_editor_buf;
    }
    save_passes();
    open_library();
}

static void on_delete_from_editor(lv_event_t *e)
{
    (void)e;
    if (s_edit_idx >= 0 && s_edit_idx < s_n_passes) {
        for (int i = s_edit_idx; i < s_n_passes - 1; i++) s_passes[i] = s_passes[i+1];
        s_n_passes--;
        save_passes();
    }
    open_library();
}

static void on_edit_title(lv_event_t *e)    { (void)e; open_field(F_TITLE); }
static void on_edit_subtitle(lv_event_t *e) { (void)e; open_field(F_SUBTITLE); }
static void on_edit_code(lv_event_t *e)     { (void)e; open_field(F_CODE); }

/* ---- field input ---- */

static void on_field_save(lv_event_t *e)
{
    (void)e;
    const char *txt = lv_textarea_get_text(s_f_ta);
    int n = 0; while (txt[n] && n < MAX_FIELD - 1) n++;
    /* trim trailing whitespace */
    while (n > 0 && (txt[n-1]==' '||txt[n-1]=='\n'||txt[n-1]=='\r'||txt[n-1]=='\t')) n--;
    char *dst = (s_editing_field == F_TITLE)    ? s_editor_buf.title :
                (s_editing_field == F_SUBTITLE) ? s_editor_buf.subtitle :
                                                  s_editor_buf.code;
    for (int i = 0; i < n; i++) dst[i] = txt[i];
    dst[n] = 0;
    /* refresh editor labels */
    lv_label_set_text(s_e_title_label,    s_editor_buf.title[0]    ? s_editor_buf.title    : "(tap to set title)");
    lv_label_set_text(s_e_subtitle_label, s_editor_buf.subtitle[0] ? s_editor_buf.subtitle : "(tap to set subtitle)");
    lv_label_set_text(s_e_code_label,     s_editor_buf.code[0]     ? s_editor_buf.code     : "(tap to set code)");
    /* return to editor */
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view,    LV_OBJ_FLAG_HIDDEN);
    s_view = VIEW_EDITOR;
}

static void on_field_cancel(lv_event_t *e)
{
    (void)e;
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);
    s_view = VIEW_EDITOR;
}

static void open_field(field_id_t fid)
{
    s_editing_field = fid;
    s_view = VIEW_FIELD_INPUT;
    const char *labels[3] = { "Title", "Subtitle", "Code" };
    lv_label_set_text(s_f_title_label, labels[fid]);
    const char *cur = (fid == F_TITLE) ? s_editor_buf.title :
                      (fid == F_SUBTITLE) ? s_editor_buf.subtitle :
                                            s_editor_buf.code;
    lv_textarea_set_text(s_f_ta, cur);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);
}

/* ---- views ---- */

static void open_library(void)
{
    s_view = VIEW_LIBRARY;
    lv_obj_clear_flag(s_library_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clean(s_list_box);

    if (s_n_passes == 0) {
        lv_obj_t *empty = lv_label_create(s_list_box);
        lv_label_set_text(empty, "No passes yet.\nTap + to add one.");
        lv_obj_set_style_text_color(empty, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    for (int i = 0; i < s_n_passes; i++) {
        pass_t *p = &s_passes[i];
        lv_obj_t *body = hb_swipe_row_create(s_list_box, 224, 88, 64,
                            LV_SYMBOL_TRASH, 0xe63946,
                            on_row_tap, on_row_delete,
                            (void *)(uintptr_t)i);
        if (!body) continue;
        lv_obj_set_style_bg_color(body, lv_color_hex(p->color), 0);
        /* Subtle darker gradient bottom for depth. */
        uint32_t dark = (p->color & 0xfefefe) >> 1;
        lv_obj_set_style_bg_grad_color(body, lv_color_hex(dark), 0);
        lv_obj_set_style_bg_grad_dir(body, LV_GRAD_DIR_VER, 0);

        lv_obj_t *t = lv_label_create(body);
        lv_label_set_text(t, p->title);
        lv_obj_set_style_text_color(t, lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, 200);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 10, 8);

        if (p->subtitle[0]) {
            lv_obj_t *sub = lv_label_create(body);
            lv_label_set_text(sub, p->subtitle);
            lv_obj_set_style_text_color(sub, lv_color_hex(hb_color_text()), 0);
            lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
            lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
            lv_obj_set_width(sub, 200);
            lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 10, 36);
        }

        if (p->code[0]) {
            lv_obj_t *c = lv_label_create(body);
            lv_label_set_text(c, p->code);
            lv_obj_set_style_text_color(c, lv_color_hex(hb_color_text()), 0);
            lv_obj_set_style_text_font(c, &lv_font_montserrat_14, 0);
            lv_obj_align(c, LV_ALIGN_BOTTOM_LEFT, 10, -8);
        }
    }
}

static void on_detail_edit(lv_event_t *e) { (void)e; open_editor(s_edit_idx); }

static void open_detail(int idx)
{
    if (idx < 0 || idx >= s_n_passes) return;
    s_edit_idx = idx;
    pass_t *p = &s_passes[idx];
    s_view = VIEW_DETAIL;
    lv_obj_add_flag(s_library_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_style_bg_color(s_d_card, lv_color_hex(p->color), 0);
    uint32_t dark = (p->color & 0xfefefe) >> 1;
    lv_obj_set_style_bg_grad_color(s_d_card, lv_color_hex(dark), 0);
    lv_label_set_text(s_d_title, p->title);
    lv_label_set_text(s_d_subtitle, p->subtitle[0] ? p->subtitle : "");
    lv_label_set_text(s_d_code, p->code[0] ? p->code : "(no code)");
    /* Render the code as a QR. We hand the raw code text to qrcodegen
     * (it auto-picks numeric / alphanumeric / byte encoding). LVGL
     * picks the smallest version that fits. */
    if (s_d_qr) {
        if (p->code[0]) {
            uint32_t len = 0; while (p->code[len]) len++;
            lv_qrcode_update(s_d_qr, p->code, len);
            lv_obj_clear_flag(s_d_qr, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_d_qr, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_d_barcode) {
        if (p->code[0]) {
            uint32_t len = 0; while (p->code[len]) len++;
            lv_barcode_update(s_d_barcode, p->code);
            lv_obj_clear_flag(s_d_barcode, LV_OBJ_FLAG_HIDDEN);
            (void)len;
        } else {
            lv_obj_add_flag(s_d_barcode, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void open_editor(int idx)
{
    s_view = VIEW_EDITOR;
    s_edit_idx = idx;
    if (idx >= 0 && idx < s_n_passes) {
        s_editor_buf = s_passes[idx];
    } else {
        s_clear(s_editor_buf.title, MAX_FIELD);
        s_clear(s_editor_buf.subtitle, MAX_FIELD);
        s_clear(s_editor_buf.code, MAX_FIELD);
        s_editor_buf.color = 0x457b9d;
    }
    lv_label_set_text(s_e_title_label,    s_editor_buf.title[0]    ? s_editor_buf.title    : "(tap to set title)");
    lv_label_set_text(s_e_subtitle_label, s_editor_buf.subtitle[0] ? s_editor_buf.subtitle : "(tap to set subtitle)");
    lv_label_set_text(s_e_code_label,     s_editor_buf.code[0]     ? s_editor_buf.code     : "(tap to set code)");
    lv_obj_set_style_bg_color(s_e_color_chip,
        lv_color_hex(s_editor_buf.color), 0);

    lv_obj_add_flag(s_library_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);
}

/* ---- view construction ---- */

static void build_library(void)
{
    s_library_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_library_view, 240, 432);
    lv_obj_set_style_radius(s_library_view, 0, 0);
    lv_obj_set_pos(s_library_view, 0, 0);
    lv_obj_set_style_bg_color(s_library_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_library_view, 0, 0);
    lv_obj_set_style_pad_all(s_library_view, 0, 0);
    lv_obj_clear_flag(s_library_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(s_library_view);
    lv_label_set_text(hdr, "Wallet");
    lv_obj_set_style_text_color(hdr, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_24, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 12, 10);

    lv_obj_t *btn_new = lv_button_create(s_library_view);
    lv_obj_set_size(btn_new, 40, 36);
    lv_obj_align(btn_new, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_bg_color(btn_new, lv_color_hex(hb_color_success()), 0);
    lv_obj_set_style_radius(btn_new, 8, 0);
    lv_obj_add_event_cb(btn_new, on_new, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(btn_new);
    lv_label_set_text(nl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(nl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_20, 0);
    lv_obj_center(nl);

    s_list_box = lv_obj_create(s_library_view);
    lv_obj_set_size(s_list_box, 240, 432 - 56);
    lv_obj_align(s_list_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_list_box, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_list_box, 0, 0);
    lv_obj_set_style_pad_all(s_list_box, 6, 0);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
}

static void build_detail(void)
{
    s_detail_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_detail_view, 240, 432);
    lv_obj_set_style_radius(s_detail_view, 0, 0);
    lv_obj_set_pos(s_detail_view, 0, 0);
    lv_obj_set_style_bg_color(s_detail_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_detail_view, 0, 0);
    lv_obj_set_style_pad_all(s_detail_view, 0, 0);
    lv_obj_clear_flag(s_detail_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_detail_view);
    lv_obj_set_size(back, 56, 32);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(back, on_back_to_library, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    lv_obj_t *edit = lv_button_create(s_detail_view);
    lv_obj_set_size(edit, 56, 32);
    lv_obj_align(edit, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_bg_color(edit, lv_color_hex(hb_color_surface()), 0);
    lv_obj_add_event_cb(edit, on_detail_edit, LV_EVENT_CLICKED, NULL);
    lv_obj_t *el = lv_label_create(edit);
    lv_label_set_text(el, "Edit");
    lv_obj_set_style_text_color(el, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(el);

    s_d_card = lv_obj_create(s_detail_view);
    lv_obj_set_size(s_d_card, 220, 340);
    lv_obj_align(s_d_card, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(s_d_card, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_radius(s_d_card, 18, 0);
    lv_obj_set_style_border_width(s_d_card, 0, 0);
    lv_obj_set_style_shadow_width(s_d_card, 0, 0);
    lv_obj_clear_flag(s_d_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_d_card, LV_OBJ_FLAG_CLICKABLE);

    s_d_title = lv_label_create(s_d_card);
    lv_label_set_text(s_d_title, "");
    lv_obj_set_style_text_color(s_d_title, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_d_title, &lv_font_montserrat_28, 0);
    lv_obj_align(s_d_title, LV_ALIGN_TOP_MID, 0, 16);

    s_d_subtitle = lv_label_create(s_d_card);
    lv_label_set_text(s_d_subtitle, "");
    lv_obj_set_style_text_color(s_d_subtitle, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_d_subtitle, &lv_font_montserrat_16, 0);
    lv_obj_align(s_d_subtitle, LV_ALIGN_TOP_MID, 0, 58);

    s_d_qr = lv_qrcode_create(s_d_card);
    lv_qrcode_set_size(s_d_qr, 140);
    lv_qrcode_set_dark_color(s_d_qr, lv_color_hex(0x0a0a0a));
    lv_qrcode_set_light_color(s_d_qr, lv_color_hex(hb_color_text()));
    lv_obj_align(s_d_qr, LV_ALIGN_TOP_MID, 0, 96);

    s_d_barcode = lv_barcode_create(s_d_card);
    lv_barcode_set_dark_color(s_d_barcode, lv_color_hex(0x0a0a0a));
    lv_barcode_set_light_color(s_d_barcode, lv_color_hex(hb_color_text()));
    lv_barcode_set_scale(s_d_barcode, 1);
    lv_barcode_set_direction(s_d_barcode, LV_DIR_HOR);
    lv_obj_set_size(s_d_barcode, 200, 32);
    lv_obj_align(s_d_barcode, LV_ALIGN_TOP_MID, 0, 244);

    s_d_code = lv_label_create(s_d_card);
    lv_label_set_text(s_d_code, "");
    lv_obj_set_style_text_color(s_d_code, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_d_code, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_d_code, 200);
    lv_label_set_long_mode(s_d_code, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_d_code, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_d_code, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static lv_obj_t *make_editor_field_row(lv_obj_t *parent, int y,
                                       const char *label_text,
                                       lv_event_cb_t cb,
                                       lv_obj_t **out_label)
{
    lv_obj_t *lab_hdr = lv_label_create(parent);
    lv_label_set_text(lab_hdr, label_text);
    lv_obj_set_style_text_color(lab_hdr, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(lab_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lab_hdr, 12, y);

    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, 220, 36);
    lv_obj_set_pos(row, 10, y + 20);
    lv_obj_set_style_bg_color(row, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 200);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
    *out_label = l;
    return row;
}

static void build_editor(void)
{
    s_editor_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_editor_view, 240, 432);
    lv_obj_set_style_radius(s_editor_view, 0, 0);
    lv_obj_set_pos(s_editor_view, 0, 0);
    lv_obj_set_style_bg_color(s_editor_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_editor_view, 0, 0);
    lv_obj_set_style_pad_all(s_editor_view, 0, 0);
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_editor_view);
    lv_obj_set_size(back, 56, 32);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(back, on_back_to_library, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    lv_obj_t *save = lv_button_create(s_editor_view);
    lv_obj_set_size(save, 56, 32);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_bg_color(save, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(save, on_save_editor, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(sl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(sl);

    make_editor_field_row(s_editor_view, 50,  "Title",    on_edit_title,    &s_e_title_label);
    make_editor_field_row(s_editor_view, 116, "Subtitle", on_edit_subtitle, &s_e_subtitle_label);
    make_editor_field_row(s_editor_view, 182, "Code",     on_edit_code,     &s_e_code_label);

    /* Color picker — single chip that cycles palette. */
    lv_obj_t *col_hdr = lv_label_create(s_editor_view);
    lv_label_set_text(col_hdr, "Color");
    lv_obj_set_style_text_color(col_hdr, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(col_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(col_hdr, 12, 248);

    s_e_color_chip = lv_button_create(s_editor_view);
    lv_obj_set_size(s_e_color_chip, 60, 36);
    lv_obj_set_pos(s_e_color_chip, 10, 268);
    lv_obj_set_style_bg_color(s_e_color_chip, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_radius(s_e_color_chip, 6, 0);
    lv_obj_add_event_cb(s_e_color_chip, on_color_cycle, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cyl = lv_label_create(s_e_color_chip);
    lv_label_set_text(cyl, "tap");
    lv_obj_set_style_text_color(cyl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(cyl);

    /* Delete (shown only when editing existing — for simplicity it's
       always present; tapping when adding-new is a no-op via the
       editor's s_edit_idx == -1 branch). */
    lv_obj_t *del = lv_button_create(s_editor_view);
    lv_obj_set_size(del, 140, 36);
    lv_obj_set_pos(del, 90, 268);
    lv_obj_set_style_bg_color(del, lv_color_hex(hb_color_danger()), 0);
    lv_obj_set_style_radius(del, 6, 0);
    lv_obj_add_event_cb(del, on_delete_from_editor, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(del);
    lv_label_set_text(dl, LV_SYMBOL_TRASH "  Delete pass");
    lv_obj_set_style_text_color(dl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(dl);
}

static void build_field_input(void)
{
    s_field_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_field_view, 240, 432);
    lv_obj_set_style_radius(s_field_view, 0, 0);
    lv_obj_set_pos(s_field_view, 0, 0);
    lv_obj_set_style_bg_color(s_field_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_field_view, 0, 0);
    lv_obj_set_style_pad_all(s_field_view, 0, 0);
    lv_obj_clear_flag(s_field_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_field_view);
    lv_obj_set_size(back, 56, 32);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(back, on_field_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    s_f_title_label = lv_label_create(s_field_view);
    lv_label_set_text(s_f_title_label, "Field");
    lv_obj_set_style_text_color(s_f_title_label, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_f_title_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_f_title_label, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *save = lv_button_create(s_field_view);
    lv_obj_set_size(save, 56, 32);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_bg_color(save, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(save, on_field_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(sl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(sl);

    s_f_ta = lv_textarea_create(s_field_view);
    lv_obj_set_size(s_f_ta, 240, 152);
    lv_obj_align(s_f_ta, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(s_f_ta, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_text_color(s_f_ta, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_border_color(s_f_ta, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_radius(s_f_ta, 0, 0);
    lv_textarea_set_one_line(s_f_ta, true);

    lv_obj_t *kb_box = lv_obj_create(s_field_view);
    lv_obj_set_size(kb_box, 240, 232);
    lv_obj_align(kb_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb_box, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_border_width(kb_box, 0, 0);
    lv_obj_set_style_pad_all(kb_box, 0, 0);
    lv_obj_clear_flag(kb_box, LV_OBJ_FLAG_SCROLLABLE);
    hb_t9_create(kb_box, s_f_ta);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    load_passes();

    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    build_library();
    build_detail();
    build_editor();
    build_field_input();

    open_library();
}
