/*
 * Free RAM (from nano7kit by Shimku, MIT): how much of the OS heap the next app
 * will find, and the music player that holds part of it.
 *
 * Shows the largest free block and the total free in the OS heap twice a
 * second, the player state, and three actions:
 *   Pause music  the SDK's pause (hb_media_set_paused), as Remote does.
 *   Stop music   the playback worker's queued "stop" (hb_media.c A_TASK_STOP,
 *                defined in the SDK but never called by it: a hardware test).
 *   Refresh      measure again (for example after turning Bluetooth off in
 *                Settings and coming back).
 * After an action it shows the largest block before, 1 s after and 3 s
 * after, since the player lets go of its buffers on its own task. Every
 * step goes to the trace (FRAM*), so ./start trace shows it after a crash.
 * the last four actions stay listed with what each one freed
 * (largest block and total, before and 3 s after), and a line gives the
 * biggest app image the loader would accept right now.
 *
 * Nothing here writes files, settings or NAND.
 */
#include "hb_sdk.h"
#include "hb_heap.h"
#include "lvgl/lvgl.h"

#define A_TASK_GET  0x0841bfb4u
#define A_TASK_STOP 0x083bf040u

/* The loader refuses an image unless the largest block covers its span plus
 * this margin (HB_RELOC_HEAP_MARGIN in sdk/hb_reloc_file.c). */
#define LOAD_MARGIN (256u << 10)
#define HISTORY 4

static lv_obj_t *s_largest, *s_free, *s_music, *s_result, *s_verdict, *s_hist[HISTORY];
static char s_hist_text[HISTORY][64];
static uint32_t s_last_ms, s_action_ms, s_before, s_free_before;
static const char *s_action;
static int s_step;

static const char *state_text(int st)
{
    switch (st) {
        case 0:  return "Music: playing";
        case 1:  return "Music: paused";
        case 2:  return "Music: stopped";
        default: return "Music: no player";
    }
}

static void refresh(void)
{
    uint32_t largest = hb_os_heap_largest(), total = hb_os_heap_free();
    lv_label_set_text_fmt(s_largest, "Largest block %lu KB", (unsigned long)(largest / 1024u));
    lv_label_set_text_fmt(s_free, "Free in all %lu KB", (unsigned long)(total / 1024u));
    lv_label_set_text(s_music, state_text(hb_media_state()));
    if (largest > LOAD_MARGIN)
        lv_label_set_text_fmt(s_verdict, "Fits an app up to %lu KB",
                              (unsigned long)((largest - LOAD_MARGIN) / 1024u));
    else
        lv_label_set_text(s_verdict, "No room to load an app");
}

/* Newest first: "Stop 2048>2048 / 4904>5045" (largest, then all, in KB). */
static void remember(const char *what, uint32_t big0, uint32_t big1, uint32_t all0, uint32_t all1)
{
    int i;
    for (i = HISTORY - 1; i > 0; i--)
        lv_snprintf(s_hist_text[i], sizeof s_hist_text[i], "%s", s_hist_text[i - 1]);
    lv_snprintf(s_hist_text[0], sizeof s_hist_text[0], "%s %lu>%lu / %lu>%lu", what,
                (unsigned long)(big0 / 1024u), (unsigned long)(big1 / 1024u),
                (unsigned long)(all0 / 1024u), (unsigned long)(all1 / 1024u));
    for (i = 0; i < HISTORY; i++)
        lv_label_set_text(s_hist[i], s_hist_text[i]);
}

static void start(const char *what)
{
    s_before = hb_os_heap_largest();
    s_free_before = hb_os_heap_free();
    s_action = what;
    s_action_ms = hb_time_uptime_ms();
    s_step = 1;
    lv_label_set_text_fmt(s_result, "%s: measuring...", what);
}

