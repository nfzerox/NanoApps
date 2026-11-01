/*
 * contacts — full-featured contacts app.
 *
 * Views:
 *   LIBRARY  -- scrolling list with monogram avatar + name + phone
 *               preview. Search box at top filters by substring on
 *               name. Swipe-row delete; tap to open.
 *   DETAIL   -- big monogram, name, then sectioned fields
 *               (phone, email, address, notes). Edit / Back buttons.
 *   EDITOR   -- tap a field to open the field-input view; T9
 *               keyboard for editing.
 *   FIELD    -- T9 keyboard fills a single field.
 *
 * Storage at /Data/Contacts/items.txt:
 *   name<TAB>phone<TAB>email<TAB>address<TAB>notes<NL>
 * Multi-line address/notes use literal '|' as a soft newline so the
 * record stays on one disk line.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_t9.h"
#include "hb_swipe_row.h"
#include "lvgl/lvgl.h"

#define MAX_CONTACTS 64
#define MAX_FIELD    96
#define DATA_DIR     "/Data/Contacts"
#define ITEMS_PATH   "/Data/Contacts/items.txt"
#define FILE_BUF_SZ  (MAX_CONTACTS * (MAX_FIELD * 5 + 16))

typedef struct {
    char name[MAX_FIELD];
    char phone[MAX_FIELD];
    char email[MAX_FIELD];
    char address[MAX_FIELD];
    char notes[MAX_FIELD];
} contact_t;

static contact_t s_contacts[MAX_CONTACTS];
static int       s_n = 0;
static int       s_open_idx = -1;
static contact_t s_buf;          /* editor scratch */
static char     *s_active_field = NULL; /* pointer into s_buf for field input */
static int       s_active_max = 0;
static const char *s_active_label = "";

typedef enum {
    V_LIBRARY, V_DETAIL, V_EDITOR, V_FIELD
} view_t;
static view_t s_view = V_LIBRARY;

static lv_obj_t *s_scr;
static lv_obj_t *s_lib_view;
static lv_obj_t *s_det_view;
static lv_obj_t *s_edit_view;
static lv_obj_t *s_fld_view;
static lv_obj_t *s_search_ta;
static lv_obj_t *s_list_box;

/* detail labels */
static lv_obj_t *s_d_mono, *s_d_name, *s_d_phone, *s_d_email, *s_d_address, *s_d_notes;

/* editor labels */
static lv_obj_t *s_e_name, *s_e_phone, *s_e_email, *s_e_address, *s_e_notes;

/* field input */
static lv_obj_t *s_fld_label, *s_fld_ta;
static lv_obj_t *s_t9;

/* ---- helpers ---- */
static void sclear(char *s, int n) { for (int i = 0; i < n; i++) s[i] = 0; }
static int  slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scopy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static int starts_with_ci(const char *hay, const char *needle)
{
    int i = 0;
    while (needle[i]) {
        char a = hay[i]; char b = needle[i];
        if (!a) return 0;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        i++;
    }
    return 1;
}
static int contains_ci(const char *hay, const char *needle)
{
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) if (starts_with_ci(hay + i, needle)) return 1;
    return 0;
}

/* '|' inside fields (used as soft newline on disk) <-> '\n' in memory */
static void disk_to_mem(char *s)
{
    for (int i = 0; s[i]; i++) if (s[i] == '|') s[i] = '\n';
}
static void mem_to_disk(char *s)
{
    for (int i = 0; s[i]; i++) if (s[i] == '\n' || s[i] == '\t') s[i] = '|';
}

/* ---- persistence ---- */
static void load_all(void)
{
    s_n = 0;
    static char buf[FILE_BUF_SZ];
    uint32_t n = hb_fs_read(ITEMS_PATH, buf, sizeof buf - 1);
    if (n == 0) return;
    buf[n] = 0;
    uint32_t i = 0;
    while (i < n && s_n < MAX_CONTACTS) {
        contact_t *c = &s_contacts[s_n];
        sclear(c->name, MAX_FIELD); sclear(c->phone, MAX_FIELD);
        sclear(c->email, MAX_FIELD); sclear(c->address, MAX_FIELD);
        sclear(c->notes, MAX_FIELD);
        char *targets[5] = { c->name, c->phone, c->email, c->address, c->notes };
        for (int f = 0; f < 5; f++) {
            int k = 0;
            while (i < n && buf[i] != '\t' && buf[i] != '\n' && k < MAX_FIELD - 1) {
                targets[f][k++] = buf[i++];
            }
            targets[f][k] = 0;
            disk_to_mem(targets[f]);
            if (i < n && buf[i] == '\t') i++;
            else break;
        }
        while (i < n && buf[i] != '\n') i++;
        if (i < n && buf[i] == '\n') i++;
        if (c->name[0]) s_n++;
    }
}

