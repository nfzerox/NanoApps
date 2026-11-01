/*
 * crosswords — solve a custom crossword puzzle.
 *
 * Puzzle file format (drop at /Data/Crosswords/<name>.txt):
 *
 *   TITLE: Sample
 *   SIZE: 9          # square grid up to 13
 *   GRID:
 *   CAT##DOG##
 *   A##....###
 *   ...                (one row per line, '#' is a black square,
 *                       letters are answers)
 *   ACROSS:
 *   1 1 Feline pet
 *   1 5 Loyal pet
 *   ...                (row col clue — row/col are 1-based starting
 *                       coordinate of the word; word length is
 *                       inferred from the grid)
 *   DOWN:
 *   1 1 Auto
 *   ...
 *
 * One built-in puzzle is shipped so the app always works. Picker
 * shows built-in + every *.txt in /Data/Crosswords.
 *
 * Grid is canvas-rendered (one widget) to stay inside the LVGL
 * heap. Input keypad is a 7-column button matrix (A-Z + del).
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define MAX_SIZE       13
#define MAX_CLUES      40
#define MAX_PUZZLES    16
#define DATA_DIR       "/Data/Crosswords"
#define PLAY_W         240
#define PLAY_H         240

typedef struct {
    int  row, col;       /* 1-based start */
    int  len;
    char clue[64];
} clue_t;

typedef struct {
    char name[32];
    int  size;
    char solution[MAX_SIZE][MAX_SIZE + 1];  /* uppercase letter or '#' */
    clue_t across[MAX_CLUES];
    int    n_across;
    clue_t down[MAX_CLUES];
    int    n_down;
} puzzle_t;

static puzzle_t s_puzzle;
static char s_state[MAX_SIZE][MAX_SIZE + 1];      /* user entries */
static int  s_cur_r, s_cur_c;
static int  s_dir = 0;          /* 0=across, 1=down */
/* In the app's own arena: the old fixed 0x092E0000 lies inside the OS heap
 * and was overwriting live objects (and sometimes this app's own code). */
static uint8_t s_canvas_buf[PLAY_W * PLAY_H * 4] __attribute__((aligned(4)));

static lv_obj_t *s_canvas;
static lv_obj_t *s_clue_lbl;
static lv_obj_t *s_status_lbl;

/* ---- built-in puzzle (always available) ---- */
static const char *s_builtin_grid[] = {
    "CAT##DOG#",
    "A#R#O###A",
    "T#EAGLES#",
    "###D##A#L",
    "BIRD#KOI#",
    "I###O####",
    "T#SWAN###",
    "###I#EEL#",
    "###P#####",
};

static void load_builtin(void)
{
    int k = 0;
    const char *p = "Built-in";
    while (*p) s_puzzle.name[k++] = *p++;
    s_puzzle.name[k] = 0;
    s_puzzle.size = 9;
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) s_puzzle.solution[r][c] = s_builtin_grid[r][c];
        s_puzzle.solution[r][9] = 0;
    }
    /* clues (no need to be tight; the binary just wants something) */
    s_puzzle.n_across = 0;
    s_puzzle.across[s_puzzle.n_across++] = (clue_t){1,1,3,"Feline pet"};
    s_puzzle.across[s_puzzle.n_across++] = (clue_t){1,6,3,"Loyal pet"};
    s_puzzle.across[s_puzzle.n_across++] = (clue_t){3,3,6,"Soaring birds"};
    s_puzzle.across[s_puzzle.n_across++] = (clue_t){5,1,4,"Has wings"};
    s_puzzle.across[s_puzzle.n_across++] = (clue_t){5,6,3,"Orange pond fish"};
    s_puzzle.across[s_puzzle.n_across++] = (clue_t){7,3,4,"White water bird"};
    s_puzzle.across[s_puzzle.n_across++] = (clue_t){8,6,3,"Slithery fish"};
    s_puzzle.n_down = 0;
    s_puzzle.down[s_puzzle.n_down++] = (clue_t){1,1,3,"Feline pet"};
    s_puzzle.down[s_puzzle.n_down++] = (clue_t){1,5,3,"Hot drink (3, sing.)"};
    s_puzzle.down[s_puzzle.n_down++] = (clue_t){1,9,2,"Affirmative"};
    s_puzzle.down[s_puzzle.n_down++] = (clue_t){2,3,2,"Reddish color"};
    s_puzzle.down[s_puzzle.n_down++] = (clue_t){3,8,2,"Lance"};
    s_puzzle.down[s_puzzle.n_down++] = (clue_t){5,1,4,"Small piece"};
    s_puzzle.down[s_puzzle.n_down++] = (clue_t){7,3,3,"Drink loudly"};
}

