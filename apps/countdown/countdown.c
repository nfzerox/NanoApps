/*
 * countdown_days — multi-countdown app with CRUD + yearly repeats.
 *
 * Views:
 *   - List view (default): scrollable rows showing each countdown's
 *     name and days-until. Swipe-left to reveal delete. Tap to edit.
 *   - Grid view: paginated 2×3 of gradient cards, each card shows
 *     the big day count + name. Swipe between pages.
 *   - Editor view: name field (tap opens T9 keyboard sub-view),
 *     date pickers (year/month/day rollers), repeats-yearly switch,
 *     Save / Delete actions.
 *   - Name editor sub-view: T9 keyboard over a textarea.
 *
 * Storage: /Data/CountdownDays/items.txt — one line per item:
 *   "YYYY-MM-DD<TAB>repeats<TAB>name"
 * View mode persisted at /Data/CountdownDays/mode.
 *
 * Yearly repeats: when computing days_until for a repeating event we
 * advance its target year forward until target_date >= today. So a
 * birthday from 1994 still reads "12 days to go" if it falls 12 days
 * from now this year.
 */

#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_t9.h"
#include "hb_swipe_row.h"
#include "lvgl/lvgl.h"

#define MAX_ITEMS    64
#define MAX_NAME     48
#define FILE_BUF_SZ  (MAX_ITEMS * (MAX_NAME + 16))
#define DATA_DIR     "/Data/CountdownDays"
#define ITEMS_PATH   "/Data/CountdownDays/items.txt"
#define MODE_PATH    "/Data/CountdownDays/mode"

#define MODE_LIST 0
#define MODE_GRID 1

/* Year roller range — covers reasonable birthdays plus future dates. */
#define YEAR_MIN  1900
#define YEAR_MAX  2100
#define YEAR_N    (YEAR_MAX - YEAR_MIN + 1)

typedef struct {
    int  year, month, day;
    bool repeats_yearly;
    char name[MAX_NAME];
} item_t;

static item_t s_items[MAX_ITEMS];
static int    s_n_items   = 0;
static int    s_view_mode = MODE_LIST;
static int    s_edit_idx  = -1;    /* index being edited, -1 = new */

typedef enum { VIEW_LIST, VIEW_EDITOR, VIEW_NAME_INPUT } view_t;
static view_t s_view = VIEW_LIST;

static lv_obj_t *s_scr;
static lv_obj_t *s_list_view;
static lv_obj_t *s_editor_view;
static lv_obj_t *s_name_view;
static lv_obj_t *s_body;
static lv_obj_t *s_toggle_btn_label;
static lv_obj_t *s_ed_name_label;
static lv_obj_t *s_ed_year, *s_ed_month, *s_ed_day;
static lv_obj_t *s_ed_repeats;
static lv_obj_t *s_ed_delete_btn;
static lv_obj_t *s_name_ta;

static int  s_edit_year, s_edit_month, s_edit_day;
static bool s_edit_repeats;
static char s_edit_name[MAX_NAME];

/* ---- string helpers ---- */

static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void itoa_signed(int v, char *out)
{
    char buf[12]; int i = 0;
    bool neg = v < 0;
    uint32_t u = neg ? (uint32_t)(-v) : (uint32_t)v;
    if (u == 0) { out[0] = '0'; out[1] = 0; return; }
    while (u) { buf[i++] = '0' + (u % 10); u /= 10; }
    int k = 0;
    if (neg) out[k++] = '-';
    while (i > 0) out[k++] = buf[--i];
    out[k] = 0;
}

/* ---- date math ---- */

static int32_t days_from_epoch(int y, int m, int d)
{
    if (m <= 2) { y -= 1; m += 12; }
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153u * (uint32_t)(m - 3) + 2u) / 5u + (uint32_t)d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe;
}

static int is_leap(int y) { return (y%4==0 && y%100!=0) || (y%400==0); }
static int days_in_month(int y, int m)
{
    static const int dm[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && is_leap(y)) return 29;
    if (m < 1 || m > 12) return 30;
    return dm[m - 1];
}

/* Returns days_until, taking yearly-repeat into account by advancing
   the year so the target is >= today. The effective year used for the
   reading is also written to out_eff_year (caller may want it for
   display). */
