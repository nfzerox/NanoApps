/*
 * rhythm — falling-note tap game (visuals only, no audio yet).
 *
 * 4 lanes; notes spawn at top and fall to a target line near the
 * bottom. Tap the lane while the note crosses the target zone:
 *   ±50ms  -> PERFECT (+100)
 *   ±150ms -> GOOD    (+50)
 *   else   -> MISS    (combo break)
 *
 * Built-in track is a hand-tuned 30-second sequence with a 480-BPM
 * pulse. Drop additional tracks at /Data/Rhythm/<name>.txt
 * (one note per line: "<time_ms> <lane>"; lane 0..3).
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define PLAY_W       240
#define PLAY_H       380
#define LANES        4
#define MAX_NOTES    300
#define TRAVEL_MS    1400      /* time from spawn to target */
#define TARGET_Y     320
#define LANE_W       (PLAY_W / LANES)
#define PERFECT_MS   50
#define GOOD_MS      150
#define FLASH_MS     180

typedef struct { uint32_t t_ms; uint8_t lane; uint8_t hit; } note_t;

static note_t s_notes[MAX_NOTES];
static int    s_n_notes = 0;
static int    s_next_idx;       /* next note that hasn't been judged */
static uint32_t s_t0;
static uint32_t s_track_len_ms;

static int s_score, s_combo, s_max_combo;
static uint32_t s_judge_label_until;
static const char *s_judge_label = "";

static uint32_t s_lane_flash_until[LANES];
static uint32_t s_lane_glow_until[LANES];

/* In the app's own arena: the old fixed 0x092E0000 lies inside the OS heap
 * and was overwriting live objects (and sometimes this app's own code). */
static uint8_t s_canvas_buf[PLAY_W * PLAY_H * 4] __attribute__((aligned(4)));
static lv_obj_t *s_canvas;
static lv_obj_t *s_score_lbl;
static lv_obj_t *s_combo_lbl;

/* ---- built-in track ---- */
static const uint16_t s_builtin_t[]    = {
     500, 1000, 1500, 2000,
     2500, 2750, 3000, 3250, 3500, 4000,
     4500, 5000, 5500, 5750, 6000,
     6500, 7000, 7500, 7750, 8000, 8250, 8500,
     9000, 9500, 10000, 10250, 10500,
     11000, 11500, 12000, 12250, 12500, 12750, 13000,
     13500, 14000, 14500,
     15000, 15250, 15500, 15750,
     16000, 16500, 17000, 17500,
     18000, 18250, 18500, 18750, 19000,
     19500, 20000, 20500,
     21000, 21250, 21500, 21750, 22000,
     22500, 23000, 23500, 24000,
     24500, 25000, 25500, 25750, 26000, 26250, 26500,
     27000, 27500, 28000, 28250, 28500, 29000, 29500, 30000
};
static const uint8_t s_builtin_l[] = {
    0, 1, 2, 3,
    0, 1, 0, 1, 2, 3,
    0, 2, 1, 2, 3,
    0, 1, 2, 1, 0, 1, 2,
    3, 2, 1, 0, 3,
    0, 2, 1, 3, 0, 2, 1,
    3, 0, 2,
    1, 2, 1, 0,
    3, 0, 1, 2,
    3, 2, 1, 0, 1,
    2, 3, 0,
    1, 2, 3, 2, 1,
    0, 1, 2, 3,
    0, 2, 1, 2, 0, 1, 3,
    0, 1, 2, 3, 0, 2, 1, 3
};

#define TRACK_DIR "/Data/Rhythm"

/* If /Data/Rhythm/track.txt exists, load it. Each line is
 * "<time_ms> <lane>" with whitespace separators. Returns true if a
 * track was loaded. */
