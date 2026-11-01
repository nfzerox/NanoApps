/*
 * reminders — a checkable to-do list with T9 input.
 *
 * List view: scrollable rows, each with a checkbox + text. Tapping
 * the checkbox toggles done. A "+" button (top right) opens the
 * editor to add a new item. The first row of the list is itself a
 * "Show/Hide done" toggle (scrolled out of view by default so it
 * doesn't clutter the toolbar — pull-down to access).
 *
 * Editor view: header (Back / Save) + textarea + T9 keyboard.
 *
 * Each row supports iOS-style swipe-to-reveal delete.
 *
 * Storage: /Data/Reminders/items.txt — one line per item,
 * format "<status>\t<text>\n" where status is '0' (pending) or '1'
 * (done). Atomic full-file rewrite on every change.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_t9.h"
#include "hb_swipe_row.h"
#include "lvgl/lvgl.h"

#define MAX_ITEMS    128
#define MAX_TEXT     128
#define FILE_BUF_SZ  (MAX_ITEMS * (MAX_TEXT + 4))
#define DATA_DIR     "/Data/Reminders"
#define MAX_LISTS    16
#define LIST_LEN     32

static char s_list[LIST_LEN] = "";              /* "" = default list (root) */
static char s_lists[MAX_LISTS][LIST_LEN];
static int  s_n_lists = 0;

static void list_dir_path(char *out)
{
    int i = 0;
    const char *p = DATA_DIR; while (*p) out[i++] = *p++;
    if (s_list[0]) {
        out[i++] = '/';
        for (int k = 0; s_list[k]; k++) out[i++] = s_list[k];
    }
    out[i] = 0;
}
static void items_path(char *out)
{
    list_dir_path(out);
    int i = 0; while (out[i]) i++;
    out[i++] = '/'; out[i++] = 'i'; out[i++] = 't'; out[i++] = 'e';
    out[i++] = 'm'; out[i++] = 's'; out[i++] = '.'; out[i++] = 't';
    out[i++] = 'x'; out[i++] = 't'; out[i] = 0;
}
static void hide_path(char *out)
{
    list_dir_path(out);
    int i = 0; while (out[i]) i++;
    out[i++] = '/'; out[i++] = 'h'; out[i++] = 'i'; out[i++] = 'd';
    out[i++] = 'e'; out[i++] = '_'; out[i++] = 'd'; out[i++] = 'o';
    out[i++] = 'n'; out[i++] = 'e'; out[i] = 0;
}

typedef struct {
    bool done;
    char text[MAX_TEXT];
} item_t;

static item_t s_items[MAX_ITEMS];
static int    s_n_items   = 0;
static bool   s_hide_done = false;

typedef enum { VIEW_LIST, VIEW_EDITOR } view_t;
static view_t   s_view = VIEW_LIST;

static lv_obj_t *s_scr;
static lv_obj_t *s_list_view;
static lv_obj_t *s_editor_view;
static lv_obj_t *s_list_box;
static lv_obj_t *s_empty_msg;       /* "All done" / "No reminders" overlay */
static lv_obj_t *s_ta;
static lv_obj_t *s_hide_btn_label;  /* label inside the in-list toggle row */

/* ---- persistence ---- */

static void load_items(void)
{
    s_n_items = 0;
    static char buf[FILE_BUF_SZ];
    uint32_t n = ({ char _p[128]; items_path(_p); hb_fs_read(_p, buf, sizeof buf - 1); });
    if (n == 0) return;
    buf[n] = 0;

    uint32_t i = 0;
    while (i < n && s_n_items < MAX_ITEMS) {
        if (buf[i] != '0' && buf[i] != '1') {
            while (i < n && buf[i] != '\n') i++;
            if (i < n) i++;
            continue;
        }
        item_t *it = &s_items[s_n_items];
        it->done = (buf[i] == '1');
        i++;
        if (i < n && buf[i] == '\t') i++;
        int k = 0;
        while (i < n && buf[i] != '\n' && k < MAX_TEXT - 1) {
            it->text[k++] = buf[i++];
        }
        it->text[k] = 0;
        while (i < n && buf[i] != '\n') i++;
        if (i < n) i++;
        s_n_items++;
    }
}