static void save_all(void)
{
    static char buf[FILE_BUF_SZ];
    int p = 0;
    for (int idx = 0; idx < s_n; idx++) {
        contact_t tmp = s_contacts[idx];
        char *fields[5] = { tmp.name, tmp.phone, tmp.email, tmp.address, tmp.notes };
        for (int f = 0; f < 5; f++) {
            mem_to_disk(fields[f]);
            for (int k = 0; fields[f][k] && p < (int)sizeof(buf) - 4; k++) buf[p++] = fields[f][k];
            buf[p++] = (f == 4) ? '\n' : '\t';
        }
    }
    hb_fs_mkdir("/Data"); hb_fs_mkdir(DATA_DIR);
    hb_fs_write(ITEMS_PATH, buf, p);
}

/* ---- views ---- */
static void show_library(void);
static void show_detail(int idx);
static void show_editor(int idx);
static void show_field(const char *label, char *target, int max_len);

static void on_row_tap(void *cookie)
{
    int idx = (int)(intptr_t)cookie;
    show_detail(idx);
}
static void on_row_delete(void *cookie)
{
    int idx = (int)(intptr_t)cookie;
    for (int i = idx; i < s_n - 1; i++) s_contacts[i] = s_contacts[i + 1];
    s_n--;
    save_all();
    show_library();
}

static char monogram_of(const contact_t *c)
{
    if (c->name[0] >= 'a' && c->name[0] <= 'z') return c->name[0] - 32;
    if (c->name[0]) return c->name[0];
    return '?';
}
static uint32_t monogram_color(const contact_t *c)
{
    static const uint32_t pal[8] = {
        0x457b9d, 0x2a9d8f, 0xe63946, 0xf4a261,
        0x6a4c93, 0x118ab2, 0x06d6a0, 0xef476f
    };
    uint32_t h = 0;
    for (int i = 0; c->name[i]; i++) h = h * 131 + (uint8_t)c->name[i];
    return pal[h & 7];
}

/* lv_list_add_button is one widget per row vs hb_swipe_row's ~5. With
 * 100+ contacts that 5x ratio blows the LVGL TLSF heap on launch, so
 * we use the simpler list and reach delete via long-press from detail. */
static void on_list_button_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    show_detail(idx);
}

