/*
 * wav_player — browse + play WAV files from either:
 *   - Resources volume (system sounds — hardcoded list since dir enum
 *     is not always reliable there)
 *   - /WAV on main volume (user files — live dir enum)
 *
 * UI:
 *   Top bar: SYS | USR tabs to switch source
 *   Middle:  list of files (paginated)
 *   Bottom:  PREV  PAGE x/y  NEXT
 *   Tap row to play. HOME/VOL = exit.
 *
 * Audio: each play does the 4-step ctor / loadfile / setfields / play
 * pattern with a scale-3 chk draw between each — the MIPI activity
 * keeps the OS audio subsystem happy. Whole app runs in a 64 KB
 * pthread.
 */

#include "hb_sdk.h"

#define PTHREAD_CREATE_ADDR (0x080226f8u | 1u)
typedef int (*pthread_create_t)(uint32_t *thread, void *attr,
                                void *(*start)(void *), void *arg);

/* ---- sources ---- */

typedef struct {
    const char *path;
    int         volume;
} src_entry_t;

/* Both sources are dynamically enumerated. Resource-volume enumeration
   is still experimental, so user WAV files are the recommended path. */
#define MAX_ENTRIES  32
#define NAME_BUF_LEN 96

static char g_sys_names[MAX_ENTRIES][NAME_BUF_LEN];
static int  g_n_sys_entries;
static char g_usr_names[MAX_ENTRIES][NAME_BUF_LEN];
static int  g_n_usr_entries;

#define SYS_DIR_PATH "Resources/Sounds"
#define SYS_VOLUME   4
#define USR_DIR_PATH "/WAV"
#define USR_VOLUME   0

static bool ends_with_wav(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    if (n < 4) return false;
    if (s[n-4] != '.') return false;
    char a = s[n-3], b = s[n-2], c = s[n-1];
    return (a=='w'||a=='W') && (b=='a'||b=='A') && (c=='v'||c=='V');
}

