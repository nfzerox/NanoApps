/*
 * golf — top-down mini golf.
 *
 * Drag-and-release from the ball to aim+power-up the putt: vector
 * from release back to the ball sets the initial velocity. Ball
 * rolls with friction, bounces off walls and rough/sand patches
 * (slowing it more). Sink the ball in the hole. 9 hand-laid holes,
 * scored by stroke count; lower is better.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"
#include "golf_physics.h"

#define PLAY_W       240
#define PLAY_H       432          /* full screen — no borders */
#define BALL_R       6
#define HOLE_R       10
#define MAX_WALLS    10
#define MAX_SAND     6

/* Tuning constants — not finalized. */
#define FIXED_STEP_US      8333u            /* 120 Hz physics tick */
#define MAX_ACCUM_US       (12u * FIXED_STEP_US)   /* ~100ms bounded catch-up budget */
#define PAUSE_THRESHOLD_US 200000u          /* >200ms gap = pause, not jitter */
#define A_GRASS_PXS2       300.0f           /* px/s^2 */
#define A_SAND_PXS2        1200.0f          /* px/s^2 (4x grass) */
#define V_MAX_PXS          600.0f           /* px/s, vector-length clamp */
#define DRAG_GAIN          7.5f             /* 1/s, K = V_MAX / L_FULL (80px) */

typedef struct { int x, y, w, h; } rect_t;

typedef struct {
    int      par;
    int      tee_x, tee_y;
    int      hole_x, hole_y;
    int      n_walls;
    rect_t   walls[MAX_WALLS];
    int      n_sand;
    rect_t   sand[MAX_SAND];
} hole_t;

static const hole_t s_holes[9] = {
    /* 1 — straight shot */
    { 2, 120, 340, 120,  60, 0, {{0,0,0,0}}, 0, {{0,0,0,0}} },
    /* 2 — one offset wall */
    { 3, 60,  340, 180,  60, 1, { {120, 160, 60, 8} }, 0, {{0,0,0,0}} },
    /* 3 — dogleg right */
    { 4, 40,  340, 200, 60, 2,
      { {120, 220, 100, 8}, {120, 160, 8, 60} }, 0, {{0,0,0,0}} },
    /* 4 — sand trap in middle */
    { 3, 120, 340, 120,  60, 0, {{0,0,0,0}},
      1, { {80, 180, 80, 50} } },
    /* 5 — channel between walls */
    { 3, 120, 340, 120,  60, 2,
      { {40, 140, 60, 8}, {140, 140, 60, 8} }, 0, {{0,0,0,0}} },
    /* 6 — diagonal walls */
    { 4, 40,  340, 200,  60, 2,
      { {100, 220, 8, 70}, {160, 130, 8, 60} }, 0, {{0,0,0,0}} },
    /* 7 — two sand traps */
    { 4, 120, 340, 120,  60, 0, {{0,0,0,0}},
      2, { {30, 200, 60, 40}, {150, 200, 60, 40} } },
    /* 8 — narrow gate */
    { 4, 120, 340, 120,  60, 2,
      { {40, 200, 70, 8}, {130, 200, 70, 8} }, 0, {{0,0,0,0}} },
    /* 9 — L-shape */
    { 5, 40,  340, 200,  60, 3,
      { {40, 240, 8, 70}, {40, 240, 100, 8}, {130, 130, 8, 70} }, 0, {{0,0,0,0}} }
};

static int s_hole_idx = 0;
static int s_strokes = 0;
static int s_total = 0;
static int s_par_total = 0;
static int s_state = 0;     /* 0=aim, 1=rolling, 2=sunk, 3=tour-end */
static float s_ball_x, s_ball_y;     /* px, real-valued physics state — never rounded in place */
static float s_ball_vx, s_ball_vy;   /* px/s */
static uint32_t s_last_now_us, s_accum_us;
static int s_drag_active;
static int s_drag_x, s_drag_y;

/*
 * Canvas pixel buffer, PLAY_W*PLAY_H px @ 4 bytes/px (XRGB8888) = 414720 B.
 *
 * Lives in the app's own .bss rather than at a fixed DRAM address: this app
 * kind (LV_SURFACE + HB_LV_RELOC) is loaded into a dynamically allocated
 * arena sized to cover its own .bss (the same arena LVGL's own memory pool
 * lives in, per lv_conf.h's LV_MEM_ADR=0), so a plain static array here is
 * covered by that same allocation and freed the same way — no separate,
 * hand-picked address to keep safe.
 * `aligned(4)` because cfill()/cdisc() draw through it as uint32_t*.
 */