static void rebuild_list(const char *filter)
{
    lv_obj_clean(s_list_box);
    for (int i = 0; i < s_n; i++) {
        if (filter && filter[0] && !contains_ci(s_contacts[i].name, filter)) continue;
        const char *sym = LV_SYMBOL_DUMMY;     /* lv_list_add_button needs a non-NULL symbol */
        (void)sym;
        lv_obj_t *btn = lv_list_add_button(s_list_box, NULL, s_contacts[i].name);
        lv_obj_add_event_cb(btn, on_list_button_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void on_search_changed(lv_event_t *e)
{
    (void)e;
    rebuild_list(lv_textarea_get_text(s_search_ta));
}

/* Tapping the search field toggles a T9 keyboard bound to it (results filter
   live above as you type); tap again to dismiss. */
static void on_search_clicked(lv_event_t *e)
{
    (void)e;
    if (s_t9) { lv_obj_del(s_t9); s_t9 = NULL; return; }
    s_t9 = hb_t9_create(s_lib_view, s_search_ta);
    if (s_t9) lv_obj_align(s_t9, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void on_add(lv_event_t *e)
{
    (void)e;
    show_editor(-1);
}

static void on_back_to_lib(lv_event_t *e) { (void)e; show_library(); }
static void on_back_to_det(lv_event_t *e) { (void)e; show_detail(s_open_idx); }
static void on_back_to_edit(lv_event_t *e) { (void)e; show_editor(s_open_idx); }

static void on_edit(lv_event_t *e) { (void)e; show_editor(s_open_idx); }
static void on_save(lv_event_t *e)
{
    (void)e;
    if (!s_buf.name[0]) { show_editor(s_open_idx); return; }   /* require name */
    if (s_open_idx < 0) {
        if (s_n < MAX_CONTACTS) { s_contacts[s_n++] = s_buf; s_open_idx = s_n - 1; }
    } else {
        s_contacts[s_open_idx] = s_buf;
    }
    save_all();
    show_detail(s_open_idx);
}

static void on_edit_name(lv_event_t *e) { (void)e; show_field("Name", s_buf.name, MAX_FIELD); }
static void on_edit_phone(lv_event_t *e) { (void)e; show_field("Phone", s_buf.phone, MAX_FIELD); }
static void on_edit_email(lv_event_t *e) { (void)e; show_field("Email", s_buf.email, MAX_FIELD); }
static void on_edit_addr(lv_event_t *e) { (void)e; show_field("Address", s_buf.address, MAX_FIELD); }
static void on_edit_notes(lv_event_t *e) { (void)e; show_field("Notes", s_buf.notes, MAX_FIELD); }

static void on_field_done(lv_event_t *e)
{
    (void)e;
    const char *txt = lv_textarea_get_text(s_fld_ta);
    scopy(s_active_field, txt, s_active_max);
    show_editor(s_open_idx);
}

/* ---- view builders ---- */
static void build_library(void)
{
    s_lib_view = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_lib_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_pad_all(s_lib_view, 6, 0);
    /* Prevent the screen from auto-scrolling on textarea focus. */
    lv_obj_clear_flag(s_lib_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_lib_view);
    lv_label_set_text(title, "Contacts");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *add = lv_button_create(s_lib_view);
    lv_obj_set_size(add, 40, 32);
    lv_obj_align(add, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *al = lv_label_create(add); lv_label_set_text(al, LV_SYMBOL_PLUS); lv_obj_center(al);
    lv_obj_add_event_cb(add, on_add, LV_EVENT_CLICKED, NULL);

    s_search_ta = lv_textarea_create(s_lib_view);
    lv_textarea_set_one_line(s_search_ta, true);
    lv_textarea_set_placeholder_text(s_search_ta, "Search...");
    lv_obj_set_size(s_search_ta, 228, 36);
    lv_obj_align(s_search_ta, LV_ALIGN_TOP_MID, 0, 34);
    /* The theme's vertical padding left less than one font line inside this
     * 36px field. Textarea's cursor positioning scrolls programmatically even
     * when SCROLLABLE is cleared, repeatedly chasing the clipped line. Fit the
     * line instead; retain normal cursor blinking and horizontal text movement. */
    lv_obj_set_style_pad_ver(s_search_ta, 4, 0);
    lv_obj_set_scrollbar_mode(s_search_ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_search_ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scroll_dir(s_search_ta, LV_DIR_HOR);
    /* Cancel a scroll queued during sizing, before the padding was corrected. */
    lv_obj_update_layout(s_search_ta);
    lv_obj_scroll_to_y(s_search_ta, 0, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_search_ta, on_search_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_search_ta, on_search_clicked, LV_EVENT_CLICKED, NULL);

    s_list_box = lv_list_create(s_lib_view);
    lv_obj_set_size(s_list_box, 228, 332);
    lv_obj_align(s_list_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_list_box, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_list_box, 0, 0);
    lv_obj_set_style_pad_all(s_list_box, 0, 0);
    /* Lock to vertical scroll only; horizontal-pan was firing on
     * accidental diagonal swipes and shifting rows off-screen. */
    lv_obj_set_scroll_dir(s_list_box, LV_DIR_VER);
}

static void show_library(void)
{
    s_view = V_LIBRARY;
    s_open_idx = -1;
    rebuild_list(lv_textarea_get_text(s_search_ta));
    lv_screen_load(s_lib_view);
}

static void build_detail(void)
{
    s_det_view = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_det_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_pad_all(s_det_view, 6, 0);

    lv_obj_t *back = lv_button_create(s_det_view);
    lv_obj_set_size(back, 48, 30);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT); lv_obj_center(bl);
    lv_obj_add_event_cb(back, on_back_to_lib, LV_EVENT_CLICKED, NULL);

    lv_obj_t *edit = lv_button_create(s_det_view);
    lv_obj_set_size(edit, 60, 30);
    lv_obj_align(edit, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *el = lv_label_create(edit); lv_label_set_text(el, "Edit"); lv_obj_center(el);
    lv_obj_add_event_cb(edit, on_edit, LV_EVENT_CLICKED, NULL);

    s_d_mono = lv_obj_create(s_det_view);
    lv_obj_set_size(s_d_mono, 80, 80);
    lv_obj_align(s_d_mono, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_radius(s_d_mono, 40, 0);
    lv_obj_set_style_border_width(s_d_mono, 0, 0);
    lv_obj_set_style_shadow_width(s_d_mono, 0, 0);
    lv_obj_set_style_pad_all(s_d_mono, 0, 0);
    lv_obj_clear_flag(s_d_mono, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_d_mono, LV_OBJ_FLAG_CLICKABLE);

    s_d_name = lv_label_create(s_det_view);
    lv_obj_set_style_text_color(s_d_name, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_d_name, &lv_font_montserrat_24, 0);
    lv_obj_align(s_d_name, LV_ALIGN_TOP_MID, 0, 122);

    int y = 160;
    /* Wrap every field (incl. phone/email) at the view width so a long value
       wraps instead of forcing the page to scroll horizontally. */
    s_d_phone   = lv_label_create(s_det_view); lv_obj_set_pos(s_d_phone,   12, y); lv_obj_set_width(s_d_phone, 216); lv_label_set_long_mode(s_d_phone, LV_LABEL_LONG_WRAP); y += 34;
    s_d_email   = lv_label_create(s_det_view); lv_obj_set_pos(s_d_email,   12, y); lv_obj_set_width(s_d_email, 216); lv_label_set_long_mode(s_d_email, LV_LABEL_LONG_WRAP); y += 34;
    s_d_address = lv_label_create(s_det_view); lv_obj_set_pos(s_d_address, 12, y); lv_obj_set_width(s_d_address, 216); lv_label_set_long_mode(s_d_address, LV_LABEL_LONG_WRAP); y += 50;
    s_d_notes   = lv_label_create(s_det_view); lv_obj_set_pos(s_d_notes,   12, y); lv_obj_set_width(s_d_notes, 216); lv_label_set_long_mode(s_d_notes, LV_LABEL_LONG_WRAP);

    lv_obj_t *fields[4] = { s_d_phone, s_d_email, s_d_address, s_d_notes };
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_text_color(fields[i], lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_style_text_font(fields[i], &lv_font_montserrat_14, 0);
    }
}

static void show_detail(int idx)
{
    if (idx < 0 || idx >= s_n) { show_library(); return; }
    s_view = V_DETAIL;
    s_open_idx = idx;
    if (!s_det_view) build_detail();
    contact_t *c = &s_contacts[idx];
    lv_obj_set_style_bg_color(s_d_mono, lv_color_hex(monogram_color(c)), 0);
    lv_obj_clean(s_d_mono);
    char ml[2] = { monogram_of(c), 0 };
    lv_obj_t *ml_lab = lv_label_create(s_d_mono);
    lv_label_set_text(ml_lab, ml);
    lv_obj_set_style_text_color(ml_lab, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(ml_lab, &lv_font_montserrat_36, 0);
    lv_obj_center(ml_lab);

    lv_label_set_text(s_d_name, c->name);
    char tmp[MAX_FIELD + 16];
    int k = 0; const char *p = LV_SYMBOL_CALL "  "; while (*p) tmp[k++] = *p++;
    if (c->phone[0]) { for (int i = 0; c->phone[i]; i++) tmp[k++] = c->phone[i]; } else { p = "(no phone)"; while (*p) tmp[k++] = *p++; }
    tmp[k] = 0; lv_label_set_text(s_d_phone, tmp);

    k = 0; p = LV_SYMBOL_ENVELOPE "  "; while (*p) tmp[k++] = *p++;
    if (c->email[0]) { for (int i = 0; c->email[i]; i++) tmp[k++] = c->email[i]; } else { p = "(no email)"; while (*p) tmp[k++] = *p++; }
    tmp[k] = 0; lv_label_set_text(s_d_email, tmp);

    k = 0; p = LV_SYMBOL_HOME "  "; while (*p) tmp[k++] = *p++;
    if (c->address[0]) { for (int i = 0; c->address[i]; i++) tmp[k++] = c->address[i]; } else { p = "(no address)"; while (*p) tmp[k++] = *p++; }
    tmp[k] = 0; lv_label_set_text(s_d_address, tmp);

    k = 0; p = LV_SYMBOL_EDIT "  "; while (*p) tmp[k++] = *p++;
    if (c->notes[0]) { for (int i = 0; c->notes[i]; i++) tmp[k++] = c->notes[i]; } else { p = "(no notes)"; while (*p) tmp[k++] = *p++; }
    tmp[k] = 0; lv_label_set_text(s_d_notes, tmp);

    lv_screen_load(s_det_view);
}

static void build_editor(void)
{
    s_edit_view = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_edit_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_pad_all(s_edit_view, 6, 0);

    lv_obj_t *back = lv_button_create(s_edit_view);
    lv_obj_set_size(back, 48, 30);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT); lv_obj_center(bl);
    lv_obj_add_event_cb(back, on_back_to_lib, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save = lv_button_create(s_edit_view);
    lv_obj_set_size(save, 60, 30);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(hb_color_success()), 0);
    lv_obj_t *sl = lv_label_create(save); lv_label_set_text(sl, "Save"); lv_obj_center(sl);
    lv_obj_add_event_cb(save, on_save, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(s_edit_view);
    lv_label_set_text(title, "Contact");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    int y = 50;
    lv_event_cb_t cbs[5] = { on_edit_name, on_edit_phone, on_edit_email, on_edit_addr, on_edit_notes };
    const char *labels[5] = { "Name", "Phone", "Email", "Address", "Notes" };
    lv_obj_t **outs[5] = { &s_e_name, &s_e_phone, &s_e_email, &s_e_address, &s_e_notes };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *hdr = lv_label_create(s_edit_view);
        lv_label_set_text(hdr, labels[i]);
        lv_obj_set_style_text_color(hdr, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(hdr, 12, y); y += 16;
        lv_obj_t *row = lv_button_create(s_edit_view);
        lv_obj_set_size(row, 220, 32); lv_obj_set_pos(row, 10, y); y += 38;
        lv_obj_set_style_bg_color(row, lv_color_hex(hb_color_surface()), 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_add_event_cb(row, cbs[i], LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(row);
        lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
        *outs[i] = l;
    }
}

static void show_editor(int idx)
{
    s_view = V_EDITOR;
    s_open_idx = idx;
    if (!s_edit_view) build_editor();
    if (idx >= 0 && idx < s_n) s_buf = s_contacts[idx];
    else {
        sclear(s_buf.name, MAX_FIELD); sclear(s_buf.phone, MAX_FIELD);
        sclear(s_buf.email, MAX_FIELD); sclear(s_buf.address, MAX_FIELD);
        sclear(s_buf.notes, MAX_FIELD);
    }
    lv_label_set_text(s_e_name,    s_buf.name[0]    ? s_buf.name    : "(tap to set name)");
    lv_label_set_text(s_e_phone,   s_buf.phone[0]   ? s_buf.phone   : "(tap to set phone)");
    lv_label_set_text(s_e_email,   s_buf.email[0]   ? s_buf.email   : "(tap to set email)");
    lv_label_set_text(s_e_address, s_buf.address[0] ? s_buf.address : "(tap to set address)");
    lv_label_set_text(s_e_notes,   s_buf.notes[0]   ? s_buf.notes   : "(tap to set notes)");
    lv_screen_load(s_edit_view);
}

static void build_field(void)
{
    s_fld_view = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_fld_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_pad_all(s_fld_view, 6, 0);

    lv_obj_t *back = lv_button_create(s_fld_view);
    lv_obj_set_size(back, 48, 30);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT); lv_obj_center(bl);
    lv_obj_add_event_cb(back, on_back_to_edit, LV_EVENT_CLICKED, NULL);

    s_fld_label = lv_label_create(s_fld_view);
    lv_obj_set_style_text_color(s_fld_label, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_fld_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_fld_label, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *done = lv_button_create(s_fld_view);
    lv_obj_set_size(done, 60, 30);
    lv_obj_align(done, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(done, lv_color_hex(hb_color_success()), 0);
    lv_obj_t *dl = lv_label_create(done); lv_label_set_text(dl, "Done"); lv_obj_center(dl);
    lv_obj_add_event_cb(done, on_field_done, LV_EVENT_CLICKED, NULL);

    s_fld_ta = lv_textarea_create(s_fld_view);
    lv_obj_set_size(s_fld_ta, 232, 60);
    lv_obj_align(s_fld_ta, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_text_color(s_fld_ta, lv_color_hex(hb_color_text()), 0);
}

static void show_field(const char *label, char *target, int max_len)
{
    s_view = V_FIELD;
    s_active_field = target;
    s_active_max = max_len;
    s_active_label = label;
    if (!s_fld_view) build_field();
    lv_label_set_text(s_fld_label, label);
    lv_textarea_set_text(s_fld_ta, target);

    /* (re)build T9 every time so it's freshly bound to the textarea */
    if (s_t9) { lv_obj_del(s_t9); s_t9 = NULL; }
    s_t9 = hb_t9_create(s_fld_view, s_fld_ta);
    if (s_t9) lv_obj_align(s_t9, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_screen_load(s_fld_view);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    hb_fs_mkdir("/Data"); hb_fs_mkdir(DATA_DIR);
    load_all();

    build_library();
    /* Lazy: detail/editor/field are built on first transition.
     * Pre-building all three at startup added ~80 widgets and
     * pushed Contacts over the LVGL TLSF heap (panic at launch). */

    show_library();
}
