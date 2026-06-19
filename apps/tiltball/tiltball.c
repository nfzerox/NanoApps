/*
 * tilt_ball — roll a ball with the accelerometer, collect targets,
 * avoid holes.
 *
 * Each round the ball spawns at the center and 5 collectible coins
 * are placed at random non-overlapping positions, plus 3 holes. The
 * player tilts the device to roll the ball; touching a coin gives
 * +1 and removes it; touching a hole ends the run.
 *
 * Tilt → acceleration model: gravity_x ≈ -accel_x (mg), scaled to a
 * pixel-per-frame² acceleration; integrate that into vx, then apply
 * friction. Same for y. Reflect off walls with a damping factor.
 */

#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define PLAY_W    240
#define PLAY_H    380
#define BALL_R    10
#define COIN_R    8
#define HOLE_R    14
#define N_COINS   5
#define N_HOLES   3

typedef struct { int x, y; bool alive; } sprite_t;

static int      s_bx_q8, s_by_q8;     /* Q8 fixed-point position (pixels * 256) */
static int      s_vx_q8, s_vy_q8;
static sprite_t s_coins[N_COINS];
static sprite_t s_holes[N_HOLES];
static int      s_score   = 0;
static int      s_best    = 0;
static bool     s_dead    = false;

static lv_obj_t *s_play;
static lv_obj_t *s_ball;
static lv_obj_t *s_coin_objs[N_COINS];
static lv_obj_t *s_hole_objs[N_HOLES];
static lv_obj_t *s_score_lbl;
static lv_obj_t *s_overlay;
static lv_obj_t *s_overlay_lbl;

/* PRNG */
static uint32_t s_rng;
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

static void itoa_u(uint32_t v, char *out)
{
    char b[12]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    int k = 0; while (i) out[k++] = b[--i];
    out[k] = 0;
}

/* Distance² between two points. */
static inline int dist2(int ax, int ay, int bx, int by)
{
    int dx = ax - bx, dy = ay - by;
    return dx*dx + dy*dy;
}

static bool overlap_any(int x, int y, int r,
                        sprite_t *list, int n)
{
    for (int i = 0; i < n; i++) {
        if (!list[i].alive) continue;
        if (dist2(x, y, list[i].x, list[i].y) < (r + r) * (r + r)) return true;
    }
    return false;
}

static void spawn_sprites(void)
{
    /* Coins first */
    for (int i = 0; i < N_COINS; i++) s_coins[i].alive = false;
    for (int i = 0; i < N_HOLES; i++) s_holes[i].alive = false;

    for (int i = 0; i < N_COINS; i++) {
        for (int t = 0; t < 100; t++) {
            int x = COIN_R + 6 + (int)(rnd() % (uint32_t)(PLAY_W - 2 * (COIN_R + 6)));
            int y = COIN_R + 6 + (int)(rnd() % (uint32_t)(PLAY_H - 2 * (COIN_R + 6)));
            /* Avoid the center where the ball spawns */
            if (dist2(x, y, PLAY_W/2, PLAY_H/2) < 40 * 40) continue;
            if (overlap_any(x, y, COIN_R + 4, s_coins, N_COINS)) continue;
            s_coins[i].x = x; s_coins[i].y = y; s_coins[i].alive = true;
            break;
        }
    }
    for (int i = 0; i < N_HOLES; i++) {
        for (int t = 0; t < 100; t++) {
            int x = HOLE_R + 6 + (int)(rnd() % (uint32_t)(PLAY_W - 2 * (HOLE_R + 6)));
            int y = HOLE_R + 6 + (int)(rnd() % (uint32_t)(PLAY_H - 2 * (HOLE_R + 6)));
            if (dist2(x, y, PLAY_W/2, PLAY_H/2) < 60 * 60) continue;
            if (overlap_any(x, y, HOLE_R + 4, s_coins, N_COINS)) continue;
            if (overlap_any(x, y, HOLE_R + 4, s_holes, N_HOLES)) continue;
            s_holes[i].x = x; s_holes[i].y = y; s_holes[i].alive = true;
            break;
        }
    }
}

