/*
 * paint — fast finger painting, drawn straight into the OS-composited surface
 * framebuffer (RAW_SURFACE: no LVGL). Brush strokes are direct memory writes, so
 * there's zero render-pipeline lag — the same speed as the old direct-MIPI paint,
 * but as a real home-screen app (composited, real touch, Silver lifecycle).
 *
 * Two views: a LIBRARY grid of saved drawings (+ a New tile) and the EDITOR
 * (canvas + 8-color palette + clear/fill/back). The canvas is a 240x320 region
 * of the framebuffer — contiguous (fb stride == canvas width), so load/save is a
 * single block read/write.
 *
 * Storage (/Data/Paint):
 *   <id>.bin   full canvas, 240x320 XRGB8888 (307200 B)
 *   <id>.tmb   thumbnail, 48x64 XRGB8888 (5x downsample, 12288 B)
 */
#include "hb_raw_surface.h"
#include "hb_sdk.h"

/* ---- layout (framebuffer is 240x432) ---- */
#define FB_W        240
#define TOP_Y       0
#define TOP_H       40
#define CANVAS_Y    40
#define CANVAS_W    240
#define CANVAS_H    360
#define PAL_Y       400
#define PAL_H       32                   /* short palette strip                  */
#define PAL_N       8
#define PAL_CW      (CANVAS_W / PAL_N)   /* 30 */

#define BRUSH_R     2

#define TW          48                   /* thumbnail = 5x downsample of canvas  */
#define TH          72
#define THUMB_BYTES (TW * TH * 4)
#define CANVAS_BYTES (CANVAS_W * CANVAS_H * 4)

#define DATA_DIR    "/Data/Paint"
#define MAX_DRAWINGS 60

static const uint32_t PALETTE[PAL_N] = {
    0x000000, 0xffffff, 0xe63946, 0xf4a261,
    0xffd60a, 0x2a9d8f, 0x0a84ff, 0xc468ee,
};

/* top-bar zones (4 x 60): library | swatch | clear | fill */
#define Z_LIB    0
#define Z_SWATCH 1
#define Z_CLEAR  2
#define Z_FILL   3
#define ZONE_W   60

enum { VIEW_LIBRARY, VIEW_EDITOR };
static int      s_view = VIEW_LIBRARY;
static uint32_t s_color = 0x000000;
static uint32_t s_last_fill = 0xffffff;  /* Clear fills with the most recent Fill color */
static int      s_open_id = -1;

/* Debounced autosave. The runtime has no app exit hook, so a Home exit would
   otherwise drop edits made since the last library-return save. We mark the
   canvas dirty on any change and flush it once the finger has been up for a
   short idle window, so the open drawing is always current on disk. */
static int      s_canvas_dirty;
static int      s_idle_frames;
#define AUTOSAVE_IDLE_FRAMES 40          /* ~0.65s at the ~60 fps heartbeat */

static int s_ids[MAX_DRAWINGS];
static int s_n;

/* touch tracking */
static int s_prev_down, s_press_x, s_press_y, s_moved;
static int s_last_x, s_last_y;          /* last canvas point while drawing */
static int s_press_ms;                  /* press duration (frames ~16ms) for long-press */

/* library scroll + redraw */
static int s_scroll;
static int s_dirty = 1;                 /* library needs a full redraw */

static uint32_t s_thumb_scratch[TW * TH];    /* one thumbnail (THUMB_BYTES); reused per tile */

/* ---------------------------------------------------------------- storage ---- */

static int id_from_name(const char *fn)
{
    int id = 0, any = 0;
    while (*fn >= '0' && *fn <= '9') { id = id * 10 + (*fn - '0'); fn++; any = 1; }
    /* accept "<id>.bin" only (ignore .tmb so each drawing counts once) */
    if (!any || fn[0] != '.' || fn[1] != 'b') return -1;
    return id;
}

static void scan_drawings(void)
{
    hb_dir_t d;
    char fn[64];
    bool is_dir;
    s_n = 0;
    if (!hb_fs_dir_open(&d, DATA_DIR, false)) return;
    while (hb_fs_dir_next(&d, fn, sizeof fn, &is_dir)) {
        int id;
        if (is_dir) continue;
        id = id_from_name(fn);
        if (id < 0 || s_n >= MAX_DRAWINGS) continue;
        s_ids[s_n++] = id;
    }
    hb_fs_dir_close(&d);
    /* newest first (descending id) — simple insertion sort */
    for (int i = 1; i < s_n; i++) {
        int v = s_ids[i], j = i;
        while (j > 0 && s_ids[j - 1] < v) { s_ids[j] = s_ids[j - 1]; j--; }
        s_ids[j] = v;
    }
}

