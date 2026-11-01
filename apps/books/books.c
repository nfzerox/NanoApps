/*
 * books — plain-text book reader with paging + bookmarks.
 *
 * Library view: scrollable list of .txt files in /Data/Books/.
 * Reader view: shows one "page" of the book at a time (~30 lines of
 * 14-pt text). Tap left third to go back, right two-thirds to go
 * forward. Reading position is persisted per book at
 * /Data/Books/.pos.<name> so reopening a book picks up where
 * you left off.
 *
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_heap.h"
#include "lvgl/lvgl.h"

#define DATA_DIR     "/Data/Books"
#define MAX_BOOKS    64
#define MAX_FILENAME 64
#define PAGE_CHARS   700
/* Books that load via lv_malloc would be capped to the 64 KB LVGL
 * heap. Instead we park the book body at a fixed high-DRAM address
 * (above the per-app canvas scratch and Paint thumb pool) so we can
 * comfortably hold real Project Gutenberg novels. */
#define MAX_BOOK_SZ     (2 * 1024 * 1024)   /* 2 MB cap per book */

typedef struct {
    char name[MAX_FILENAME];
    uint32_t size;
} book_t;
static book_t s_books[MAX_BOOKS];
static int    s_n_books = 0;

/* Open book state */
static char     s_open_name[MAX_FILENAME];
static char    *s_book_blob = NULL;
static uint32_t s_book_size = 0;
static uint32_t s_page_starts[2048];   /* file offsets per page */
static int      s_n_pages = 0;
static int      s_cur_page = 0;

typedef enum { VIEW_LIBRARY, VIEW_READER } view_t;
static view_t s_view = VIEW_LIBRARY;

static lv_obj_t *s_scr;
static lv_obj_t *s_library_view;
static lv_obj_t *s_reader_view;
static lv_obj_t *s_list_box;
static lv_obj_t *s_page_lbl;
static lv_obj_t *s_progress_lbl;
static lv_obj_t *s_title_lbl;

static void itoa_u(uint32_t v, char *out)
{
    char b[12]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    int k = 0; while (i) out[k++] = b[--i];
    out[k] = 0;
}
static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* ---- library scan ---- */

static bool ends_with_txt(const char *s)
{
    int n = s_len(s);
    return n >= 5 && s[n-4]=='.' && s[n-3]=='t' && s[n-2]=='x' && s[n-1]=='t';
}

static void scan_books(void)
{
    s_n_books = 0;
    hb_dir_t d;
    if (!hb_fs_dir_open(&d, DATA_DIR, false)) return;
    char fn[MAX_FILENAME]; bool is_dir = false;
    while (hb_fs_dir_next(&d, fn, sizeof fn, &is_dir)) {
        if (is_dir || fn[0] == '.') continue;
        if (!ends_with_txt(fn)) continue;
        if (s_n_books >= MAX_BOOKS) break;
        int i = 0;
        while (fn[i] && i < MAX_FILENAME - 1) { s_books[s_n_books].name[i] = fn[i]; i++; }
        s_books[s_n_books].name[i] = 0;
        char path[256]; int pk = 0;
        const char *p = DATA_DIR "/"; while (*p) path[pk++] = *p++;
        for (int j = 0; fn[j] && pk < 254; j++) path[pk++] = fn[j];
        path[pk] = 0;
        s_books[s_n_books].size = hb_fs_size(path);
        s_n_books++;
    }
    hb_fs_dir_close(&d);
}

/* ---- text normalization ----
 * Project Gutenberg text uses hard-wrapped lines (~70 chars) and
 * UTF-8 smart quotes / em-dashes / ellipsis. Our montserrat font
 * doesn't have glyphs for those code points so they render as
 * tofu. And the hard wrap looks ragged at 240 px wide.
 *
 * Normalize the blob in place before paging:
 *   - reflow: single '\n' inside a paragraph -> ' '. Blank line
 *     (two or more consecutive '\n') -> single '\n' (paragraph
 *     break). This preserves chapter structure while letting LVGL
 *     decide where to soft-wrap.
 *   - replace U+2018/2019 -> ', U+201C/201D -> ", U+2013/2014 -> -,
 *     U+2026 -> ...,  U+00A0 -> ' '.
 *
 * The on-disk file is untouched.
 */