static int days_until_effective(const item_t *it,
                                const hb_rtc_time_t *now,
                                int *out_eff_year)
{
    int y = it->year;
    if (it->repeats_yearly) {
        int32_t today = days_from_epoch(now->year, now->month, now->day_of_month);
        while (days_from_epoch(y, it->month, it->day) < today) y++;
    }
    if (out_eff_year) *out_eff_year = y;
    int32_t target = days_from_epoch(y, it->month, it->day);
    int32_t today  = days_from_epoch(now->year, now->month, now->day_of_month);
    return (int)(target - today);
}

/* ---- persistence ---- */

static void load_items(void)
{
    s_n_items = 0;
    static char buf[FILE_BUF_SZ];
    uint32_t n = hb_fs_read(ITEMS_PATH, buf, sizeof buf - 1);
    if (n == 0) return;
    buf[n] = 0;

    uint32_t i = 0;
    while (i < n && s_n_items < MAX_ITEMS) {
        /* parse YYYY-MM-DD\trepeats\tname\n */
        int y = 0, mo = 0, d = 0;
        while (i < n && buf[i] >= '0' && buf[i] <= '9') { y = y*10 + (buf[i]-'0'); i++; }
        if (i >= n || buf[i] != '-') { while (i<n && buf[i]!='\n') i++; if (i<n) i++; continue; }
        i++;
        while (i < n && buf[i] >= '0' && buf[i] <= '9') { mo = mo*10 + (buf[i]-'0'); i++; }
        if (i >= n || buf[i] != '-') { while (i<n && buf[i]!='\n') i++; if (i<n) i++; continue; }
        i++;
        while (i < n && buf[i] >= '0' && buf[i] <= '9') { d = d*10 + (buf[i]-'0'); i++; }
        if (i >= n || buf[i] != '\t') { while (i<n && buf[i]!='\n') i++; if (i<n) i++; continue; }
        i++;
        bool rep = (buf[i] == '1');
        while (i < n && buf[i] != '\t' && buf[i] != '\n') i++;
        if (buf[i] == '\t') i++;
        int k = 0;
        item_t *it = &s_items[s_n_items];
        while (i < n && buf[i] != '\n' && k < MAX_NAME - 1) it->name[k++] = buf[i++];
        it->name[k] = 0;
        if (i < n && buf[i] == '\n') i++;
        if (y < YEAR_MIN || y > YEAR_MAX || mo < 1 || mo > 12 || d < 1 || d > 31) continue;
        it->year = y; it->month = mo; it->day = d;
        it->repeats_yearly = rep;
        s_n_items++;
    }
}

static void save_items(void)
{
    static char buf[FILE_BUF_SZ];
    uint32_t pos = 0;
    for (int i = 0; i < s_n_items; i++) {
        item_t *it = &s_items[i];
        if (pos + MAX_NAME + 16 > sizeof buf) break;
        char tmp[12];
        /* year (4 digits) */
        itoa_signed(it->year, tmp); int yl = s_len(tmp);
        while (yl < 4) { buf[pos++] = '0'; yl++; }
        for (int j = 0; tmp[j]; j++) buf[pos++] = tmp[j];
        buf[pos++] = '-';
        if (it->month < 10) buf[pos++] = '0';
        itoa_signed(it->month, tmp);
        for (int j = 0; tmp[j]; j++) buf[pos++] = tmp[j];
        buf[pos++] = '-';
        if (it->day < 10) buf[pos++] = '0';
        itoa_signed(it->day, tmp);
        for (int j = 0; tmp[j]; j++) buf[pos++] = tmp[j];
        buf[pos++] = '\t';
        buf[pos++] = it->repeats_yearly ? '1' : '0';
        buf[pos++] = '\t';
        for (int j = 0; it->name[j] && pos < sizeof buf - 2; j++) buf[pos++] = it->name[j];
        buf[pos++] = '\n';
    }
    hb_fs_write(ITEMS_PATH, buf, pos);
}