static int next_id(void)
{
    int mx = 0;
    for (int i = 0; i < s_n; i++) if (s_ids[i] > mx) mx = s_ids[i];
    return mx + 1;
}

static void path_for(char *out, int id, const char *ext)
{
    /* DATA_DIR "/" <id> ext   (no sprintf in the SDK; build it by hand) */
    int i = 0, j;
    const char *d = DATA_DIR;
    char num[12]; int n = 0, v = id;
    while (*d) out[i++] = *d++;
    out[i++] = '/';
    if (v == 0) num[n++] = '0';
    else while (v) { num[n++] = (char)('0' + v % 10); v /= 10; }
    for (j = n - 1; j >= 0; j--) out[i++] = num[j];
    for (j = 0; ext[j]; j++) out[i++] = ext[j];
    out[i] = 0;
}

/* The canvas region of the framebuffer is contiguous (stride == width). */
static uint32_t *canvas_fb(void) { return hb_raw_fb() + CANVAS_Y * FB_W; }

static void write_thumb(int id)
{
    uint32_t *src = canvas_fb();
    uint32_t *dst = s_thumb_scratch;
    char path[80];
    for (int ty = 0; ty < TH; ty++)
        for (int tx = 0; tx < TW; tx++)
            dst[ty * TW + tx] = src[(ty * 5) * CANVAS_W + (tx * 5)] | 0xff000000u;
    path_for(path, id, ".tmb");
    hb_fs_write(path, dst, THUMB_BYTES);
}

static void save_open(void)
{
    char path[80];
    if (s_open_id < 0) return;
    path_for(path, s_open_id, ".bin");
    hb_fs_write(path, canvas_fb(), CANVAS_BYTES);
    write_thumb(s_open_id);
}

static void delete_id(int id)
{
    char path[80];
    path_for(path, id, ".bin"); hb_fs_remove(path);
    path_for(path, id, ".tmb"); hb_fs_remove(path);
}

/* ------------------------------------------------------------------ draw ----- */

/* Brush dab in framebuffer coords, clipped to the canvas band. */
static void dab(int fx, int fy)
{
    uint32_t *fb = hb_raw_fb();
    uint32_t c = s_color | 0xff000000u;
    int dy, dx;
    s_canvas_dirty = 1;
    for (dy = -BRUSH_R; dy <= BRUSH_R; dy++) {
        int y = fy + dy;
        if (y < CANVAS_Y || y >= CANVAS_Y + CANVAS_H) continue;
        for (dx = -BRUSH_R; dx <= BRUSH_R; dx++) {
            int x = fx + dx;
            if (x < 0 || x >= CANVAS_W) continue;
            if (dx * dx + dy * dy > BRUSH_R * BRUSH_R + 1) continue;
            fb[(long)y * FB_W + x] = c;
        }
    }
}