static void save_items(void)
{
    static char buf[FILE_BUF_SZ];
    uint32_t pos = 0;
    for (int i = 0; i < s_n_items; i++) {
        item_t *it = &s_items[i];
        if (pos + MAX_TEXT + 4 > sizeof buf) break;
        buf[pos++] = it->done ? '1' : '0';
        buf[pos++] = '\t';
        for (int j = 0; it->text[j] && pos < sizeof buf - 1; j++) {
            buf[pos++] = it->text[j];
        }
        buf[pos++] = '\n';
    }
    ({ char _p[128]; items_path(_p); hb_fs_write(_p, buf, pos); });
}

static void load_pref(void)
{
    char b[2];
    if (({ char _p[128]; hide_path(_p); hb_fs_read(_p, b, 1); }) > 0) {
        s_hide_done = (b[0] == '1');
    }
}

static void save_pref(void)
{
    char c = s_hide_done ? '1' : '0';
    ({ char _p[128]; hide_path(_p); hb_fs_write(_p, &c, 1); });
}

/* ---- view machinery ---- */

static void open_list(void);
static void open_editor(void);

static void on_check_toggled(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_n_items) return;
    s_items[idx].done = !s_items[idx].done;
    save_items();
    open_list();
}

/* swipe-row callbacks — cookie = item index */
static void on_row_tap(void *cookie)
{
    /* Tapping the body itself isn't a deletion; for reminders, the
       body has no separate "open" action (no editor for existing
       items in v1). So tap is a no-op — checkbox handles done, swipe
       handles delete. */
    (void)cookie;
}

static void on_row_delete(void *cookie)
{
    int idx = (int)(uintptr_t)cookie;
    if (idx < 0 || idx >= s_n_items) return;
    for (int i = idx; i < s_n_items - 1; i++) s_items[i] = s_items[i + 1];
    s_n_items--;
    save_items();
    open_list();
}

static void on_new_clicked(lv_event_t *e)
{
    (void)e;
    open_editor();
}

static void on_toggle_hide(lv_event_t *e)
{
    (void)e;
    s_hide_done = !s_hide_done;
    save_pref();
    open_list();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    open_list();
}

static void on_save_new(lv_event_t *e)
{
    (void)e;
    const char *txt = lv_textarea_get_text(s_ta);
    int n = 0; while (txt[n]) n++;
    while (n > 0 && (txt[n-1] == ' ' || txt[n-1] == '\n' ||
                     txt[n-1] == '\r' || txt[n-1] == '\t')) n--;
    if (n > 0 && s_n_items < MAX_ITEMS) {
        item_t *it = &s_items[s_n_items++];
        it->done = false;
        int copy = n < MAX_TEXT - 1 ? n : MAX_TEXT - 1;
        for (int i = 0; i < copy; i++) it->text[i] = txt[i];
        it->text[copy] = 0;
        save_items();
    }
    open_list();
}

/* Build the "Show/Hide done" toggle row that sits as the FIRST item
   in the list. Tap toggles the filter; it's intentionally above the
   first visible reminder so it's only seen if the user scrolls up. */
