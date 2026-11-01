/*
 * calendar — Day / Week / Month / Year views.
 *
 * Tab bar at the top switches between four views over a single state
 * (s_year/s_month/s_day, initialized from the OS RTC). The bottom of
 * each view has prev/today/next buttons that step the state by the
 * appropriate amount; switching tabs preserves the selected date so
 * "tap a day in Month, switch to Day" works.
 */

#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_t9.h"
#include "lvgl/lvgl.h"

/* ---- events ----
   Stored as YYYY-MM-DD<TAB>text<NL> in /Data/Calendar/events.txt.
   Loaded once at startup; saved on add/delete. */

#define EVENTS_PATH    "/Data/Calendar/events.txt"
#define EVENTS_DIR     "/Data/Calendar"
#define MAX_EVENTS     128
#define MAX_EVENT_TEXT 64

/* hour/minute: 0..23/0..59 for timed events; both -1 for all-day. */
typedef struct {
    int   year, month, day;
    int   hour, minute;
    char  text[MAX_EVENT_TEXT];
} event_t;

static event_t s_events[MAX_EVENTS];
static int     s_n_events = 0;

/* ---- current selection ---- */
static int s_year, s_month, s_day;

/* ---- views ---- */
static lv_obj_t *s_day_title, *s_day_body;
static lv_obj_t *s_week_grid;
static lv_obj_t *s_month_title, *s_month_grid;
static lv_obj_t *s_year_title, *s_year_grid;

static lv_obj_t *s_tabview;
/* Mirror the tabview's active index. lv_tabview_get_tab_active() was
   returning 0 regardless of which tab the user had swiped/tapped to,
   so prev/next only stepped by a day (matching tab 0). We track it
   ourselves via LV_EVENT_VALUE_CHANGED to keep it correct. */
static uint32_t s_active_tab = 2;

/* Day view event list container — rebuilt by refresh_day. */
static lv_obj_t *s_day_events_list;

/* Event input sub-view (full-screen overlay over the tabview). */
static lv_obj_t *s_event_view;
static lv_obj_t *s_event_ta;
static lv_obj_t *s_event_date_lbl;