static void on_pause(lv_event_t *e)
{
    (void)e;
    hb_trace_log("FRAMPAU", hb_os_heap_largest(), (uint32_t)hb_media_state());
    start("Pause");
    hb_media_set_paused(true);
    refresh();
}

static void on_stop(lv_event_t *e)
{
    void *t;
    (void)e;
    hb_trace_log("FRAMSTP", hb_os_heap_largest(), (uint32_t)hb_media_state());
    start("Stop");
    t = ((void *(*)(void))(A_TASK_GET | 1u))();
    /* Called like pause (self, fade): a one-argument function ignores r1. */
    if (t)
        ((void (*)(void *, int))(A_TASK_STOP | 1u))(t, -1);
    hb_trace_log("FRAMST2", hb_os_heap_largest(), (uint32_t)hb_media_state());
    refresh();
}

static void on_refresh(lv_event_t *e)
{
    (void)e;
    hb_trace_log("FRAMREF", hb_os_heap_largest(), hb_os_heap_free());
    start("Refresh");
    refresh();
}

static void on_tick(void)
{
    uint32_t now = hb_time_uptime_ms();
    if (now - s_last_ms >= 500) {
        refresh();
        s_last_ms = now;
    }
    if (s_step && now - s_action_ms >= (s_step == 1 ? 1000u : 3000u)) {
        uint32_t after = hb_os_heap_largest();
        lv_label_set_text_fmt(s_result, "%s: %lu -> %lu KB after %d s", s_action,
                              (unsigned long)(s_before / 1024u), (unsigned long)(after / 1024u),
                              s_step == 1 ? 1 : 3);
        hb_trace_log("FRAMAFT", after, (uint32_t)s_step);
        if (s_step == 2) {
            hb_trace_log("FRAMDLT", after - s_before, hb_os_heap_free() - s_free_before);
            remember(s_action, s_before, after, s_free_before, hb_os_heap_free());
        }
        s_step = s_step == 1 ? 2 : 0;
    }
}

static lv_obj_t *label(lv_obj_t *scr, int y, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_width(l, 216);
    lv_obj_set_pos(l, 12, y);
    lv_label_set_text(l, "");
    return l;
}

static void button(lv_obj_t *scr, int y, uint32_t bg, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_t *l;
    lv_obj_set_size(b, 216, 48);
    lv_obj_set_pos(b, 12, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
}

HB_APP_ENTRY(payload_entry)
{
    lv_obj_t *scr, *title, *hint;
    hb_trace_init();
    hb_trace_log("FRAMUP", hb_os_heap_largest(), hb_os_heap_free());

    scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b1020), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    title = lv_label_create(scr);
    lv_label_set_text(title, "Free RAM");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    s_largest = label(scr, 56, &lv_font_montserrat_14, 0xa5f3fc);
    s_free = label(scr, 78, &lv_font_montserrat_14, 0xa5f3fc);
    s_music = label(scr, 100, &lv_font_montserrat_14, 0xffffff);
    s_result = label(scr, 122, &lv_font_montserrat_14, 0xfde68a);
    lv_label_set_long_mode(s_result, LV_LABEL_LONG_WRAP);

    button(scr, 176, 0x4338ca, "Pause music", on_pause);
    button(scr, 232, 0x7f1d1d, "Stop music (test)", on_stop);
    button(scr, 288, 0x334155, "Refresh", on_refresh);

    s_verdict = label(scr, 150, &lv_font_montserrat_14, 0xfecaca);
    {
        int i;
        for (i = 0; i < HISTORY; i++) {
            s_hist[i] = label(scr, 360 + i * 18, &lv_font_montserrat_14, i ? 0x94a3b8 : 0xe2e8f0);
            s_hist_text[i][0] = 0;
        }
        lv_label_set_text(label(scr, 342, &lv_font_montserrat_14, 0x67e8f9), "After 3 s, KB: largest / all");
    }

    refresh();
    s_last_ms = hb_time_uptime_ms();
    hb_lv_set_frame_cb(on_tick);
}
