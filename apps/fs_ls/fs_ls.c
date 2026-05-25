/*
 * fs_ls — list contents of /iPod_Control via hb_fs_dir_* API.
 * Verifies SDK directory iteration end-to-end.
 *
 * Tap to switch between /iPod_Control and / (root). Home/vol = exit.
 */

#include "hb_sdk.h"

#define MAX_ENTRIES   24
#define NAME_BUF_LEN  96

typedef struct {
    char name[NAME_BUF_LEN];
    bool is_dir;
} entry_t;

static entry_t g_entries[MAX_ENTRIES];
static int     g_n_entries;

static const char *paths[] = {
    "/iPod_Control",
    "/",
    "/iPod_Control/iTunes",
    "/iPod_Control/Music",
};
#define N_PATHS (sizeof(paths)/sizeof(paths[0]))
static int g_path_idx;

static void scan(const char *path)
{
    g_n_entries = 0;
    hb_dir_t d;
    if (!hb_fs_dir_open(&d, path, false)) return;
    bool is_dir;
    while (g_n_entries < MAX_ENTRIES &&
           hb_fs_dir_next(&d, g_entries[g_n_entries].name, NAME_BUF_LEN,
                          &is_dir)) {
        g_entries[g_n_entries].is_dir = is_dir;
        g_n_entries++;
    }
    hb_fs_dir_close(&d);
}

static void redraw(void)
{
    hb_fill_screen(HB_BLACK);
    hb_draw_str(4, 0, "FS LS", 2, HB_YELLOW, HB_BLACK);
    hb_draw_str(4, 22, paths[g_path_idx], 1, HB_CYAN, HB_BLACK);

    char buf[16];
    int n = g_n_entries;
    int i, ix = 0;
    static const char *prefix = "entries: ";
    while (prefix[ix]) { buf[ix] = prefix[ix]; ix++; }
    static const uint32_t pow10[3] = {100, 10, 1};
    uint32_t v = (uint32_t)n;
    for (int j = 0; j < 3; j++) {
        char d = '0';
        while (v >= pow10[j]) { v -= pow10[j]; d++; }
        buf[ix++] = d;
    }
    buf[ix] = 0;
    hb_draw_str(4, 36, buf, 1, HB_WHITE, HB_BLACK);

    int y = 56;
    for (i = 0; i < g_n_entries && y < HB_SCREEN_H - 16; i++) {
        hb_color_t fg = g_entries[i].is_dir ? HB_GREEN : HB_WHITE;
        hb_draw_str(4, y, g_entries[i].name, 1, fg, HB_BLACK);
        y += 12;
    }

    hb_draw_str(4, HB_SCREEN_H - 14, "TAP: next dir  HOME: exit",
                1, HB_RGB(0x80,0x80,0x80), HB_BLACK);
}

HB_APP_ENTRY(payload_entry)
{
    hb_ui_init();
    g_path_idx = 0;
    scan(paths[g_path_idx]);
    redraw();

    for (uint32_t frame = 0; frame < 20000000; frame++) {
        int16_t tx, ty;
        hb_ui_event_t e = hb_ui_poll(&tx, &ty);
        if (e == HB_UI_EXIT) break;
        if (e == HB_UI_TAP) {
            g_path_idx = (g_path_idx + 1) % N_PATHS;
            scan(paths[g_path_idx]);
            redraw();
        }
        hb_ui_pace();
    }

    hb_ui_done();
}
