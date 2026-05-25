/*
 * notes — nav-controller notes app.
 *   List view:    10 slots; tap a row to drill in.
 *   Editor view:  BACK (top-left) | "Note N" (center) | SAVE (top-right)
 *                 text editor + on-screen QWERTY keyboard.
 *
 * 10 fixed slots, files at /iPod_Control/hbnote0..9.txt.
 * Vol button = exit at any time.
 *
 * Deploy after `tools/eject.sh`.
 */

#include "hb_sdk.h"

/* ---- Colors ---- */
#define BG           HB_BLACK
#define TITLE_FG     HB_YELLOW
#define ROW_BG_EMPTY HB_RGB(0x18, 0x18, 0x20)
#define ROW_BG_FILLED HB_RGB(0x08, 0x30, 0x08)
#define ROW_SEP      HB_RGB(0x40, 0x40, 0x40)
#define BACK_BG      HB_RGB(0x30, 0x30, 0x80)
#define SAVE_BG      HB_RGB(0x08, 0x60, 0x08)
#define HEADER_BG    HB_RGB(0x18, 0x18, 0x28)

/* ---- Layout ---- */
#define N_SLOTS         10

/* List view: row per slot, 36 px tall */
#define LIST_HEADER_H   28
#define LIST_ROW_H      36

/* Editor view */
#define ED_HEADER_H     32
#define ED_BTN_W        64
#define ED_EDITOR_Y     (ED_HEADER_H + 4)
#define ED_EDITOR_H     180
#define ED_KB_Y         (ED_EDITOR_Y + ED_EDITOR_H + 6)
#define ED_KB_CELL_W    (HB_SCREEN_W / HB_KB_COLS)
#define ED_KB_CELL_H    45

#define EDITOR_CHAR_W   8
#define EDITOR_CHAR_H   8
#define EDITOR_PAD      4
#define EDITOR_COLS     ((HB_SCREEN_W - 2 * EDITOR_PAD) / EDITOR_CHAR_W)
#define EDITOR_ROWS     (ED_EDITOR_H / EDITOR_CHAR_H - 1)

#define MAX_NOTE        256
#define PREVIEW_LEN     28

/* ---- State ---- */
typedef enum { VIEW_LIST, VIEW_EDITOR } view_t;

static view_t   g_view;
static char     g_editor[MAX_NOTE];
static uint16_t g_editor_len;
static int      g_current_slot;
static bool     g_slot_has_data[N_SLOTS];
static char     g_slot_path[N_SLOTS][32];
/* First PREVIEW_LEN chars of each slot, refreshed at startup +
   after each save, so the list view shows previews. */
static char     g_slot_preview[N_SLOTS][PREVIEW_LEN + 1];
static hb_kb_t  g_kb;

/* ---- Slot file helpers ---- */
static void build_slot_paths(void)
{
    const char *prefix = "/iPod_Control/hbnote";
    for (int s = 0; s < N_SLOTS; s++) {
        int i = 0;
        while (prefix[i]) { g_slot_path[s][i] = prefix[i]; i++; }
        g_slot_path[s][i++] = (char)('0' + s);
        g_slot_path[s][i++] = '.';
        g_slot_path[s][i++] = 't';
        g_slot_path[s][i++] = 'x';
        g_slot_path[s][i++] = 't';
        g_slot_path[s][i] = '\0';
    }
}

static void refresh_slot_preview(int s)
{
    for (int i = 0; i <= PREVIEW_LEN; i++) g_slot_preview[s][i] = 0;
    if (!g_slot_has_data[s]) return;
    char buf[PREVIEW_LEN + 1];
    for (int i = 0; i <= PREVIEW_LEN; i++) buf[i] = 0;
    uint32_t n = hb_fs_read(g_slot_path[s], buf, PREVIEW_LEN);
    for (uint32_t i = 0; i < n && i < PREVIEW_LEN; i++) {
        char ch = buf[i];
        if (ch == '\n' || ch == '\r') ch = ' ';
        if ((uint8_t)ch < 0x20 || (uint8_t)ch >= 0x7F) ch = '.';
        g_slot_preview[s][i] = ch;
    }
}