static void normalize_blob(void)
{
    if (!s_book_blob || s_book_size == 0) return;
    char *p = s_book_blob;
    uint32_t in = 0, out = 0;
    while (in < s_book_size) {
        unsigned char c = (unsigned char)p[in];
        /* Multi-byte UTF-8 sequences we care about (all 3-byte E2 80 xx). */
        if (c == 0xE2 && in + 2 < s_book_size && (unsigned char)p[in+1] == 0x80) {
            unsigned char x = (unsigned char)p[in+2];
            switch (x) {
                case 0x98: case 0x99: p[out++] = '\''; in += 3; continue;
                case 0x9C: case 0x9D: p[out++] = '"';  in += 3; continue;
                case 0x93: case 0x94: p[out++] = '-';  in += 3; continue;
                case 0xA6: p[out++]='.'; p[out++]='.'; p[out++]='.'; in += 3; continue;
                default: break;
            }
        }
        /* NBSP (C2 A0) */
        if (c == 0xC2 && in + 1 < s_book_size && (unsigned char)p[in+1] == 0xA0) {
            p[out++] = ' '; in += 2; continue;
        }
        /* Reflow: collapse a run of \n into ' ' (single newline) or
         * '\n' (two or more = paragraph break). */
        if (c == '\n' || c == '\r') {
            int nl = 0;
            while (in < s_book_size && (p[in] == '\n' || p[in] == '\r')) {
                if (p[in] == '\n') nl++;
                in++;
            }
            p[out++] = (nl >= 2) ? '\n' : ' ';
            continue;
        }
        /* Strip any other bytes outside printable ASCII + tab. */
        if (c >= 0x20 && c < 0x7F) { p[out++] = (char)c; in++; continue; }
        if (c == '\t') { p[out++] = ' '; in++; continue; }
        /* Drop high bytes we didn't translate (rare in PG plain text). */
        in++;
    }
    /* Collapse double spaces. */
    uint32_t out2 = 0;
    int last_space = 0;
    for (uint32_t i = 0; i < out; i++) {
        if (p[i] == ' ') {
            if (last_space) continue;
            last_space = 1;
        } else last_space = 0;
        p[out2++] = p[i];
    }
    s_book_size = out2;
    s_book_blob[out2] = 0;
}

/* ---- pagination ---- */

/* Page geometry — must match the s_page_lbl (width 220, montserrat_14, at y=40;
   tap-to-flip so the page runs to near the bottom). */
#define PAGE_W          220
#define PAGE_H          368
#define PAGE_MAX_CHARS  1600

static char s_meas[PAGE_MAX_CHARS + 1];

/* Rendered height of book bytes [pos, pos+len) wrapped at PAGE_W. */
static int32_t measured_height(uint32_t pos, uint32_t len)
{
    uint32_t i = 0;
    for (; i < len && i < PAGE_MAX_CHARS; i++) {
        char c = s_book_blob[pos + i];
        s_meas[i] = (c == '\r') ? ' ' : c;
    }
    s_meas[i] = 0;
    lv_point_t sz;
    lv_text_get_size(&sz, s_meas, &lv_font_montserrat_14, 0, 0, PAGE_W, LV_TEXT_FLAG_NONE);
    return sz.y;
}

/* Break each page where the rendered text actually fills PAGE_H (binary search),
   not by a fixed char count — the old 700-char slice overflowed the visible area
   so the clipped tail was skipped (lost between pages). */
static void compute_pages(void)
{
    s_n_pages = 0;
    if (!s_book_blob || s_book_size == 0) return;
    uint32_t pos = 0;
    const int MAXP = (int)(sizeof s_page_starts / sizeof s_page_starts[0]);
    while (pos < s_book_size && s_n_pages < MAXP) {
        s_page_starts[s_n_pages++] = pos;
        uint32_t remaining = s_book_size - pos;
        uint32_t hi = remaining < PAGE_MAX_CHARS ? remaining : PAGE_MAX_CHARS;
        uint32_t lo = 1, best = 1;
        while (lo <= hi) {
            uint32_t mid = (lo + hi) / 2;
            if (measured_height(pos, mid) <= PAGE_H) { best = mid; lo = mid + 1; }
            else { if (mid == 0) break; hi = mid - 1; }
        }
        uint32_t len = best;
        /* back up to a word boundary unless we consumed the rest of the book */
        if (pos + len < s_book_size) {
            uint32_t b = len;
            while (b > 1 && s_book_blob[pos + b] != ' ' && s_book_blob[pos + b] != '\n') b--;
            if (b > 1) len = b;
        }
        pos += len;
        while (pos < s_book_size && (s_book_blob[pos] == ' ' || s_book_blob[pos] == '\n')) pos++;
    }
}