static void stroke(int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        dab(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_glyph_x(int cx, int cy, int r, uint32_t c)
{
    for (int i = -r; i <= r; i++) {
        hb_raw_fill_rect(cx + i - 1, cy + i - 1, 3, 3, c);
        hb_raw_fill_rect(cx + i - 1, cy - i - 1, 3, 3, c);
    }
}

/* A bucket icon: handle + a trapezoid body filled with `col` (with a light rim
 * so a dark/black color stays visible on the dark bar). */
static void draw_bucket(int bx, int by, int bw, int bh, uint32_t col)
{
    int row, topw = bw, botw = (bw * 3) / 5;
    hb_raw_fill_rect(bx + 3, by - 5, bw - 6, 2, 0x9aa5b1);       /* handle top   */
    hb_raw_fill_rect(bx + 3, by - 5, 2, 5, 0x9aa5b1);           /* handle left  */
    hb_raw_fill_rect(bx + bw - 5, by - 5, 2, 5, 0x9aa5b1);      /* handle right */
    for (row = 0; row < bh; row++) {
        int w = topw - (topw - botw) * row / bh;
        hb_raw_fill_rect(bx + (bw - w) / 2, by + row, w, 1, col);
    }
    hb_raw_fill_rect(bx, by, bw, 2, 0xeef1f8);                  /* light rim     */
}

static void draw_topbar(void)
{
    hb_raw_fill_rect(0, TOP_Y, FB_W, TOP_H, 0x1a1f2e);
    /* zone 0: library (hamburger), leftmost, nudged 2px up */
    for (int i = 0; i < 3; i++)
        hb_raw_fill_rect(14, 9 + i * 9, ZONE_W - 28, 4, 0xffffff);
    /* zone 1: current-color swatch */
    hb_raw_fill_rect(ZONE_W + 8, 6, ZONE_W - 16, TOP_H - 12, s_color);
    hb_raw_rect_outline(ZONE_W + 8, 6, ZONE_W - 16, TOP_H - 12, 1, 0x9aa5b1);
    /* zone 2: clear (white box + red X) */
    hb_raw_fill_rect(2 * ZONE_W + 10, 8, ZONE_W - 20, TOP_H - 16, 0xffffff);
    draw_glyph_x(2 * ZONE_W + ZONE_W / 2, TOP_H / 2, 6, 0xe63946);
    /* zone 3: fill (bucket of the current color) */
    draw_bucket(3 * ZONE_W + 16, 12, ZONE_W - 32, TOP_H - 20, s_color);
}

static void draw_palette(void)
{
    for (int i = 0; i < PAL_N; i++) {
        hb_raw_fill_rect(i * PAL_CW, PAL_Y, PAL_CW, PAL_H, PALETTE[i]);
        if (PALETTE[i] == s_color)
            hb_raw_rect_outline(i * PAL_CW, PAL_Y, PAL_CW, PAL_H, 3, 0x0a84ff);
    }
}

/* ---------------------------------------------------------------- editor ----- */

static void open_editor(int id, int is_new)
{
    char path[80];
    s_open_id = id;
    s_view = VIEW_EDITOR;
    s_last_x = -1;
    s_canvas_dirty = 0;
    s_idle_frames = 0;
    if (is_new) {
        hb_raw_fill_rect(0, CANVAS_Y, CANVAS_W, CANVAS_H, 0xffffff);
    } else {
        path_for(path, id, ".bin");
        if (hb_fs_read(path, canvas_fb(), CANVAS_BYTES) < CANVAS_BYTES)
            hb_raw_fill_rect(0, CANVAS_Y, CANVAS_W, CANVAS_H, 0xffffff);
    }
    draw_topbar();
    draw_palette();
}

static void editor_touch(const hb_spoint_t *t)
{
    int x = t->x, y = t->y, down = t->down;

    if (down && !s_prev_down) {                 /* press */
        s_press_x = x; s_press_y = y; s_moved = 0;
        if (y >= CANVAS_Y && y < CANVAS_Y + CANVAS_H) {
            s_last_x = x; s_last_y = y; dab(x, y);
        } else {
            s_last_x = -1;
        }
    } else if (down && s_prev_down) {           /* drag */
        s_moved += (x > s_press_x ? x - s_press_x : s_press_x - x);
        if (s_last_x >= 0 && y >= CANVAS_Y && y < CANVAS_Y + CANVAS_H) {
            stroke(s_last_x, s_last_y, x, y);
            s_last_x = x; s_last_y = y;
        } else if (y >= CANVAS_Y && y < CANVAS_Y + CANVAS_H) {
            s_last_x = x; s_last_y = y; dab(x, y);
        }
    } else if (!down && s_prev_down) {          /* release — button taps */
        s_last_x = -1;
        int moved = s_moved > 12;
        if (!moved && s_press_y < TOP_H) {
            int z = s_press_x / ZONE_W;
            if (z == Z_CLEAR) {
                hb_raw_fill_rect(0, CANVAS_Y, CANVAS_W, CANVAS_H, s_last_fill);
                s_canvas_dirty = 1;
            } else if (z == Z_FILL) {
                s_last_fill = s_color;
                hb_raw_fill_rect(0, CANVAS_Y, CANVAS_W, CANVAS_H, s_color);
                s_canvas_dirty = 1;
            } else if (z == Z_LIB) {
                save_open();
                s_canvas_dirty = 0;
                scan_drawings();
                s_view = VIEW_LIBRARY; s_scroll = 0; s_dirty = 1;
            }
        } else if (!moved && s_press_y >= PAL_Y) {
            int i = s_press_x / PAL_CW;
            if (i >= 0 && i < PAL_N) { s_color = PALETTE[i]; draw_topbar(); draw_palette(); }
        }
    }
}

/* --------------------------------------------------------------- library ----- */

#define LIB_COLS 4
#define CELL_W   60
#define CELL_H   84
#define TX_PAD   ((CELL_W - TW) / 2)     /* 6 */

static void blit_thumb(int cellx, int celly, int id)
{
    char path[80];
    path_for(path, id, ".tmb");
    if (hb_fs_read(path, s_thumb_scratch, THUMB_BYTES) >= THUMB_BYTES)
        hb_raw_blit(cellx + TX_PAD, celly + 6, TW, TH, (uint32_t *)s_thumb_scratch);
    else
        hb_raw_fill_rect(cellx + TX_PAD, celly + 6, TW, TH, 0x333845);
    hb_raw_rect_outline(cellx + TX_PAD, celly + 6, TW, TH, 1, 0x4a5160);
}

static void draw_library(void)
{
    int total = s_n + 1;                 /* tile 0 = New */
    hb_raw_fill(0x0a0e1a);
    for (int idx = 0; idx < total; idx++) {
        int col = idx % LIB_COLS, row = idx / LIB_COLS;
        int cx = col * CELL_W, cy = row * CELL_H - s_scroll;
        if (cy <= -CELL_H || cy >= 432) continue;
        if (idx == 0) {                  /* New tile: '+' */
            hb_raw_fill_rect(cx + TX_PAD, cy + 6, TW, TH, 0x1a1f2e);
            hb_raw_rect_outline(cx + TX_PAD, cy + 6, TW, TH, 1, 0x2a9d8f);
            hb_raw_fill_rect(cx + CELL_W / 2 - 2, cy + 6 + TH / 2 - 12, 4, 24, 0x2a9d8f);
            hb_raw_fill_rect(cx + CELL_W / 2 - 12, cy + 6 + TH / 2 - 2, 24, 4, 0x2a9d8f);
        } else {
            blit_thumb(cx, cy, s_ids[idx - 1]);
        }
    }
    s_dirty = 0;
}

static int lib_max_scroll(void)
{
    int total = s_n + 1;
    int rows = (total + LIB_COLS - 1) / LIB_COLS;
    int h = rows * CELL_H - 432;
    return h > 0 ? h : 0;
}

static void library_touch(const hb_spoint_t *t)
{
    int x = t->x, y = t->y, down = t->down;

    if (down && !s_prev_down) {
        s_press_x = x; s_press_y = y; s_moved = 0; s_press_ms = 0;
    } else if (down && s_prev_down) {
        int dyt = y - s_press_y;
        s_moved += (x > s_press_x ? x - s_press_x : s_press_x - x)
                 + (dyt > 0 ? dyt : -dyt);
        s_press_ms++;
        /* drag = scroll */
        if (s_moved > 8) {
            int ns = s_scroll - dyt;
            int mx = lib_max_scroll();
            if (ns < 0) ns = 0; if (ns > mx) ns = mx;
            if (ns != s_scroll) { s_scroll = ns; s_dirty = 1; }
            s_press_y = y;
        }
    } else if (!down && s_prev_down) {
        if (s_moved <= 8) {              /* a tap (not a scroll) */
            int col = x / CELL_W, row = (y + s_scroll) / CELL_H;
            int idx = row * LIB_COLS + col;
            if (col < LIB_COLS && idx >= 0 && idx <= s_n) {
                if (idx == 0) open_editor(next_id(), 1);
                else {
                    if (s_press_ms > 40) {       /* long-press (~0.65s) -> delete */
                        delete_id(s_ids[idx - 1]);
                        scan_drawings();
                        s_dirty = 1;
                    } else {
                        open_editor(s_ids[idx - 1], 0);
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------ raw surface ---- */

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    hb_fs_mkdir(DATA_DIR);
    scan_drawings();
    s_view = VIEW_LIBRARY;
    s_dirty = 1;
}

void hb_raw_frame(const hb_spoint_t *t)
{
    if (s_view == VIEW_EDITOR) {
        editor_touch(t);
        /* Flush the canvas once the finger has been up for the idle window, so
           edits survive a Home exit even though the runtime has no exit hook. */
        if (s_canvas_dirty) {
            if (t->down) {
                s_idle_frames = 0;          /* still drawing */
            } else if (++s_idle_frames >= AUTOSAVE_IDLE_FRAMES) {
                save_open();
                s_canvas_dirty = 0;
                s_idle_frames = 0;
            }
        }
    } else {
        if (s_dirty) draw_library();
        library_touch(t);
    }
    s_prev_down = t->down;
}