static void load_slot_into_editor(int s)
{
    g_current_slot = s;
    for (uint16_t i = 0; i < MAX_NOTE; i++) g_editor[i] = 0;
    g_editor_len = 0;
    if (g_slot_has_data[s]) {
        uint32_t n = hb_fs_read(g_slot_path[s], g_editor, MAX_NOTE - 1);
        if (n > 0 && n < MAX_NOTE) {
            g_editor_len = (uint16_t)n;
            g_editor[g_editor_len] = '\0';
        }
    }
}

static void save_current_slot(void)
{
    bool ok = hb_fs_write(g_slot_path[g_current_slot],
                          g_editor, g_editor_len);
    if (ok) {
        g_slot_has_data[g_current_slot] = true;
        refresh_slot_preview(g_current_slot);
    }
}

/* ---- List view ---- */
static int list_row_at(int16_t y)
{
    if (y < LIST_HEADER_H) return -1;
    int row = 0;
    int16_t yy = y - LIST_HEADER_H;
    while (row < N_SLOTS && yy >= LIST_ROW_H) { row++; yy -= LIST_ROW_H; }
    if (row >= N_SLOTS) return -1;
    return row;
}

static void draw_list_row(int s)
{
    int16_t y = LIST_HEADER_H + s * LIST_ROW_H;
    hb_color_t bg = g_slot_has_data[s] ? ROW_BG_FILLED : ROW_BG_EMPTY;
    hb_fill_rect(0, y, HB_SCREEN_W, LIST_ROW_H - 1, bg);
    hb_fill_rect(0, y + LIST_ROW_H - 1, HB_SCREEN_W, 1, ROW_SEP);

    /* Slot number "N." */
    char num[3] = { (char)('0' + s), '.', '\0' };
    hb_draw_str(8, y + 10, num, 2, HB_WHITE, bg);

    /* Preview text on the right */
    const char *prev = g_slot_has_data[s] ? g_slot_preview[s] : "(empty)";
    hb_draw_str(56, y + 14, prev, 1, HB_RGB(0xC0, 0xC0, 0xC0), bg);
}

static void draw_list_view(void)
{
    hb_fill_screen(BG);
    hb_fill_rect(0, 0, HB_SCREEN_W, LIST_HEADER_H, HEADER_BG);
    hb_draw_str(8, 6, "HBNOTES", 2, TITLE_FG, HEADER_BG);
    for (int s = 0; s < N_SLOTS; s++) draw_list_row(s);
}

/* ---- Editor view ---- */
static void draw_editor_text(void)
{
    hb_fill_rect(0, ED_EDITOR_Y, HB_SCREEN_W, ED_EDITOR_H, BG);
    hb_fill_rect(0, ED_EDITOR_Y, HB_SCREEN_W, 1, HB_WHITE);
    hb_fill_rect(0, ED_EDITOR_Y + ED_EDITOR_H - 1, HB_SCREEN_W, 1, HB_WHITE);

    int row = 0, col = 0;
    char line[40];
    for (uint16_t i = 0; i < g_editor_len; i++) {
        char ch = g_editor[i];
        if (ch == '\n' || col >= EDITOR_COLS - 1) {
            line[col] = '\0';
            hb_draw_str(EDITOR_PAD,
                        ED_EDITOR_Y + EDITOR_PAD + row * EDITOR_CHAR_H,
                        line, 1, HB_WHITE, BG);
            row++; col = 0;
            if (row >= EDITOR_ROWS) break;
            if (ch == '\n') continue;
        }
        line[col++] = ch;
    }
    if (col > 0 && row < EDITOR_ROWS) {
        line[col] = '\0';
        hb_draw_str(EDITOR_PAD,
                    ED_EDITOR_Y + EDITOR_PAD + row * EDITOR_CHAR_H,
                    line, 1, HB_WHITE, BG);
    }
    if (row < EDITOR_ROWS) {
        int16_t cx = EDITOR_PAD + col * EDITOR_CHAR_W;
        int16_t cy = ED_EDITOR_Y + EDITOR_PAD + row * EDITOR_CHAR_H;
        hb_fill_rect(cx, cy, 2, EDITOR_CHAR_H, HB_CYAN);
    }
}