static void make_pos_path(const char *name, char *out)
{
    const char *p = DATA_DIR "/.pos."; int k = 0;
    while (*p) out[k++] = *p++;
    for (int i = 0; name[i] && k < 250; i++) out[k++] = name[i];
    out[k] = 0;
}

static void load_position(const char *name)
{
    char path[260]; make_pos_path(name, path);
    char buf[12];
    uint32_t n = hb_fs_read(path, buf, sizeof buf - 1);
    if (n == 0) { s_cur_page = 0; return; }
    buf[n] = 0;
    int v = 0;
    for (uint32_t i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) v = v * 10 + (buf[i] - '0');
    s_cur_page = v;
    if (s_cur_page < 0) s_cur_page = 0;
    if (s_cur_page >= s_n_pages) s_cur_page = s_n_pages - 1;
}
static void save_position(void)
{
    if (!s_open_name[0]) return;
    char path[260]; make_pos_path(s_open_name, path);
    char buf[12]; itoa_u((uint32_t)s_cur_page, buf);
    int n = 0; while (buf[n]) n++;
    hb_fs_write(path, buf, (uint32_t)n);
}

/* ---- views ---- */

static void show_library(void);
static void show_reader(int idx);

static void on_row_clicked(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    show_reader(idx);
}

static void render_page(void)
{
    if (!s_book_blob || s_n_pages == 0) {
        lv_label_set_text(s_page_lbl, "(empty book)");
        lv_label_set_text(s_progress_lbl, "");
        return;
    }
    if (s_cur_page < 0) s_cur_page = 0;
    if (s_cur_page >= s_n_pages) s_cur_page = s_n_pages - 1;
    uint32_t start = s_page_starts[s_cur_page];
    uint32_t end = (s_cur_page + 1 < s_n_pages)
        ? s_page_starts[s_cur_page + 1] : s_book_size;
    static char page_buf[PAGE_MAX_CHARS + 1];
    uint32_t take = end - start;
    if (take > sizeof page_buf - 1) take = sizeof page_buf - 1;
    for (uint32_t i = 0; i < take; i++) {
        char c = s_book_blob[start + i];
        if (c == '\r') c = ' ';
        page_buf[i] = c;
    }
    page_buf[take] = 0;
    lv_label_set_text(s_page_lbl, page_buf);

    char prog[32]; int k = 0;
    char nb[12]; itoa_u((uint32_t)(s_cur_page + 1), nb);
    for (int i = 0; nb[i]; i++) prog[k++] = nb[i];
    prog[k++] = '/';
    itoa_u((uint32_t)s_n_pages, nb);
    for (int i = 0; nb[i]; i++) prog[k++] = nb[i];
    prog[k] = 0;
    lv_label_set_text(s_progress_lbl, prog);
}

static void on_reader_tap(lv_event_t *e)
{
    (void)e;
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    /* Tap left third = prev page; rest = next page. */
    if (p.x < 80) {
        if (s_cur_page > 0) { s_cur_page--; save_position(); render_page(); }
    } else {
        if (s_cur_page + 1 < s_n_pages) { s_cur_page++; save_position(); render_page(); }
    }
}

/* The book text lives in an OS-heap block registered as the app's aux block, so
 * the resident frees it if the user quits mid-book (the old fixed 0x09400000
 * buffer lay inside the live OS heap). */
static void book_blob_free(void)
{
    hb_os_free(HB_APP_AUX_BLOCK);
    HB_APP_AUX_BLOCK = 0;
    s_book_blob = NULL;
}

static void on_back_to_library(lv_event_t *e)
{
    (void)e;
    save_position();
    book_blob_free();
    show_library();
}