static const char *weekday_long[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};
static const char *weekday_short[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *weekday_initial[] = {
    "S", "M", "T", "W", "T", "F", "S"
};
static const char *month_long[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char *month_short[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static int is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m)
{
    static const int dm[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && is_leap(y)) return 29;
    return dm[m - 1];
}

/* Zeller's congruence — returns 0=Sunday .. 6=Saturday for any
   Gregorian date in our supported range. */
static int day_of_week(int y, int m, int d)
{
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100;
    int J = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    /* h: 0=Saturday, 1=Sunday, ... 6=Friday. Convert to 0=Sun .. 6=Sat. */
    return (h + 6) % 7;
}

static void itoa_u(uint32_t v, char *out)
{
    char buf[12]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    int k = 0;
    while (i) out[k++] = buf[--i];
    out[k] = 0;
}

static void normalize_date(void)
{
    while (s_month < 1)  { s_month += 12; s_year--; }
    while (s_month > 12) { s_month -= 12; s_year++; }
    if (s_year < 2000) s_year = 2000;
    if (s_year > 2099) s_year = 2099;
    int dim = days_in_month(s_year, s_month);
    if (s_day < 1)   s_day = 1;
    if (s_day > dim) s_day = dim;
}

static void reset_to_today(void)
{
    hb_rtc_time_t t;
    hb_rtc_read(&t);
    if (t.year >= 2000 && t.year <= 2099 &&
        t.month >= 1 && t.month <= 12 &&
        t.day_of_month >= 1 && t.day_of_month <= 31) {
        s_year = t.year;
        s_month = t.month;
        s_day = t.day_of_month;
    } else {
        s_year = 2026; s_month = 5; s_day = 28;
    }
    normalize_date();
}

/* Forward declarations of view refreshers — called whenever the
   selected date changes. */
static void refresh_day(void);
static void refresh_week(void);
static void refresh_month(void);
static void refresh_year(void);

static void refresh_all(void)
{
    refresh_day();
    refresh_week();
    refresh_month();
    refresh_year();
}

/* ---- events: persistence ---- */

static void load_events(void)
{
    s_n_events = 0;
    static char buf[MAX_EVENTS * (MAX_EVENT_TEXT + 16)];
    uint32_t n = hb_fs_read(EVENTS_PATH, buf, sizeof buf - 1);
    if (n == 0) return;
    buf[n] = 0;
    uint32_t i = 0;
    while (i < n && s_n_events < MAX_EVENTS) {
        int y = 0, mo = 0, d = 0;
        while (i < n && buf[i] >= '0' && buf[i] <= '9') { y = y*10 + (buf[i]-'0'); i++; }
        if (i < n && buf[i] == '-') i++; else { while (i<n && buf[i]!='\n') i++; if (i<n) i++; continue; }
        while (i < n && buf[i] >= '0' && buf[i] <= '9') { mo = mo*10 + (buf[i]-'0'); i++; }
        if (i < n && buf[i] == '-') i++; else { while (i<n && buf[i]!='\n') i++; if (i<n) i++; continue; }
        while (i < n && buf[i] >= '0' && buf[i] <= '9') { d = d*10 + (buf[i]-'0'); i++; }
        if (i < n && buf[i] == '\t') i++;
        event_t *e = &s_events[s_n_events];
        e->year = y; e->month = mo; e->day = d;
        e->hour = -1; e->minute = -1;
        /* Optional time prefix: "HH:MM\t" or "-\t". If neither, the
         * whole rest is the text (backward compatibility with old
         * untyped files). */
        if (i + 1 < n && buf[i] == '-' && buf[i+1] == '\t') {
            i += 2;  /* explicit all-day marker */
        } else if (i + 4 < n &&
                   buf[i] >= '0' && buf[i] <= '9' &&
                   buf[i+1] >= '0' && buf[i+1] <= '9' &&
                   buf[i+2] == ':' &&
                   buf[i+3] >= '0' && buf[i+3] <= '9' &&
                   buf[i+4] >= '0' && buf[i+4] <= '9' &&
                   (i + 5 < n && buf[i+5] == '\t')) {
            e->hour   = (buf[i] - '0') * 10 + (buf[i+1] - '0');
            e->minute = (buf[i+3] - '0') * 10 + (buf[i+4] - '0');
            i += 6;
        }
        int k = 0;
        while (i < n && buf[i] != '\n' && k < MAX_EVENT_TEXT - 1) e->text[k++] = buf[i++];
        e->text[k] = 0;
        if (i < n && buf[i] == '\n') i++;
        if (e->text[0]) s_n_events++;
    }
}

static void save_events(void)
{
    static char buf[MAX_EVENTS * (MAX_EVENT_TEXT + 16)];
    uint32_t pos = 0;
    for (int i = 0; i < s_n_events; i++) {
        event_t *e = &s_events[i];
        char tmp[8];
        /* year padded 4 */
        itoa_u(e->year, tmp); int yl = 0; while (tmp[yl]) yl++;
        while (yl < 4) { buf[pos++] = '0'; yl++; }
        for (int j = 0; tmp[j]; j++) buf[pos++] = tmp[j];
        buf[pos++] = '-';
        if (e->month < 10) buf[pos++] = '0';
        itoa_u(e->month, tmp); for (int j = 0; tmp[j]; j++) buf[pos++] = tmp[j];
        buf[pos++] = '-';
        if (e->day < 10) buf[pos++] = '0';
        itoa_u(e->day, tmp); for (int j = 0; tmp[j]; j++) buf[pos++] = tmp[j];
        buf[pos++] = '\t';
        if (e->hour < 0) {
            buf[pos++] = '-';
        } else {
            if (e->hour < 10) buf[pos++] = '0';
            itoa_u(e->hour, tmp); for (int j = 0; tmp[j]; j++) buf[pos++] = tmp[j];
            buf[pos++] = ':';
            if (e->minute < 10) buf[pos++] = '0';
            itoa_u(e->minute, tmp); for (int j = 0; tmp[j]; j++) buf[pos++] = tmp[j];
        }
        buf[pos++] = '\t';
        for (int j = 0; e->text[j] && pos < sizeof buf - 2; j++) buf[pos++] = e->text[j];
        buf[pos++] = '\n';
    }
    hb_fs_write(EVENTS_PATH, buf, pos);
}

/* True if the given date has any events. */
static bool date_has_events(int y, int m, int d)
{
    for (int i = 0; i < s_n_events; i++) {
        if (s_events[i].year == y && s_events[i].month == m && s_events[i].day == d) return true;
    }
    return false;
}

/* ---- Day view ---- */

static void open_event_input(void);

static void on_add_event(lv_event_t *e) { (void)e; open_event_input(); }

static void on_delete_event(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_n_events) return;
    for (int i = idx; i < s_n_events - 1; i++) s_events[i] = s_events[i+1];
    s_n_events--;
    save_events();
    refresh_day();
}

static void refresh_day(void)
{
    if (!s_day_title) return;
    int wd = day_of_week(s_year, s_month, s_day);
    char buf[64]; int k = 0;
    const char *w = weekday_long[wd];
    while (*w) buf[k++] = *w++;
    buf[k++] = ',';
    buf[k++] = ' ';
    const char *m = month_long[s_month - 1];
    while (*m) buf[k++] = *m++;
    buf[k++] = ' ';
    char d[8]; itoa_u(s_day, d);
    for (int i = 0; d[i]; i++) buf[k++] = d[i];
    buf[k] = 0;
    lv_label_set_text(s_day_title, buf);

    /* Date sub-line: "2026" */
    if (s_day_body) {
        char y[8]; itoa_u(s_year, y);
        lv_label_set_text(s_day_body, y);
    }

    /* Event list: any event whose date matches s_year/s_month/s_day. */
    if (s_day_events_list) {
        lv_obj_clean(s_day_events_list);
        bool any = false;
        for (int i = 0; i < s_n_events; i++) {
            if (s_events[i].year != s_year || s_events[i].month != s_month ||
                s_events[i].day  != s_day) continue;
            any = true;
            lv_obj_t *row = lv_obj_create(s_day_events_list);
            lv_obj_set_size(row, 220, 32);
            lv_obj_set_style_bg_color(row, lv_color_hex(hb_color_surface()), 0);
            lv_obj_set_style_radius(row, 6, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_set_style_margin_bottom(row, 4, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            char line[MAX_EVENT_TEXT + 12];
            int p = 0;
            if (s_events[i].hour >= 0) {
                int h = s_events[i].hour, mi = s_events[i].minute;
                line[p++] = '0' + (h / 10); line[p++] = '0' + (h % 10);
                line[p++] = ':';
                line[p++] = '0' + (mi / 10); line[p++] = '0' + (mi % 10);
                line[p++] = ' '; line[p++] = ' ';
            }
            for (int j = 0; s_events[i].text[j] && p < (int)sizeof(line) - 1; j++)
                line[p++] = s_events[i].text[j];
            line[p] = 0;

            lv_obj_t *t = lv_label_create(row);
            lv_label_set_text(t, line);
            lv_obj_set_style_text_color(t, lv_color_hex(hb_color_text()), 0);
            lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
            lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
            lv_obj_set_width(t, 170);
            lv_obj_align(t, LV_ALIGN_LEFT_MID, 8, 0);

            lv_obj_t *del = lv_button_create(row);
            lv_obj_set_size(del, 28, 28);
            lv_obj_align(del, LV_ALIGN_RIGHT_MID, -4, 0);
            lv_obj_set_style_bg_color(del, lv_color_hex(hb_color_text_dim()), 0);
            lv_obj_set_style_radius(del, 6, 0);
            lv_obj_set_style_shadow_width(del, 0, 0);
            lv_obj_add_event_cb(del, on_delete_event, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)i);
            lv_obj_t *xl = lv_label_create(del);
            lv_label_set_text(xl, LV_SYMBOL_TRASH);
            lv_obj_set_style_text_color(xl, lv_color_hex(hb_color_text()), 0);
            lv_obj_center(xl);
        }
        if (!any) {
            lv_obj_t *none = lv_label_create(s_day_events_list);
            lv_label_set_text(none, "No events. Tap + to add one.");
            lv_obj_set_style_text_color(none, lv_color_hex(hb_color_text_dim()), 0);
            lv_obj_set_style_text_font(none, &lv_font_montserrat_14, 0);
            lv_obj_align(none, LV_ALIGN_CENTER, 0, 0);
        }
    }
}

static lv_obj_t *make_day_tab(lv_obj_t *parent)
{
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    s_day_title = lv_label_create(parent);
    lv_obj_set_style_text_color(s_day_title, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_day_title, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_day_title, 220);
    lv_label_set_long_mode(s_day_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_day_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_day_title, LV_ALIGN_TOP_MID, 0, 8);

    s_day_body = lv_label_create(parent);
    lv_obj_set_style_text_color(s_day_body, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_day_body, &lv_font_montserrat_16, 0);
    lv_obj_align(s_day_body, LV_ALIGN_TOP_MID, 0, 64);

    /* "+" button to add a new event — floated to the bottom-right so it no longer
       overlaps the centred day title at the top. */
    lv_obj_t *plus = lv_button_create(parent);
    lv_obj_set_size(plus, 48, 44);
    lv_obj_align(plus, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(plus, lv_color_hex(hb_color_success()), 0);
    lv_obj_set_style_radius(plus, 8, 0);
    lv_obj_add_event_cb(plus, on_add_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = lv_label_create(plus);
    lv_label_set_text(pl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(pl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_20, 0);
    lv_obj_center(pl);

    /* Event list fills the rest of the tab content. */
    s_day_events_list = lv_obj_create(parent);
    lv_obj_set_size(s_day_events_list, 232, 220);
    lv_obj_align(s_day_events_list, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_bg_opa(s_day_events_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_day_events_list, 0, 0);
    lv_obj_set_style_pad_all(s_day_events_list, 4, 0);
    lv_obj_set_flex_flow(s_day_events_list, LV_FLEX_FLOW_COLUMN);

    /* The transparent event list is created after (and overlaps) the + button, so
       it would otherwise capture the taps. Keep + on top of the z-order. */
    lv_obj_move_foreground(plus);

    return parent;
}

/* ---- Week view ---- */

static void refresh_week(void)
{
    if (!s_week_grid) return;
    lv_obj_clean(s_week_grid);

    /* Compute Sunday of the week containing s_day. */
    int wd = day_of_week(s_year, s_month, s_day);
    int start_d = s_day - wd;
    int start_m = s_month, start_y = s_year;
    if (start_d < 1) {
        start_m--;
        if (start_m < 1) { start_m = 12; start_y--; }
        start_d += days_in_month(start_y, start_m);
    }

    /* 7 columns laid out horizontally. */
    for (int i = 0; i < 7; i++) {
        int dd = start_d + i;
        int dm = start_m, dy = start_y;
        if (dd > days_in_month(dy, dm)) {
            dd -= days_in_month(dy, dm);
            dm++;
            if (dm > 12) { dm = 1; dy++; }
        }

        lv_obj_t *col = lv_obj_create(s_week_grid);
        lv_obj_set_size(col, 30, 80);
        lv_obj_set_pos(col, 4 + i * 32, 8);
        lv_obj_set_style_pad_all(col, 0, 0);
        lv_obj_set_style_radius(col, 8, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        bool is_today = (dy == s_year && dm == s_month && dd == s_day);
        lv_obj_set_style_bg_color(col,
            is_today ? lv_color_hex(hb_color_primary()) : lv_color_hex(hb_color_surface()), 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *wd_lbl = lv_label_create(col);
        lv_label_set_text(wd_lbl, weekday_initial[i]);
        lv_obj_set_style_text_color(wd_lbl,
            is_today ? lv_color_hex(0x000000) : lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_style_text_font(wd_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(wd_lbl, LV_ALIGN_TOP_MID, 0, 6);

        char dnum[4]; itoa_u(dd, dnum);
        lv_obj_t *d_lbl = lv_label_create(col);
        lv_label_set_text(d_lbl, dnum);
        lv_obj_set_style_text_color(d_lbl,
            is_today ? lv_color_hex(0x000000) : lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(d_lbl, &lv_font_montserrat_20, 0);
        lv_obj_align(d_lbl, LV_ALIGN_BOTTOM_MID, 0, -6);
    }

    /* Header: month + year of the focused day. */
    lv_obj_t *hdr = lv_label_create(s_week_grid);
    char hbuf[40]; int k = 0;
    const char *mn = month_long[s_month - 1];
    while (*mn) hbuf[k++] = *mn++;
    hbuf[k++] = ' ';
    char y[8]; itoa_u(s_year, y);
    for (int i = 0; y[i]; i++) hbuf[k++] = y[i];
    hbuf[k] = 0;
    lv_label_set_text(hdr, hbuf);
    lv_obj_set_style_text_color(hdr, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
    lv_obj_align(hdr, LV_ALIGN_BOTTOM_MID, 0, -10);
}

static lv_obj_t *make_week_tab(lv_obj_t *parent)
{
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    s_week_grid = lv_obj_create(parent);
    lv_obj_set_size(s_week_grid, 240, 200);
    lv_obj_align(s_week_grid, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_week_grid, lv_color_hex(0x0a0a0f), 0);
    lv_obj_set_style_border_width(s_week_grid, 0, 0);
    lv_obj_set_style_pad_all(s_week_grid, 0, 0);
    lv_obj_clear_flag(s_week_grid, LV_OBJ_FLAG_SCROLLABLE);
    return parent;
}

/* ---- Month view ---- */

static void on_month_day(lv_event_t *e)
{
    int day = (int)(uintptr_t)lv_event_get_user_data(e);
    if (day < 1 || day > days_in_month(s_year, s_month)) return;
    s_day = day;
    refresh_all();
}

static void refresh_month(void)
{
    if (!s_month_title) return;
    char hbuf[40]; int k = 0;
    const char *mn = month_long[s_month - 1];
    while (*mn) hbuf[k++] = *mn++;
    hbuf[k++] = ' ';
    char y[8]; itoa_u(s_year, y);
    for (int i = 0; y[i]; i++) hbuf[k++] = y[i];
    hbuf[k] = 0;
    lv_label_set_text(s_month_title, hbuf);

    lv_obj_clean(s_month_grid);

    /* Header row: weekday letters. */
    for (int i = 0; i < 7; i++) {
        lv_obj_t *l = lv_label_create(s_month_grid);
        lv_label_set_text(l, weekday_initial[i]);
        lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(l, 16 + i * 30, 0);
    }

    int first_dow = day_of_week(s_year, s_month, 1);
    int dim = days_in_month(s_year, s_month);

    /* 6 rows × 7 cols of day cells. */
    for (int day = 1; day <= dim; day++) {
        int idx = first_dow + day - 1;
        int row = idx / 7;
        int col = idx % 7;
        lv_obj_t *cell = lv_button_create(s_month_grid);
        lv_obj_set_size(cell, 26, 26);
        lv_obj_set_pos(cell, 10 + col * 30, 22 + row * 32);
        lv_obj_set_style_radius(cell, 13, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        bool is_sel = (day == s_day);
        lv_obj_set_style_bg_color(cell,
            is_sel ? lv_color_hex(hb_color_primary()) : lv_color_hex(0x222531), 0);
        lv_obj_add_event_cb(cell, on_month_day, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)day);
        lv_obj_t *l = lv_label_create(cell);
        char dn[4]; itoa_u(day, dn);
        lv_label_set_text(l, dn);
        lv_obj_set_style_text_color(l,
            is_sel ? lv_color_hex(0x000000) : lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_center(l);
        /* Tiny dot for days with events. */
        if (date_has_events(s_year, s_month, day)) {
            lv_obj_t *dot = lv_obj_create(cell);
            lv_obj_set_size(dot, 4, 4);
            lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -1);
            lv_obj_set_style_radius(dot, 2, 0);
            lv_obj_set_style_bg_color(dot,
                lv_color_hex(is_sel ? 0x000000 : 0x2a9d8f), 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

static lv_obj_t *make_month_tab(lv_obj_t *parent)
{
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    s_month_title = lv_label_create(parent);
    lv_obj_set_style_text_color(s_month_title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_month_title, &lv_font_montserrat_20, 0);
    lv_obj_align(s_month_title, LV_ALIGN_TOP_MID, 0, 4);

    s_month_grid = lv_obj_create(parent);
    lv_obj_set_size(s_month_grid, 240, 230);
    lv_obj_align(s_month_grid, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(s_month_grid, lv_color_hex(0x0a0a0f), 0);
    lv_obj_set_style_border_width(s_month_grid, 0, 0);
    lv_obj_set_style_pad_all(s_month_grid, 0, 0);
    lv_obj_clear_flag(s_month_grid, LV_OBJ_FLAG_SCROLLABLE);

    return parent;
}

/* ---- Year view ---- */

static void on_year_month(lv_event_t *e)
{
    int mo = (int)(uintptr_t)lv_event_get_user_data(e);
    if (mo < 1 || mo > 12) return;
    s_month = mo;
    if (s_day > days_in_month(s_year, s_month)) {
        s_day = days_in_month(s_year, s_month);
    }
    refresh_all();
    /* Switch to Month view to show the selected month. */
    if (s_tabview) lv_tabview_set_active(s_tabview, 2, LV_ANIM_OFF);
}

static void refresh_year(void)
{
    if (!s_year_title) return;
    char y[8]; itoa_u(s_year, y);
    lv_label_set_text(s_year_title, y);

    lv_obj_clean(s_year_grid);
    /* 4 rows × 3 cols of month buttons. */
    for (int mo = 1; mo <= 12; mo++) {
        int row = (mo - 1) / 3;
        int col = (mo - 1) % 3;
        lv_obj_t *box = lv_button_create(s_year_grid);
        lv_obj_set_size(box, 64, 62);
        lv_obj_set_pos(box, 12 + col * 74, 4 + row * 68);
        lv_obj_set_style_radius(box, 10, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        bool is_cur = (mo == s_month);
        lv_obj_set_style_bg_color(box,
            is_cur ? lv_color_hex(hb_color_primary()) : lv_color_hex(0x222531), 0);
        lv_obj_add_event_cb(box, on_year_month, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)mo);

        lv_obj_t *mn = lv_label_create(box);
        lv_label_set_text(mn, month_short[mo - 1]);
        lv_obj_set_style_text_color(mn,
            is_cur ? lv_color_hex(0x000000) : lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(mn, &lv_font_montserrat_20, 0);
        lv_obj_align(mn, LV_ALIGN_TOP_MID, 0, 8);

        /* Day count for context. */
        lv_obj_t *cnt = lv_label_create(box);
        char d[8]; itoa_u(days_in_month(s_year, mo), d);
        lv_label_set_text(cnt, d);
        lv_obj_set_style_text_color(cnt,
            is_cur ? lv_color_hex(0x000000) : lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_style_text_font(cnt, &lv_font_montserrat_16, 0);
        lv_obj_align(cnt, LV_ALIGN_BOTTOM_MID, 0, -6);
    }
}

static lv_obj_t *make_year_tab(lv_obj_t *parent)
{
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    s_year_title = lv_label_create(parent);
    lv_obj_set_style_text_color(s_year_title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_year_title, &lv_font_montserrat_28, 0);
    lv_obj_align(s_year_title, LV_ALIGN_TOP_MID, 0, 4);

    s_year_grid = lv_obj_create(parent);
    /* Shorter grid (smaller month boxes) so the bottom row clears the
       < Today > nav bar instead of being overlapped by it. */
    lv_obj_set_size(s_year_grid, 240, 286);
    lv_obj_align(s_year_grid, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_color(s_year_grid, lv_color_hex(0x0a0a0f), 0);
    lv_obj_set_style_border_width(s_year_grid, 0, 0);
    lv_obj_set_style_pad_all(s_year_grid, 0, 0);
    lv_obj_clear_flag(s_year_grid, LV_OBJ_FLAG_SCROLLABLE);

    return parent;
}

/* ---- Nav (prev / today / next) ---- */

static void on_prev(lv_event_t *e)
{
    (void)e;
    switch (s_active_tab) {
        case 0: s_day -= 1; break;           /* Day -> -1 day */
        case 1: s_day -= 7; break;           /* Week -> -7 days */
        case 2: s_month -= 1; break;         /* Month -> -1 month */
        case 3: s_year -= 1; break;          /* Year -> -1 year */
    }
    normalize_date();
    refresh_all();
}

static void on_next(lv_event_t *e)
{
    (void)e;
    switch (s_active_tab) {
        case 0: s_day += 1; break;
        case 1: s_day += 7; break;
        case 2: s_month += 1; break;
        case 3: s_year += 1; break;
    }
    normalize_date();
    refresh_all();
}

static void on_tab_changed(lv_event_t *e)
{
    (void)e;
    if (s_tabview) s_active_tab = lv_tabview_get_tab_active(s_tabview);
}

/* ---- Event input sub-view (overlays the tabview) ---- */

static void close_event_view(void)
{
    lv_obj_add_flag(s_event_view, LV_OBJ_FLAG_HIDDEN);
    refresh_all();
}

static void on_event_back(lv_event_t *e) { (void)e; close_event_view(); }

static void on_event_save(lv_event_t *e)
{
    (void)e;
    const char *txt = lv_textarea_get_text(s_event_ta);
    int n = 0; while (txt[n] && n < MAX_EVENT_TEXT - 1) n++;
    while (n > 0 && (txt[n-1] == ' '||txt[n-1]=='\n'||txt[n-1]=='\r'||txt[n-1]=='\t')) n--;
    if (n > 0 && s_n_events < MAX_EVENTS) {
        event_t *ev = &s_events[s_n_events++];
        ev->year = s_year; ev->month = s_month; ev->day = s_day;
        ev->hour = -1; ev->minute = -1;       /* events created in UI default to all-day */
        for (int i = 0; i < n; i++) ev->text[i] = txt[i];
        ev->text[n] = 0;
        save_events();
    }
    close_event_view();
}

static void build_event_view(lv_obj_t *scr);

static void open_event_input(void)
{
    /* Lazy-build the event input view (T9 keyboard etc. — ~50 widgets)
       on first use. The tabview alone already pushes the LVGL heap
       budget; eagerly building the event view at startup was enough
       to panic the device on launch. Building it on first + tap means
       startup heap stays small and the user only pays the cost the
       first time they create an event. */
    if (!s_event_view) {
        build_event_view(lv_screen_active());
        if (!s_event_view) return;
    }
    /* Update date label */
    char buf[40]; int k = 0;
    const char *m = month_long[s_month - 1];
    while (*m) buf[k++] = *m++;
    buf[k++] = ' ';
    char dn[8]; itoa_u(s_day, dn);
    for (int i = 0; dn[i]; i++) buf[k++] = dn[i];
    buf[k++] = ',';
    buf[k++] = ' ';
    char y[8]; itoa_u(s_year, y);
    for (int i = 0; y[i]; i++) buf[k++] = y[i];
    buf[k] = 0;
    lv_label_set_text(s_event_date_lbl, buf);
    lv_textarea_set_text(s_event_ta, "");
    lv_obj_clear_flag(s_event_view, LV_OBJ_FLAG_HIDDEN);
}

static void build_event_view(lv_obj_t *scr)
{
    s_event_view = lv_obj_create(scr);
    lv_obj_set_size(s_event_view, 240, 432);
    lv_obj_set_pos(s_event_view, 0, 0);
    lv_obj_set_style_bg_color(s_event_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_event_view, 0, 0);
    lv_obj_set_style_radius(s_event_view, 0, 0);   /* full-screen view: flush corners */
    lv_obj_set_style_pad_all(s_event_view, 0, 0);
    lv_obj_clear_flag(s_event_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_event_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_event_view);
    lv_obj_set_size(back, 56, 32);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(back, on_event_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    s_event_date_lbl = lv_label_create(s_event_view);
    lv_label_set_text(s_event_date_lbl, "");
    lv_obj_set_style_text_color(s_event_date_lbl, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_event_date_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_event_date_lbl, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *save = lv_button_create(s_event_view);
    lv_obj_set_size(save, 56, 32);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_bg_color(save, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(save, on_event_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(sl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(sl);

    s_event_ta = lv_textarea_create(s_event_view);
    lv_obj_set_size(s_event_ta, 240, 152);
    lv_obj_align(s_event_ta, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(s_event_ta, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_text_color(s_event_ta, lv_color_hex(hb_color_text()), 0);
    lv_textarea_set_one_line(s_event_ta, true);

    lv_obj_t *kb_box = lv_obj_create(s_event_view);
    lv_obj_set_size(kb_box, 240, 232);
    lv_obj_align(kb_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb_box, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_border_width(kb_box, 0, 0);
    lv_obj_set_style_pad_all(kb_box, 0, 0);
    lv_obj_clear_flag(kb_box, LV_OBJ_FLAG_SCROLLABLE);
    hb_t9_create(kb_box, s_event_ta);
}

static void on_today(lv_event_t *e)
{
    (void)e;
    reset_to_today();
    refresh_all();
}

HB_APP_ENTRY(payload_entry)
{
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(EVENTS_DIR);
    load_events();

    hb_trace_init();

    reset_to_today();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a0a0f), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Tab view across most of the screen, nav bar at the bottom. */
    s_tabview = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_size(s_tabview, 36);
    lv_obj_set_size(s_tabview, 240, 432 - 48);
    lv_obj_align(s_tabview, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_tabview, lv_color_hex(0x0a0a0f), 0);

    lv_obj_t *tab_day   = lv_tabview_add_tab(s_tabview, "Day");
    lv_obj_t *tab_week  = lv_tabview_add_tab(s_tabview, "Week");
    lv_obj_t *tab_month = lv_tabview_add_tab(s_tabview, "Month");
    lv_obj_t *tab_year  = lv_tabview_add_tab(s_tabview, "Year");

    make_day_tab(tab_day);
    make_week_tab(tab_week);
    make_month_tab(tab_month);
    make_year_tab(tab_year);

    /* Nav bar at the bottom: < Today > */
    lv_obj_t *btn_prev = lv_button_create(scr);
    lv_obj_set_size(btn_prev, 70, 40);
    lv_obj_align(btn_prev, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(hb_color_surface()), 0);
    lv_obj_add_event_cb(btn_prev, on_prev, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_prev = lv_label_create(btn_prev);
    lv_label_set_text(l_prev, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(l_prev, &lv_font_montserrat_20, 0);
    lv_obj_center(l_prev);

    lv_obj_t *btn_today = lv_button_create(scr);
    lv_obj_set_size(btn_today, 92, 40);
    lv_obj_align(btn_today, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(btn_today, lv_color_hex(hb_color_primary()), 0);
    lv_obj_add_event_cb(btn_today, on_today, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_today = lv_label_create(btn_today);
    lv_label_set_text(l_today, "Today");
    lv_obj_set_style_text_color(l_today, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l_today, &lv_font_montserrat_16, 0);
    lv_obj_center(l_today);

    lv_obj_t *btn_next = lv_button_create(scr);
    lv_obj_set_size(btn_next, 70, 40);
    lv_obj_align(btn_next, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(hb_color_surface()), 0);
    lv_obj_add_event_cb(btn_next, on_next, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_next = lv_label_create(btn_next);
    lv_label_set_text(l_next, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(l_next, &lv_font_montserrat_20, 0);
    lv_obj_center(l_next);

    /* Note: event_view is built lazily on first "+" tap to keep the
       LVGL widget budget down — see open_event_input. */

    refresh_all();
    /* Open Month view by default — the most useful single screen. */
    lv_tabview_set_active(s_tabview, 2, LV_ANIM_OFF);
    s_active_tab = 2;
    /* Mirror tab changes (swipe or tap) into our own index so prev/next
       step the right unit. */
    lv_obj_add_event_cb(s_tabview, on_tab_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);

}