/* ---- file parser ---- */
static int starts_with(const char *s, const char *pfx)
{
    while (*pfx) { if (*s++ != *pfx++) return 0; } return 1;
}
static int read_int(const char *s, int *end_idx)
{
    int v = 0; int i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    *end_idx = i; return v;
}

static int parse_puzzle(const char *blob, int n, puzzle_t *out)
{
    out->n_across = 0; out->n_down = 0; out->size = 0;
    out->name[0] = 0;
    int section = 0;  /* 0 none, 1 grid, 2 across, 3 down */
    int row_idx = 0;
    int i = 0;
    while (i < n) {
        int j = i;
        while (j < n && blob[j] != '\n' && blob[j] != '\r') j++;
        const char *line = blob + i;
        int len = j - i;
        /* trim */
        while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t')) len--;
        if (len > 0) {
            char buf[160];
            int cp = len < 159 ? len : 159;
            for (int k = 0; k < cp; k++) buf[k] = line[k];
            buf[cp] = 0;
            if (starts_with(buf, "TITLE:")) {
                const char *p = buf + 6; while (*p == ' ') p++;
                int k = 0; while (*p && k < 31) out->name[k++] = *p++;
                out->name[k] = 0;
            } else if (starts_with(buf, "SIZE:")) {
                int e; out->size = read_int(buf + 5, &e);
                if (out->size > MAX_SIZE) out->size = MAX_SIZE;
            } else if (starts_with(buf, "GRID:")) {
                section = 1; row_idx = 0;
            } else if (starts_with(buf, "ACROSS:")) {
                section = 2;
            } else if (starts_with(buf, "DOWN:")) {
                section = 3;
            } else if (section == 1 && row_idx < out->size) {
                for (int c = 0; c < out->size; c++) {
                    char ch = (c < cp) ? buf[c] : '#';
                    if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
                    if (!(ch >= 'A' && ch <= 'Z') && ch != '#') ch = '#';
                    out->solution[row_idx][c] = ch;
                }
                out->solution[row_idx][out->size] = 0;
                row_idx++;
            } else if (section == 2 || section == 3) {
                int e; int r = read_int(buf, &e);
                int e2; int c = read_int(buf + e, &e2);
                e += e2;
                while (buf[e] == ' ' || buf[e] == '\t') e++;
                clue_t cl; cl.row = r; cl.col = c; cl.len = 0;
                int k = 0; while (buf[e] && k < 63) cl.clue[k++] = buf[e++];
                cl.clue[k] = 0;
                if (r < 1 || c < 1 || r > out->size || c > out->size) goto next_line;
                /* compute length from grid */
                if (section == 2) {
                    int cc = c - 1;
                    while (cc < out->size && out->solution[r-1][cc] != '#') { cl.len++; cc++; }
                    if (out->n_across < MAX_CLUES) out->across[out->n_across++] = cl;
                } else {
                    int rr = r - 1;
                    while (rr < out->size && out->solution[rr][c-1] != '#') { cl.len++; rr++; }
                    if (out->n_down < MAX_CLUES) out->down[out->n_down++] = cl;
                }
            }
        }
next_line:
        i = j;
        while (i < n && (blob[i] == '\n' || blob[i] == '\r')) i++;
    }
    return (out->size > 0 && (out->n_across + out->n_down) > 0);
}

static int try_load_file(const char *path)
{
    uint32_t sz = hb_fs_size(path);
    if (sz == 0 || sz > 16384) return 0;
    static char buf[16384];
    uint32_t n = hb_fs_read(path, buf, sizeof(buf) - 1);
    if (n == 0) return 0;
    return parse_puzzle(buf, (int)n, &s_puzzle);
}

