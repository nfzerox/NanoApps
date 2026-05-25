/*
 * api_tests.c — touch-driven tester for some SDK APIs.
 *
 * Pages:
 *   0: FS advanced    (size, is_dir, mkdir, rmrf, set_attr)
 *   1: Settings R/W   (brightness, backlight timer, EQ)
 *   2: RTC set        (sets time to a fixed test value)
 *   3: Accelerometer  (live X/Y/Z milli-g)
 *   4: Media          (state + has_session + skip next/prev buttons)
 *   5: Orientation    (read current, set to next 90 deg)
 *
 * Controls:
 *   tap "<" / ">" at bottom to switch pages
 *   tap "RUN" to execute the page's action
 *   vol-up to exit
 */

#include "hb_sdk.h"

#define BG          HB_RGB(0x0a, 0x10, 0x18)
#define HDR_BG      HB_RGB(0x20, 0x20, 0x40)
#define FG          HB_WHITE
#define OK_C        HB_GREEN
#define ERR_C       HB_RED
#define ACCENT      HB_RGB(0xff, 0xa8, 0x00)

#define HDR_H       40
#define FOOT_H      56
#define CONTENT_Y   (HDR_H + 4)
#if defined(NANOAPPS_RELEASE)
#define MAX_PAGES   6
#else
#define MAX_PAGES   7
#endif

/* --- helpers --- */
static int my_strlen(const char *s) { int n=0; while(s[n])n++; return n; }
static void my_strcpy(char *d, const char *s, int max) {
    int i=0; while(s[i]&&i<max-1){d[i]=s[i];i++;} d[i]=0;
}

static void draw_str_fit(int16_t x, int16_t y, const char *s, uint8_t scale,
                         hb_color_t fg, hb_color_t bg)
{
    int16_t max_chars = (HB_SCREEN_W - x) / (8 * scale);
    for (int16_t i = 0; s[i] && i < max_chars; i++) {
        hb_draw_char((int16_t)(x + i * 8 * scale), y, s[i], scale, fg, bg);
    }
}

static void draw_str_fit_width(int16_t x, int16_t y, const char *s,
                               uint8_t scale, hb_color_t fg, hb_color_t bg,
                               int16_t max_px)
{
    int16_t max_chars = max_px / (8 * scale);
    for (int16_t i = 0; s[i] && i < max_chars; i++) {
        hb_draw_char((int16_t)(x + i * 8 * scale), y, s[i], scale, fg, bg);
    }
}

static int dec_to_str(int32_t v, char *out)
{
    int i = 0;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { out[i++] = '0'; }
    else {
        char tmp[16]; int t = 0;
        while (v > 0 && t < 15) {
            int32_t q = 0, r = v;
            while (r >= 10) { r -= 10; q++; }
            tmp[t++] = '0' + r;
            v = q;
        }
        if (neg) out[i++] = '-';
        while (t > 0) out[i++] = tmp[--t];
    }
    out[i] = 0;
    return i;
}

static void label(int16_t y, const char *s, hb_color_t c)
{
    draw_str_fit(8, y, s, 2, c, BG);
}
static void small(int16_t x, int16_t y, const char *s, hb_color_t c)
{
    draw_str_fit(x, y, s, 2, c, BG);
}

/* --- page renderers --- */

static const char *PAGE_TITLES[MAX_PAGES] = {
    "Files", "Settings", "Clock",
    "Accel", "Media", "Rotate",
};

static char g_status[64] = "tap RUN to test";

static void draw_header(int page)
{
    hb_fill_rect(0, 0, HB_SCREEN_W, HDR_H, HDR_BG);
    draw_str_fit_width(8, 10, PAGE_TITLES[page], 2, FG, HDR_BG, 160);
    char p[8] = "p _/_";
    p[2] = '0' + (page + 1);
    p[4] = '0' + MAX_PAGES;
    draw_str_fit(176, 10, p, 2, FG, HDR_BG);
}

