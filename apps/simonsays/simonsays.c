/*
 * simon_says — memory game with 4 colored quadrants.
 *
 * Each round, the device "plays" a growing sequence by flashing one
 * pad at a time. The player taps them back in order. Mistakes end
 * the round; successfully reproducing the full sequence appends a
 * random pad and the next round starts. High score persisted.
 *
 * Pads: top-left green, top-right red, bottom-left yellow, bottom-
 * right blue. A pad "flashes" by brightening for ~250 ms then
 * fading back.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define MAX_SEQ      200
#define FLASH_MS     350u
#define BETWEEN_MS   180u
#define BEST_PATH    "/Data/SimonSays/best.txt"
#define DATA_DIR     "/Data/SimonSays"

static int     s_seq[MAX_SEQ];
static int     s_seq_len = 0;
static int     s_player_pos = 0;
static int     s_best = 0;

typedef enum {
    PHASE_INTRO,           /* "Tap to start" */
    PHASE_PLAYBACK,        /* device is showing the sequence */
    PHASE_PLAYER,          /* player is reproducing */
    PHASE_GAMEOVER,
} phase_t;
static phase_t s_phase = PHASE_INTRO;

/* During PHASE_PLAYBACK: which step are we on, and when does the
   current flash end / next start. */
static int      s_pb_step;
static int      s_pb_lit_pad;        /* -1 = waiting between flashes */
static uint32_t s_pb_next_ms;

static lv_obj_t *s_pads[4];
static lv_obj_t *s_status;
static lv_obj_t *s_score_lbl;

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

/* Pad colors: dim (resting) and lit (flash). */
static const uint32_t s_dim[4] = { 0x1a5c54, 0x8d1c25, 0xa07a23, 0x1d3557 };
static const uint32_t s_lit[4] = { 0x2bf3c9, 0xff4060, 0xfff070, 0x4dabff };

static void load_best(void)
{
    char buf[12];
    uint32_t n = hb_fs_read(BEST_PATH, buf, sizeof buf - 1);
    if (n == 0) return;
    buf[n] = 0;
    int v = 0;
    for (uint32_t i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) {
        v = v * 10 + (buf[i] - '0');
    }
    s_best = v;
}
static void save_best(void)
{
    char buf[12]; itoa_u((uint32_t)s_best, buf);
    int n = 0; while (buf[n]) n++;
    hb_fs_write(BEST_PATH, buf, (uint32_t)n);
}

static void refresh_score(void)
{
    char buf[40]; int k = 0;
    const char *p = "Round "; while (*p) buf[k++] = *p++;
    char nb[12]; itoa_u((uint32_t)s_seq_len, nb);
    for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    if (s_best > 0) {
        p = "  Best "; while (*p) buf[k++] = *p++;
        itoa_u((uint32_t)s_best, nb);
        for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    }
    buf[k] = 0;
    lv_label_set_text(s_score_lbl, buf);
}

static void set_pad(int idx, bool lit)
{
    lv_obj_set_style_bg_color(s_pads[idx],
        lv_color_hex(lit ? s_lit[idx] : s_dim[idx]), 0);
}

static void all_dim(void)
{
    for (int i = 0; i < 4; i++) set_pad(i, false);
}

static void start_round(void)
{
    s_seq[s_seq_len++] = (int)(rnd() % 4);
    s_phase = PHASE_PLAYBACK;
    s_pb_step = 0;
    s_pb_lit_pad = -1;
    s_pb_next_ms = hb_time_uptime_ms();
    s_player_pos = 0;
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xfff070), 0);
    lv_label_set_text(s_status, "Watch the sequence");
    refresh_score();
}

static void on_start(void)
{
    s_seq_len = 0;
    start_round();
    /* Clear stale game-over state. */
}

