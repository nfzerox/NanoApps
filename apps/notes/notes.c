/*
 * notes — multi-note editor on LVGL.
 *
 * List view: scrollable list of notes, each row shows the first line
 * as a preview + a small "×" delete button. Tap a row to open it in
 * the editor; tap × to delete (with confirmation modal). A "+" button
 * in the top-right corner creates a fresh note and opens it.
 *
 * Editor view: title bar with Back arrow + delete button, a textarea
 * filling the upper half, and LVGL's QWERTY keyboard below. There is
 * NO save button — typing autosaves on every keystroke and on exit.
 *
 * Storage: each note is /Data/Notes/note<id>.txt, where <id> is
 * a monotonically-increasing sequence number. IDs are reused only if
 * the user deletes the highest-id note; otherwise we always pick
 * max_id + 1 so the URL of a note doesn't change once created.
 *
 * The list is bounded only by MAX_NOTES (256) which is well past what
 * fits comfortably on a 240×432 screen even with scrolling.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_t9.h"
#include "hb_swipe_row.h"
#include "hb_vlist.h"
#include "lvgl/lvgl.h"

#define MAX_NOTES   256
#define MAX_NOTE_SZ 4096
#define PREVIEW_LEN 28
#define DATA_DIR    "/Data/Notes"
#define MAX_FOLDERS 32
#define FOLDER_LEN  32

typedef struct {
    int   id;                    /* matches noteN.txt filename */
    char  preview[PREVIEW_LEN + 1];
} note_entry_t;

static note_entry_t s_notes[MAX_NOTES];
static int          s_n_notes  = 0;
/* Current folder ("" = root). Notes default to root; users opt in by
 * tapping the title to open the folder picker. */
static char    s_folder[FOLDER_LEN] = "";
static char    s_folders[MAX_FOLDERS][FOLDER_LEN];
static int     s_n_folders = 0;

typedef enum { VIEW_LIST, VIEW_EDITOR, VIEW_FOLDERS } view_t;
static view_t   s_view = VIEW_LIST;
static int      s_open_id = -1;       /* id of note currently in the editor */
static int      s_pending_delete_id = -1;  /* used by confirm dialog */

static lv_obj_t *s_scr;
static lv_obj_t *s_list_view;
static lv_obj_t *s_editor_view;
static lv_obj_t *s_folders_view;
static lv_obj_t *s_folders_list;
static lv_obj_t *s_list_obj;
static hb_vlist_t *s_vlist;
static lv_obj_t  *s_empty_label;
static lv_obj_t *s_ta;
static lv_obj_t *s_title;
static lv_obj_t *s_list_header;
static char      s_editor_buf[MAX_NOTE_SZ];

/* ---- path helpers ---- */

static void int_to_str(int v, char *out)
{
    char buf[12]; int i = 0;
    if (v <= 0) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    int k = 0; while (i > 0) out[k++] = buf[--i];
    out[k] = 0;
}

static void folder_path(char *out)
{
    int i = 0;
    const char *p = DATA_DIR; while (*p) out[i++] = *p++;
    if (s_folder[0]) {
        out[i++] = '/';
        for (int k = 0; s_folder[k]; k++) out[i++] = s_folder[k];
    }
    out[i] = 0;
}

static void path_for(int id, char *out)
{
    /* "/Data/Notes/[<folder>/]noteN.txt" */
    int i = 0;
    folder_path(out); while (out[i]) i++;
    out[i++] = '/';
    out[i++] = 'n'; out[i++] = 'o'; out[i++] = 't'; out[i++] = 'e';
    char nb[12]; int_to_str(id, nb);
    int j = 0; while (nb[j]) out[i++] = nb[j++];
    out[i++] = '.'; out[i++] = 't'; out[i++] = 'x'; out[i++] = 't';
    out[i] = 0;
}

/* Parse "note<digits>.txt" → id, or return -1 if it doesn't match. */
static int parse_note_filename(const char *fn)
{
    if (fn[0]!='n' || fn[1]!='o' || fn[2]!='t' || fn[3]!='e') return -1;
    int i = 4, v = 0, any = 0;
    while (fn[i] >= '0' && fn[i] <= '9') {
        v = v * 10 + (fn[i] - '0');
        i++; any = 1;
    }
    if (!any) return -1;
    if (fn[i]!='.' || fn[i+1]!='t' || fn[i+2]!='x' || fn[i+3]!='t' || fn[i+4]) return -1;
    return v;
}