/* ---- state ---- */
static void reset_state(void)
{
    for (int r = 0; r < s_puzzle.size; r++) {
        for (int c = 0; c < s_puzzle.size; c++) {
            s_state[r][c] = (s_puzzle.solution[r][c] == '#') ? '#' : 0;
        }
        s_state[r][s_puzzle.size] = 0;
    }
    s_cur_r = 0; s_cur_c = 0;
    for (int r = 0; r < s_puzzle.size; r++)
        for (int c = 0; c < s_puzzle.size; c++)
            if (s_puzzle.solution[r][c] != '#') { s_cur_r = r; s_cur_c = c; goto done; }
done:;
}

/* ---- numbering: a cell starts a word if (top is black|out) for down, or (left is black|out) for across */
static int cell_number(int r, int c)
{
    int n = 1;
    for (int rr = 0; rr < s_puzzle.size; rr++) {
        for (int cc = 0; cc < s_puzzle.size; cc++) {
            if (s_puzzle.solution[rr][cc] == '#') continue;
            int starts_across = (cc == 0 || s_puzzle.solution[rr][cc-1] == '#');
            int starts_down   = (rr == 0 || s_puzzle.solution[rr-1][cc] == '#');
            if (starts_across || starts_down) {
                if (rr == r && cc == c) return n;
                n++;
            }
        }
    }
    return 0;
}

/* ---- rendering ---- */
static uint32_t pack(uint32_t hex) { return hex & 0xFFFFFF; }

static void cfill(int x, int y, int w, int h, uint32_t color)
{
    uint32_t *buf = (uint32_t *)s_canvas_buf;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= PLAY_H) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= PLAY_W) continue;
            buf[yy * PLAY_W + xx] = color;
        }
    }
}
/* Tiny 3x5 bitmap font for cell labels (numbers + letters). */
static void cputc(int x, int y, char ch, uint32_t color)
{
    static const uint8_t glyphs[37][5] = {
        {0x7E,0x81,0x81,0x81,0x7E}, /* 0 */
        {0x00,0x82,0xFF,0x80,0x00}, /* 1 */
        {0xC2,0xA1,0x91,0x89,0x86}, /* 2 */
        {0x42,0x81,0x89,0x89,0x76}, /* 3 */
        {0x18,0x14,0x12,0xFF,0x10}, /* 4 */
        {0x4F,0x89,0x89,0x89,0x71}, /* 5 */
        {0x7E,0x89,0x89,0x89,0x72}, /* 6 */
        {0x01,0xE1,0x11,0x09,0x07}, /* 7 */
        {0x76,0x89,0x89,0x89,0x76}, /* 8 */
        {0x4E,0x91,0x91,0x91,0x7E}, /* 9 */
        {0xFE,0x11,0x11,0x11,0xFE}, /* A */
        {0xFF,0x89,0x89,0x89,0x76}, /* B */
        {0x7E,0x81,0x81,0x81,0x42}, /* C */
        {0xFF,0x81,0x81,0x81,0x7E}, /* D */
        {0xFF,0x89,0x89,0x89,0x81}, /* E */
        {0xFF,0x09,0x09,0x09,0x01}, /* F */
        {0x7E,0x81,0x81,0x91,0x72}, /* G */
        {0xFF,0x08,0x08,0x08,0xFF}, /* H */
        {0x81,0x81,0xFF,0x81,0x81}, /* I */
        {0x40,0x80,0x80,0x80,0x7F}, /* J */
        {0xFF,0x08,0x14,0x22,0xC1}, /* K */
        {0xFF,0x80,0x80,0x80,0x80}, /* L */
        {0xFF,0x02,0x0C,0x02,0xFF}, /* M */
        {0xFF,0x02,0x0C,0x30,0xFF}, /* N */
        {0x7E,0x81,0x81,0x81,0x7E}, /* O */
        {0xFF,0x11,0x11,0x11,0x0E}, /* P */
        {0x7E,0x81,0xA1,0x41,0xBE}, /* Q */
        {0xFF,0x09,0x19,0x29,0xC6}, /* R */
        {0x46,0x89,0x89,0x89,0x72}, /* S */
        {0x01,0x01,0xFF,0x01,0x01}, /* T */
        {0x7F,0x80,0x80,0x80,0x7F}, /* U */
        {0x1F,0x60,0x80,0x60,0x1F}, /* V */
        {0xFF,0x40,0x30,0x40,0xFF}, /* W */
        {0xC3,0x24,0x18,0x24,0xC3}, /* X */
        {0x03,0x04,0xF8,0x04,0x03}, /* Y */
        {0xC1,0xA1,0x99,0x85,0x83}, /* Z */
        {0x00,0x00,0x00,0x00,0x00}, /* space */
    };
    int idx = 36;
    if (ch >= '0' && ch <= '9') idx = ch - '0';
    else if (ch >= 'A' && ch <= 'Z') idx = ch - 'A' + 10;
    else if (ch >= 'a' && ch <= 'z') idx = ch - 'a' + 10;
    else return;
    for (int col = 0; col < 5; col++) {
        uint8_t row = glyphs[idx][col];
        for (int bit = 0; bit < 8; bit++) {
            if (row & (1 << bit)) {
                int xx = x + col, yy = y + bit;
                if (xx >= 0 && xx < PLAY_W && yy >= 0 && yy < PLAY_H)
                    ((uint32_t *)s_canvas_buf)[yy * PLAY_W + xx] = color;
            }
        }
    }
}
static void cputs(int x, int y, const char *s, uint32_t color)
{
    while (*s) { cputc(x, y, *s++, color); x += 6; }
}