static void draw_footer(int page)
{
    hb_fill_rect(0, HB_SCREEN_H - FOOT_H, HB_SCREEN_W, FOOT_H, HDR_BG);
    /* "<" button left, ">" right, "RUN" middle */
    hb_fill_rect(8,   HB_SCREEN_H - FOOT_H + 8, 56, 40, ACCENT);
    draw_str_fit(28,   HB_SCREEN_H - FOOT_H + 20, "<", 2, BG, ACCENT);

    hb_fill_rect(72,  HB_SCREEN_H - FOOT_H + 8, 96, 40, OK_C);
    draw_str_fit(94,   HB_SCREEN_H - FOOT_H + 20, "RUN", 2, BG, OK_C);

    hb_fill_rect(176, HB_SCREEN_H - FOOT_H + 8, 56, 40, ACCENT);
    draw_str_fit(196,  HB_SCREEN_H - FOOT_H + 20, ">", 2, BG, ACCENT);
    (void)page;
}

static void clear_content(void)
{
    hb_fill_rect(0, CONTENT_Y, HB_SCREEN_W, HB_SCREEN_H - CONTENT_Y - FOOT_H, BG);
}

static void show_status(void)
{
    int16_t y0 = HB_SCREEN_H - FOOT_H - 44;
    hb_fill_rect(0, y0, HB_SCREEN_W, 42, BG);
    draw_str_fit(8, y0, g_status, 2, FG, BG);
    if (my_strlen(g_status) > 14) {
        draw_str_fit(8, (int16_t)(y0 + 20), g_status + 14, 2, FG, BG);
    }
}

/* --- page-specific tests --- */

static void test_fs(void)
{
    const char *dir = "/Apps/.api_test_dir";
    const char *file = "/Apps/.api_test_dir/sub/x.txt";
    const char *parent = "/Apps/.api_test_dir/sub";
    const char *data = "hello";

    bool mk = hb_fs_mkdir(parent);
    bool wr = hb_fs_write(file, data, 5);
    uint32_t sz = hb_fs_size(file);
    bool is_d = hb_fs_is_dir(dir);
    bool rm = hb_fs_rmrf(dir);

    char buf[64];
    int i = 0;
    my_strcpy(buf, "mk", 64); i = 2;
    buf[i++] = mk ? 'Y' : 'N';
    my_strcpy(buf+i, " wr", 64-i); i += 3;
    buf[i++] = wr ? 'Y' : 'N';
    my_strcpy(buf+i, " sz", 64-i); i += 3;
    i += dec_to_str(sz, buf+i);
    my_strcpy(buf+i, " dir", 64-i); i += 4;
    buf[i++] = is_d ? 'Y' : 'N';
    my_strcpy(buf+i, " rm", 64-i); i += 3;
    buf[i++] = rm ? 'Y' : 'N';
    buf[i] = 0;
    my_strcpy(g_status, buf, 64);
}

static void test_settings(void)
{
    int32_t b = hb_settings_get_brightness();
    int32_t t = hb_settings_get_backlight_timer();
    int32_t eq = hb_settings_get_int(HB_PREF_PLAYBACK_EQ, -1);

    char buf[64];
    int i = 0;
    my_strcpy(buf, "br=", 64); i = 3;
    i += dec_to_str(b, buf+i);
    my_strcpy(buf+i, " btmr=", 64-i); i += 6;
    i += dec_to_str(t, buf+i);
    my_strcpy(buf+i, " eq=", 64-i); i += 4;
    i += dec_to_str(eq, buf+i);
    buf[i] = 0;
    my_strcpy(g_status, buf, 64);
}

static void test_rtc(void)
{
    bool ok = hb_rtc_set(2024, 1, 2, 9, 30, 0);
    hb_rtc_time_t now;
    hb_rtc_read(&now);
    char buf[64];
    int i = 0;
    my_strcpy(buf, ok ? "setY " : "setN ", 64); i = 5;
    i += dec_to_str(now.year, buf+i);
    buf[i++] = '/';
    i += dec_to_str(now.month, buf+i);
    buf[i++] = '/';
    i += dec_to_str(now.day_of_month, buf+i);
    buf[i++] = ' ';
    i += dec_to_str(now.hours, buf+i);
    buf[i++] = ':';
    i += dec_to_str(now.minutes, buf+i);
    buf[i] = 0;
    my_strcpy(g_status, buf, 64);
}

static int32_t g_accel_xyz[3] = {0,0,0};
static void test_accel(void)
{
    hb_accel_read_milli_g(g_accel_xyz);
    char buf[64];
    int i = 0;
    my_strcpy(buf, "x=", 64); i = 2;
    i += dec_to_str(g_accel_xyz[0], buf+i);
    my_strcpy(buf+i, " y=", 64-i); i += 3;
    i += dec_to_str(g_accel_xyz[1], buf+i);
    my_strcpy(buf+i, " z=", 64-i); i += 3;
    i += dec_to_str(g_accel_xyz[2], buf+i);
    buf[i] = 0;
    my_strcpy(g_status, buf, 64);
}