static void load_mode(void)
{
    char b[2];
    if (hb_fs_read(MODE_PATH, b, 1) > 0) {
        s_view_mode = (b[0] == '1') ? MODE_GRID : MODE_LIST;
    }
}
static void save_mode(void)
{
    char c = (s_view_mode == MODE_GRID) ? '1' : '0';
    hb_fs_write(MODE_PATH, &c, 1);
}

/* ---- hashed gradient color for an item (deterministic per name) ---- */

static uint32_t name_hash(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    while (*s) { h ^= (uint8_t)*s++; h *= 0x01000193u; }
    return h;
}

/* A small palette of pleasing card colors — picked rather than computed
   so the dark side of each gradient remains readable. */
static const uint32_t s_palette[][2] = {
    { 0x457b9d, 0x1d3557 },   /* blue */
    { 0x2a9d8f, 0x1a5c54 },   /* teal */
    { 0xe63946, 0x8d1c25 },   /* red */
    { 0xf77f00, 0x9c4f00 },   /* orange */
    { 0xfcbf49, 0xa07a23 },   /* gold */
    { 0xa663cc, 0x683d8a },   /* purple */
    { 0x3a86ff, 0x1f4790 },   /* royal */
    { 0xff006e, 0x9c004a },   /* magenta */
};
#define N_PALETTE  (int)(sizeof s_palette / sizeof s_palette[0])

static uint32_t color_top_for(const char *name)    { return s_palette[name_hash(name) % N_PALETTE][0]; }
static uint32_t color_bot_for(const char *name)    { return s_palette[name_hash(name) % N_PALETTE][1]; }

/* ---- forward decls ---- */
static void open_list(void);
static void open_editor(int idx);
static void open_name_input(void);

/* ---- list view ---- */

static void on_row_tap(void *cookie)
{
    int idx = (int)(uintptr_t)cookie;
    open_editor(idx);
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
    open_editor(-1);
}

static void on_view_toggle(lv_event_t *e)
{
    (void)e;
    s_view_mode = (s_view_mode == MODE_LIST) ? MODE_GRID : MODE_LIST;
    save_mode();
    open_list();
}