static void refresh_sprites(void)
{
    for (int i = 0; i < N_COINS; i++) {
        if (s_coins[i].alive) {
            lv_obj_clear_flag(s_coin_objs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_coin_objs[i], s_coins[i].x - COIN_R, s_coins[i].y - COIN_R);
        } else {
            lv_obj_add_flag(s_coin_objs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < N_HOLES; i++) {
        if (s_holes[i].alive) {
            lv_obj_clear_flag(s_hole_objs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_hole_objs[i], s_holes[i].x - HOLE_R, s_holes[i].y - HOLE_R);
        } else {
            lv_obj_add_flag(s_hole_objs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* Field background palette — shifts as coins are collected, so the board feels
   like it "levels up". All mid/dark tones that keep the green ball, black/red
   holes and yellow coins clearly legible. */
static const uint32_t FIELD_BG[] = {
    0x111522, 0x14233a, 0x1c2440, 0x2a1f3d, 0x3a1f2b, 0x143028, 0x243042,
};
#define N_FIELD_BG ((int)(sizeof FIELD_BG / sizeof FIELD_BG[0]))

static void refresh_score(void)
{
    char buf[40]; int k = 0;
    const char *p = "Coins: "; while (*p) buf[k++] = *p++;
    char nb[12]; itoa_u((uint32_t)s_score, nb);
    for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    if (s_best > 0) {
        p = "  Best: "; while (*p) buf[k++] = *p++;
        itoa_u((uint32_t)s_best, nb);
        for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    }
    buf[k] = 0;
    lv_label_set_text(s_score_lbl, buf);
    if (s_play)
        lv_obj_set_style_bg_color(s_play,
            lv_color_hex(FIELD_BG[(s_score / 2) % N_FIELD_BG]), 0);
}

static void reset_round(void)
{
    s_bx_q8 = (PLAY_W / 2) << 8;
    s_by_q8 = (PLAY_H / 2) << 8;
    s_vx_q8 = 0;
    s_vy_q8 = 0;
    s_dead = false;
    s_score = 0;
    spawn_sprites();
    refresh_sprites();
    refresh_score();
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void show_game_over(void)
{
    if (s_score > s_best) s_best = s_score;
    char buf[40]; int k = 0;
    const char *p = "You fell in!\nCoins: "; while (*p) buf[k++] = *p++;
    char nb[12]; itoa_u((uint32_t)s_score, nb);
    for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    p = "\nTap to retry"; while (*p) buf[k++] = *p++;
    buf[k] = 0;
    lv_label_set_text(s_overlay_lbl, buf);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    refresh_score();
}

static void on_overlay_tap(lv_event_t *e)
{
    (void)e;
    if (s_dead) reset_round();
}

static void on_frame(void)
{
    if (s_dead) return;


    int32_t a[3];
    hb_accel_read_milli_g(a);
    /* tilt → acceleration: 1000 mg ≈ 1g; scale to ~0.5 px/frame²
       per mg of lateral acceleration. Inverted because the device's
       x grows in opposite direction to gravity vector for natural roll. */
    int ax_q8 = -a[0] / 8;       /* mg/8 ≈ 0..125 → modest accel */
    int ay_q8 = -a[1] / 8;

    s_vx_q8 += ax_q8;
    s_vy_q8 += ay_q8;

    /* Friction — multiply velocity by 31/32 each frame. */
    s_vx_q8 = (s_vx_q8 * 31) / 32;
    s_vy_q8 = (s_vy_q8 * 31) / 32;
    /* Cap velocity. */
    int max_v = 8 << 8;        /* 8 px/frame max */
    if (s_vx_q8 >  max_v) s_vx_q8 =  max_v;
    if (s_vx_q8 < -max_v) s_vx_q8 = -max_v;
    if (s_vy_q8 >  max_v) s_vy_q8 =  max_v;
    if (s_vy_q8 < -max_v) s_vy_q8 = -max_v;

    s_bx_q8 += s_vx_q8;
    s_by_q8 += s_vy_q8;

    /* Walls */
    int min_q8 = (BALL_R) << 8;
    int max_x_q8 = (PLAY_W - BALL_R) << 8;
    int max_y_q8 = (PLAY_H - BALL_R) << 8;
    if (s_bx_q8 < min_q8) { s_bx_q8 = min_q8; s_vx_q8 = -s_vx_q8 / 2; }
    if (s_bx_q8 > max_x_q8) { s_bx_q8 = max_x_q8; s_vx_q8 = -s_vx_q8 / 2; }
    if (s_by_q8 < min_q8) { s_by_q8 = min_q8; s_vy_q8 = -s_vy_q8 / 2; }
    if (s_by_q8 > max_y_q8) { s_by_q8 = max_y_q8; s_vy_q8 = -s_vy_q8 / 2; }

    int bx = s_bx_q8 >> 8;
    int by = s_by_q8 >> 8;
    lv_obj_set_pos(s_ball, bx - BALL_R, by - BALL_R);

    /* Coin pickup */
    for (int i = 0; i < N_COINS; i++) {
        if (!s_coins[i].alive) continue;
        if (dist2(bx, by, s_coins[i].x, s_coins[i].y) <= (BALL_R + COIN_R) * (BALL_R + COIN_R)) {
            s_coins[i].alive = false;
            s_score++;
            refresh_score();
            refresh_sprites();
            /* When all coins eaten, spawn a fresh batch (longer runs). */
            int alive_count = 0;
            for (int j = 0; j < N_COINS; j++) if (s_coins[j].alive) alive_count++;
            if (alive_count == 0) {
                spawn_sprites();
                refresh_sprites();
            }
        }
    }

    /* Hole detection — closer center-to-center than HOLE_R = swallowed. */
    for (int i = 0; i < N_HOLES; i++) {
        if (!s_holes[i].alive) continue;
        if (dist2(bx, by, s_holes[i].x, s_holes[i].y) <= HOLE_R * HOLE_R) {
            s_dead = true;
            show_game_over();
            return;
        }
    }
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    s_rng = hb_time_uptime_us() | 1u;
   
    // we dont want the device to fall asleep while playing.
    // seems to get reset when going to the homescreen.
    hb_wake_lock(true);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_score_lbl = lv_label_create(scr);
    lv_label_set_text(s_score_lbl, "Coins: 0");
    lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_score_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(s_score_lbl, LV_ALIGN_TOP_MID, 0, 16);

    s_play = lv_obj_create(scr);
    lv_obj_set_size(s_play, PLAY_W, PLAY_H);
    lv_obj_align(s_play, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_play, lv_color_hex(0x111522), 0);
    lv_obj_set_style_border_width(s_play, 0, 0);
    lv_obj_set_style_radius(s_play, 0, 0);
    lv_obj_set_style_pad_all(s_play, 0, 0);
    lv_obj_clear_flag(s_play, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_play, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < N_COINS; i++) {
        s_coin_objs[i] = lv_obj_create(s_play);
        lv_obj_set_size(s_coin_objs[i], COIN_R * 2, COIN_R * 2);
        lv_obj_set_style_radius(s_coin_objs[i], COIN_R, 0);
        lv_obj_set_style_bg_color(s_coin_objs[i], lv_color_hex(0xfcbf49), 0);
        lv_obj_set_style_border_width(s_coin_objs[i], 0, 0);
        lv_obj_clear_flag(s_coin_objs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_coin_objs[i], LV_OBJ_FLAG_CLICKABLE);
    }
    for (int i = 0; i < N_HOLES; i++) {
        s_hole_objs[i] = lv_obj_create(s_play);
        lv_obj_set_size(s_hole_objs[i], HOLE_R * 2, HOLE_R * 2);
        lv_obj_set_style_radius(s_hole_objs[i], HOLE_R, 0);
        lv_obj_set_style_bg_color(s_hole_objs[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_color(s_hole_objs[i], lv_color_hex(hb_color_danger()), 0);
        lv_obj_set_style_border_width(s_hole_objs[i], 2, 0);
        lv_obj_clear_flag(s_hole_objs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_hole_objs[i], LV_OBJ_FLAG_CLICKABLE);
    }

    s_ball = lv_obj_create(s_play);
    lv_obj_set_size(s_ball, BALL_R * 2, BALL_R * 2);
    lv_obj_set_style_radius(s_ball, BALL_R, 0);
    lv_obj_set_style_bg_color(s_ball, lv_color_hex(hb_color_success()), 0);
    lv_obj_set_style_border_width(s_ball, 0, 0);
    lv_obj_clear_flag(s_ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ball, LV_OBJ_FLAG_CLICKABLE);

    /* Game-over overlay (clickable to retry). */
    s_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_overlay, 200, 130);
    lv_obj_center(s_overlay);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_border_width(s_overlay, 2, 0);
    lv_obj_set_style_radius(s_overlay, 12, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, on_overlay_tap, LV_EVENT_CLICKED, NULL);
    s_overlay_lbl = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(s_overlay_lbl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_overlay_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_overlay_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_overlay_lbl);

    reset_round();
    hb_lv_set_frame_cb(on_frame);
}