static bool load_custom_track(void)
{
    char buf[8192];
    uint32_t n = hb_fs_read(TRACK_DIR "/track.txt", buf, sizeof buf - 1);
    if (n == 0) return false;
    buf[n] = 0;
    s_n_notes = 0;
    uint32_t i = 0;
    uint32_t last_t = 0;
    while (i < n && s_n_notes < MAX_NOTES) {
        while (i < n && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) i++;
        if (i >= n) break;
        if (buf[i] == '#') { while (i < n && buf[i] != '\n') i++; continue; }
        uint32_t t = 0; int got = 0;
        while (i < n && buf[i] >= '0' && buf[i] <= '9') { t = t * 10 + (buf[i] - '0'); i++; got = 1; }
        if (!got) { while (i < n && buf[i] != '\n') i++; continue; }
        while (i < n && (buf[i] == ' ' || buf[i] == '\t')) i++;
        int lane = 0;
        while (i < n && buf[i] >= '0' && buf[i] <= '9') { lane = lane * 10 + (buf[i] - '0'); i++; }
        while (i < n && buf[i] != '\n') i++;
        s_notes[s_n_notes].t_ms = t;
        s_notes[s_n_notes].lane = (uint8_t)(lane % LANES);
        s_notes[s_n_notes].hit = 0;
        if (t > last_t) last_t = t;
        s_n_notes++;
    }
    if (s_n_notes == 0) return false;
    s_track_len_ms = last_t + 3000;
    return true;
}

static void load_builtin(void)
{
    if (load_custom_track()) return;
    int n = sizeof(s_builtin_t) / sizeof(s_builtin_t[0]);
    int m = sizeof(s_builtin_l) / sizeof(s_builtin_l[0]);
    if (m < n) n = m;
    if (n > MAX_NOTES) n = MAX_NOTES;
    for (int i = 0; i < n; i++) {
        s_notes[i].t_ms = s_builtin_t[i];
        s_notes[i].lane = s_builtin_l[i] % LANES;
        s_notes[i].hit  = 0;
    }
    s_n_notes = n;
    s_track_len_ms = s_notes[n - 1].t_ms + 3000;
}

/* ---- drawing ---- */
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

static const uint32_t s_lane_colors[LANES] = {
    0xff595e, 0xffca3a, 0x8ac926, 0x1982c4
};

