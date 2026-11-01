/*
 * breakout — brick-breaker clone.
 *
 * 8 cols × 6 rows of colored bricks at the top, paddle near the
 * bottom, ball bounces between them. Drag anywhere on the playfield
 * to move the paddle horizontally. Ball reflects off side walls,
 * top wall, paddle, and bricks (clearing the brick + scoring on
 * each hit). Lose a life when the ball drops past the paddle; game
 * ends at 0 lives or when all bricks are gone.
 *
 * Single canvas + small widgets for the score/lives header.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define PLAY_W       240
#define PLAY_H       380
#define HEADER_H     32
#define BRICK_COLS   8
#define BRICK_ROWS   6
#define BRICK_W      (PLAY_W / BRICK_COLS)
#define BRICK_H      14
#define BRICKS_Y     20
#define PADDLE_W     56
#define PADDLE_H     8
#define BALL_R       5

/* In the app's own arena: the old fixed 0x092E0000 lies inside the OS heap
 * and was overwriting live objects (and sometimes this app's own code). */
static uint8_t s_canvas_buf[PLAY_W * PLAY_H * 4] __attribute__((aligned(4)));

static uint8_t s_bricks[BRICK_ROWS][BRICK_COLS];     /* 0 = cleared, else color 1..6 */
static int s_paddle_x;
static int s_bx_q4, s_by_q4;      /* ball pos × 16 (Q4 fixed-point) */
static int s_vx_q4, s_vy_q4;
static int s_lives = 3;
static int s_score = 0;
static int s_bricks_left = 0;
static bool s_paused = true;
static bool s_dead = false;
static bool s_won  = false;

static lv_obj_t *s_canvas;
static lv_obj_t *s_status_lbl;

static const uint32_t s_brick_color[7] = {
    0x000000, 0xe63946, 0xf77f00, 0xfcbf49, 0x2a9d8f, 0x457b9d, 0xa663cc,
};

static uint32_t pack(uint32_t hex)
{
    uint8_t r=(hex>>16)&0xFF, g=(hex>>8)&0xFF, b=hex&0xFF;
    return ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)b;
}
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
            if (dx*dx + dy*dy <= r2) buf[yy * PLAY_W + xx] = color;
        }
    }
}

static void itoa_u(uint32_t v, char *out)
{
    char b[12]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    int k = 0; while (i) out[k++] = b[--i];
    out[k] = 0;
}

static void reset_bricks(void)
{
    s_bricks_left = 0;
    for (int r = 0; r < BRICK_ROWS; r++) for (int c = 0; c < BRICK_COLS; c++) {
        s_bricks[r][c] = (uint8_t)((r % 6) + 1);
        s_bricks_left++;
    }
}

static void reset_ball(void)
{
    s_paddle_x = (PLAY_W - PADDLE_W) / 2;
    s_bx_q4 = (PLAY_W / 2) << 4;
    s_by_q4 = (PLAY_H - 60) << 4;
    s_vx_q4 = 32;          /* 2 px/frame */
    s_vy_q4 = -48;         /* -3 px/frame */
    s_paused = true;
}

static void start_round(void)
{
    s_score = 0;
    s_lives = 3;
    s_dead = false;
    s_won = false;
    reset_bricks();
    reset_ball();
}

static void render(void)
{
    cfill(0, 0, PLAY_W, PLAY_H, pack(0x0a0e1a));
    /* Bricks */
    for (int r = 0; r < BRICK_ROWS; r++) for (int c = 0; c < BRICK_COLS; c++) {
        uint8_t col = s_bricks[r][c];
        if (col == 0) continue;
        int x = c * BRICK_W;
        int y = BRICKS_Y + r * BRICK_H;
        cfill(x + 1, y + 1, BRICK_W - 2, BRICK_H - 2, pack(s_brick_color[col]));
    }
    /* Paddle */
    int py = PLAY_H - PADDLE_H - 16;
    cfill(s_paddle_x, py, PADDLE_W, PADDLE_H, pack(0xc8d3df));
    /* Ball */
    cdisc(s_bx_q4 >> 4, s_by_q4 >> 4, BALL_R, pack(0xfcbf49));
    lv_obj_invalidate(s_canvas);
}