static uint8_t s_canvas_buf[PLAY_W * PLAY_H * 4] __attribute__((aligned(4)));
static lv_obj_t *s_canvas;
static lv_obj_t *s_stat_bl;     /* bottom-left: hole / par   */
static lv_obj_t *s_stat_br;     /* bottom-right: strokes / total */
static lv_obj_t *s_center_lbl;  /* centred "IN!" / "Done"    */

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
static void cdisc(int cx, int cy, int r, uint32_t color)
{
    int r2 = r * r;
    uint32_t *buf = (uint32_t *)s_canvas_buf;
    for (int dy = -r; dy <= r; dy++) {
        int yy = cy + dy;
        if (yy < 0 || yy >= PLAY_H) continue;
        for (int dx = -r; dx <= r; dx++) {
            int xx = cx + dx;
            if (xx < 0 || xx >= PLAY_W) continue;
            if (dx * dx + dy * dy <= r2)
                buf[yy * PLAY_W + xx] = color;
        }
    }
}

static int in_rect(int x, int y, const rect_t *r)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static void render(void)
{
    const hole_t *h = &s_holes[s_hole_idx];
    /* green */
    cfill(0, 0, PLAY_W, PLAY_H, pack(0x2e7d32));
    /* darker stripes for putting feel */
    for (int y = 0; y < PLAY_H; y += 24)
        cfill(0, y, PLAY_W, 12, pack(0x33892f));
    /* sand */
    for (int i = 0; i < h->n_sand; i++) {
        cfill(h->sand[i].x, h->sand[i].y, h->sand[i].w, h->sand[i].h, pack(0xfff176));
    }
    /* walls */
    for (int i = 0; i < h->n_walls; i++) {
        cfill(h->walls[i].x, h->walls[i].y, h->walls[i].w, h->walls[i].h, pack(0x6d4c41));
        cfill(h->walls[i].x, h->walls[i].y, h->walls[i].w, 2, pack(0x8d6e63));
    }
    /* hole */
    cdisc(h->hole_x, h->hole_y, HOLE_R, pack(0x101820));
    /* flag */
    cfill(h->hole_x + 1, h->hole_y - 26, 1, 26, pack(0xc8d3df));
    cfill(h->hole_x + 1, h->hole_y - 26, 10, 7, pack(0xe63946));
    /* aim guide */
    if (s_state == 0 && s_drag_active) {
        int x0 = phys_round(s_ball_x), y0 = phys_round(s_ball_y);
        int dx = s_drag_x - x0, dy = s_drag_y - y0;
        for (int i = 1; i <= 16; i++) {
            int x = x0 - (dx * i) / 16;
            int y = y0 - (dy * i) / 16;
            cfill(x - 1, y - 1, 2, 2, pack(0xffffff));
        }
    }
    /* ball */
    if (s_state != 2 && s_state != 3) {
        int bx = phys_round(s_ball_x), by = phys_round(s_ball_y);
        cdisc(bx, by, BALL_R, pack(0xffffff));
        cdisc(bx - 1, by - 1, 2, pack(0xeeeeee));
    }
}

static void itoa_i(int v, char *out)
{
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    char b[12]; int i = 0;
    int neg = v < 0; uint32_t u = neg ? (uint32_t)(-v) : (uint32_t)v;
    while (u) { b[i++] = '0' + u % 10; u /= 10; }
    int k = 0; if (neg) out[k++] = '-';
    while (i) out[k++] = b[--i];
    out[k] = 0;
}
static void put(char *b, int *k, const char *s) { while (*s) b[(*k)++] = *s++; }