static void build_list_body(void)
{
    hb_rtc_time_t now; hb_rtc_read(&now);

    if (s_n_items == 0) {
        lv_obj_t *empty = lv_label_create(s_body);
        lv_label_set_text(empty,
            "No countdowns yet.\nTap + to add one.");
        lv_obj_set_style_text_color(empty, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    for (int i = 0; i < s_n_items; i++) {
        item_t *it = &s_items[i];
        int eff_y = 0;
        int days = days_until_effective(it, &now, &eff_y);

        lv_obj_t *body = hb_swipe_row_create(s_body, 224, 56, 64,
                            LV_SYMBOL_TRASH, 0xe63946,
                            on_row_tap, on_row_delete,
                            (void *)(uintptr_t)i);
        if (!body) continue;
        lv_obj_set_style_bg_color(body, lv_color_hex(color_top_for(it->name)), 0);
        lv_obj_set_style_bg_grad_color(body, lv_color_hex(color_bot_for(it->name)), 0);
        lv_obj_set_style_bg_grad_dir(body, LV_GRAD_DIR_VER, 0);

        lv_obj_t *name = lv_label_create(body);
        lv_label_set_text(name, it->name[0] ? it->name : "(no name)");
        lv_obj_set_style_text_color(name, lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        /* Fixed 1-line height so a long name ("US Independence Day") truncates with
           an ellipsis instead of wrapping + spilling into the row below. */
        lv_obj_set_size(name, 138, 22);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 10, 6);

        /* Date sub-line */
        char date[24]; int k = 0;
        char tmp[12]; itoa_signed(eff_y, tmp);
        int yl = s_len(tmp); while (yl < 4) { date[k++] = '0'; yl++; }
        for (int j = 0; tmp[j]; j++) date[k++] = tmp[j];
        date[k++] = '-';
        if (it->month < 10) date[k++] = '0';
        itoa_signed(it->month, tmp); for (int j=0;tmp[j];j++) date[k++] = tmp[j];
        date[k++] = '-';
        if (it->day < 10) date[k++] = '0';
        itoa_signed(it->day, tmp); for (int j=0;tmp[j];j++) date[k++] = tmp[j];
        if (it->repeats_yearly) { date[k++]=' '; date[k++]=LV_SYMBOL_REFRESH[0]; }
        date[k] = 0;
        lv_obj_t *dlab = lv_label_create(body);
        lv_label_set_text(dlab, date);
        lv_obj_set_style_text_color(dlab, lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(dlab, &lv_font_montserrat_14, 0);
        lv_obj_align(dlab, LV_ALIGN_BOTTOM_LEFT, 10, -6);

        /* Big days number on the right side */
        char nb[16];
        itoa_signed(days < 0 ? -days : days, nb);
        lv_obj_t *nlab = lv_label_create(body);
        lv_label_set_text(nlab, nb);
        lv_obj_set_style_text_color(nlab, lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(nlab, &lv_font_montserrat_24, 0);
        lv_obj_align(nlab, LV_ALIGN_RIGHT_MID, -10, -4);

        const char *suffix = (days == 0) ? "today" :
                             (days == 1 || days == -1) ? "day" : "days";
        lv_obj_t *slab = lv_label_create(body);
        lv_label_set_text(slab, suffix);
        lv_obj_set_style_text_color(slab, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_style_text_font(slab, &lv_font_montserrat_14, 0);
        lv_obj_align(slab, LV_ALIGN_RIGHT_MID, -10, 12);
    }
}

/* Grid card */
static void on_grid_tap(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    open_editor(idx);
}

static void build_grid_body(void)
{
    hb_rtc_time_t now; hb_rtc_read(&now);

    if (s_n_items == 0) {
        lv_obj_t *empty = lv_label_create(s_body);
        lv_label_set_text(empty,
            "No countdowns yet.\nTap + to add one.");
        lv_obj_set_style_text_color(empty, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    int per_page = 6;          /* 2 cols × 3 rows */
    int n_pages = (s_n_items + per_page - 1) / per_page;
    if (n_pages < 1) n_pages = 1;

    lv_obj_t *pager = lv_tileview_create(s_body);
    lv_obj_set_size(pager, 240, 360);
    lv_obj_align(pager, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pager, 0, 0);

    for (int p = 0; p < n_pages; p++) {
        lv_obj_t *tile = lv_tileview_add_tile(pager, p, 0, LV_DIR_HOR);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        for (int slot = 0; slot < per_page; slot++) {
            int idx = p * per_page + slot;
            if (idx >= s_n_items) break;
            item_t *it = &s_items[idx];
            int eff_y = 0;
            int days = days_until_effective(it, &now, &eff_y);

            int col = slot % 2;
            int row = slot / 2;
            int x = col * 120;
            int y = row * 120;

            lv_obj_t *card = lv_button_create(tile);
            lv_obj_set_size(card, 116, 116);
            lv_obj_set_pos(card, x + 2, y + 2);
            lv_obj_set_style_bg_color(card, lv_color_hex(color_top_for(it->name)), 0);
            lv_obj_set_style_bg_grad_color(card, lv_color_hex(color_bot_for(it->name)), 0);
            lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
            lv_obj_set_style_radius(card, 12, 0);
            lv_obj_set_style_pad_all(card, 0, 0);
            lv_obj_set_style_border_width(card, 0, 0);
            lv_obj_set_style_shadow_width(card, 0, 0);
            lv_obj_add_event_cb(card, on_grid_tap, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)idx);

            lv_obj_t *nlab = lv_label_create(card);
            lv_label_set_text(nlab, it->name[0] ? it->name : "(no name)");
            lv_obj_set_style_text_color(nlab, lv_color_hex(hb_color_text()), 0);
            lv_obj_set_style_text_font(nlab, &lv_font_montserrat_14, 0);
            lv_label_set_long_mode(nlab, LV_LABEL_LONG_DOT);
            lv_obj_set_width(nlab, 108);
            lv_obj_align(nlab, LV_ALIGN_TOP_MID, 0, 6);

            char nb[16]; itoa_signed(days < 0 ? -days : days, nb);
            lv_obj_t *dlab = lv_label_create(card);
            lv_label_set_text(dlab, nb);
            lv_obj_set_style_text_color(dlab, lv_color_hex(hb_color_text()), 0);
            lv_obj_set_style_text_font(dlab, &lv_font_montserrat_36, 0);
            lv_obj_align(dlab, LV_ALIGN_CENTER, 0, 0);

            const char *suffix = (days == 0) ? "today" :
                                 (days == 1 || days == -1) ? "day" : "days";
            lv_obj_t *slab = lv_label_create(card);
            lv_label_set_text(slab, suffix);
            lv_obj_set_style_text_color(slab, lv_color_hex(hb_color_text_dim()), 0);
            lv_obj_set_style_text_font(slab, &lv_font_montserrat_14, 0);
            lv_obj_align(slab, LV_ALIGN_BOTTOM_MID, 0, -6);
        }
    }
}

static void open_list(void)
{
    s_view = VIEW_LIST;
    lv_obj_clear_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_name_view, LV_OBJ_FLAG_HIDDEN);

    /* Update toggle label */
    if (s_toggle_btn_label) {
        lv_label_set_text(s_toggle_btn_label,
            s_view_mode == MODE_LIST ? "Grid" : "List");
    }

    lv_obj_clean(s_body);
    if (s_view_mode == MODE_LIST) {
        lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(s_body, 6, 0);
        build_list_body();
    } else {
        lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_ROW);     /* not really used */
        lv_obj_set_style_pad_all(s_body, 0, 0);
        build_grid_body();
    }
}

/* ---- editor ---- */

/* Year roller buffer: 1900..2100 inclusive (201 years) generated at
   startup so users can pick any birthday year, not just the next
   decade. */
static char s_year_opts_buf[YEAR_N * 5 + 1];
static const char *s_year_opts = s_year_opts_buf;

static void build_year_opts(void)
{
    int k = 0;
    for (int y = YEAR_MIN; y <= YEAR_MAX; y++) {
        if (k > 0) s_year_opts_buf[k++] = '\n';
        s_year_opts_buf[k++] = (char)('0' + (y / 1000) % 10);
        s_year_opts_buf[k++] = (char)('0' + (y / 100) % 10);
        s_year_opts_buf[k++] = (char)('0' + (y / 10) % 10);
        s_year_opts_buf[k++] = (char)('0' + (y % 10));
    }
    s_year_opts_buf[k] = 0;
}
static const char *s_month_opts =
    "Jan\nFeb\nMar\nApr\nMay\nJun\nJul\nAug\nSep\nOct\nNov\nDec";

static void build_day_opts(int y, int m, char *out, int out_sz)
{
    int dim = days_in_month(y, m);
    int k = 0;
    for (int d = 1; d <= dim && k < out_sz - 4; d++) {
        if (d > 1) out[k++] = '\n';
        if (d >= 10) out[k++] = '0' + (d / 10);
        out[k++] = '0' + (d % 10);
    }
    out[k] = 0;
}

static void update_day_roller(void)
{
    int year_sel  = lv_roller_get_selected(s_ed_year);
    int month_sel = lv_roller_get_selected(s_ed_month);
    int y = YEAR_MIN + year_sel;
    int m = month_sel + 1;
    static char day_opts[256];
    build_day_opts(y, m, day_opts, sizeof day_opts);
    int prev_day = lv_roller_get_selected(s_ed_day);
    lv_roller_set_options(s_ed_day, day_opts, LV_ROLLER_MODE_NORMAL);
    int dim = days_in_month(y, m);
    if (prev_day >= dim) prev_day = dim - 1;
    lv_roller_set_selected(s_ed_day, prev_day, LV_ANIM_OFF);
}

static void on_ym_changed(lv_event_t *e) { (void)e; update_day_roller(); }

static void on_save_editor(lv_event_t *e)
{
    (void)e;
    int year_sel  = lv_roller_get_selected(s_ed_year);
    int month_sel = lv_roller_get_selected(s_ed_month);
    int day_sel   = lv_roller_get_selected(s_ed_day);
    int y = YEAR_MIN + year_sel;
    int m = month_sel + 1;
    int d = day_sel + 1;
    bool rep = lv_obj_has_state(s_ed_repeats, LV_STATE_CHECKED);

    item_t *it;
    if (s_edit_idx >= 0 && s_edit_idx < s_n_items) {
        it = &s_items[s_edit_idx];
    } else if (s_n_items < MAX_ITEMS) {
        it = &s_items[s_n_items++];
    } else {
        open_list(); return;
    }
    it->year = y; it->month = m; it->day = d;
    it->repeats_yearly = rep;
    int k = 0;
    while (s_edit_name[k] && k < MAX_NAME - 1) { it->name[k] = s_edit_name[k]; k++; }
    it->name[k] = 0;
    save_items();
    open_list();
}

static void on_back_editor(lv_event_t *e) { (void)e; open_list(); }

static void on_delete_editor(lv_event_t *e)
{
    (void)e;
    if (s_edit_idx < 0 || s_edit_idx >= s_n_items) return;
    int idx = s_edit_idx;
    for (int i = idx; i < s_n_items - 1; i++) s_items[i] = s_items[i + 1];
    s_n_items--;
    save_items();
    open_list();
}

static void on_edit_name(lv_event_t *e) { (void)e; open_name_input(); }

static void open_editor(int idx)
{
    s_view = VIEW_EDITOR;
    s_edit_idx = idx;

    if (idx >= 0 && idx < s_n_items) {
        item_t *it = &s_items[idx];
        s_edit_year = it->year;
        s_edit_month = it->month;
        s_edit_day = it->day;
        s_edit_repeats = it->repeats_yearly;
        int k = 0;
        while (it->name[k] && k < MAX_NAME - 1) { s_edit_name[k] = it->name[k]; k++; }
        s_edit_name[k] = 0;
    } else {
        hb_rtc_time_t now; hb_rtc_read(&now);
        s_edit_year = now.year < 2024 ? 2026 : now.year;
        s_edit_month = (now.month >= 1 && now.month <= 12) ? now.month : 1;
        s_edit_day = (now.day_of_month >= 1 && now.day_of_month <= 31) ? now.day_of_month : 1;
        s_edit_repeats = false;
        s_edit_name[0] = 0;
    }

    /* sync UI from state */
    lv_label_set_text(s_ed_name_label,
        s_edit_name[0] ? s_edit_name : "Tap to set name...");
    if (s_edit_year >= YEAR_MIN && s_edit_year <= YEAR_MAX) {
        lv_roller_set_selected(s_ed_year, s_edit_year - YEAR_MIN, LV_ANIM_OFF);
    }
    lv_roller_set_selected(s_ed_month, s_edit_month - 1, LV_ANIM_OFF);
    update_day_roller();
    int dim = days_in_month(s_edit_year, s_edit_month);
    int d_sel = s_edit_day - 1;
    if (d_sel >= dim) d_sel = dim - 1;
    lv_roller_set_selected(s_ed_day, d_sel, LV_ANIM_OFF);

    if (s_edit_repeats) lv_obj_add_state(s_ed_repeats, LV_STATE_CHECKED);
    else                lv_obj_remove_state(s_ed_repeats, LV_STATE_CHECKED);

    if (idx >= 0) lv_obj_clear_flag(s_ed_delete_btn, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag  (s_ed_delete_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_name_view, LV_OBJ_FLAG_HIDDEN);
}

/* ---- name input sub-view ---- */

static void on_name_save(lv_event_t *e)
{
    (void)e;
    const char *txt = lv_textarea_get_text(s_name_ta);
    int n = 0; while (txt[n] && n < MAX_NAME - 1) n++;
    while (n > 0 && (txt[n-1]==' '||txt[n-1]=='\n'||txt[n-1]=='\r'||txt[n-1]=='\t')) n--;
    for (int i = 0; i < n; i++) s_edit_name[i] = txt[i];
    s_edit_name[n] = 0;
    lv_label_set_text(s_ed_name_label,
        s_edit_name[0] ? s_edit_name : "Tap to set name...");
    lv_obj_add_flag(s_name_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    s_view = VIEW_EDITOR;
}

static void on_name_cancel(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_name_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    s_view = VIEW_EDITOR;
}

static void open_name_input(void)
{
    s_view = VIEW_NAME_INPUT;
    lv_textarea_set_text(s_name_ta, s_edit_name);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_name_view, LV_OBJ_FLAG_HIDDEN);
}

/* ---- build views ---- */

static void build_list_view(void)
{
    s_list_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_list_view, 240, 432);
    lv_obj_set_style_radius(s_list_view, 0, 0);
    lv_obj_set_pos(s_list_view, 0, 0);
    lv_obj_set_style_bg_color(s_list_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_list_view, 0, 0);
    lv_obj_set_style_pad_all(s_list_view, 0, 0);
    lv_obj_clear_flag(s_list_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_list_view);
    lv_label_set_text(title, "Countdowns");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 10);

    lv_obj_t *btn_toggle = lv_button_create(s_list_view);
    lv_obj_set_size(btn_toggle, 56, 32);
    lv_obj_align(btn_toggle, LV_ALIGN_TOP_RIGHT, -52, 6);
    lv_obj_set_style_bg_color(btn_toggle, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_radius(btn_toggle, 6, 0);
    lv_obj_add_event_cb(btn_toggle, on_view_toggle, LV_EVENT_CLICKED, NULL);
    s_toggle_btn_label = lv_label_create(btn_toggle);
    lv_label_set_text(s_toggle_btn_label, "Grid");
    lv_obj_set_style_text_color(s_toggle_btn_label, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_toggle_btn_label, &lv_font_montserrat_14, 0);
    lv_obj_center(s_toggle_btn_label);

    lv_obj_t *btn_new = lv_button_create(s_list_view);
    lv_obj_set_size(btn_new, 40, 32);
    lv_obj_align(btn_new, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_bg_color(btn_new, lv_color_hex(hb_color_success()), 0);
    lv_obj_set_style_radius(btn_new, 6, 0);
    lv_obj_add_event_cb(btn_new, on_new_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(btn_new);
    lv_label_set_text(nl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(nl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_20, 0);
    lv_obj_center(nl);

    s_body = lv_obj_create(s_list_view);
    lv_obj_set_size(s_body, 240, 432 - 48);
    lv_obj_align(s_body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_body, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 6, 0);
}

static void build_editor_view(void)
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

    /* Header */
    lv_obj_t *btn_back = lv_button_create(s_editor_view);
    lv_obj_set_size(btn_back, 56, 32);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(btn_back, on_back_editor, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn_back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    lv_obj_t *etitle = lv_label_create(s_editor_view);
    lv_label_set_text(etitle, "Countdown");
    lv_obj_set_style_text_color(etitle, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(etitle, &lv_font_montserrat_16, 0);
    lv_obj_align(etitle, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *btn_save = lv_button_create(s_editor_view);
    lv_obj_set_size(btn_save, 56, 32);
    lv_obj_align(btn_save, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(btn_save, on_save_editor, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(btn_save);
    lv_label_set_text(sl, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(sl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(sl);

    /* Name row — clickable to enter T9 sub-view */
    lv_obj_t *name_row = lv_button_create(s_editor_view);
    lv_obj_set_size(name_row, 228, 40);
    lv_obj_align(name_row, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(name_row, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_radius(name_row, 8, 0);
    lv_obj_add_event_cb(name_row, on_edit_name, LV_EVENT_CLICKED, NULL);
    s_ed_name_label = lv_label_create(name_row);
    lv_label_set_text(s_ed_name_label, "Tap to set name...");
    lv_obj_set_style_text_color(s_ed_name_label, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_ed_name_label, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(s_ed_name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ed_name_label, 200);
    lv_obj_align(s_ed_name_label, LV_ALIGN_LEFT_MID, 6, 0);

    /* Date pickers — 3 rollers side by side */
    s_ed_year = lv_roller_create(s_editor_view);
    lv_roller_set_options(s_ed_year, s_year_opts, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_size(s_ed_year, 70, 120);
    lv_obj_align(s_ed_year, LV_ALIGN_TOP_LEFT, 8, 100);
    lv_obj_set_style_bg_color(s_ed_year, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_text_color(s_ed_year, lv_color_hex(hb_color_text()), 0);
    lv_obj_add_event_cb(s_ed_year, on_ym_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_ed_month = lv_roller_create(s_editor_view);
    lv_roller_set_options(s_ed_month, s_month_opts, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_size(s_ed_month, 70, 120);
    lv_obj_align(s_ed_month, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_bg_color(s_ed_month, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_text_color(s_ed_month, lv_color_hex(hb_color_text()), 0);
    lv_obj_add_event_cb(s_ed_month, on_ym_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_ed_day = lv_roller_create(s_editor_view);
    {
        static char day_opts[256];
        build_day_opts(2026, 1, day_opts, sizeof day_opts);
        lv_roller_set_options(s_ed_day, day_opts, LV_ROLLER_MODE_NORMAL);
    }
    lv_obj_set_size(s_ed_day, 70, 120);
    lv_obj_align(s_ed_day, LV_ALIGN_TOP_RIGHT, -8, 100);
    lv_obj_set_style_bg_color(s_ed_day, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_text_color(s_ed_day, lv_color_hex(hb_color_text()), 0);

    /* Repeats yearly switch */
    lv_obj_t *rep_lab = lv_label_create(s_editor_view);
    lv_label_set_text(rep_lab, "Repeats yearly");
    lv_obj_set_style_text_color(rep_lab, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(rep_lab, &lv_font_montserrat_16, 0);
    /* Nudge down so the label is vertically centred on the switch (~26px tall) rather
       than top-aligned with it (which read as "too high"). */
    lv_obj_align(rep_lab, LV_ALIGN_TOP_LEFT, 12, 243);

    s_ed_repeats = lv_switch_create(s_editor_view);
    lv_obj_align(s_ed_repeats, LV_ALIGN_TOP_RIGHT, -12, 240);

    /* Delete (only shown when editing existing) */
    s_ed_delete_btn = lv_button_create(s_editor_view);
    lv_obj_set_size(s_ed_delete_btn, 228, 40);
    lv_obj_align(s_ed_delete_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(s_ed_delete_btn, lv_color_hex(hb_color_danger()), 0);
    lv_obj_set_style_radius(s_ed_delete_btn, 8, 0);
    lv_obj_add_event_cb(s_ed_delete_btn, on_delete_editor, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(s_ed_delete_btn);
    lv_label_set_text(dl, LV_SYMBOL_TRASH "  Delete");
    lv_obj_set_style_text_color(dl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(dl);
}

static void build_name_view(void)
{
    s_name_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_name_view, 240, 432);
    lv_obj_set_style_radius(s_name_view, 0, 0);
    lv_obj_set_pos(s_name_view, 0, 0);
    lv_obj_set_style_bg_color(s_name_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_name_view, 0, 0);
    lv_obj_set_style_pad_all(s_name_view, 0, 0);
    lv_obj_clear_flag(s_name_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_name_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *btn_back = lv_button_create(s_name_view);
    lv_obj_set_size(btn_back, 56, 32);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(btn_back, on_name_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn_back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    lv_obj_t *title = lv_label_create(s_name_view);
    lv_label_set_text(title, "Name");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *btn_save = lv_button_create(s_name_view);
    lv_obj_set_size(btn_save, 56, 32);
    lv_obj_align(btn_save, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(btn_save, on_name_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(btn_save);
    lv_label_set_text(sl, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(sl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(sl);

    s_name_ta = lv_textarea_create(s_name_view);
    lv_obj_set_size(s_name_ta, 240, 152);
    lv_obj_align(s_name_ta, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(s_name_ta, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_text_color(s_name_ta, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_border_color(s_name_ta, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_radius(s_name_ta, 0, 0);
    lv_textarea_set_one_line(s_name_ta, true);

    lv_obj_t *kb_box = lv_obj_create(s_name_view);
    lv_obj_set_size(kb_box, 240, 232);
    lv_obj_align(kb_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb_box, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_border_width(kb_box, 0, 0);
    lv_obj_set_style_pad_all(kb_box, 0, 0);
    lv_obj_clear_flag(kb_box, LV_OBJ_FLAG_SCROLLABLE);
    hb_t9_create(kb_box, s_name_ta);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    build_year_opts();
    load_mode();
    load_items();

    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    build_list_view();
    build_editor_view();
    build_name_view();

    open_list();
}