static void add_toggle_row(void)
{
    lv_obj_t *row = lv_obj_create(s_list_box);
    lv_obj_set_size(row, 224, 40);
    lv_obj_set_style_bg_color(row, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_margin_bottom(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_toggle_hide, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, s_hide_done ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(icon, lv_color_hex(hb_color_primary()), 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);

    s_hide_btn_label = lv_label_create(row);
    lv_label_set_text(s_hide_btn_label,
        s_hide_done ? "Show completed" : "Hide completed");
    lv_obj_set_style_text_color(s_hide_btn_label, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_hide_btn_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_hide_btn_label, LV_ALIGN_LEFT_MID, 40, 0);
}

static void open_list(void)
{
    s_view = VIEW_LIST;
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clean(s_list_box);

    /* Toggle row first — scrolled away by default since it sits above
       the natural top-visible content when the list overflows. We
       want it accessible but not in the way. */
    add_toggle_row();

    int visible_items = 0;
    for (int i = 0; i < s_n_items; i++) {
        if (s_hide_done && s_items[i].done) continue;
        visible_items++;
    }

    /* Empty message — centered over the WHOLE view (not just the
       list_box), so it dominates the screen when there's nothing to
       show. Created lazily inside the list_view as an overlay. */
    if (visible_items == 0) {
        if (!s_empty_msg) {
            s_empty_msg = lv_label_create(s_list_view);
            lv_obj_set_style_text_color(s_empty_msg, lv_color_hex(hb_color_text_dim()), 0);
            lv_obj_set_style_text_font(s_empty_msg, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_align(s_empty_msg, LV_TEXT_ALIGN_CENTER, 0);
        }
        lv_label_set_text(s_empty_msg,
            (s_hide_done && s_n_items > 0)
                ? "All done!\nNothing pending."
                : "No reminders yet.\nTap + to add one.");
        lv_obj_clear_flag(s_empty_msg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_empty_msg, LV_ALIGN_CENTER, 0, 0);
    } else if (s_empty_msg) {
        lv_obj_add_flag(s_empty_msg, LV_OBJ_FLAG_HIDDEN);
    }

    /* Scroll the toggle row out of immediate view so the user sees
       items first; the toggle is one short "pull down" away. */
    if (visible_items > 0) {
        lv_obj_scroll_to_y(s_list_box, 44, LV_ANIM_OFF);
    }

    for (int i = 0; i < s_n_items; i++) {
        if (s_hide_done && s_items[i].done) continue;

        lv_obj_t *body = hb_swipe_row_create(s_list_box,
                            224, 44, 64,
                            LV_SYMBOL_TRASH, 0xe63946,
                            on_row_tap, on_row_delete,
                            (void *)(uintptr_t)i);
        if (!body) continue;
        lv_obj_set_style_bg_color(body,
            s_items[i].done ? lv_color_hex(hb_color_surface()) : lv_color_hex(hb_color_surface()), 0);

        /* checkbox button */
        lv_obj_t *cb = lv_button_create(body);
        lv_obj_set_size(cb, 28, 28);
        lv_obj_align(cb, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_set_style_bg_color(cb,
            s_items[i].done ? lv_color_hex(hb_color_success()) : lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_style_radius(cb, 6, 0);
        lv_obj_set_style_pad_all(cb, 0, 0);
        lv_obj_set_style_shadow_width(cb, 0, 0);
        lv_obj_add_event_cb(cb, on_check_toggled, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        if (s_items[i].done) {
            lv_obj_t *chk = lv_label_create(cb);
            lv_label_set_text(chk, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(chk, lv_color_hex(hb_color_text()), 0);
            lv_obj_center(chk);
        }

        /* item text */
        lv_obj_t *lab = lv_label_create(body);
        lv_label_set_text(lab, s_items[i].text);
        lv_obj_set_style_text_color(lab,
            s_items[i].done ? lv_color_hex(hb_color_text_dim()) : lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_16, 0);
        lv_obj_set_width(lab, 180);
        lv_label_set_long_mode(lab, LV_LABEL_LONG_DOT);
        lv_obj_align(lab, LV_ALIGN_LEFT_MID, 42, 0);
    }
}

static void open_editor(void)
{
    s_view = VIEW_EDITOR;
    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(s_ta, "");
}

/* ---- list/folder picker ---- */
static lv_obj_t *s_lists_view;

static void scan_lists(void)
{
    s_n_lists = 0;
    hb_dir_t d;
    if (!hb_fs_dir_open(&d, DATA_DIR, false)) return;
    char fn[LIST_LEN]; bool is_dir;
    while (hb_fs_dir_next(&d, fn, sizeof fn, &is_dir)) {
        if (!is_dir) continue;
        if (fn[0] == '.') continue;
        if (s_n_lists >= MAX_LISTS) break;
        int k = 0;
        while (fn[k] && k < LIST_LEN - 1) { s_lists[s_n_lists][k] = fn[k]; k++; }
        s_lists[s_n_lists][k] = 0;
        s_n_lists++;
    }
    hb_fs_dir_close(&d);
}

static void on_list_pick(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == -1) {
        s_list[0] = 0;
    } else if (idx >= 0 && idx < s_n_lists) {
        int k = 0;
        while (s_lists[idx][k] && k < LIST_LEN - 1) { s_list[k] = s_lists[idx][k]; k++; }
        s_list[k] = 0;
    }
    char dir[160]; list_dir_path(dir);
    hb_fs_mkdir(dir);
    load_items();
    load_pref();
    if (s_lists_view) lv_obj_add_flag(s_lists_view, LV_OBJ_FLAG_HIDDEN);
    open_list();
}

static void on_new_list(lv_event_t *e)
{
    (void)e;
    char name[LIST_LEN];
    int n = 1;
    for (;;) {
        char nb[12]; int nb_i = 0;
        if (n == 0) nb[nb_i++] = '0';
        else { int t = n; char tmp[12]; int ti = 0;
               while (t) { tmp[ti++] = '0' + t % 10; t /= 10; }
               while (ti) nb[nb_i++] = tmp[--ti]; }
        nb[nb_i] = 0;
        int k = 0; const char *p = "List "; while (*p) name[k++] = *p++;
        int j = 0; while (nb[j]) name[k++] = nb[j++]; name[k] = 0;
        char dir[160]; int dk = 0;
        const char *dp = DATA_DIR; while (*dp) dir[dk++] = *dp++;
        dir[dk++] = '/';
        for (int i = 0; name[i]; i++) dir[dk++] = name[i];
        dir[dk] = 0;
        if (!hb_fs_exists(dir)) { hb_fs_mkdir(dir); break; }
        n++;
        if (n > 99) return;
    }
    int k = 0; while (name[k] && k < LIST_LEN - 1) { s_list[k] = name[k]; k++; }
    s_list[k] = 0;
    load_items();
    load_pref();
    if (s_lists_view) lv_obj_add_flag(s_lists_view, LV_OBJ_FLAG_HIDDEN);
    open_list();
}

static void open_lists(void)
{
    scan_lists();
    if (!s_lists_view) {
        s_lists_view = lv_obj_create(s_scr);
        lv_obj_set_size(s_lists_view, 240, 432);
        lv_obj_set_style_radius(s_lists_view, 0, 0);
        lv_obj_set_pos(s_lists_view, 0, 0);
        lv_obj_set_style_bg_color(s_lists_view, lv_color_hex(hb_color_bg()), 0);
        lv_obj_set_style_border_width(s_lists_view, 0, 0);
        lv_obj_set_style_pad_all(s_lists_view, 0, 0);
        lv_obj_clear_flag(s_lists_view, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_clean(s_lists_view);
    }

    lv_obj_t *hdr = lv_label_create(s_lists_view);
    lv_label_set_text(hdr, "Lists");
    lv_obj_set_style_text_color(hdr, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_24, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 12, 10);

    lv_obj_t *btn_add = lv_button_create(s_lists_view);
    lv_obj_set_size(btn_add, 40, 36);
    lv_obj_align(btn_add, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_bg_color(btn_add, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(btn_add, on_new_list, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = lv_label_create(btn_add); lv_label_set_text(al, LV_SYMBOL_PLUS); lv_obj_center(al);

    lv_obj_t *list = lv_list_create(s_lists_view);
    lv_obj_set_size(list, 240, 380);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_t *root = lv_list_add_button(list, LV_SYMBOL_HOME, "(Default)");
    lv_obj_add_event_cb(root, on_list_pick, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    for (int i = 0; i < s_n_lists; i++) {
        lv_obj_t *b = lv_list_add_button(list, LV_SYMBOL_LIST, s_lists[i]);
        lv_obj_add_event_cb(b, on_list_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_lists_view, LV_OBJ_FLAG_HIDDEN);
}

void reminders_on_lists_clicked(lv_event_t *e) { (void)e; open_lists(); }

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    load_pref();
    load_items();

    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    /* ---- List view ---- */
    s_list_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_list_view, 240, 432);
    lv_obj_set_style_radius(s_list_view, 0, 0);
    lv_obj_set_pos(s_list_view, 0, 0);
    lv_obj_set_style_bg_color(s_list_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_list_view, 0, 0);
    lv_obj_set_style_pad_all(s_list_view, 0, 0);
    lv_obj_clear_flag(s_list_view, LV_OBJ_FLAG_SCROLLABLE);

    /* Top bar: just the title and the + button — toggle moved into
       the list (first row, scrolled away by default). */
    lv_obj_t *hdr = lv_label_create(s_list_view);
    lv_label_set_text(hdr, "Reminders");
    lv_obj_set_style_text_color(hdr, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 12, 14);

    extern void reminders_on_lists_clicked(lv_event_t *e);
    lv_obj_t *btn_lists = lv_button_create(s_list_view);
    lv_obj_set_size(btn_lists, 40, 36);
    lv_obj_align(btn_lists, LV_ALIGN_TOP_RIGHT, -56, 8);
    lv_obj_set_style_bg_color(btn_lists, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_radius(btn_lists, 8, 0);
    lv_obj_add_event_cb(btn_lists, reminders_on_lists_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ll = lv_label_create(btn_lists);
    lv_label_set_text(ll, LV_SYMBOL_LIST);
    lv_obj_set_style_text_color(ll, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(ll);

    lv_obj_t *btn_new = lv_button_create(s_list_view);
    lv_obj_set_size(btn_new, 40, 36);
    lv_obj_align(btn_new, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_bg_color(btn_new, lv_color_hex(hb_color_success()), 0);
    lv_obj_set_style_radius(btn_new, 8, 0);
    lv_obj_add_event_cb(btn_new, on_new_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(btn_new);
    lv_label_set_text(nl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(nl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_20, 0);
    lv_obj_center(nl);

    s_list_box = lv_obj_create(s_list_view);
    lv_obj_set_size(s_list_box, 240, 432 - 56);
    lv_obj_align(s_list_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_list_box, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_list_box, 0, 0);
    lv_obj_set_style_pad_all(s_list_box, 6, 0);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);

    /* ---- Editor view ---- */
    s_editor_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_editor_view, 240, 432);
    lv_obj_set_style_radius(s_editor_view, 0, 0);
    lv_obj_set_pos(s_editor_view, 0, 0);
    lv_obj_set_style_bg_color(s_editor_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_editor_view, 0, 0);
    lv_obj_set_style_pad_all(s_editor_view, 0, 0);
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *btn_back = lv_button_create(s_editor_view);
    lv_obj_set_size(btn_back, 60, 36);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_back = lv_label_create(btn_back);
    lv_label_set_text(l_back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(l_back, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(l_back);

    lv_obj_t *etitle = lv_label_create(s_editor_view);
    lv_label_set_text(etitle, "New");
    lv_obj_set_style_text_color(etitle, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(etitle, &lv_font_montserrat_16, 0);
    lv_obj_align(etitle, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *btn_save = lv_button_create(s_editor_view);
    lv_obj_set_size(btn_save, 60, 36);
    lv_obj_align(btn_save, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(btn_save, on_save_new, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_save = lv_label_create(btn_save);
    lv_label_set_text(l_save, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(l_save, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(l_save);

    s_ta = lv_textarea_create(s_editor_view);
    lv_obj_set_size(s_ta, 240, 152);
    lv_obj_align(s_ta, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(s_ta, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_text_color(s_ta, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_border_color(s_ta, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_radius(s_ta, 0, 0);
    lv_textarea_set_one_line(s_ta, false);

    lv_obj_t *kb_box = lv_obj_create(s_editor_view);
    lv_obj_set_size(kb_box, 240, 232);
    lv_obj_align(kb_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb_box, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_border_width(kb_box, 0, 0);
    lv_obj_set_style_pad_all(kb_box, 0, 0);
    lv_obj_clear_flag(kb_box, LV_OBJ_FLAG_SCROLLABLE);
    hb_t9_create(kb_box, s_ta);

    open_list();
}