static void copy_str(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void scan_into(char names[][NAME_BUF_LEN], int *count,
                      const char *path, int volume)
{
    *count = 0;
    hb_dir_t d;
    if (!hb_fs_dir_open_at(&d, path, false, volume)) return;
    char name[NAME_BUF_LEN];
    bool is_dir;
    while (*count < MAX_ENTRIES &&
           hb_fs_dir_next(&d, name, NAME_BUF_LEN, &is_dir)) {
        if (is_dir) continue;
        if (!ends_with_wav(name)) continue;
        copy_str(names[*count], name, NAME_BUF_LEN);
        (*count)++;
    }
    hb_fs_dir_close(&d);
}

/* ---- audio (raw 4-step pattern; chk-between is load-bearing) ---- */

static uint8_t g_desc[0x80];

static void chk(const char *msg)
{
    int16_t y = HB_SCREEN_H - 90;
    hb_fill_rect(0, y, HB_SCREEN_W, 26, HB_BLACK);
    hb_draw_str(4, y, msg, 3, HB_YELLOW, HB_BLACK);
}

static void play_audio(const char *path, int volume)
{
    chk("step1 ctor");
    hb_audio_ctor(g_desc);

    chk("step2 load");
    int rc = hb_audio_loadfile(g_desc, path, volume);
    if (rc != 0) { chk("LOAD FAIL"); return; }

    chk("step3 fields");
    hb_audio_setfields(g_desc, 0x7fff);

    chk("step4 play");
    hb_audio_play_now(g_desc);
}

/* ---- UI ---- */

enum { SRC_SYS = 0, SRC_USR = 1 };
static int g_source = SRC_SYS;
static hb_paged_list_t g_page;

#define TAB_Y  20
#define TAB_H  28
#define TAB_W  60
#define SYS_X  4
#define USR_X  (SYS_X + TAB_W + 8)

#define LIST_Y      56
#define ROW_H       16
#define PAGE_SIZE   16

#define PAGER_Y    (LIST_Y + ROW_H * PAGE_SIZE + 8)
#define PREV_X     4
#define NEXT_X     (HB_SCREEN_W - 4 - 50)
#define PAGER_BTN_W 50
#define PAGER_BTN_H 28

static int n_entries(void)
{
    return g_source == SRC_SYS ? g_n_sys_entries : g_n_usr_entries;
}

static const char *entry_name(int i)
{
    return g_source == SRC_SYS ? g_sys_names[i] : g_usr_names[i];
}

static const char *entry_path(int i, int *out_volume)
{
    *out_volume = g_source == SRC_SYS ? SYS_VOLUME : USR_VOLUME;
    static char buf[160];
    int pi = 0;
    const char *prefix = g_source == SRC_SYS ? SYS_DIR_PATH "/"
                                             : USR_DIR_PATH "/";
    while (prefix[pi] && pi < (int)sizeof(buf) - 1) {
        buf[pi] = prefix[pi]; pi++;
    }
    int ni = 0;
    const char *name = entry_name(i);
    while (name[ni] && pi < (int)sizeof(buf) - 1) {
        buf[pi++] = name[ni++];
    }
    buf[pi] = 0;
    return buf;
}

static void redraw(void)
{
    hb_fill_screen(HB_BLACK);
    hb_draw_str(4, 0, "WAV PLAYER", 2, HB_YELLOW, HB_BLACK);

    /* Tabs */
    hb_ui_button_draw(SYS_X, TAB_Y, TAB_W, TAB_H, "SYS",
                      g_source == SRC_SYS ? HB_RGB(0x00,0x60,0x60)
                                          : HB_RGB(0x20,0x20,0x20),
                      HB_WHITE);
    hb_ui_button_draw(USR_X, TAB_Y, TAB_W, TAB_H, "USR",
                      g_source == SRC_USR ? HB_RGB(0x60,0x00,0x60)
                                          : HB_RGB(0x20,0x20,0x20),
                      HB_WHITE);

    /* List rows */
    int total = n_entries();
    int from = hb_paged_list_first(&g_page);
    int to   = hb_paged_list_last(&g_page);

    if (total == 0) {
        hb_draw_str(4, LIST_Y, "(no files)", 1, HB_RED, HB_BLACK);
    } else {
        for (int i = from; i < to; i++) {
            int16_t y = LIST_Y + (i - from) * ROW_H;
            hb_draw_str(4, y, entry_name(i), 1, HB_WHITE, HB_BLACK);
        }
    }

    /* Pager */
    hb_ui_button_draw(PREV_X, PAGER_Y, PAGER_BTN_W, PAGER_BTN_H,
                      "<", HB_RGB(0x40,0x40,0x40), HB_WHITE);
    hb_ui_button_draw(NEXT_X, PAGER_Y, PAGER_BTN_W, PAGER_BTN_H,
                      ">", HB_RGB(0x40,0x40,0x40), HB_WHITE);
    /* Page indicator centered */
    char pbuf[16] = "p ";
    int pi = 2;
    int cur = g_page.current_page + 1;
    int tot = hb_paged_list_page_count(&g_page);
    /* No libgcc — subtract to produce digits. */
    int t = cur, tens = 0;
    while (t >= 10) { t -= 10; tens++; }
    if (tens) pbuf[pi++] = '0' + tens;
    pbuf[pi++] = '0' + t;
    pbuf[pi++] = '/';
    t = tot; tens = 0;
    while (t >= 10) { t -= 10; tens++; }
    if (tens) pbuf[pi++] = '0' + tens;
    pbuf[pi++] = '0' + t;
    pbuf[pi] = 0;
    hb_draw_str(HB_SCREEN_W/2 - 24, PAGER_Y + 8, pbuf, 2, HB_WHITE, HB_BLACK);

    hb_draw_str(4, HB_SCREEN_H - 14,
                "tap row to play  HOME/VOL = exit",
                1, HB_RGB(0x80,0x80,0x80), HB_BLACK);
}

static void switch_source(int new_src)
{
    g_source = new_src;
    /* Lazy-scan each side on first visit. */
    if (new_src == SRC_SYS && g_n_sys_entries == 0) {
        scan_into(g_sys_names, &g_n_sys_entries, SYS_DIR_PATH, SYS_VOLUME);
    }
    if (new_src == SRC_USR && g_n_usr_entries == 0) {
        scan_into(g_usr_names, &g_n_usr_entries, USR_DIR_PATH, USR_VOLUME);
    }
    hb_paged_list_init(&g_page, n_entries(), PAGE_SIZE);
}

static int hit_row(int16_t ty)
{
    if (ty < LIST_Y) return -1;
    /* No libgcc divmod — subtract-based. */
    int dy = ty - LIST_Y;
    int row_in_page = 0;
    while (dy >= ROW_H) { dy -= ROW_H; row_in_page++; }
    if (row_in_page >= PAGE_SIZE) return -1;
    int from = hb_paged_list_first(&g_page);
    int idx  = from + row_in_page;
    if (idx >= hb_paged_list_last(&g_page)) return -1;
    return idx;
}

static void *app_main(void *arg)
{
    (void)arg;
    hb_ui_init();
    switch_source(SRC_SYS);
    redraw();

    for (uint32_t frame = 0; frame < 20000000; frame++) {
        int16_t tx, ty;
        hb_ui_event_t e = hb_ui_poll(&tx, &ty);
        if (e == HB_UI_EXIT) break;
        if (e == HB_UI_TAP) {
            if (hb_ui_button_hit(tx, ty, SYS_X, TAB_Y, TAB_W, TAB_H)) {
                switch_source(SRC_SYS); redraw();
            } else if (hb_ui_button_hit(tx, ty, USR_X, TAB_Y, TAB_W, TAB_H)) {
                switch_source(SRC_USR); redraw();
            } else if (hb_ui_button_hit(tx, ty, PREV_X, PAGER_Y,
                                        PAGER_BTN_W, PAGER_BTN_H)) {
                hb_paged_list_prev(&g_page); redraw();
            } else if (hb_ui_button_hit(tx, ty, NEXT_X, PAGER_Y,
                                        PAGER_BTN_W, PAGER_BTN_H)) {
                hb_paged_list_next(&g_page); redraw();
            } else {
                int idx = hit_row(ty);
                if (idx >= 0) {
                    int vol;
                    const char *p = entry_path(idx, &vol);
                    play_audio(p, vol);
                }
            }
        }
        hb_ui_pace();
    }

    hb_ui_done();
    return (void *)0;
}

HB_APP_ENTRY(payload_entry)
{
    static uint32_t attr[16];
    for (int i = 0; i < 16; i++) attr[i] = 0;
    attr[0] = 0x50544841u;
    attr[2] = 2;
    attr[4] = 0x10000;
    attr[6] = 1;
    attr[7] = 1;
    attr[8] = 1;
    attr[9] = 0;
    uint32_t tid = 0;
    ((pthread_create_t)PTHREAD_CREATE_ADDR)(&tid, attr, app_main, (void *)0);
}