static void show_library(void)
{
    s_view = VIEW_LIBRARY;
    lv_obj_clear_flag(s_library_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_reader_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clean(s_list_box);
    if (s_n_books == 0) {
        /* Centre the placeholder in the flex column (lv_obj_align is ignored under
           a flex layout, so use flex alignment instead). */
        lv_obj_set_flex_align(s_list_box, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *empty = lv_label_create(s_list_box);
        lv_label_set_text(empty,
            "No books yet.\nDrop .txt files into\n/Data/Books/");
        lv_obj_set_style_text_color(empty, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_width(empty, lv_pct(90));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    lv_obj_set_flex_align(s_list_box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    for (int i = 0; i < s_n_books; i++) {
        lv_obj_t *row = lv_button_create(s_list_box);
        lv_obj_set_size(row, 224, 44);
        lv_obj_set_style_bg_color(row, lv_color_hex(hb_color_surface()), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_margin_bottom(row, 4, 0);
        lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        lv_obj_t *t = lv_label_create(row);
        lv_label_set_text(t, s_books[i].name);
        lv_obj_set_style_text_color(t, lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, 200);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 8, 0);
    }
}

static void show_reader(int idx)
{
    if (idx < 0 || idx >= s_n_books) return;
    for (int i = 0; i < MAX_FILENAME; i++) s_open_name[i] = s_books[idx].name[i];

    char path[256]; int pk = 0;
    const char *p = DATA_DIR "/"; while (*p) path[pk++] = *p++;
    for (int i = 0; s_books[idx].name[i] && pk < 254; i++) path[pk++] = s_books[idx].name[i];
    path[pk] = 0;

    uint32_t sz = s_books[idx].size;
    if (sz == 0) return;
    if (sz > MAX_BOOK_SZ) sz = MAX_BOOK_SZ;
    book_blob_free();
    s_book_blob = (char *)hb_os_alloc(sz + 1u);
    if (!s_book_blob) return;          /* no memory: stay in the library */
    HB_APP_AUX_BLOCK = s_book_blob;
    s_view = VIEW_READER;
    uint32_t rd = hb_fs_read(path, s_book_blob, sz);
    s_book_blob[rd < sz ? rd : sz] = 0;
    s_book_size = rd;
    normalize_blob();
    compute_pages();
    load_position(s_books[idx].name);

    lv_label_set_text(s_title_lbl, s_books[idx].name);

    lv_obj_add_flag(s_library_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_reader_view, LV_OBJ_FLAG_HIDDEN);

    render_page();
}

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
    lv_label_set_text(hdr, "Books");
    lv_obj_set_style_text_color(hdr, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_24, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 12, 10);

    s_list_box = lv_obj_create(s_library_view);
    lv_obj_set_size(s_list_box, 240, 432 - 48);
    lv_obj_align(s_list_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_list_box, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_list_box, 0, 0);
    lv_obj_set_style_pad_all(s_list_box, 6, 0);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
}

static void build_reader(void)
{
    s_reader_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_reader_view, 240, 432);
    lv_obj_set_style_radius(s_reader_view, 0, 0);
    lv_obj_set_pos(s_reader_view, 0, 0);
    lv_obj_set_style_bg_color(s_reader_view, lv_color_hex(0xf5f0e0), 0);
    lv_obj_set_style_border_width(s_reader_view, 0, 0);
    lv_obj_set_style_pad_all(s_reader_view, 0, 0);
    lv_obj_clear_flag(s_reader_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_reader_view, LV_OBJ_FLAG_HIDDEN);
    /* Tap anywhere on the page area to flip pages — exclude the
       top header strip. */
    lv_obj_add_flag(s_reader_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_reader_view, on_reader_tap, LV_EVENT_PRESSED, NULL);

    lv_obj_t *back = lv_button_create(s_reader_view);
    lv_obj_set_size(back, 50, 28);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(back, on_back_to_library, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    s_title_lbl = lv_label_create(s_reader_view);
    lv_label_set_text(s_title_lbl, "");
    lv_obj_set_style_text_color(s_title_lbl, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_title_lbl, 130);
    lv_obj_align(s_title_lbl, LV_ALIGN_TOP_MID, 0, 10);

    s_progress_lbl = lv_label_create(s_reader_view);
    lv_label_set_text(s_progress_lbl, "");
    lv_obj_set_style_text_color(s_progress_lbl, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(s_progress_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_progress_lbl, LV_ALIGN_TOP_RIGHT, -8, 10);

    s_page_lbl = lv_label_create(s_reader_view);
    lv_label_set_text(s_page_lbl, "");
    lv_obj_set_style_text_color(s_page_lbl, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_text_font(s_page_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_page_lbl, 220);
    lv_label_set_long_mode(s_page_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_page_lbl, LV_ALIGN_TOP_LEFT, 10, 40);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    scan_books();

    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    build_library();
    build_reader();

    show_library();
}