/* big letter (4x scale, 32x32 effective) */
static void cputc_big(int x, int y, char ch, uint32_t color)
{
    /* render via cputc multi-time, scaled 3x */
    if (ch == 0) return;
    if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
    static const uint8_t glyphs[26][5] = {
        {0xFE,0x11,0x11,0x11,0xFE},{0xFF,0x89,0x89,0x89,0x76},{0x7E,0x81,0x81,0x81,0x42},
        {0xFF,0x81,0x81,0x81,0x7E},{0xFF,0x89,0x89,0x89,0x81},{0xFF,0x09,0x09,0x09,0x01},
        {0x7E,0x81,0x81,0x91,0x72},{0xFF,0x08,0x08,0x08,0xFF},{0x81,0x81,0xFF,0x81,0x81},
        {0x40,0x80,0x80,0x80,0x7F},{0xFF,0x08,0x14,0x22,0xC1},{0xFF,0x80,0x80,0x80,0x80},
        {0xFF,0x02,0x0C,0x02,0xFF},{0xFF,0x02,0x0C,0x30,0xFF},{0x7E,0x81,0x81,0x81,0x7E},
        {0xFF,0x11,0x11,0x11,0x0E},{0x7E,0x81,0xA1,0x41,0xBE},{0xFF,0x09,0x19,0x29,0xC6},
        {0x46,0x89,0x89,0x89,0x72},{0x01,0x01,0xFF,0x01,0x01},{0x7F,0x80,0x80,0x80,0x7F},
        {0x1F,0x60,0x80,0x60,0x1F},{0xFF,0x40,0x30,0x40,0xFF},{0xC3,0x24,0x18,0x24,0xC3},
        {0x03,0x04,0xF8,0x04,0x03},{0xC1,0xA1,0x99,0x85,0x83},
    };
    if (!(ch >= 'A' && ch <= 'Z')) return;
    int idx = ch - 'A';
    int scale = 2;
    for (int col = 0; col < 5; col++) {
        uint8_t row = glyphs[idx][col];
        for (int bit = 0; bit < 8; bit++) {
            if (row & (1 << bit)) {
                cfill(x + col * scale, y + bit * scale, scale, scale, color);
            }
        }
    }
}

static int cell_px(void)
{
    /* fit grid into PLAY_W minus a small margin */
    int avail = PLAY_W - 8;
    return avail / s_puzzle.size;
}

static void render(void)
{
    cfill(0, 0, PLAY_W, PLAY_H, pack(0x1a1f2e));
    int cp = cell_px();
    int gx = (PLAY_W - cp * s_puzzle.size) / 2;
    int gy = 4;
    for (int r = 0; r < s_puzzle.size; r++) {
        for (int c = 0; c < s_puzzle.size; c++) {
            int x = gx + c * cp, y = gy + r * cp;
            if (s_puzzle.solution[r][c] == '#') {
                cfill(x, y, cp, cp, pack(0x000000));
            } else {
                uint32_t bg = pack(0xfafafa);
                if (r == s_cur_r && c == s_cur_c) bg = pack(0xffec99);
                else if ((s_dir == 0 && r == s_cur_r) || (s_dir == 1 && c == s_cur_c)) bg = pack(0xfff3bf);
                cfill(x, y, cp, cp, bg);
                /* border */
                cfill(x, y, cp, 1, pack(0x444444));
                cfill(x, y + cp - 1, cp, 1, pack(0x444444));
                cfill(x, y, 1, cp, pack(0x444444));
                cfill(x + cp - 1, y, 1, cp, pack(0x444444));
                /* number badge */
                int num = cell_number(r, c);
                if (num) {
                    char b[4];
                    if (num < 10) { b[0] = '0' + num; b[1] = 0; }
                    else { b[0] = '0' + num / 10; b[1] = '0' + num % 10; b[2] = 0; }
                    cputs(x + 2, y + 2, b, pack(0x555555));
                }
                /* letter */
                if (s_state[r][c] && s_state[r][c] != '#') {
                    cputc_big(x + cp/2 - 5, y + cp/2 - 8, s_state[r][c], pack(0x000000));
                }
            }
        }
    }
    lv_obj_invalidate(s_canvas);
}