static void on_pad_press(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (s_phase == PHASE_INTRO || s_phase == PHASE_GAMEOVER) {
        on_start();
        return;
    }
    if (s_phase != PHASE_PLAYER) return;

    /* Brief lit feedback. We use the playback machinery: set lit_pad
       and an expiration time. The frame tick will dim it. */
    set_pad(idx, true);
    s_pb_lit_pad = idx;
    s_pb_next_ms = hb_time_uptime_ms() + 250;

    if (idx != s_seq[s_player_pos]) {
        s_phase = PHASE_GAMEOVER;
        if (s_seq_len - 1 > s_best) { s_best = s_seq_len - 1; save_best(); }
        lv_label_set_text(s_status, "Wrong! Tap to retry");
        refresh_score();
        return;
    }
    s_player_pos++;
    if (s_player_pos >= s_seq_len) {
        /* Round complete — append + replay. */
        lv_label_set_text(s_status, "Nice!");
        if (s_seq_len >= MAX_SEQ) {
            s_phase = PHASE_GAMEOVER;
            lv_label_set_text(s_status, "Max round! Tap to retry");
            return;
        }
        /* Brief delay before starting next round so the feedback flash
           gets a full beat. Dim the just-tapped pad first or it sticks lit
           through the next playback. */
        set_pad(idx, false);
        s_seq[s_seq_len++] = (int)(rnd() % 4);
        s_phase = PHASE_PLAYBACK;
        s_pb_step = 0;
        s_pb_lit_pad = -1;
        s_pb_next_ms = hb_time_uptime_ms() + 400;
        s_player_pos = 0;
        lv_label_set_text(s_status, "Watch...");
        refresh_score();
    }
}

static void on_frame(void)
{
    uint32_t now = hb_time_uptime_ms();
    if (s_phase == PHASE_PLAYBACK) {
        if ((int32_t)(now - s_pb_next_ms) < 0) return;
        if (s_pb_lit_pad >= 0) {
            /* Flash ended — dim, then schedule gap before next. */
            set_pad(s_pb_lit_pad, false);
            s_pb_lit_pad = -1;
            s_pb_next_ms = now + BETWEEN_MS;
            return;
        }
        if (s_pb_step >= s_seq_len) {
            s_phase = PHASE_PLAYER;
            lv_obj_set_style_text_color(s_status, lv_color_hex(0x4ec25c), 0);
            lv_label_set_text(s_status, "Your turn!");
            return;
        }
        int pad = s_seq[s_pb_step++];
        set_pad(pad, true);
        s_pb_lit_pad = pad;
        s_pb_next_ms = now + FLASH_MS;
    } else {
        /* PLAYER or GAMEOVER: dim the tap-feedback flash once its time is up, so a
           pad never sticks lit after a tap. */
        if (s_pb_lit_pad >= 0 && (int32_t)(now - s_pb_next_ms) >= 0) {
            set_pad(s_pb_lit_pad, false);
            s_pb_lit_pad = -1;
        }
    }
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    load_best();
    s_rng = hb_time_uptime_us() | 1u;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_score_lbl = lv_label_create(scr);
    lv_label_set_text(s_score_lbl, "Round 0");
    lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_score_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(s_score_lbl, LV_ALIGN_TOP_MID, 0, 6);

    s_status = lv_label_create(scr);
    lv_label_set_text(s_status, "Tap a pad to start");
    lv_obj_set_style_text_color(s_status, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* 2x2 grid of pads — taller so they fill the space down to the status line. */
    int pad_w = 108, pad_h = 172, gap = 8;
    int top_y = 40;
    int total_w = pad_w * 2 + gap;
    int start_x = (240 - total_w) / 2;
    for (int i = 0; i < 4; i++) {
        int col = i % 2, row = i / 2;
        lv_obj_t *b = lv_button_create(scr);
        lv_obj_set_size(b, pad_w, pad_h);
        lv_obj_set_pos(b, start_x + col * (pad_w + gap),
                          top_y + row * (pad_h + gap));
        lv_obj_set_style_bg_color(b, lv_color_hex(s_dim[i]), 0);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, on_pad_press, LV_EVENT_PRESSED,
                            (void *)(uintptr_t)i);
        s_pads[i] = b;
    }

    refresh_score();
    hb_lv_set_frame_cb(on_frame);
}