static void render(uint32_t now)
{
    cfill(0, 0, PLAY_W, PLAY_H, pack(0x0a0e1a));
    /* lane separators */
    for (int i = 1; i < LANES; i++) {
        cfill(i * LANE_W - 1, 0, 1, PLAY_H, pack(0x1f2738));
    }
    /* target zone */
    cfill(0, TARGET_Y - 3, PLAY_W, 6, pack(0x2a3550));
    cfill(0, TARGET_Y, PLAY_W, 1, pack(0xc8d3df));

    /* lane pads at bottom (touch hints) */
    for (int i = 0; i < LANES; i++) {
        uint32_t base = s_lane_colors[i];
        uint32_t shade = pack(base & 0x404040);
        int alpha_lift = 0;
        if (now < s_lane_flash_until[i]) shade = pack(base);
        else if (now < s_lane_glow_until[i]) shade = pack(((base >> 1) & 0x7f7f7f));
        (void)alpha_lift;
        cfill(i * LANE_W + 4, TARGET_Y + 8, LANE_W - 8, 40, shade);
    }

    /* notes */
    for (int i = 0; i < s_n_notes; i++) {
        if (s_notes[i].hit) continue;
        int ms_until = (int)s_notes[i].t_ms - (int)(now - s_t0);
        if (ms_until > TRAVEL_MS) continue;          /* not spawned yet */
        if (ms_until < -GOOD_MS - 50) continue;      /* off-bottom */
        int y = TARGET_Y - (ms_until * TARGET_Y) / TRAVEL_MS;
        int lane = s_notes[i].lane;
        int x = lane * LANE_W + 8;
        int w = LANE_W - 16;
        cfill(x, y - 8, w, 16, pack(s_lane_colors[lane]));
        cfill(x, y - 8, w, 2, pack(0xffffff));
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
    b[k] = 0;
    lv_label_set_text(s_score_lbl, b);

    k = 0;
    if (s_combo > 0) {
        p = "Combo "; while (*p) b[k++] = *p++;
        itoa_u(s_combo, nb);
        for (int i = 0; nb[i]; i++) b[k++] = nb[i];
    } else if (s_judge_label[0]) {
        p = s_judge_label; while (*p) b[k++] = *p++;
    }
    b[k] = 0;
    lv_label_set_text(s_combo_lbl, b);
}

static void judge(int lane, uint32_t now)
{
    uint32_t track_ms = now - s_t0;
    int best = -1, best_diff = 1 << 30;
    for (int i = 0; i < s_n_notes; i++) {
        if (s_notes[i].hit) continue;
        if (s_notes[i].lane != lane) continue;
        int diff = (int)s_notes[i].t_ms - (int)track_ms;
        int ad = diff < 0 ? -diff : diff;
        if (ad < best_diff) { best_diff = ad; best = i; }
        if (diff > GOOD_MS) break;
    }
    s_lane_flash_until[lane] = now + 80;
    s_lane_glow_until[lane]  = now + FLASH_MS;
    if (best < 0 || best_diff > GOOD_MS) {
        s_judge_label = "MISS";
        s_judge_label_until = now + 500;
        s_combo = 0;
    } else {
        s_notes[best].hit = 1;
        if (best_diff <= PERFECT_MS) {
            s_score += 100;
            s_judge_label = "PERFECT";
        } else {
            s_score += 50;
            s_judge_label = "GOOD";
        }
        s_judge_label_until = now + 400;
        s_combo++;
        if (s_combo > s_max_combo) s_max_combo = s_combo;
    }
    refresh_hud();
}

static void on_press(lv_event_t *e)
{
    (void)e;
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    lv_area_t a; lv_obj_get_coords(s_canvas, &a);
    int rx = p.x - a.x1;
    if (rx < 0 || rx >= PLAY_W) return;
    int lane = rx / LANE_W;
    if (lane < 0) lane = 0; else if (lane >= LANES) lane = LANES - 1;
    judge(lane, hb_time_uptime_ms());
}

static void on_frame(void)
{
    uint32_t now = hb_time_uptime_ms();
    /* auto-miss notes that have passed the target window */
    uint32_t track_ms = now - s_t0;
    for (int i = s_next_idx; i < s_n_notes; i++) {
        if (s_notes[i].hit) continue;
        if ((int)track_ms - (int)s_notes[i].t_ms > GOOD_MS) {
            s_notes[i].hit = 2;        /* missed */
            s_combo = 0;
            s_judge_label = "MISS";
            s_judge_label_until = now + 300;
            s_next_idx = i + 1;
            refresh_hud();
        } else break;
    }
    /* loop track */
    if (track_ms > s_track_len_ms) {
        s_t0 = now;
        s_next_idx = 0;
        for (int i = 0; i < s_n_notes; i++) s_notes[i].hit = 0;
    }
    if (s_judge_label[0] && now >= s_judge_label_until) {
        s_judge_label = "";
        refresh_hud();
    }
    render(now);
    lv_obj_invalidate(s_canvas);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    load_builtin();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(hb_color_bg()), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Rhythm");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 4);

    s_score_lbl = lv_label_create(scr);
    lv_label_set_text(s_score_lbl, "Score 0");
    lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_align(s_score_lbl, LV_ALIGN_TOP_RIGHT, -6, 4);

    s_combo_lbl = lv_label_create(scr);
    lv_label_set_text(s_combo_lbl, "");
    lv_obj_set_style_text_color(s_combo_lbl, lv_color_hex(hb_color_primary()), 0);
    lv_obj_align(s_combo_lbl, LV_ALIGN_TOP_RIGHT, -6, 22);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, PLAY_W, PLAY_H,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(s_canvas, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_press, LV_EVENT_PRESSED, NULL);

    s_t0 = hb_time_uptime_ms();
    s_next_idx = 0;

    hb_lv_set_frame_cb(on_frame);
}