static const clue_t *find_clue(int dir, int r, int c)
{
    /* walk back along direction to find start, then match clue */
    if (dir == 0) {
        while (c > 0 && s_puzzle.solution[r][c-1] != '#') c--;
        for (int i = 0; i < s_puzzle.n_across; i++)
            if (s_puzzle.across[i].row == r + 1 && s_puzzle.across[i].col == c + 1)
                return &s_puzzle.across[i];
    } else {
        while (r > 0 && s_puzzle.solution[r-1][c] != '#') r--;
        for (int i = 0; i < s_puzzle.n_down; i++)
            if (s_puzzle.down[i].row == r + 1 && s_puzzle.down[i].col == c + 1)
                return &s_puzzle.down[i];
    }
    return NULL;
}

static void refresh_clue(void)
{
    const clue_t *cl = find_clue(s_dir, s_cur_r, s_cur_c);
    if (!cl) {
        /* fall back to other direction */
        cl = find_clue(1 - s_dir, s_cur_r, s_cur_c);
        if (cl) s_dir = 1 - s_dir;
    }
    char buf[96];
    int k = 0;
    const char *p = s_dir == 0 ? "Across: " : "Down: ";
    while (*p) buf[k++] = *p++;
    if (cl) { p = cl->clue; while (*p && k < 90) buf[k++] = *p++; }
    else { p = "(no clue)"; while (*p) buf[k++] = *p++; }
    buf[k] = 0;
    lv_label_set_text(s_clue_lbl, buf);
}

static void advance_cursor(void)
{
    if (s_dir == 0) {
        if (s_cur_c + 1 < s_puzzle.size && s_puzzle.solution[s_cur_r][s_cur_c + 1] != '#')
            s_cur_c++;
    } else {
        if (s_cur_r + 1 < s_puzzle.size && s_puzzle.solution[s_cur_r + 1][s_cur_c] != '#')
            s_cur_r++;
    }
}

static int is_solved(void)
{
    for (int r = 0; r < s_puzzle.size; r++)
        for (int c = 0; c < s_puzzle.size; c++)
            if (s_puzzle.solution[r][c] != '#' && s_state[r][c] != s_puzzle.solution[r][c])
                return 0;
    return 1;
}

static void on_canvas_click(lv_event_t *e)
{
    (void)e;
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    lv_area_t a; lv_obj_get_coords(s_canvas, &a);
    int rx = p.x - a.x1, ry = p.y - a.y1;
    int cp = cell_px();
    int gx = (PLAY_W - cp * s_puzzle.size) / 2;
    int gy = 4;
    int c = (rx - gx) / cp;
    int r = (ry - gy) / cp;
    if (r < 0 || r >= s_puzzle.size || c < 0 || c >= s_puzzle.size) return;
    if (s_puzzle.solution[r][c] == '#') return;
    if (r == s_cur_r && c == s_cur_c) s_dir = 1 - s_dir;
    s_cur_r = r; s_cur_c = c;
    refresh_clue();
    render();
}

static const char *s_kb_map[] = {
    "A","B","C","D","E","F","G","\n",
    "H","I","J","K","L","M","N","\n",
    "O","P","Q","R","S","T","U","\n",
    "V","W","X","Y","Z",LV_SYMBOL_BACKSPACE,LV_SYMBOL_OK,""
};