static void refresh_hud(void)
{
    char b[48]; int k; char nb[12];
    /* bottom-left: hole + par, two rows */
    k = 0; put(b, &k, "Hole "); itoa_i(s_hole_idx + 1, nb); put(b, &k, nb);
    put(b, &k, "/9\nPar "); itoa_i(s_holes[s_hole_idx].par, nb); put(b, &k, nb);
    b[k] = 0; lv_label_set_text(s_stat_bl, b);
    /* bottom-right: strokes + total, two rows */
    k = 0; put(b, &k, "Strokes "); itoa_i(s_strokes, nb); put(b, &k, nb);
    put(b, &k, "\nTotal "); itoa_i(s_total, nb); put(b, &k, nb);
    b[k] = 0; lv_label_set_text(s_stat_br, b);
    /* centre: big status only at sink / tour end */
    if (s_state == 2)      lv_label_set_text(s_center_lbl, "IN!");
    else if (s_state == 3) lv_label_set_text(s_center_lbl, "Done");
    else                   lv_label_set_text(s_center_lbl, "");
}

static void load_hole(int idx)
{
    s_hole_idx = idx;
    s_strokes = 0;
    s_ball_x = (float)s_holes[idx].tee_x;
    s_ball_y = (float)s_holes[idx].tee_y;
    s_ball_vx = s_ball_vy = 0.0f;
    s_state = 0;
    s_accum_us = 0;                    /* fresh clock for the new hole */
    s_last_now_us = hb_time_uptime_us();
    refresh_hud();
}

static void on_press(lv_event_t *e)
{
    (void)e;
    if (s_state == 3) {
        s_total = 0; s_par_total = 0; load_hole(0);
        return;
    }
    if (s_state == 2) {
        if (phys_advance_hole(&s_hole_idx, 9)) { s_state = 3; refresh_hud(); return; }
        load_hole(s_hole_idx);
        return;
    }
    if (s_state == 1) return;
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    lv_area_t a; lv_obj_get_coords(s_canvas, &a);
    s_drag_x = p.x - a.x1; s_drag_y = p.y - a.y1;
    s_drag_active = 1;
}
static void on_pressing(lv_event_t *e)
{
    (void)e;
    if (s_state != 0 || !s_drag_active) return;
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    lv_area_t a; lv_obj_get_coords(s_canvas, &a);
    s_drag_x = p.x - a.x1; s_drag_y = p.y - a.y1;
}
static void on_release(lv_event_t *e)
{
    (void)e;
    if (s_state != 0 || !s_drag_active) return;
    s_drag_active = 0;
    int x0 = phys_round(s_ball_x), y0 = phys_round(s_ball_y);
    int dx = s_drag_x - x0, dy = s_drag_y - y0;
    if (dx * dx + dy * dy < 64) return;       /* too small — 8px dead zone */
    /* launch opposite the drag; clamp the vector's length (V_MAX), not each
       axis independently, so shot power doesn't depend on aim angle */
    phys_launch_from_drag((float)dx, (float)dy, DRAG_GAIN, V_MAX_PXS, &s_ball_vx, &s_ball_vy);
    s_state = 1;
    s_strokes++;
    s_accum_us = 0;                    /* fresh clock for this shot */
    s_last_now_us = hb_time_uptime_us();
    refresh_hud();
}

/*
 * One fixed physics step. Order: sample terrain -> exact braking
 * integration -> wall/boundary collision -> hole test -> stop test. The
 * hole test returns immediately so a same-call stop test can never
 * overwrite a SUNK transition.
 *
 * Known limitation: the ball is tested as an axis-aligned square (not a
 * circle) against each wall with a single discrete post-move overlap check
 * (no swept/continuous test), so it can in principle tunnel through a thin
 * wall at high speed or mis-resolve a corner made of two separate wall
 * rects; wall bounces are undamped (restitution effectively 1, sign-flip
 * only). Hole capture is also end-of-step-position-only, so a fast,
 * tangential graze of the capture circle within one physics step could in
 * principle be missed.
 */
