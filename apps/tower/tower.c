/*
 * tower — Stack-style tap timing game.
 *
 * A moving block slides back and forth at the top of the stack;
 * tap to drop it. Anything that overhangs the block below gets
 * trimmed; the new block is the overlap. Trim to zero and the
 * tower falls. Each successful stack speeds the next block up.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define PLAY_W      240
#define PLAY_H      380
#define MAX_LAYERS  40
#define LAYER_H     16

typedef struct { int x, w; uint32_t color; } layer_t;
static layer_t s_layers[MAX_LAYERS];
static int     s_n_layers;

static int s_cur_w;
static int s_cur_x;
static int s_dir;            /* +1 / -1 */
static int s_speed_q4;       /* pixels per frame */
static int s_score;
static int s_view_offset;    /* world->screen y shift as tower grows */
static int s_state;          /* 0=play, 1=fell */

/* In the app's own arena: the old fixed 0x092E0000 lies inside the OS heap
 * and was overwriting live objects (and sometimes this app's own code). */
static uint8_t s_canvas_buf[PLAY_W * PLAY_H * 4] __attribute__((aligned(4)));
static lv_obj_t *s_canvas;
static lv_obj_t *s_score_lbl;

static uint32_t s_rng = 0xc0debeef;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

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

static int layer_y_screen(int idx)
{
    int top = PLAY_H - (idx + 1) * LAYER_H + s_view_offset;
    return top;
}

static void render(void)
{
    cfill(0, 0, PLAY_W, PLAY_H, pack(0x101820));
    for (int i = 0; i < s_n_layers; i++) {
        int y = layer_y_screen(i);
        if (y > PLAY_H || y + LAYER_H < 0) continue;
        cfill(s_layers[i].x, y, s_layers[i].w, LAYER_H, pack(s_layers[i].color));
        cfill(s_layers[i].x, y, s_layers[i].w, 1, pack(0xffffff));
    }
    /* moving cursor */
    if (s_state == 0) {
        int y = layer_y_screen(s_n_layers);
        uint32_t col = 0xfca311;
        cfill(s_cur_x, y, s_cur_w, LAYER_H, pack(col));
        cfill(s_cur_x, y, s_cur_w, 1, pack(0xffffff));
    }
}

static void itoa_u(uint32_t v, char *out)
{
    char b[12]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { b[i++] = '0' + v % 10; v /= 10; }
    int k = 0; while (i) out[k++] = b[--i];
    out[k] = 0;
}
static void refresh_hud(void)
{
    char b[40]; int k = 0;
    const char *p = "Score "; while (*p) b[k++] = *p++;
    char nb[12]; itoa_u(s_score, nb);
    for (int i = 0; nb[i]; i++) b[k++] = nb[i];
    if (s_state == 1) { p = "  fell - tap"; while (*p) b[k++] = *p++; }
    b[k] = 0;
    lv_label_set_text(s_score_lbl, b);
}

static void start_game(void)
{
    s_n_layers = 1;
    s_layers[0].x = 70; s_layers[0].w = 100; s_layers[0].color = 0x457b9d;
    s_cur_w = 100; s_cur_x = 0; s_dir = 1; s_speed_q4 = 32;
    s_score = 0; s_view_offset = 0; s_state = 0;
    refresh_hud();
}

static void on_press(lv_event_t *e)
{
    (void)e;
    if (s_state == 1) { start_game(); return; }
    layer_t *base = &s_layers[s_n_layers - 1];
    int new_x, new_w;
    if (s_cur_x >= base->x + base->w || s_cur_x + s_cur_w <= base->x) {
        s_state = 1;
        refresh_hud();
        return;
    }
    new_x = s_cur_x > base->x ? s_cur_x : base->x;
    int new_r = (s_cur_x + s_cur_w) < (base->x + base->w) ? (s_cur_x + s_cur_w) : (base->x + base->w);
    new_w = new_r - new_x;
    if (new_w <= 0) { s_state = 1; refresh_hud(); return; }
    if (s_n_layers >= MAX_LAYERS) { s_state = 1; refresh_hud(); return; }
    s_layers[s_n_layers].x = new_x;
    s_layers[s_n_layers].w = new_w;
    s_layers[s_n_layers].color = 0x2a9d8f + (uint32_t)(rnd() & 0x303030);
    s_n_layers++;
    s_cur_w = new_w;
    s_cur_x = new_x;
    s_dir = (s_n_layers & 1) ? 1 : -1;
    s_speed_q4 += 4;
    s_score++;
    if (s_n_layers * LAYER_H > PLAY_H * 2 / 3) s_view_offset += LAYER_H;
    refresh_hud();
}

static void on_frame(void)
{
    if (s_state == 0) {
        s_cur_x += (s_dir * s_speed_q4) >> 4;
        if (s_cur_x < 0) { s_cur_x = 0; s_dir = 1; }
        if (s_cur_x + s_cur_w > PLAY_W) { s_cur_x = PLAY_W - s_cur_w; s_dir = -1; }
    }
    render();
    lv_obj_invalidate(s_canvas);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    s_rng ^= hb_time_uptime_ms() | 1;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(hb_color_bg()), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Tower");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 4);

    s_score_lbl = lv_label_create(scr);
    lv_label_set_text(s_score_lbl, "Score 0");
    lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_align(s_score_lbl, LV_ALIGN_TOP_RIGHT, -6, 8);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, PLAY_W, PLAY_H,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(s_canvas, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_press, LV_EVENT_PRESSED, NULL);

    start_game();
    hb_lv_set_frame_cb(on_frame);
}