static int next_free_id(void)
{
    int max_id = -1;
    for (int i = 0; i < s_n_notes; i++) {
        if (s_notes[i].id > max_id) max_id = s_notes[i].id;
    }
    return max_id + 1;
}

/* ---- preview ---- */

static void load_preview(int id, char *out)
{
    for (int i = 0; i <= PREVIEW_LEN; i++) out[i] = 0;
    char path[64]; path_for(id, path);
    char buf[PREVIEW_LEN + 1] = {0};
    uint32_t n = hb_fs_read(path, buf, PREVIEW_LEN);
    for (uint32_t i = 0; i < n && i < PREVIEW_LEN; i++) {
        char ch = buf[i];
        if (ch == '\n' || ch == '\r') ch = ' ';
        if ((uint8_t)ch < 0x20 || (uint8_t)ch >= 0x7F) ch = '.';
        out[i] = ch;
    }
}

/* ---- scan ---- */

/* Sort notes by id ascending so list order stable across launches. */
static void sort_notes(void)
{
    for (int i = 1; i < s_n_notes; i++) {
        note_entry_t cur = s_notes[i];
        int j = i;
        while (j > 0 && s_notes[j-1].id > cur.id) {
            s_notes[j] = s_notes[j-1];
            j--;
        }
        s_notes[j] = cur;
    }
}

static void scan_notes(void)
{
    s_n_notes = 0;
    char dir[128]; folder_path(dir);
    hb_dir_t d;
    if (!hb_fs_dir_open(&d, dir, false)) return;
    char fn[64]; bool is_dir = false;
    while (hb_fs_dir_next(&d, fn, sizeof fn, &is_dir)) {
        if (is_dir) continue;
        int id = parse_note_filename(fn);
        if (id < 0) continue;
        if (s_n_notes >= MAX_NOTES) break;
        s_notes[s_n_notes].id = id;
        load_preview(id, s_notes[s_n_notes].preview);
        s_n_notes++;
    }
    hb_fs_dir_close(&d);
    sort_notes();
}

static void scan_folders(void)
{
    s_n_folders = 0;
    hb_dir_t d;
    if (!hb_fs_dir_open(&d, DATA_DIR, false)) return;
    char fn[64]; bool is_dir = false;
    while (hb_fs_dir_next(&d, fn, sizeof fn, &is_dir)) {
        if (!is_dir) continue;
        if (fn[0] == '.') continue;
        if (s_n_folders >= MAX_FOLDERS) break;
        int k = 0;
        while (fn[k] && k < FOLDER_LEN - 1) { s_folders[s_n_folders][k] = fn[k]; k++; }
        s_folders[s_n_folders][k] = 0;
        s_n_folders++;
    }
    hb_fs_dir_close(&d);
}

/* ---- views (forward decls) ---- */

static void open_list(void);
static void open_editor(int id);
static void open_folders(void);

/* Update the list header to reflect the current folder. */
static void refresh_header(void)
{
    if (!s_list_header) return;
    if (s_folder[0]) lv_label_set_text(s_list_header, s_folder);
    else             lv_label_set_text(s_list_header, "Notes");
}

/* ---- delete ---- */

static void delete_note(int id)
{
    char path[64]; path_for(id, path);
    hb_fs_remove(path);
    scan_notes();
    open_list();
}

static void on_confirm_yes(lv_event_t *e)
{
    lv_obj_t *box = (lv_obj_t *)lv_event_get_user_data(e);
    int id = s_pending_delete_id;
    s_pending_delete_id = -1;
    lv_obj_delete(box);
    if (id >= 0) delete_note(id);
}

static void on_confirm_no(lv_event_t *e)
{
    lv_obj_t *box = (lv_obj_t *)lv_event_get_user_data(e);
    s_pending_delete_id = -1;
    lv_obj_delete(box);
}