static void test_media(void)
{
    int s = hb_media_state();
    bool has = hb_media_has_session();
    char buf[64];
    int i = 0;
    my_strcpy(buf, "st", 64); i = 2;
    i += dec_to_str(s, buf+i);
    my_strcpy(buf+i, " has", 64-i); i += 4;
    buf[i++] = has ? 'Y' : 'N';
    buf[i] = 0;
    my_strcpy(g_status, buf, 64);
}

static void test_orientation(void)
{
    /* moved inline into payload_entry to avoid runtime divmod helper */
}


static void run_page(int page)
{
    switch (page) {
        case 0: test_fs();          break;
        case 1: test_settings();    break;
        case 2: test_rtc();         break;
        case 3: test_accel();       break;
        case 4: test_media();       break;
        case 5: test_orientation(); break;
    }
}

static void draw_page_content(int page)
{
    clear_content();
    int16_t y = CONTENT_Y + 4;
    label(y, PAGE_TITLES[page], ACCENT);
    y += 28;

    /* Page-specific static description */
    switch (page) {
        case 0:
            small(8, y, "mkdir/write", FG); y += 24;
            small(8, y, "size/dir/rm", FG); y += 24;
            small(8, y, "/Apps test", FG);
            break;
        case 1:
            small(8, y, "brightness", FG); y += 24;
            small(8, y, "timer + EQ", FG);
            break;
        case 2:
            small(8, y, "set test", FG); y += 24;
            small(8, y, "clock value", FG);
            break;
        case 3:
            small(8, y, "live sample", FG); y += 24;
            small(8, y, "RUN refresh", FG);
            break;
        case 4:
            small(8, y, "media state", FG); y += 24;
            small(8, y, "play music", FG); y += 24;
            small(8, y, "first", FG);
            break;
        case 5:
            small(8, y, "rotate 90", FG); y += 24;
            small(8, y, "tap RUN", FG);
            break;
    }

    show_status();
}

/* Hit-test buttons */
static int hit_footer(int16_t tx, int16_t ty)
{
    int by = HB_SCREEN_H - FOOT_H;
    if (ty < by + 8 || ty > by + 48) return -1;
    if (tx >= 8   && tx < 64)  return 0; /* prev */
    if (tx >= 72  && tx < 168) return 1; /* run */
    if (tx >= 176 && tx < 232) return 2; /* next */
    return -1;
}

HB_APP_ENTRY(payload_entry)
{
    hb_ui_init();
    hb_fill_screen(BG);

    int page = 0;
    draw_header(page);
    draw_page_content(page);
    draw_footer(page);

    for (uint32_t frame = 0; ; frame++) {
        int16_t tx = 0, ty = 0;
        hb_ui_event_t e = hb_ui_poll(&tx, &ty);
        if (e == HB_UI_EXIT) break;
        if (e == HB_UI_TAP) {
            int btn = hit_footer(tx, ty);
            if (btn == 0 && page > 0) {
                page--;
                draw_header(page);
                draw_page_content(page);
            } else if (btn == 2 && page + 1 < MAX_PAGES) {
                page++;
                draw_header(page);
                draw_page_content(page);
            } else if (btn == 1) {
                /* Special-case orientation to avoid mod-by-non-power-of-2 */
                if (page == 5) {
                    int32_t cur = hb_orientation_get();
                    int32_t next = cur + 90;
                    if (next >= 360) next -= 360;
                    hb_orientation_set(next);
                    char ob[32];
                    int oi = 0;
                    my_strcpy(ob, "set ", 32); oi = 4;
                    oi += dec_to_str(next, ob+oi);
                    my_strcpy(ob+oi, " (was ", 32-oi); oi += 6;
                    oi += dec_to_str(cur, ob+oi);
                    ob[oi++] = ')'; ob[oi] = 0;
                    my_strcpy(g_status, ob, 32);
                } else {
                    run_page(page);
                }
                show_status();
                /* On accel page, show live for a moment */
                if (page == 3) {
                    for (int i = 0; i < 30; i++) {
                        test_accel();
                        show_status();
                        for (volatile int j = 0; j < 10000; j++) {}
                    }
                }
            }
        }
        hb_ui_pace();
    }
    hb_ui_done();
}