static void on_key(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    const char *txt = lv_buttonmatrix_get_button_text(bm, lv_buttonmatrix_get_selected_button(bm));
    if (!txt) return;
    if (txt[0] >= 'A' && txt[0] <= 'Z' && txt[1] == 0) {
        s_state[s_cur_r][s_cur_c] = txt[0];
        advance_cursor();
    } else if (txt[0] == LV_SYMBOL_BACKSPACE[0]) {
        s_state[s_cur_r][s_cur_c] = 0;
        if (s_dir == 0 && s_cur_c > 0 && s_puzzle.solution[s_cur_r][s_cur_c-1] != '#') s_cur_c--;
        else if (s_dir == 1 && s_cur_r > 0 && s_puzzle.solution[s_cur_r-1][s_cur_c] != '#') s_cur_r--;
    } else if (txt[0] == LV_SYMBOL_OK[0]) {
        if (is_solved()) lv_label_set_text(s_status_lbl, "Solved!");
        else lv_label_set_text(s_status_lbl, "Keep going.");
        render();
        return;
    }
    refresh_clue();
    render();
}

/* ---- puzzle picker ---- */
static char s_files[MAX_PUZZLES][80];
static int  s_n_files;

static void scan_dir(void)
{
    s_n_files = 0;
    hb_dir_t d; if (!hb_fs_dir_open(&d, DATA_DIR, false)) return;
    char name[80]; bool is_dir;
    while (hb_fs_dir_next(&d, name, sizeof(name), &is_dir)) {
        if (is_dir) continue;
        int n = 0; while (name[n]) n++;
        if (n > 4 && name[n-4] == '.' && (name[n-3] == 't' || name[n-3] == 'T')) {
            if (s_n_files < MAX_PUZZLES) {
                int k = 0; while (name[k] && k < 79) { s_files[s_n_files][k] = name[k]; k++; }
                s_files[s_n_files][k] = 0;
                s_n_files++;
            }
        }
    }
    hb_fs_dir_close(&d);
}

static void start_puzzle(void)
{
    reset_state();
    refresh_clue();
    lv_label_set_text(s_status_lbl, s_puzzle.name);
    render();
}

static lv_obj_t *s_picker_scr;
static lv_obj_t *s_game_scr;
static void build_game(void);

static void on_pick(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == -1) {
        load_builtin();
    } else {
        char path[160]; int k = 0;
        const char *p = DATA_DIR "/"; while (*p) path[k++] = *p++;
        const char *fn = s_files[idx]; while (*fn) path[k++] = *fn++;
        path[k] = 0;
        if (!try_load_file(path)) load_builtin();
    }
    build_game();
    start_puzzle();
}

static void build_picker(void)
{
    s_picker_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_picker_scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_pad_all(s_picker_scr, 8, 0);
    lv_obj_t *title = lv_label_create(s_picker_scr);
    lv_label_set_text(title, "Crosswords");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *list = lv_list_create(s_picker_scr);
    lv_obj_set_size(list, 224, 340);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *btn = lv_list_add_button(list, NULL, "Built-in");
    lv_obj_add_event_cb(btn, on_pick, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    for (int i = 0; i < s_n_files; i++) {
        btn = lv_list_add_button(list, NULL, s_files[i]);
        lv_obj_add_event_cb(btn, on_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_screen_load(s_picker_scr);
}

static void build_game(void)
{
    s_game_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_game_scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_pad_all(s_game_scr, 0, 0);

    s_status_lbl = lv_label_create(s_game_scr);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 2);

    s_canvas = lv_canvas_create(s_game_scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, PLAY_W, PLAY_H,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(s_canvas, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_canvas_click, LV_EVENT_CLICKED, NULL);

    s_clue_lbl = lv_label_create(s_game_scr);
    lv_label_set_text(s_clue_lbl, "");
    lv_obj_set_width(s_clue_lbl, 236);
    lv_obj_set_style_text_color(s_clue_lbl, lv_color_hex(0xe0e6ef), 0);
    lv_label_set_long_mode(s_clue_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_clue_lbl, LV_ALIGN_TOP_MID, 0, 22 + PLAY_H + 2);

    lv_obj_t *kb = lv_buttonmatrix_create(s_game_scr);
    lv_buttonmatrix_set_map(kb, s_kb_map);
    lv_obj_set_size(kb, 240, 92);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb, on_key, LV_EVENT_VALUE_CHANGED, NULL);

    lv_screen_load(s_game_scr);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    scan_dir();
    build_picker();
}