static void draw_editor_header(void)
{
    hb_fill_rect(0, 0, HB_SCREEN_W, ED_HEADER_H, HEADER_BG);
    /* BACK button (top-left). */
    hb_fill_rect(0, 0, ED_BTN_W, ED_HEADER_H, BACK_BG);
    hb_draw_str(8, 8, "BACK", 2, HB_WHITE, BACK_BG);
    /* SAVE button (top-right). */
    hb_fill_rect(HB_SCREEN_W - ED_BTN_W, 0, ED_BTN_W, ED_HEADER_H, SAVE_BG);
    hb_draw_str(HB_SCREEN_W - ED_BTN_W + 8, 8, "SAVE", 2, HB_WHITE, SAVE_BG);
    /* Title centered. */
    char title[8] = { 'N', 'o', 't', 'e', ' ',
                      (char)('0' + g_current_slot), '\0', 0 };
    int16_t tx = (HB_SCREEN_W - 6 * 16) / 2;
    hb_draw_str(tx, 8, title, 2, TITLE_FG, HEADER_BG);
}

static void draw_editor_view(void)
{
    hb_fill_screen(BG);
    draw_editor_header();
    draw_editor_text();
    hb_kb_draw(&g_kb);
}

/* Hit-test for the editor header BACK/SAVE buttons. Returns:
       -1 = no header hit
        0 = BACK
        1 = SAVE */
static int editor_header_hit(int16_t x, int16_t y)
{
    if (y >= ED_HEADER_H) return -1;
    if (x < ED_BTN_W) return 0;
    if (x >= HB_SCREEN_W - ED_BTN_W) return 1;
    return -1;
}

/* ---- App entry ---- */
HB_APP_ENTRY(payload_entry)
{
    hb_ui_init();
    build_slot_paths();

    for (int s = 0; s < N_SLOTS; s++) {
        g_slot_has_data[s] = hb_fs_exists(g_slot_path[s]);
        if (g_slot_has_data[s]) refresh_slot_preview(s);
    }

    g_kb = hb_kb_qwerty(0, ED_KB_Y, ED_KB_CELL_W, ED_KB_CELL_H);

    g_view = VIEW_LIST;
    draw_list_view();

    for (uint32_t frame = 0; frame < 20000000; frame++) {
        int16_t tx, ty;
        hb_ui_event_t e = hb_ui_poll(&tx, &ty);
        if (e == HB_UI_VOL_EXIT) {
            /* Auto-save current note on exit if we're editing it. */
            if (g_view == VIEW_EDITOR) save_current_slot();
            break;
        }

        if (e == HB_UI_TAP) {
            if (g_view == VIEW_LIST) {
                int s = list_row_at(ty);
                if (s >= 0) {
                    load_slot_into_editor(s);
                    g_view = VIEW_EDITOR;
                    draw_editor_view();
                }
            } else {
                int hdr = editor_header_hit(tx, ty);
                if (hdr == 0) {
                    /* BACK — auto-save before returning to list. */
                    save_current_slot();
                    g_view = VIEW_LIST;
                    draw_list_view();
                } else if (hdr == 1) {
                    /* SAVE */
                    save_current_slot();
                    /* brief visual flash */
                    hb_fill_rect(HB_SCREEN_W - ED_BTN_W, 0,
                                 ED_BTN_W, ED_HEADER_H, HB_WHITE);
                    for (volatile uint32_t i = 0; i < 200000; i++) { }
                    hb_fill_rect(HB_SCREEN_W - ED_BTN_W, 0,
                                 ED_BTN_W, ED_HEADER_H, SAVE_BG);
                    hb_draw_str(HB_SCREEN_W - ED_BTN_W + 8, 8, "SAVE", 2,
                                HB_WHITE, SAVE_BG);
                } else {
                    char ch = hb_kb_hit(&g_kb, tx, ty);
                    bool dirty = false;
                    if (ch == HB_KB_DEL) {
                        if (g_editor_len > 0) {
                            g_editor_len--;
                            g_editor[g_editor_len] = '\0';
                            dirty = true;
                        }
                    } else if (ch == HB_KB_SAVE) {
                        save_current_slot();
                    } else if (ch == HB_KB_SPACE) {
                        if (g_editor_len < MAX_NOTE - 1) {
                            g_editor[g_editor_len++] = ' ';
                            g_editor[g_editor_len] = '\0';
                            dirty = true;
                        }
                    } else if (ch != 0 && g_editor_len < MAX_NOTE - 1) {
                        g_editor[g_editor_len++] = ch;
                        g_editor[g_editor_len] = '\0';
                        dirty = true;
                    }
                    if (dirty) draw_editor_text();
                }
            }
        }

        hb_ui_pace();
    }

    hb_ui_done();
}