static void simulate_step(float h_s)
{
    const hole_t *hd = &s_holes[s_hole_idx];
    int bx = phys_round(s_ball_x), by = phys_round(s_ball_y);

    int in_sand = 0;
    for (int i = 0; i < hd->n_sand; i++)
        if (in_rect(bx, by, &hd->sand[i])) { in_sand = 1; break; }
    float decel = in_sand ? A_SAND_PXS2 : A_GRASS_PXS2;

    phys_integrate_step(&s_ball_x, &s_ball_y, &s_ball_vx, &s_ball_vy, decel, h_s);

    bx = phys_round(s_ball_x); by = phys_round(s_ball_y);
    /* walls (axis-aligned) — see known-limitation note above */
    for (int i = 0; i < hd->n_walls; i++) {
        const rect_t *r = &hd->walls[i];
        if (bx + BALL_R > r->x && bx - BALL_R < r->x + r->w &&
            by + BALL_R > r->y && by - BALL_R < r->y + r->h) {
            int penL = (bx + BALL_R) - r->x;
            int penR = (r->x + r->w) - (bx - BALL_R);
            int penT = (by + BALL_R) - r->y;
            int penB = (r->y + r->h) - (by - BALL_R);
            int min_pen = penL;
            int axis = 0;
            if (penR < min_pen) { min_pen = penR; axis = 1; }
            if (penT < min_pen) { min_pen = penT; axis = 2; }
            if (penB < min_pen) { min_pen = penB; axis = 3; }
            if (axis == 0) { s_ball_x -= (float)min_pen; s_ball_vx = -s_ball_vx; }
            if (axis == 1) { s_ball_x += (float)min_pen; s_ball_vx = -s_ball_vx; }
            if (axis == 2) { s_ball_y -= (float)min_pen; s_ball_vy = -s_ball_vy; }
            if (axis == 3) { s_ball_y += (float)min_pen; s_ball_vy = -s_ball_vy; }
            bx = phys_round(s_ball_x); by = phys_round(s_ball_y);
        }
    }
    /* outer walls */
    if (bx < BALL_R) { s_ball_x = (float)BALL_R; s_ball_vx = -s_ball_vx; }
    if (bx > PLAY_W - BALL_R) { s_ball_x = (float)(PLAY_W - BALL_R); s_ball_vx = -s_ball_vx; }
    if (by < BALL_R) { s_ball_y = (float)BALL_R; s_ball_vy = -s_ball_vy; }
    if (by > PLAY_H - BALL_R) { s_ball_y = (float)(PLAY_H - BALL_R); s_ball_vy = -s_ball_vy; }
    bx = phys_round(s_ball_x); by = phys_round(s_ball_y);

    /* hole (end-of-step position only — see known-limitation note above) */
    {
        int dx = bx - hd->hole_x, dy = by - hd->hole_y;
        if (dx * dx + dy * dy < (HOLE_R - 2) * (HOLE_R - 2)) {
            s_state = 2;
            s_total += s_strokes;
            s_par_total += hd->par;
            s_accum_us = 0;
            refresh_hud();
            return;   /* structural: stop test below can never run this call after SUNK */
        }
    }
    /* stopped */
    if (s_ball_vx == 0.0f && s_ball_vy == 0.0f) {
        s_state = 0;
        s_accum_us = 0;
    }
}

static void on_frame(void)
{
    uint32_t now = hb_time_uptime_us();
    if (s_state == 1) {
        phys_clock_tick(&s_last_now_us, &s_accum_us, now,
                         PAUSE_THRESHOLD_US, MAX_ACCUM_US);
        while (s_state == 1 && s_accum_us >= FIXED_STEP_US) {
            s_accum_us -= FIXED_STEP_US;   /* subtract BEFORE the step (see phys_clock_tick doc) */
            simulate_step((float)FIXED_STEP_US * 1e-6f);
        }
    } else {
        s_last_now_us = now;   /* keep the clock fresh while not rolling (aim/sunk/done) */
    }
    render();
    lv_obj_invalidate(s_canvas);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(hb_color_bg()), 0);

    /* Full-bleed field: the canvas fills the whole screen (no title / borders). */
    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, PLAY_W, PLAY_H,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(s_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_press,    LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_canvas, on_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, on_release,  LV_EVENT_RELEASED, NULL);

    /* Stats in the two bottom corners, two rows each, over the green. */
    s_stat_bl = lv_label_create(scr);
    lv_obj_set_style_text_color(s_stat_bl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_stat_bl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_stat_bl, LV_ALIGN_BOTTOM_LEFT, 8, -8);

    s_stat_br = lv_label_create(scr);
    lv_obj_set_style_text_color(s_stat_br, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_stat_br, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_stat_br, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_stat_br, LV_ALIGN_BOTTOM_RIGHT, -8, -8);

    s_center_lbl = lv_label_create(scr);
    lv_label_set_text(s_center_lbl, "");
    lv_obj_set_style_text_color(s_center_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_center_lbl, &lv_font_montserrat_36, 0);
    lv_obj_align(s_center_lbl, LV_ALIGN_CENTER, 0, 0);

    load_hole(0);
    hb_lv_set_frame_cb(on_frame);
}