static void refresh_status(void)
{
    char buf[40]; int k = 0;
    const char *p = "Score "; while (*p) buf[k++] = *p++;
    char nb[12]; itoa_u((uint32_t)s_score, nb);
    for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    p = "  Lives "; while (*p) buf[k++] = *p++;
    itoa_u((uint32_t)s_lives, nb);
    for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    if (s_dead || s_won) {
        p = (s_won ? "  WIN! Tap" : "  GAME OVER tap"); while (*p) buf[k++] = *p++;
    }
    buf[k] = 0;
    lv_label_set_text(s_status_lbl, buf);
}

static void on_press(lv_event_t *e)
{
    (void)e;
    if (s_dead || s_won) {
        start_round();
        render(); refresh_status();
        return;
    }
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    lv_area_t area; lv_obj_get_coords(s_canvas, &area);
    int local_x = p.x - area.x1;
    s_paddle_x = local_x - PADDLE_W / 2;
    if (s_paddle_x < 0) s_paddle_x = 0;
    if (s_paddle_x > PLAY_W - PADDLE_W) s_paddle_x = PLAY_W - PADDLE_W;
    if (s_paused) s_paused = false;
}

static void on_pressing(lv_event_t *e)
{
    on_press(e);
}

static void on_frame(void)
{
    if (s_dead || s_won) return;
    if (s_paused) return;

    s_bx_q4 += s_vx_q4;
    s_by_q4 += s_vy_q4;
    int bx = s_bx_q4 >> 4;
    int by = s_by_q4 >> 4;

    /* Walls */
    if (bx - BALL_R < 0) { s_bx_q4 = BALL_R << 4; s_vx_q4 = -s_vx_q4; }
    if (bx + BALL_R > PLAY_W) { s_bx_q4 = (PLAY_W - BALL_R) << 4; s_vx_q4 = -s_vx_q4; }
    if (by - BALL_R < 0) { s_by_q4 = BALL_R << 4; s_vy_q4 = -s_vy_q4; }

    /* Paddle */
    int py = PLAY_H - PADDLE_H - 16;
    if (by + BALL_R >= py && by + BALL_R <= py + PADDLE_H &&
        bx >= s_paddle_x && bx <= s_paddle_x + PADDLE_W && s_vy_q4 > 0) {
        s_by_q4 = (py - BALL_R) << 4;
        s_vy_q4 = -s_vy_q4;
        int offset = bx - (s_paddle_x + PADDLE_W / 2);
        s_vx_q4 += offset * 2;
        if (s_vx_q4 > 100) s_vx_q4 = 100;
        if (s_vx_q4 < -100) s_vx_q4 = -100;
    }

    /* Bricks — find which brick (if any) the ball overlaps. */
    if (by - BALL_R < BRICKS_Y + BRICK_ROWS * BRICK_H &&
        by + BALL_R > BRICKS_Y) {
        int c = bx / BRICK_W;
        int r = (by - BRICKS_Y) / BRICK_H;
        if (r >= 0 && r < BRICK_ROWS && c >= 0 && c < BRICK_COLS && s_bricks[r][c]) {
            s_bricks[r][c] = 0;
            s_score += 10;
            s_bricks_left--;
            /* Pick reflection axis: smaller penetration wins. */
            int bx_l = c * BRICK_W, bx_r = bx_l + BRICK_W;
            int by_t = BRICKS_Y + r * BRICK_H, by_b = by_t + BRICK_H;
            int dx = (bx - bx_l < bx_r - bx) ? bx - bx_l : bx_r - bx;
            int dy = (by - by_t < by_b - by) ? by - by_t : by_b - by;
            if (dx < dy) s_vx_q4 = -s_vx_q4;
            else         s_vy_q4 = -s_vy_q4;
            if (s_bricks_left == 0) {
                s_won = true;
            }
        }
    }

    /* Lose ball */
    if (by - BALL_R > PLAY_H) {
        s_lives--;
        if (s_lives <= 0) s_dead = true;
        else { reset_ball(); }
    }

    render();
    refresh_status();
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "Score 0  Lives 3");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 8);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, PLAY_W, PLAY_H,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(s_canvas, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, on_pressing, LV_EVENT_PRESSING, NULL);

    start_round();
    render();
    refresh_status();

    hb_lv_set_frame_cb(on_frame);
}