static void show_delete_confirm(int id)
{
    s_pending_delete_id = id;

    lv_obj_t *box = lv_obj_create(s_scr);
    lv_obj_set_size(box, 200, 130);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1f2330), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg, "Delete this note?");
    lv_obj_set_style_text_color(msg, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_16, 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *btn_yes = lv_button_create(box);
    lv_obj_set_size(btn_yes, 76, 36);
    lv_obj_align(btn_yes, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    lv_obj_set_style_bg_color(btn_yes, lv_color_hex(hb_color_danger()), 0);
    lv_obj_add_event_cb(btn_yes, on_confirm_yes, LV_EVENT_CLICKED, box);
    lv_obj_t *yl = lv_label_create(btn_yes);
    lv_label_set_text(yl, "Delete");
    lv_obj_set_style_text_color(yl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(yl);

    lv_obj_t *btn_no = lv_button_create(box);
    lv_obj_set_size(btn_no, 76, 36);
    lv_obj_align(btn_no, LV_ALIGN_BOTTOM_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(btn_no, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(btn_no, on_confirm_no, LV_EVENT_CLICKED, box);
    lv_obj_t *nl = lv_label_create(btn_no);
    lv_label_set_text(nl, "Cancel");
    lv_obj_set_style_text_color(nl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(nl);
}

/* ---- list view event handlers (swipe-row cookie callbacks) ---- */

static void on_row_clicked(void *cookie)
{
    int id = (int)(uintptr_t)cookie;
    open_editor(id);
}

static void on_row_delete(void *cookie)
{
    int id = (int)(uintptr_t)cookie;
    show_delete_confirm(id);
}

/* ---- new button ---- */

static void on_new_clicked(lv_event_t *e)
{
    (void)e;
    int id = next_free_id();
    /* Create empty file so the note "exists" in the list immediately
       — autosave will populate it as the user types. */
    char path[64]; path_for(id, path);
    hb_fs_write(path, "", 0);
    scan_notes();
    open_editor(id);
}

/* ---- editor ---- */

static void autosave(void)
{
    if (s_open_id < 0) return;
    const char *txt = lv_textarea_get_text(s_ta);
    int n = 0; while (txt[n] && n < MAX_NOTE_SZ - 1) n++;
    char path[64]; path_for(s_open_id, path);
    hb_fs_write(path, txt, (uint32_t)n);
}

static void on_ta_changed(lv_event_t *e)
{
    (void)e;
    autosave();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    autosave();
    scan_notes();      /* preview may have changed */
    open_list();
}

static void on_editor_delete(lv_event_t *e)
{
    (void)e;
    if (s_open_id >= 0) show_delete_confirm(s_open_id);
}

/* ---- views ---- */

static void open_list(void)
{
    s_view = VIEW_LIST;
    s_open_id = -1;
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    if (s_folders_view) lv_obj_add_flag(s_folders_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    refresh_header();

    /* Empty-state label is a sibling of the vlist viewport — show/hide
       based on note count. The vlist itself is reused across folder
       navigations; only its count + cookies change per refresh. */
    if (s_n_notes == 0) {
        lv_obj_add_flag(hb_vlist_obj(s_vlist), LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(hb_vlist_obj(s_vlist), LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
    }
    hb_vlist_set_count(s_vlist, s_n_notes);
    hb_vlist_invalidate(s_vlist);   /* preview / id may have changed for an index we already render */
}

/* hb_vlist bind callback — runs whenever a pool row needs to display a
   different note. First call for a given pool row builds the swipe-row
   widget tree inside it; subsequent calls just update the cookie + text. */
static void notes_bind_row(lv_obj_t *row, int index, void *ctx)
{
    (void)ctx;
    if (index < 0 || index >= s_n_notes) return;
    lv_obj_t *body = (lv_obj_t *)lv_obj_get_user_data(row);
    lv_obj_t *lab;
    if (!body) {
        body = hb_swipe_row_create(row,
                    224, 48, 64,
                    LV_SYMBOL_TRASH, 0xe63946,
                    on_row_clicked, on_row_delete,
                    (void *)(uintptr_t)s_notes[index].id);
        if (!body) return;
        lv_obj_set_user_data(row, body);

        lv_obj_t *icon = lv_label_create(body);
        lv_label_set_text(icon, LV_SYMBOL_FILE);
        lv_obj_set_style_text_color(icon, lv_color_hex(hb_color_primary()), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);

        lab = lv_label_create(body);
        lv_obj_set_style_text_color(lab, lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_16, 0);
        lv_obj_set_width(lab, 188);
        lv_label_set_long_mode(lab, LV_LABEL_LONG_DOT);
        lv_obj_align(lab, LV_ALIGN_LEFT_MID, 36, 0);
    } else {
        hb_swipe_row_set_cookie(body, (void *)(uintptr_t)s_notes[index].id);
        /* Children of body: [0]=icon, [1]=text label. */
        lab = lv_obj_get_child(body, 1);
    }

    const char *prev = s_notes[index].preview[0] ? s_notes[index].preview : "(empty)";
    lv_label_set_text(lab, prev);
}

static void open_editor(int id)
{
    s_view = VIEW_EDITOR;
    s_open_id = id;

    for (int i = 0; i < MAX_NOTE_SZ; i++) s_editor_buf[i] = 0;
    char path[64]; path_for(id, path);
    if (hb_fs_exists(path)) {
        uint32_t n = hb_fs_read(path, s_editor_buf, MAX_NOTE_SZ - 1);
        s_editor_buf[n < MAX_NOTE_SZ ? n : MAX_NOTE_SZ - 1] = 0;
    }
    lv_textarea_set_text(s_ta, s_editor_buf);

    char title[24] = { 'N','o','t','e',' ', 0 };
    int tk = 5;
    char idbuf[12]; int_to_str(id, idbuf);
    for (int i = 0; idbuf[i] && tk < 22; i++) title[tk++] = idbuf[i];
    title[tk] = 0;
    lv_label_set_text(s_title, title);

    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
}

/* ---- folder picker ---- */
static void on_folder_pick(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == -1) {        /* (Root) */
        s_folder[0] = 0;
    } else if (idx >= 0 && idx < s_n_folders) {
        int k = 0;
        while (s_folders[idx][k] && k < FOLDER_LEN - 1) { s_folder[k] = s_folders[idx][k]; k++; }
        s_folder[k] = 0;
    }
    scan_notes();
    open_list();
}

static void on_new_folder(lv_event_t *e)
{
    (void)e;
    /* Auto-name as "Folder N" — full text input would need a T9
     * overlay; keeping this terse for v1. Users can rename by editing
     * the directory on disk. */
    char name[FOLDER_LEN];
    int n = 1;
    for (;;) {
        char nb[12]; int_to_str(n, nb);
        int k = 0; const char *p = "Folder "; while (*p) name[k++] = *p++;
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
    /* Switch into the new folder. */
    int k = 0; while (name[k] && k < FOLDER_LEN - 1) { s_folder[k] = name[k]; k++; }
    s_folder[k] = 0;
    scan_notes();
    open_list();
}

static void open_folders(void)
{
    s_view = VIEW_FOLDERS;
    scan_folders();
    if (!s_folders_view) {
        s_folders_view = lv_obj_create(s_scr);
        lv_obj_set_size(s_folders_view, 240, 432);
        lv_obj_set_style_radius(s_folders_view, 0, 0);
        lv_obj_set_pos(s_folders_view, 0, 0);
        lv_obj_set_style_bg_color(s_folders_view, lv_color_hex(hb_color_bg()), 0);
        lv_obj_set_style_border_width(s_folders_view, 0, 0);
        lv_obj_set_style_pad_all(s_folders_view, 0, 0);
        lv_obj_clear_flag(s_folders_view, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_clean(s_folders_view);
    }
    lv_obj_t *hdr = lv_label_create(s_folders_view);
    lv_label_set_text(hdr, "Folders");
    lv_obj_set_style_text_color(hdr, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_24, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 12, 10);

    lv_obj_t *btn_add = lv_button_create(s_folders_view);
    lv_obj_set_size(btn_add, 40, 36);
    lv_obj_align(btn_add, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_bg_color(btn_add, lv_color_hex(hb_color_success()), 0);
    lv_obj_set_style_radius(btn_add, 8, 0);
    lv_obj_add_event_cb(btn_add, on_new_folder, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = lv_label_create(btn_add);
    lv_label_set_text(al, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(al, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(al);

    /* Same plain lv_list styling as Paint's Albums picker (no bg/border override). */
    s_folders_list = lv_list_create(s_folders_view);
    lv_obj_set_size(s_folders_list, 240, 380);
    lv_obj_align(s_folders_list, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t *btn_root = lv_list_add_button(s_folders_list, LV_SYMBOL_HOME, "(Root)");
    lv_obj_add_event_cb(btn_root, on_folder_pick, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    for (int i = 0; i < s_n_folders; i++) {
        lv_obj_t *btn = lv_list_add_button(s_folders_list, LV_SYMBOL_DIRECTORY, s_folders[i]);
        lv_obj_add_event_cb(btn, on_folder_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_folders_view, LV_OBJ_FLAG_HIDDEN);
}

void notes_on_folders_clicked(lv_event_t *e) { (void)e; open_folders(); }

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    scan_notes();

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

    lv_obj_t *hdr = lv_label_create(s_list_view);
    s_list_header = hdr;
    lv_label_set_text(hdr, "Notes");
    lv_obj_set_style_text_color(hdr, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_24, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 12, 10);

    /* "Folders" button left of the + — opens the folder picker. */
    lv_obj_t *btn_folders = lv_button_create(s_list_view);
    lv_obj_set_size(btn_folders, 40, 36);
    lv_obj_align(btn_folders, LV_ALIGN_TOP_RIGHT, -56, 8);
    lv_obj_set_style_bg_color(btn_folders, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_radius(btn_folders, 8, 0);
    lv_obj_add_event_cb(btn_folders, notes_on_folders_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fl = lv_label_create(btn_folders);
    lv_label_set_text(fl, LV_SYMBOL_DIRECTORY);
    lv_obj_set_style_text_color(fl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(fl);

    /* "+" button (top-right) — creates a new note and opens editor. */
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

    /* Notes list = virtualised vlist. Each row is 52 px tall (48 px
       swipe row body + 4 px bottom margin) so the pool is small
       regardless of how many notes the user has. */
    s_vlist = hb_vlist_create(s_list_view, /*row_h=*/52, notes_bind_row, NULL);
    s_list_obj = hb_vlist_obj(s_vlist);
    lv_obj_set_size(s_list_obj, 240, 432 - 56);
    lv_obj_align(s_list_obj, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_list_obj, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_list_obj, 0, 0);
    lv_obj_set_style_pad_left(s_list_obj, 8, 0);
    lv_obj_set_style_pad_right(s_list_obj, 8, 0);

    s_empty_label = lv_label_create(s_list_view);
    lv_label_set_text(s_empty_label, "No notes yet.\nTap + to create one.");
    lv_obj_set_style_text_color(s_empty_label, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(s_empty_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_empty_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_empty_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);

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

    /* Header: Back | Title | Delete (no Save — typing autosaves) */
    lv_obj_t *btn_back = lv_button_create(s_editor_view);
    lv_obj_set_size(btn_back, 60, 36);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_back = lv_label_create(btn_back);
    lv_label_set_text(l_back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(l_back, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(l_back);

    s_title = lv_label_create(s_editor_view);
    lv_label_set_text(s_title, "");
    lv_obj_set_style_text_color(s_title, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *btn_del = lv_button_create(s_editor_view);
    lv_obj_set_size(btn_del, 60, 36);
    lv_obj_align(btn_del, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_bg_color(btn_del, lv_color_hex(hb_color_danger()), 0);
    lv_obj_add_event_cb(btn_del, on_editor_delete, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_del = lv_label_create(btn_del);
    lv_label_set_text(l_del, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(l_del, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(l_del);

    /* Textarea fills the gap between header (48) and the T9 keyboard
       (232 tall at bottom). 432 - 48 - 232 = 152 px tall. */
    s_ta = lv_textarea_create(s_editor_view);
    lv_obj_set_size(s_ta, 240, 152);
    lv_obj_align(s_ta, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(s_ta, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_text_color(s_ta, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_border_color(s_ta, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_radius(s_ta, 0, 0);
    lv_obj_add_event_cb(s_ta, on_ta_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* T9 predictive keyboard with built-in suggestion strip.
       The widget creates its own children inside this container;
       its tree is 240×~230 (32 px strip + 4 rows × 48 px keypad). */
    lv_obj_t *kb_box = lv_obj_create(s_editor_view);
    lv_obj_set_size(kb_box, 240, 232);
    lv_obj_align(kb_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb_box, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_border_width(kb_box, 0, 0);
    lv_obj_set_style_pad_all(kb_box, 0, 0);
    lv_obj_clear_flag(kb_box, LV_OBJ_FLAG_SCROLLABLE);
    hb_t9_create(kb_box, s_ta);

    open_list();
    if (s_view == VIEW_EDITOR) autosave();
}
