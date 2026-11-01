/*
 * ui.c — the Radio+ interface.
 *
 * 240 x 432. A tall narrow portrait, which decides the whole layout: the
 * frequency takes the top because it is what you glance at, the station and its
 * radio text take the middle because that is what you actually read, and the
 * transport sits at the bottom where a thumb reaches without moving the hand.
 *
 * Rules the screens keep to:
 *
 *   Flat fills and hairlines. No gradients, no shadows, no rounded panels
 *   pretending to be physical. A 240-wide screen has no room to spend on
 *   decoration, and the iPod's own interface does not either.
 *
 *   Colour means something. Cyan is signal, violet is RDS, amber is a traffic
 *   announcement, rose is recording, green is the live buffer. Nothing is
 *   coloured to look nice; if a thing is coloured it is because its state
 *   matters, and the rest is grey.
 *
 *   Nothing smaller than 44 px is tappable, and 12 px of margin everywhere.
 *
 *   The frequency never moves. It is the one element that must be findable
 *   without looking, so it keeps its position and its size whatever else the
 *   screen is doing - no reflowing to make room for a station name.
 *
 * Everything drawn comes from rp_model and nothing else, so the same screens
 * render on a desktop with no tuner attached.
 */

#include "ui.h"
#include "../../apps/build_stamp.h"
#include "model.h"

#include "lvgl/lvgl.h"

#include "core/affollow.h"
#include "core/ctscan.h"
#include "core/fmreg.h"
#include "core/rdsname.h"
#include "core/scan.h"
#include "core/timer.h"

/* ---- palette -------------------------------------------------------------- */

#define C_BG        0x08090D
#define C_SURFACE   0x101219
#define C_SURFACE_2 0x171B25
#define C_HAIRLINE  0x232937
#define C_TEXT      0xEDEFF4
#define C_TEXT_DIM  0x8B92A0
#define C_TEXT_MUTE 0x545B69

#define C_SIGNAL    0x22D3EE   /* signal, stereo lock */
#define C_RDS       0xA78BFA   /* anything decoded from RDS */
#define C_REC       0xF43F5E   /* recording */
#define C_TA        0xF59E0B   /* traffic announcement */
#define C_LIVE      0x34D399   /* the live buffer */

#define F_FREQ    (&lv_font_montserrat_48)
#define F_STATION (&lv_font_montserrat_24)
#define F_UNIT    (&lv_font_montserrat_20)
#define F_TITLE   (&lv_font_montserrat_20)
#define F_BODY    (&lv_font_montserrat_16)
#define F_CAPTION (&lv_font_montserrat_14)

#define MARGIN    12
#define CONTENT_W (RP_SCREEN_W - 2 * MARGIN)
#define TAP_MIN   44

/* The chip row's two magic numbers, named because rp_ui_pill_row_slack()
   has to use exactly the same ones the layout does. */
#define PILL_PAD_H 7
#define PILL_GAP   6

/* ---- state ---------------------------------------------------------------- */

static lv_obj_t *s_screen[RP_SCREEN_COUNT];
static rp_screen_t s_current = RP_SCREEN_NOW;

/* Now Playing */
static lv_obj_t *s_freq, *s_unit, *s_ps, *s_pty, *s_rt;
static lv_obj_t *s_seg[14], *s_rssi_lbl, *s_stereo, *s_ta_badge;
static lv_obj_t *s_af_badge;

/* The band scan's state. Up here with the other cross-screen state rather
   than beside the scan code: the dial draws the band profile it collects, and
   the dial is built earlier in this file. */
static en_scan_t s_scan;
static lv_obj_t *s_w_title, *s_w_artist;

/* Show or hide, in one call. There is no such helper in this file yet and
   three of these read better than six flag calls. */
static void show(lv_obj_t *o, bool on)
{
    if (!o) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *s_live_fill, *s_live_lbl, *s_rec_dot, *s_rec_lbl;
static lv_obj_t *s_band_lbl, *s_rec_btn_lbl, *s_tl_lbl, *s_tr_lbl;
static lv_obj_t *s_clock;
static lv_obj_t *s_keep_btn, *s_keep_lbl;
static lv_obj_t *s_mute_btn, *s_mute_lbl;

/* the simple screen */
static lv_obj_t *s_simple_grid, *s_simple_empty;
static lv_obj_t *s_set_simple, *s_set_wide;
static lv_obj_t *s_preset_note;

/* Forward declarations, because a control on one screen now changes another:
   flagging a preset repaints the simple screen, and turning a screen on
   repaints the settings row and the page dots. */
static void refresh_simple(void);
static void refresh_presets(void);
static void refresh_settings(void);
static void refresh_dots(void);
static void rebuild_order(void);

/*
 * Screens are built when they are first looked at, not all at once.
 *
 * rp_ui_build_all used to construct all eleven before the interface opened,
 * and on this hardware that is twenty to thirty seconds of a boot screen with
 * nothing happening on it - long enough to read as a hang, and it was
 * reported as one twice: once as "sits on tuner init forever" and once, after
 * the label was corrected, as "sits on ready". The label was never the
 * problem.
 *
 * Most of those screens are never opened in a given session. Now Playing is
 * built at startup because it is what opens; the rest cost nothing until
 * somebody swipes to them, and the one that is being swiped to is built
 * during the same gesture that asks for it.
 */
static bool s_built[RP_SCREEN_COUNT];
static void ensure_built(rp_screen_t w);

/* the landscape readout */
static lv_obj_t *s_w_freq, *s_w_unit, *s_w_clock, *s_w_date;
static lv_obj_t *s_w_ps, *s_w_pty, *s_w_rt, *s_w_state;
static lv_obj_t *s_w_seg[20];
static lv_obj_t *s_live_btn, *s_live_head, *s_bar_row;
/* One row of dots per swipe screen, because each screen owns its own copy.
   Indexed [screen][dot]. Held in a table rather than found by walking a
   screen's children looking for small objects, which is what this used to do:
   the landscape readout has twenty 12 x 8 signal segments and that heuristic
   was one layout change away from painting page dots onto a meter. */
static lv_obj_t *s_dots[RP_SWIPE_MAX][RP_SWIPE_MAX];

/* Dial */
static lv_obj_t *s_dial_freq, *s_dial_grid, *s_dial_note;

/* Presets, library, settings */
static lv_obj_t *s_preset_list, *s_library_list;
static lv_obj_t *s_set_af, *s_set_af_note, *s_set_af_save;
static lv_obj_t *s_set_output, *s_set_output_note;
static lv_obj_t *s_set_vol, *s_set_vol_val;
static lv_obj_t *s_set_hp, *s_set_hp_val;
static lv_obj_t *s_vol_overlay, *s_vol_bar, *s_vol_txt;
static uint8_t   s_vol_last = 0xFF;    /* 0xFF: nothing seen yet */
static uint32_t  s_vol_until;
#define VOL_SHOW_MS 1500u
static lv_obj_t *s_set_play;
static lv_obj_t *s_set_region, *s_set_std, *s_set_backend, *s_set_capture,
                *s_set_ta;
static lv_obj_t *s_adv_list;

/* The register being edited, and its live payload. */
static const en_fm_reg_t *s_reg;
static uint8_t s_reg_val[8];
static lv_obj_t *s_reg_title, *s_reg_name, *s_reg_doc, *s_reg_body,
                *s_reg_hex, *s_reg_revert;

/* ---- small builders ------------------------------------------------------- */

/* A flat panel: no border, no radius, no shadow. Everything in this interface
   is one of these or a label on one. */
static void flat(lv_obj_t *o, uint32_t colour)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h,
                       uint32_t colour)
{
    lv_obj_t *o = lv_obj_create(parent);
    flat(o, colour);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

static lv_obj_t *hairline(lv_obj_t *parent, int y)
{
    return panel(parent, MARGIN, y, CONTENT_W, 1, C_HAIRLINE);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text,
                       const lv_font_t *font, uint32_t colour)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    return l;
}

/* A label that wraps. LVGL will not wrap without an explicit width, and a label
   that silently runs off the right edge is the single easiest mistake to make
   on a screen this narrow - so paragraphs go through here. */
static lv_obj_t *para(lv_obj_t *parent, const char *text,
                      const lv_font_t *font, uint32_t colour, int w)
{
    lv_obj_t *l = label(parent, text, font, colour);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

/* A pill: small, uppercase, coloured text on a dark chip. Used for STEREO, RDS,
   TRAFFIC - states worth noticing but not worth a whole row. */
static lv_obj_t *pill(lv_obj_t *parent, int x, int y, const char *text,
                      uint32_t colour)
{
    lv_obj_t *p = lv_obj_create(parent);
    flat(p, C_SURFACE_2);
    lv_obj_set_style_radius(p, 3, 0);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, LV_SIZE_CONTENT, 20);
    lv_obj_set_style_pad_hor(p, PILL_PAD_H, 0);

    lv_obj_t *l = label(p, text, F_CAPTION, colour);
    lv_obj_center(l);
    return p;
}

static void pill_set(lv_obj_t *p, bool on, uint32_t colour)
{
    if (!p) return;
    lv_obj_t *l = lv_obj_get_child(p, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(on ? C_SURFACE_2 : C_SURFACE), 0);
    if (l)
        lv_obj_set_style_text_color(l, lv_color_hex(on ? colour : C_TEXT_MUTE), 0);
}

/* A full-width tappable row. The whole row is the target, not just the text -
   44 px tall, which is the smallest thing a finger hits reliably. */
static lv_obj_t *row(lv_obj_t *parent, int y, int h, lv_event_cb_t cb,
                     void *user)
{
    lv_obj_t *r = panel(parent, 0, y, RP_SCREEN_W, h, C_BG);
    if (cb) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, user);
        lv_obj_set_style_bg_color(r, lv_color_hex(C_SURFACE), LV_STATE_PRESSED);
    }
    return r;
}

/* ---- formatting ----------------------------------------------------------- */

static void put_uint(char *out, uint32_t v, int pad)
{
    char tmp[12];
    int n = 0, i = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10u); v /= 10u; }
    while (n < pad) tmp[n++] = '0';
    while (n) out[i++] = tmp[--n];
    out[i] = 0;
}

static void cat(char *dst, const char *src, int cap)
{
    int i = 0;
    while (dst[i] && i < cap - 1) i++;
    while (*src && i < cap - 1) dst[i++] = *src++;
    dst[i] = 0;
}

/* 98500 -> "98.5". One decimal is what a dial shows and what people say. */
static void fmt_mhz(char *out, int cap, uint32_t khz)
{
    char a[12], b[4];
    put_uint(a, khz / 1000u, 0);
    put_uint(b, (khz % 1000u) / 100u, 0);
    out[0] = 0;
    cat(out, a, cap);
    cat(out, ".", cap);
    cat(out, b, cap);
}

/* Milliseconds to m:ss, which is how long a recording or a buffer ever is. */
/* The station's clock as HH:MM, or empty when no 4A group has arrived. Note
   what is NOT done here: nothing is inferred from the device's own clock. A
   blank means the station sends no time, and showing the device's time in its
   place would be a different fact wearing this one's clothes. */
static void fmt_ct(char *out, int cap, const en_rds_t *r)
{
    out[0] = 0;
    if (!r->ct_valid) return;
    put_uint(out, r->ct_hour, 2);
    cat(out, ":", cap);
    char m[8];
    put_uint(m, r->ct_minute, 2);
    cat(out, m, cap);
}

static void fmt_time(char *out, int cap, uint32_t ms)
{
    char a[12], b[4];
    uint32_t s = ms / 1000u;
    put_uint(a, s / 60u, 0);
    put_uint(b, s % 60u, 2);
    out[0] = 0;
    cat(out, a, cap);
    cat(out, ":", cap);
    cat(out, b, cap);
}

/* ---- navigation ----------------------------------------------------------- */

/*
 * The swipe order, built at run time because two of its screens are optional.
 *
 * Kept as an explicit list rather than as "the enum order, skipping the ones
 * that are off": the position of a screen in the sequence is what the dots
 * count and what a gesture steps through, and deriving that twice from two
 * different rules is how they come to disagree.
 */
static rp_screen_t s_order[RP_SWIPE_MAX];
static int         s_order_n;

static void rebuild_order(void)
{
    s_order_n = 0;
    if (rp_model.simple_screen) s_order[s_order_n++] = RP_SCREEN_SIMPLE;
    s_order[s_order_n++] = RP_SCREEN_NOW;
    if (rp_model.wide_screen)   s_order[s_order_n++] = RP_SCREEN_WIDE;
    s_order[s_order_n++] = RP_SCREEN_DIAL;
    s_order[s_order_n++] = RP_SCREEN_PRESETS;
    s_order[s_order_n++] = RP_SCREEN_LIBRARY;
    s_order[s_order_n++] = RP_SCREEN_SETTINGS;
}

int rp_ui_swipe_count(void) { return s_order_n; }

rp_screen_t rp_ui_swipe_at(int i)
{
    if (i < 0 || i >= s_order_n) return RP_SCREEN_NOW;
    return s_order[i];
}

/* Where `s` sits in the sequence, or -1 if it is not in it. */
static int swipe_pos_of(rp_screen_t s)
{
    for (int i = 0; i < s_order_n; i++)
        if (s_order[i] == s) return i;
    return -1;
}

static void build_dots(int screen, lv_obj_t *parent)
{
    /* RP_SWIPE_MAX of them, always. How many are actually shown depends on
       settings and can change while the app runs, and creating them lazily
       would mean creating widgets during a screen change. The extras are
       hidden in refresh_dots. */
    const int gap = 14;
    for (int i = 0; i < RP_SWIPE_MAX; i++) {
        lv_obj_t *d = panel(parent, i * gap, RP_SCREEN_H - 18, 4, 4,
                            C_TEXT_MUTE);
        lv_obj_set_style_radius(d, 2, 0);
        s_dots[screen][i] = d;
    }
}

static void refresh_dots(void)
{
    const int gap = 14;
    const int n = s_order_n;
    const int x0 = (RP_SCREEN_W - (n - 1) * gap) / 2 - 2;
    const int here = swipe_pos_of(s_current);

    if ((int)s_current >= RP_SWIPE_MAX) return;
    lv_obj_t **row_dots = s_dots[(int)s_current];

    for (int i = 0; i < RP_SWIPE_MAX; i++) {
        if (!row_dots[i]) continue;
        if (i >= n) { lv_obj_add_flag(row_dots[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(row_dots[i], LV_OBJ_FLAG_HIDDEN);

        bool on = (i == here);
        /* Re-centred every time, because the row narrows when a screen is
           turned off and dots left where they were would sit off to one side. */
        lv_obj_set_pos(row_dots[i], x0 + i * gap, RP_SCREEN_H - 18);
        lv_obj_set_style_bg_color(row_dots[i],
                                  lv_color_hex(on ? C_TEXT : C_TEXT_MUTE), 0);
        lv_obj_set_size(row_dots[i], on ? 6 : 4, on ? 6 : 4);
    }
}

static void on_gesture(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());

    int at = swipe_pos_of(s_current);
    if (at < 0) return;          /* not a swipe screen; it has its own way out */

    /* Left and right move through the sequence; the register screens are
       excluded because they belong to settings rather than to it. */
    if (dir == LV_DIR_LEFT)  at++;
    else if (dir == LV_DIR_RIGHT) at--;
    else return;

    if (at < 0) at = 0;
    if (at > s_order_n - 1) at = s_order_n - 1;
    rp_ui_show(rp_ui_swipe_at(at));
}

/* ---- the simple screen ---------------------------------------------------
 *
 * Six buttons and nothing else. Not a cut-down version of Now Playing - a
 * different answer to a different question. Everything else in this app is for
 * finding out what the radio is doing; this is for pressing the station you
 * always press without reading anything.
 *
 * Which six is the user's choice, made on the Presets screen, and it is a
 * choice rather than "the first six" precisely because the interesting
 * presets are rarely the lowest frequencies.
 */

static void on_simple_pick(lv_event_t *e)
{
    rp_act_tune((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

static void build_simple(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_SIMPLE];

    lv_obj_t *cap = label(s, "PRESETS", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(cap, MARGIN, 10);
    hairline(s, 32);

    s_simple_grid = panel(s, 0, 40, RP_SCREEN_W, RP_SCREEN_H - 66, C_BG);

    s_simple_empty = para(s, "", F_BODY, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(s_simple_empty, MARGIN, 150);
}

static void refresh_simple(void)
{
    /* Not built yet - see ensure_built. */
    if (!s_built[RP_SCREEN_SIMPLE]) return;
    lv_obj_clean(s_simple_grid);

    const en_preset_t *pick[EN_SIMPLE_MAX];
    uint8_t n = en_presets_simple(&rp_model.presets, pick, EN_SIMPLE_MAX);

    if (!n) {
        lv_label_set_text(s_simple_empty,
            "No stations chosen yet.\n\n"
            "On the Presets screen, tap the dot beside a station to put it "
            "here.");
        return;
    }
    lv_label_set_text(s_simple_empty, "");

    /* Two columns of 104, three rows of 100. Every target is far larger than
       the 44 px minimum, which is the entire point of the screen. */
    const int bw = 104, bh = 100, gap = 8;
    char buf[64];

    for (uint8_t i = 0; i < n; i++) {
        int col = i % 2, rowi = i / 2;
        int x = MARGIN + col * (bw + gap);
        int y = 8 + rowi * (bh + gap);

        bool tuned = (pick[i]->khz == rp_model.khz);

        lv_obj_t *b = panel(s_simple_grid, x, y, bw, bh,
                            tuned ? C_SURFACE_2 : C_SURFACE);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, on_simple_pick, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)pick[i]->khz);

        /* The tuned one is marked by a bar rather than only by its fill: a
           slightly lighter panel is not a difference you can see across a
           room, which is the distance this screen is designed for. */
        if (tuned) panel(b, 0, 0, bw, 3, C_SIGNAL);

        fmt_mhz(buf, sizeof buf, pick[i]->khz);
        lv_obj_t *f = label(b, buf, F_STATION, C_TEXT);
        lv_obj_set_style_text_align(f, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(f, bw);
        lv_obj_set_pos(f, 0, 22);

        lv_obj_t *nm = label(b, pick[i]->name[0] ? pick[i]->name : "",
                             F_CAPTION, C_RDS);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_size(nm, bw, 20);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(nm, 0, 58);
    }
}

/* ---- the landscape readout -----------------------------------------------
 *
 * Turn the device counter-clockwise - home button to the right - and this
 * reads the long way: the frequency and the station's own clock across the
 * top, its name under them, and the radio text given the whole width, which
 * is the one thing a 240 px portrait column can never do well. Sixty-four
 * characters of radio text is four cramped lines there and two comfortable
 * ones here.
 *
 * The DISPLAY is not rotated. The framebuffer, the touch mapping and every
 * other screen are exactly as they were; only the items on this screen turn.
 * That is also the only affordable way to do it - rotating one full-screen
 * layer would need a 432 x 240 x 4 buffer, 414 KB out of a 640 KB LVGL heap -
 * and it means the cost is proportional to the text actually shown rather
 * than to the whole panel.
 *
 * Everything below is positioned in LANDSCAPE coordinates: x along the long
 * edge, y across it, origin at the top-left as you hold it turned. The two
 * helpers convert. Getting that conversion wrong in one place and right in
 * another is the whole risk here, so nothing on this screen is positioned by
 * hand.
 */

#define WIDE_W RP_SCREEN_H      /* 432 across the long edge */
#define WIDE_H RP_SCREEN_W      /* 240 across the short one */

/* Landscape rect -> portrait rect. A rectangle turned ninety degrees is still
   an axis-aligned rectangle with its sides swapped, so panels need no
   transform at all - only text does. */
static void wide_box(int lx, int ly, int lw, int lh,
                     int *px, int *py, int *pw, int *ph)
{
    const int lcx = lx + lw / 2, lcy = ly + lh / 2;
    *pw = lh;
    *ph = lw;
    *px = RP_SCREEN_W - lcy - lh / 2;
    *py = lcx - lw / 2;
}

static lv_obj_t *wide_panel(lv_obj_t *parent, int lx, int ly, int lw, int lh,
                            uint32_t colour)
{
    int px, py, pw, ph;
    wide_box(lx, ly, lw, lh, &px, &py, &pw, &ph);
    return panel(parent, px, py, pw, ph, colour);
}

/* A label laid out in landscape and drawn turned. The object keeps its natural
   horizontal box - so wrapping, eliding and alignment all work normally - and
   only the drawing is rotated, about the box's own centre. */
static lv_obj_t *wide_label(lv_obj_t *parent, int lx, int ly, int lw, int lh,
                            const char *txt, const lv_font_t *f, uint32_t col)
{
    const int lcx = lx + lw / 2, lcy = ly + lh / 2;

    lv_obj_t *o = label(parent, txt, f, col);
    lv_obj_set_size(o, lw, lh);
    lv_obj_set_pos(o, RP_SCREEN_W - lcy - lw / 2, lcx - lh / 2);

    lv_obj_set_style_transform_pivot_x(o, lw / 2, 0);
    lv_obj_set_style_transform_pivot_y(o, lh / 2, 0);
    lv_obj_set_style_transform_rotation(o, 900, 0);   /* tenths of a degree */
    return o;
}

static void build_wide(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_WIDE];

    /* The frequency, given the height it can only have this way round. */
    s_w_freq = wide_label(s, 16, 14, 220, 54, "--.-", F_FREQ, C_TEXT);
    s_w_unit = wide_label(s, 16, 68, 220, 20, "MHz", F_CAPTION, C_TEXT_MUTE);

    /* The station's clock, from group 4A, at the same weight as the
       frequency - it is the other number you would look across a room to
       read. Blank when the station sends no time. */
    s_w_clock = wide_label(s, 250, 14, 166, 54, "", F_FREQ, C_RDS);
    lv_obj_set_style_text_align(s_w_clock, LV_TEXT_ALIGN_RIGHT, 0);

    s_w_date = wide_label(s, 250, 68, 166, 20, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(s_w_date, LV_TEXT_ALIGN_RIGHT, 0);

    wide_panel(s, 16, 94, WIDE_W - 32, 1, C_HAIRLINE);

    s_w_ps = wide_label(s, 16, 100, 240, 28, "", F_STATION, C_RDS);
    s_w_pty = wide_label(s, 260, 106, 156, 20, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_w_pty, LV_TEXT_ALIGN_RIGHT, 0);

    /*
     * Radio text, across the whole long edge and given three lines.
     *
     * Two at 16 pt was the first attempt and it clipped: 400 px of this face
     * holds about thirty-two characters, so a full 64-character message needs
     * more than two lines and the tail was simply cut off. Three lines of the
     * caption face hold the longest message the standard allows with room to
     * spare, which is the point of having the long edge at all.
     */
    s_w_rt = wide_label(s, 16, 134, WIDE_W - 32, 58, "", F_CAPTION, C_TEXT_DIM);
    lv_label_set_long_mode(s_w_rt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(s_w_rt, 3, 0);

    /*
     * RadioText+, when the station sends it: the title and the artist pulled
     * out of that same text as separate fields.
     *
     * Laid over the radio text rather than beside it. The two are the same
     * characters - RT+ is markers INTO the RT buffer, not a second message -
     * so showing both would be showing the same words twice, and the tidy
     * version is the one worth the long edge. The raw text comes back the
     * moment RT+ stops arriving.
     */
    s_w_title = wide_label(s, 16, 134, WIDE_W - 32, 30, "", F_STATION, C_TEXT);
    lv_label_set_long_mode(s_w_title, LV_LABEL_LONG_DOT);
    s_w_artist = wide_label(s, 16, 166, WIDE_W - 32, 24, "", F_BODY, C_RDS);
    lv_label_set_long_mode(s_w_artist, LV_LABEL_LONG_DOT);

    /* Signal, as plain rectangles - no transform needed for those. */
    for (int i = 0; i < 20; i++)
        s_w_seg[i] = wide_panel(s, 16 + i * 12, 206, 8, 12, C_SURFACE_2);

    s_w_state = wide_label(s, 280, 202, 136, 20, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(s_w_state, LV_TEXT_ALIGN_RIGHT, 0);
}

static void refresh_wide(void)
{
    /* Not built yet - see ensure_built. */
    if (!s_built[RP_SCREEN_WIDE]) return;
    char buf[96], n[16];

    fmt_mhz(buf, sizeof buf, rp_model.khz);
    lv_label_set_text(s_w_freq, rp_model.khz ? buf : "--.-");

    fmt_ct(buf, sizeof buf, &rp_model.rds);
    lv_label_set_text(s_w_clock, buf);

    /* The date comes from the same group as the time, so it is shown only
       when that group has arrived - never reconstructed from anything else. */
    buf[0] = 0;
    if (rp_model.rds.ct_valid) {
        put_uint(buf, rp_model.rds.ct_year, 0);
        cat(buf, "-", sizeof buf);
        put_uint(n, rp_model.rds.ct_month, 2);
        cat(buf, n, sizeof buf);
        cat(buf, "-", sizeof buf);
        put_uint(n, rp_model.rds.ct_day, 2);
        cat(buf, n, sizeof buf);
    }
    lv_label_set_text(s_w_date, buf);

    lv_label_set_text(s_w_ps, rp_model.rds.ps_valid ? rp_model.rds.ps : "");

    /* Tidy or raw, never both: RT+ is markers into the radio text, so showing
       the two together would be showing the same words twice. */
    bool tidy = rp_model.rds.rt_title_valid || rp_model.rds.rt_artist_valid;
    show(s_w_title, tidy);
    show(s_w_artist, tidy);
    show(s_w_rt, !tidy);
    if (tidy) {
        lv_label_set_text(s_w_title,
                          rp_model.rds.rt_title_valid ? rp_model.rds.rt_title
                                                      : "");
        lv_label_set_text(s_w_artist,
                          rp_model.rds.rt_artist_valid ? rp_model.rds.rt_artist
                                                       : "");
    }

    buf[0] = 0;
    if (rp_model.rds.pi_valid)
        cat(buf, en_rds_pty_name(rp_model.rds.pty, rp_model.rds.rbds),
            sizeof buf);
    lv_label_set_text(s_w_pty, buf);

    if (!rp_model.tuner_ok && rp_model.tuner_note) {
        lv_label_set_text(s_w_rt, rp_model.tuner_note);
        lv_obj_set_style_text_color(s_w_rt, lv_color_hex(C_TA), 0);
    } else {
        lv_label_set_text(s_w_rt, rp_model.rds.rt_valid ? rp_model.rds.rt : "");
        lv_obj_set_style_text_color(s_w_rt, lv_color_hex(C_TEXT_DIM), 0);
    }

    int lit = (int)((rp_model.rssi * 20u) / 96u);
    if (lit > 20) lit = 20;
    for (int i = 0; i < 20; i++) {
        uint32_t c = C_SURFACE_2;
        if (i < lit) c = (i < 5) ? C_TA : C_SIGNAL;
        lv_obj_set_style_bg_color(s_w_seg[i], lv_color_hex(c), 0);
    }

    /* One line for everything that is a state rather than a reading. */
    buf[0] = 0;
    if (rp_model.recording) cat(buf, "REC  ", sizeof buf);
    if (rp_model.rds.ta)    cat(buf, "TRAFFIC  ", sizeof buf);
    if (rp_model.stereo)    cat(buf, "STEREO", sizeof buf);
    lv_label_set_text(s_w_state, buf);
    lv_obj_set_style_text_color(s_w_state,
                                lv_color_hex(rp_model.recording ? C_REC
                                                                : C_TEXT_MUTE),
                                0);
}

/* ---- Now Playing ---------------------------------------------------------- */

/* True when the headphones are carrying the radio as it happens. Everything
   contextual on this screen turns on this one question. */
static bool at_live(void)
{
    return !rp_model.play_file && rp_model.behind_ms == 0;
}

static void on_left(lv_event_t *e)
{
    (void)e;
    if (at_live()) rp_act_seek(false);
    else rp_act_nudge(-15000);
}

static void on_right(lv_event_t *e)
{
    (void)e;
    if (at_live()) rp_act_seek(true);
    else rp_act_nudge(15000);
}

static void on_middle(lv_event_t *e)
{
    (void)e;
    if (at_live()) rp_act_record_toggle();
    else rp_act_pause_toggle();
}

static void on_go_live(lv_event_t *e) { (void)e; rp_act_play_live(); }

/* How long the KEEP button says it saved something, in refresh ticks. The
   model gains a recording in the library and nothing else changes on this
   screen, so without a word here the tap looks like it did nothing. */
static uint8_t s_kept_for;

static void on_keep(lv_event_t *e)
{
    (void)e;
    if (!rp_model.capture_ok || !rp_model.live_ms) return;
    rp_act_save_live(rp_model.live_ms);
    s_kept_for = 8;
}

/* Tapping the buffer bar seeks to that point, which is how every buffered
   radio behaves and saves inventing a control for entering the scrub. */
static void on_bar_tap(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);

    int32_t x = p.x - MARGIN;
    if (x < 0) x = 0;
    if (x > CONTENT_W) x = CONTENT_W;

    if (rp_model.play_file) {
        if (rp_model.play_len_ms)
            rp_act_nudge((int32_t)((int64_t)x * rp_model.play_len_ms / CONTENT_W)
                         - (int32_t)rp_model.play_pos_ms);
        return;
    }

    /* The bar runs oldest on the left to live on the right, so distance from
       the right edge is how far behind the tap is. */
    uint32_t span = rp_model.behind_max_ms;
    if (!span) return;
    uint32_t behind = (uint32_t)((int64_t)(CONTENT_W - x) * span / CONTENT_W);
    rp_act_nudge((int32_t)rp_model.behind_ms - (int32_t)behind);
}

/*
 * Tap the stereo pill to cycle auto -> mono -> stereo.
 *
 * Forced mono is the one worth having: a weak station is steadier in mono
 * than blending in and out of stereo every few seconds, and that judgement is
 * the listener's rather than the chip's.
 *
 * The pill then shows two things at once, deliberately. Its LABEL is the mode
 * that was asked for; its COLOUR stays driven by the chip's own "stereo
 * active" flag. Which way round the manual-select bit runs is read off the
 * bit's name in the register table rather than pinned down by the bring-up
 * sequence, so being able to ask for mono and see the pill stay lit is how
 * that gets found out - immediately, and by looking.
 */
/* ---- following the station ------------------------------------------------
 *
 * Pumped from the frame callback like the scan and the recording timer, so it
 * keeps working while the user is on another screen - which is the entire
 * point of a feature for driving out of range.
 */
static void af_pump(void)
{
    static uint32_t last_ms;
    if (!rp_model.af.enabled) { last_ms = lv_tick_get(); return; }

    uint32_t now = lv_tick_get();
    uint32_t dt = now - last_ms;
    last_ms = now;
    if (dt > 1000) dt = 0;      /* first tick, or a clock that jumped */

    uint32_t khz = 0;
    if (en_af_tick(&rp_model.af, dt, rp_model.khz, rp_model.rssi,
                   &rp_model.rds, &khz) == EN_AF_GOTO)
        rp_act_tune_quiet(khz);
}

/*
 * Tapping the badge turns following off.
 *
 * The badge is the control on purpose. The situation this feature can get
 * wrong is standing near a transmitter advertising frequencies that are not
 * what it says they are, and in that situation the thing showing you it is
 * happening should be the thing that stops it - not a switch three screens
 * away in a settings list.
 */
static void on_af_tap(lv_event_t *e)
{
    (void)e;
    rp_act_af_follow(!rp_model.af.enabled);
}

/*
 * Mute.
 *
 * This toggles the user's mute and nothing else. The band scan sets its own
 * squelch flag and the chip is silent while either is set, so a scan finishing
 * cannot unmute a radio that was silenced before it started - and this button
 * cannot un-squelch a sweep that is still running, which would be a control
 * lying about what it did.
 *
 * The glyph follows rp_model.muted rather than the audio, for the same reason.
 * During a scan the progress bar already says why nothing is coming out.
 */
static void on_mute_tap(lv_event_t *e)
{
    (void)e;
    rp_act_mute(!rp_model.muted);
}

static void on_stereo_tap(lv_event_t *e)
{
    (void)e;
    rp_act_stereo_mode((uint8_t)((rp_model.stereo_mode + 1u) % 3u));
}

/*
 * What the pill says, which is what was asked for rather than what is
 * happening - the colour and the on/off state carry the second question.
 *
 * "ST HOLD" and not "ST FORCED": forced stereo tells the chip to hold the
 * stereo decoder open instead of blending down to mono as the signal weakens,
 * which is what the shorter name actually describes. Two characters shorter
 * also keeps the widest possible row - this, TRAFFIC, and "AF WEAK" - inside
 * the content width, which the flex row above would otherwise push off the
 * right edge instead of overlapping.
 */
static const char *stereo_mode_text(uint8_t mode)
{
    switch (mode) {
    case EN_FM_STEREO_MONO:   return "MONO";
    case EN_FM_STEREO_STEREO: return "ST HOLD";
    default:                  return "STEREO";
    }
}

static void build_now(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_NOW];

    /* Status strip. Band and standard on the left, recording on the right -
       the two things that are true of the whole screen rather than of any one
       element on it. */
    s_band_lbl = label(s, "FM", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(s_band_lbl, MARGIN, 10);

    /* The station's own clock, from group 4A. It was being decoded and thrown
       away. Shown only once a 4A has actually arrived, so an empty space here
       means the station sends no time rather than that the time is midnight.

       Right-aligned rather than centred: centred put it straight through
       "Americas RBDS", and the band label is the one thing on this strip whose
       width depends on data. It shares the slot with the recording readout and
       yields to it - see refresh_now - because while you are recording, that
       the recording is running matters more than what time it is. */
    s_clock = label(s, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_clock, 56);
    lv_obj_set_pos(s_clock, RP_SCREEN_W - MARGIN - 56, 10);

    s_rec_dot = panel(s, RP_SCREEN_W - MARGIN - 52, 14, 7, 7, C_REC);
    lv_obj_set_style_radius(s_rec_dot, 4, 0);
    lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);

    s_rec_lbl = label(s, "0:00", F_CAPTION, C_REC);
    lv_obj_set_pos(s_rec_lbl, RP_SCREEN_W - MARGIN - 40, 10);
    lv_obj_add_flag(s_rec_lbl, LV_OBJ_FLAG_HIDDEN);

    hairline(s, 32);

    /* The frequency. Fixed position and fixed size - it is the one thing that
       must be findable without reading, so nothing below is allowed to move
       it. The unit sits on its baseline rather than beside its centre. */
    s_freq = label(s, "--.-", F_FREQ, C_TEXT);
    lv_obj_set_style_text_align(s_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_freq, CONTENT_W - 40);
    lv_obj_set_pos(s_freq, MARGIN, 46);

    s_unit = label(s, "MHz", F_UNIT, C_TEXT_MUTE);
    lv_obj_set_pos(s_unit, RP_SCREEN_W - MARGIN - 42, 82);

    /*
     * Mute, in the corner of the frequency block.
     *
     * Not on the pill row with STEREO and AF, which is where a fourth pill
     * would belong semantically: that row is full. STEREO already reaches the
     * TRAFFIC pill when it reads "ST FORCED", and adding a fourth chip to the
     * thirty-odd pixels left over would be a layout that survives until the
     * first long string.
     *
     * Here instead, above the unit and clear of everything: the frequency
     * label stops at MARGIN + CONTENT_W - 40, the unit sits below at y 82, and
     * this is the rectangle between them. It is also where the control belongs
     * on a radio - beside the dial, not three screens into a settings list -
     * which is the same argument the AF badge is a control for.
     *
     * The extended click area is what makes a 40x30 chip a finger target
     * without a 44 px hole in the layout.
     */
    s_mute_btn = panel(s, RP_SCREEN_W - MARGIN - 40, 48, 40, 30, C_SURFACE);
    lv_obj_set_style_radius(s_mute_btn, 3, 0);
    lv_obj_add_flag(s_mute_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mute_btn, on_mute_tap, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(s_mute_btn, lv_color_hex(C_SURFACE_2),
                              LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(s_mute_btn, 10);
    s_mute_lbl = label(s_mute_btn, LV_SYMBOL_VOLUME_MAX, F_BODY, C_TEXT_DIM);
    lv_obj_center(s_mute_lbl);

    /*
     * The volume, shown only while it is being changed.
     *
     * The volume keys are the two buttons on this device that are labelled for
     * a job, and until now they tuned the radio. They set the volume instead,
     * which needs no explaining to anybody - but a control with no readout is
     * a control you have to guess at, and there is no room on this screen for
     * a permanent one.
     *
     * So it appears over the station name for a moment and goes away. Over the
     * station name deliberately: that is the least useful thing to cover for a
     * second and a half, and it puts the readout where the eye already is
     * rather than in a corner.
     *
     * Hidden at rest, so this costs the layout nothing and the screen looks
     * exactly as it did when nobody is touching the volume.
     */
    /* 142 to 188, which is the programme type and the rule under it. Chosen
       by what it covers: the station name above ends at 152 and the radio
       text below starts at 190, so this is the one band on the screen where
       a second and a half of cover costs nothing anybody was reading. */
    s_vol_overlay = panel(s, MARGIN + 14, 142, CONTENT_W - 28, 46, C_SURFACE);
    lv_obj_set_style_radius(s_vol_overlay, 4, 0);
    lv_obj_add_flag(s_vol_overlay, LV_OBJ_FLAG_HIDDEN);

    s_vol_txt = label(s_vol_overlay, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_pos(s_vol_txt, 12, 8);

    /* Track first, fill second: the fill has to be drawn over it. */
    panel(s_vol_overlay, 12, 30, CONTENT_W - 52, 6, C_SURFACE_2);
    s_vol_bar = panel(s_vol_overlay, 12, 30, 1, 6, C_SIGNAL);

    /* Station name, from RDS. Falls back to nothing rather than to a
       placeholder: an empty line reads as "not yet", where "Unknown" reads as
       a decode that failed. */
    s_ps = para(s, "", F_STATION, C_RDS, CONTENT_W);
    lv_obj_set_style_text_align(s_ps, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ps, MARGIN, 118);

    s_pty = label(s, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_pty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_pty, CONTENT_W);
    lv_obj_set_pos(s_pty, MARGIN, 152);

    hairline(s, 178);

    /* Radio text. Three lines is enough for almost every message and keeps the
       block from pushing the transport around when a station is chatty. */
    s_rt = para(s, "", F_CAPTION, C_TEXT_DIM, CONTENT_W);
    lv_obj_set_pos(s_rt, MARGIN, 190);
    lv_obj_set_style_text_line_space(s_rt, 4, 0);
    lv_label_set_long_mode(s_rt, LV_LABEL_LONG_WRAP);

    /*
     * The three chips, laid out rather than placed.
     *
     * They used to sit at MARGIN, MARGIN + 76 and MARGIN + 150, which works
     * for exactly the strings that were there when those numbers were chosen.
     * All three are LV_SIZE_CONTENT and all three change text: the stereo pill
     * has three modes, the AF badge has five states, and hand-placed x
     * coordinates are a promise about string widths that nobody can keep.
     * "ST FORCED" reached into TRAFFIC, which is the second time something on
     * this row has overlapped.
     *
     * A flex row makes the overlap unrepresentable - each chip starts where
     * the last one ended, whatever it says. Position is now the layout's job,
     * and the only thing left to get right is that the widest combination
     * still fits inside CONTENT_W, which the preview now renders as its own
     * shot so it is checked rather than estimated.
     */
    lv_obj_t *pills = panel(s, MARGIN, 256, CONTENT_W, 24, C_BG);
    lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    /* After panel(), because flat() zeroes every pad. */
    lv_obj_set_style_pad_column(pills, PILL_GAP, 0);

    /* Added in reading order; flex keeps them in it. */
    s_stereo = pill(pills, 0, 0, "STEREO", C_SIGNAL);
    /* A pill is normally a readout. This one is also a control, so it gets a
       target the size of the chip rather than the size of the text. */
    lv_obj_add_flag(s_stereo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_stereo, on_stereo_tap, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(s_stereo, lv_color_hex(C_SURFACE_2),
                              LV_STATE_PRESSED);

    /*
     * "TA" and not "TRAFFIC".
     *
     * This is the chip with slack to give, and something had to give: the
     * widest row is forced stereo plus a traffic announcement plus the AF
     * follower reporting a weak signal, and with "TRAFFIC" in it that row is
     * wider than the screen. TA is what the RDS flag is called, it is what
     * every other radio labels it, and the landscape readout still spells out
     * TRAFFIC where there is room for it.
     *
     * rp_ui_pill_row_slack() is what says whether this was enough.
     */
    s_ta_badge = pill(pills, 0, 0, "TA", C_TA);

    s_af_badge = pill(pills, 0, 0, "AF", C_RDS);
    lv_obj_add_flag(s_af_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_af_badge, on_af_tap, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(s_af_badge, lv_color_hex(C_SURFACE_2),
                              LV_STATE_PRESSED);

    /* Signal. A segmented meter rather than a number, because the number means
       nothing to anyone and the shape means everything - you can see at a
       glance whether moving the headphone cable helped. */

    for (int i = 0; i < 14; i++) {
        s_seg[i] = panel(s, MARGIN + i * 12, 288, 8, 14, C_SURFACE_2);
        lv_obj_set_style_radius(s_seg[i], 1, 0);
    }
    s_rssi_lbl = label(s, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(s_rssi_lbl, MARGIN, 306);

    hairline(s, 322);

    /* The live buffer. Shown as how much has been captured rather than as a
       scrubber with a handle: there is nothing to drag to yet, and a control
       that looks draggable and is not is worse than a readout. */
    s_tl_lbl = label(s, "", F_CAPTION, C_LIVE);
    lv_obj_set_pos(s_tl_lbl, MARGIN, 328);

    /* Narrow enough to clear both buttons to its right. It used to stop only
       at LIVE; KEEP now sits beside LIVE and the readout ran underneath it. */
    s_live_lbl = label(s, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(s_live_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_live_lbl, CONTENT_W - 108);
    lv_obj_set_pos(s_live_lbl, MARGIN, 328);

    /*
     * Keep the buffer. rp_act_save_live has worked since the capture backend
     * landed and nothing ever called it, so "record the thing that already
     * happened" - the one feature a timeshift buffer exists for - was written
     * and unreachable.
     *
     * It saves the whole buffer rather than asking how much. The buffer is
     * bounded at a few minutes by the setting that allocated it, and a length
     * picker is a decision to make afterwards, in a file manager, not in the
     * two seconds before the moment falls off the end.
     */
    s_keep_btn = panel(s, RP_SCREEN_W - MARGIN - 100, 322, 50, 26, C_SURFACE_2);
    lv_obj_set_style_radius(s_keep_btn, 3, 0);
    lv_obj_add_flag(s_keep_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_keep_btn, on_keep, LV_EVENT_CLICKED, 0);
    lv_obj_set_ext_click_area(s_keep_btn, 12);
    s_keep_lbl = label(s_keep_btn, "KEEP", F_CAPTION, C_REC);
    lv_obj_center(s_keep_lbl);

    /* Shown only when it would do something. A LIVE button that is already
       live is a button that teaches you it does nothing. */
    s_live_btn = panel(s, RP_SCREEN_W - MARGIN - 46, 322, 46, 26, C_SURFACE_2);
    lv_obj_set_style_radius(s_live_btn, 3, 0);
    lv_obj_add_flag(s_live_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_live_btn, on_go_live, LV_EVENT_CLICKED, 0);
    lv_obj_set_ext_click_area(s_live_btn, 12);
    lv_obj_center(label(s_live_btn, "LIVE", F_CAPTION, C_LIVE));

    /* The bar is inside a taller row so the whole strip is tappable - a 3 px
       target would be a decoration, not a control. */
    s_bar_row = panel(s, 0, 344, RP_SCREEN_W, 22, C_BG);
    lv_obj_add_flag(s_bar_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_bar_row, on_bar_tap, LV_EVENT_CLICKED, 0);

    panel(s_bar_row, MARGIN, 8, CONTENT_W, 3, C_SURFACE_2);
    s_live_fill = panel(s_bar_row, MARGIN, 8, 0, 3, C_LIVE);
    s_live_head = panel(s_bar_row, MARGIN, 4, 3, 11, C_TEXT);

    /* Transport. Three targets across the full width, each well over the
       minimum, with the record button given the middle and the most weight. */
    const int ty = 362, th = 50;

    lv_obj_t *dn = panel(s, MARGIN, ty, 64, th, C_SURFACE);
    lv_obj_add_flag(dn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dn, on_left, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(dn, lv_color_hex(C_SURFACE_2), LV_STATE_PRESSED);
    s_tl_lbl = label(dn, LV_SYMBOL_PREV, F_BODY, C_TEXT);
    lv_obj_center(s_tl_lbl);

    lv_obj_t *rec = panel(s, MARGIN + 72, ty, 72, th, C_SURFACE);
    lv_obj_add_flag(rec, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rec, on_middle, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(rec, lv_color_hex(C_SURFACE_2), LV_STATE_PRESSED);
    s_rec_btn_lbl = label(rec, "REC", F_BODY, C_REC);
    lv_obj_center(s_rec_btn_lbl);

    lv_obj_t *up = panel(s, MARGIN + 152, ty, 64, th, C_SURFACE);
    lv_obj_add_flag(up, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(up, on_right, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(up, lv_color_hex(C_SURFACE_2), LV_STATE_PRESSED);
    s_tr_lbl = label(up, LV_SYMBOL_NEXT, F_BODY, C_TEXT);
    lv_obj_center(s_tr_lbl);

    /*
     * The volume overlay, in front of everything.
     *
     * Last, and it has to be last: z order here is creation order, and the
     * overlay is created up beside the mute button it belongs with. Without
     * this the station name and the programme type draw straight through it,
     * which reads as a transparency bug and is really a question of which
     * sibling came second.
     */
    lv_obj_move_foreground(s_vol_overlay);
}

static void refresh_now(void)
{
    /* Not built yet - see ensure_built. */
    if (!s_built[RP_SCREEN_NOW]) return;
    char buf[96];

    fmt_mhz(buf, sizeof buf, rp_model.khz);
    lv_label_set_text(s_freq, rp_model.khz ? buf : "--.-");

    /* Band and standard. Worth showing because they change what everything
       else means - a programme type is a different word in each standard. */
    buf[0] = 0;
    cat(buf, rp_model.region ? rp_model.region->name : "FM", sizeof buf);
    cat(buf, "  ", sizeof buf);
    cat(buf, rp_model.rds.rbds ? "RBDS" : "RDS", sizeof buf);
    lv_label_set_text(s_band_lbl, buf);

    fmt_ct(buf, sizeof buf, &rp_model.rds);
    lv_label_set_text(s_clock, rp_model.recording ? "" : buf);

    lv_label_set_text(s_ps, rp_model.rds.ps_valid ? rp_model.rds.ps : "");

    /* Programme type, and the refinement if the station sends one. */
    buf[0] = 0;
    if (rp_model.rds.pi_valid) {
        cat(buf, en_rds_pty_name(rp_model.rds.pty, rp_model.rds.rbds),
            sizeof buf);
        if (rp_model.rds.ptyn_valid) {
            cat(buf, "  ", sizeof buf);
            cat(buf, rp_model.rds.ptyn, sizeof buf);
        }
    }
    lv_label_set_text(s_pty, buf);

    /* With no tuner there is no radio text and never will be, so the space
       says why instead of staying blank. A dead radio that explains nothing is
       indistinguishable from a broken app. */
    if (!rp_model.tuner_ok && rp_model.tuner_note) {
        lv_label_set_text(s_rt, rp_model.tuner_note);
        lv_obj_set_style_text_color(s_rt, lv_color_hex(C_TA), 0);
    } else {
        lv_label_set_text(s_rt, rp_model.rds.rt_valid ? rp_model.rds.rt : "");
        lv_obj_set_style_text_color(s_rt, lv_color_hex(C_TEXT_DIM), 0);
    }

    /* The meter. RSSI is reported over the full byte range but everything
       real lives in the bottom third, so the scale is compressed to where the
       signal actually is rather than wasting most of the bar. */
    int lit = (int)((rp_model.rssi * 14u) / 96u);
    if (lit > 14) lit = 14;
    for (int i = 0; i < 14; i++) {
        uint32_t c = C_SURFACE_2;
        if (i < lit) c = (i < 4) ? C_TA : C_SIGNAL;   /* weak reads amber */
        lv_obj_set_style_bg_color(s_seg[i], lv_color_hex(c), 0);
    }

    buf[0] = 0;
    cat(buf, "RSSI ", sizeof buf);
    char n[12];
    put_uint(n, rp_model.rssi, 0);
    cat(buf, n, sizeof buf);
    if (rp_model.snr) {
        cat(buf, "   SNR ", sizeof buf);
        put_uint(n, (uint32_t)(rp_model.snr < 0 ? -rp_model.snr : rp_model.snr), 0);
        if (rp_model.snr < 0) cat(buf, "-", sizeof buf);
        cat(buf, n, sizeof buf);
    }
    lv_label_set_text(s_rssi_lbl, buf);

    {
        /* Label says what was asked for; colour says what the chip is doing.
           Forced mono is called out in amber because it is a choice that
           stays made, and one the user should be able to see they made. */
        lv_obj_t *sl = lv_obj_get_child(s_stereo, 0);
        if (sl) lv_label_set_text(sl, stereo_mode_text(rp_model.stereo_mode));
        pill_set(s_stereo, rp_model.stereo,
                 rp_model.stereo_mode == EN_FM_STEREO_AUTO ? C_SIGNAL : C_TA);
    }

    if (s_af_badge) {
        /* The label says what it is doing - watching, weak, trying, waiting -
           so a retune the listener did not ask for has something on screen
           that already explained itself. */
        lv_obj_t *al = lv_obj_get_child(s_af_badge, 0);
        if (al) lv_label_set_text(al, en_af_state_text(&rp_model.af));
        pill_set(s_af_badge, rp_model.af.enabled,
                 rp_model.af.phase == EN_AF_TRYING ? C_TA : C_RDS);
    }
    pill_set(s_ta_badge, rp_model.rds.ta, C_TA);

    /*
     * Mute.
     *
     * Hidden outright when the card does not publish the control, rather than
     * shown and inert - the same rule the LIVE button follows, and the same one
     * the register explorer follows on a backend that cannot do raw access.
     *
     * Amber and not the recording red: the radio being silenced is a state the
     * listener chose and can see they chose, which is what amber already means
     * on this screen for forced mono. Red on this screen means a recording is
     * running and should keep meaning only that.
     */
    /*
     * The overlay follows the value rather than the button, so it appears
     * whichever way the volume was changed - the keys, the settings slider, or
     * tinymix on a serial console. The first value seen is not a change, or
     * the overlay would greet everybody at startup.
     */
    if (s_vol_last != rp_model.volume) {
        if (s_vol_last != 0xFF)
            s_vol_until = lv_tick_get() + VOL_SHOW_MS;
        s_vol_last = rp_model.volume;
    }
    if (s_vol_overlay) {
        bool on = rp_model.volume_ok
               && (int32_t)(lv_tick_get() - s_vol_until) < 0;

        show(s_vol_overlay, on);
        if (on) {
            const int track = CONTENT_W - 52;
            int w = track * (int)rp_model.volume / 100;
            char b[40], pct[8];

            pct[0] = 0;
            put_uint(pct, rp_model.volume, 0);

            b[0] = 0;
            cat(b, rp_out_label(rp_model.output), sizeof b);
            cat(b, "  ", sizeof b);
            cat(b, pct, sizeof b);
            cat(b, "%", sizeof b);
            lv_label_set_text(s_vol_txt, b);

            /* At least one pixel at zero: a bar that vanishes entirely reads
               as a missing control rather than as silence. */
            lv_obj_set_width(s_vol_bar, w < 1 ? 1 : w);
        }
    }

    if (s_mute_btn) {
        show(s_mute_btn, rp_model.mute_ok);
        if (rp_model.mute_ok && s_mute_lbl) {
            lv_label_set_text(s_mute_lbl, rp_model.muted ? LV_SYMBOL_MUTE
                                                         : LV_SYMBOL_VOLUME_MAX);
            lv_obj_set_style_text_color(s_mute_lbl,
                lv_color_hex(rp_model.muted ? C_TA : C_TEXT_DIM), 0);
            lv_obj_set_style_bg_color(s_mute_btn,
                lv_color_hex(rp_model.muted ? C_SURFACE_2 : C_SURFACE), 0);
        }
    }

    /* The buffer strip. How much has been captured, and where in it the
       headphones are - which is the same picture whether that is the live edge
       or ten minutes back. */
    bool live = at_live();

    int w = 0, head = 0;
    if (rp_model.play_file) {
        if (rp_model.play_len_ms)
            head = (int)((uint64_t)rp_model.play_pos_ms * CONTENT_W
                         / rp_model.play_len_ms);
        w = CONTENT_W;
    } else {
        if (rp_model.live_cap_ms)
            w = (int)((uint64_t)rp_model.live_ms * CONTENT_W
                      / rp_model.live_cap_ms);
        head = w;
        if (rp_model.behind_max_ms && rp_model.behind_ms)
            head = w - (int)((uint64_t)rp_model.behind_ms * w
                             / rp_model.behind_max_ms);
    }
    if (w > CONTENT_W) w = CONTENT_W;
    if (head < 0) head = 0;
    if (head > CONTENT_W) head = CONTENT_W;

    lv_obj_set_width(s_live_fill, w);
    lv_obj_set_pos(s_live_head, MARGIN + head - 1, 4);
    lv_obj_set_style_bg_color(s_live_head,
                              lv_color_hex(live ? C_LIVE : C_TEXT), 0);

    /* Left of the row says what is playing; right says how much there is. */
    if (rp_model.play_file) {
        lv_label_set_text(s_tl_lbl, "");
        buf[0] = 0;
        fmt_time(n, sizeof n, rp_model.play_pos_ms);
        cat(buf, n, sizeof buf);
        cat(buf, " / ", sizeof buf);
        fmt_time(n, sizeof n, rp_model.play_len_ms);
        cat(buf, n, sizeof buf);
        lv_label_set_text(s_live_lbl, buf);
    } else if (!rp_model.capture_ok) {
        lv_label_set_text(s_live_lbl, "no capture");
    } else if (live) {
        buf[0] = 0;
        fmt_time(n, sizeof n, rp_model.live_ms);
        cat(buf, n, sizeof buf);
        cat(buf, " buffered", sizeof buf);
        lv_label_set_text(s_live_lbl, buf);
    } else {
        buf[0] = 0;
        cat(buf, "-", sizeof buf);
        fmt_time(n, sizeof n, rp_model.behind_ms);
        cat(buf, n, sizeof buf);
        cat(buf, " behind", sizeof buf);
        lv_label_set_text(s_live_lbl, buf);
    }

    if (live || !rp_model.capture_ok)
        lv_obj_add_flag(s_live_btn, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(s_live_btn, LV_OBJ_FLAG_HIDDEN);

    /* KEEP needs something in the buffer to keep, and is pointless while a
       recording is already running - that is capturing the same audio. */
    if (rp_model.capture_ok && rp_model.live_ms && !rp_model.recording &&
        !rp_model.play_file)
        lv_obj_remove_flag(s_keep_btn, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_keep_btn, LV_OBJ_FLAG_HIDDEN);

    if (s_kept_for) {
        s_kept_for--;
        lv_label_set_text(s_keep_lbl, "SAVED");
        lv_obj_set_style_text_color(s_keep_lbl, lv_color_hex(C_LIVE), 0);
    } else {
        lv_label_set_text(s_keep_lbl, "KEEP");
        lv_obj_set_style_text_color(s_keep_lbl, lv_color_hex(C_REC), 0);
    }

    /* No capture means no buffer to scrub and nothing to record. The strip and
       the record button are hidden rather than left as controls that quietly
       do nothing when pressed. */
    if (rp_model.capture_ok) lv_obj_remove_flag(s_bar_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_bar_row, LV_OBJ_FLAG_HIDDEN);

    /* Recording. The dot and the elapsed time appear together and only while
       recording; a permanent readout showing 0:00 would be noise. */
    if (rp_model.recording) {
        lv_obj_remove_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_rec_lbl, LV_OBJ_FLAG_HIDDEN);
        buf[0] = 0;
        if (rp_model.ta_recording) cat(buf, "TA ", sizeof buf);
        fmt_time(n, sizeof n, rp_model.rec_ms);
        cat(buf, n, sizeof buf);
        lv_label_set_text(s_rec_lbl, buf);
    } else {
        lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_rec_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    /* The transport means different things depending on where the headphones
       are, so it says so rather than leaving the user to find out. */
    if (live) {
        lv_label_set_text(s_tl_lbl, LV_SYMBOL_PREV);
        lv_label_set_text(s_tr_lbl, LV_SYMBOL_NEXT);
        if (rp_model.capture_ok) {
            lv_label_set_text(s_rec_btn_lbl,
                              rp_model.recording ? "STOP" : "REC");
            lv_obj_set_style_text_color(s_rec_btn_lbl, lv_color_hex(C_REC), 0);
        } else {
            /* Greyed and inert, not hidden: the button leaving a hole would
               make the row look broken rather than limited. */
            lv_label_set_text(s_rec_btn_lbl, "REC");
            lv_obj_set_style_text_color(s_rec_btn_lbl,
                                        lv_color_hex(C_TEXT_MUTE), 0);
        }
    } else {
        lv_label_set_text(s_tl_lbl, LV_SYMBOL_LEFT " 15");
        lv_label_set_text(s_tr_lbl, "15 " LV_SYMBOL_RIGHT);
        lv_label_set_text(s_rec_btn_lbl,
                          rp_model.play_paused ? LV_SYMBOL_PLAY
                                               : LV_SYMBOL_PAUSE);
        lv_obj_set_style_text_color(s_rec_btn_lbl, lv_color_hex(C_TEXT), 0);
    }
}

/* ---- Dial ----------------------------------------------------------------- */

static void on_step_down(lv_event_t *e) { (void)e; rp_act_step(false); }
static void on_step_up(lv_event_t *e)   { (void)e; rp_act_step(true); }
static void on_preset_here(lv_event_t *e) { (void)e; rp_act_preset_toggle(); }

/*
 * Tapping the band strip.
 *
 * The strip spans the whole region, so where along it the finger landed is a
 * fraction of the band - turned into a channel and snapped by rp_act_tune,
 * which is the one place that knows the grid.
 */
static void on_strip_tap(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_indev_t *in = lv_indev_active();
    const en_region_t *rg = rp_model.region;
    lv_area_t a;
    lv_point_t p;
    int32_t w;

    if (!in || !rg || !rg->step_khz) return;

    lv_indev_get_point(in, &p);
    lv_obj_get_coords(o, &a);
    w = a.x2 - a.x1;
    if (w <= 0) return;

    {
        int32_t x = p.x - a.x1;
        uint32_t span = rg->high_khz - rg->low_khz;

        if (x < 0) x = 0;
        if (x > w) x = w;
        rp_act_tune(rg->low_khz + (uint32_t)((uint64_t)span * (uint32_t)x
                                             / (uint32_t)w));
    }
}

/* ---- typing a frequency ---------------------------------------------------
 *
 * Stepping and seeking are both relative, and neither answers "put me on
 * 98.5". On a radio with four buttons that was a fair limitation; with a
 * touchscreen it is just a missing feature, and the one people reach for
 * first when they already know the station they want.
 *
 * Digits are collected as tenths of a megahertz - "985" is 98.5 - because
 * that is how the number is said out loud and it means no decimal point to
 * place. The entry is checked against the region before it is offered, so the
 * button that commits it is simply unavailable for a frequency this band does
 * not have, rather than accepting it and snapping somewhere else.
 */
static lv_obj_t *s_pad, *s_pad_entry, *s_pad_note, *s_pad_go;
static char      s_pad_buf[8];

/* What has been typed, as kHz, or 0 for nothing usable yet. */
static uint32_t pad_khz(void)
{
    uint32_t v = 0;
    int i;

    if (!s_pad_buf[0]) return 0;
    for (i = 0; s_pad_buf[i]; i++) v = v * 10u + (uint32_t)(s_pad_buf[i] - '0');
    return v * 100u;            /* tenths of a MHz -> kHz */
}

static void pad_refresh(void)
{
    uint32_t khz = pad_khz();
    bool ok = khz && rp_model.region &&
              en_region_on_grid(rp_model.region, khz);
    char buf[48];

    if (!s_pad) return;

    if (s_pad_buf[0]) {
        int n = 0, i;

        /* Written as it is typed - "9", "98", "98.5" - so the decimal point
           appears where the eye expects it rather than at the end. */
        for (i = 0; s_pad_buf[i] && n < (int)sizeof buf - 2; i++) {
            if (i == 3) buf[n++] = '.';
            else if (i == 2 && !s_pad_buf[3]) { buf[n++] = s_pad_buf[i];
                                                buf[n++] = 0; goto done; }
            buf[n++] = s_pad_buf[i];
        }
        buf[n] = 0;
done:
        ;
    } else {
        buf[0] = 0;
    }
    lv_label_set_text(s_pad_entry, s_pad_buf[0] ? buf : "---");

    if (!s_pad_buf[0])
        lv_label_set_text(s_pad_note, "Enter a frequency");
    else if (ok)
        lv_label_set_text(s_pad_note, "MHz");
    else
        lv_label_set_text(s_pad_note, "not a channel in this band");

    /* Unavailable rather than absent: a button that vanishes as you type is
       harder to aim at than one that is simply dim until it means something. */
    lv_obj_set_style_bg_color(s_pad_go,
                              lv_color_hex(ok ? C_SIGNAL : C_SURFACE_2), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_pad_go, 0),
                                lv_color_hex(ok ? C_BG : C_TEXT_MUTE), 0);
}

static void pad_close(void)
{
    if (s_pad) lv_obj_add_flag(s_pad, LV_OBJ_FLAG_HIDDEN);
}

static void on_pad_open(lv_event_t *e)
{
    (void)e;
    if (!s_pad) return;
    s_pad_buf[0] = 0;
    pad_refresh();
    lv_obj_remove_flag(s_pad, LV_OBJ_FLAG_HIDDEN);
}

static void on_pad_digit(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int n = 0;

    while (s_pad_buf[n]) n++;
    /* Four digits is the whole band - 87.9 through 107.9 - and a fifth could
       only be a mistake, so it is refused rather than silently dropped from
       the front. */
    if (n >= 4) return;
    s_pad_buf[n] = (char)('0' + d);
    s_pad_buf[n + 1] = 0;
    pad_refresh();
}

static void on_pad_back(lv_event_t *e)
{
    int n = 0;

    (void)e;
    while (s_pad_buf[n]) n++;
    if (n) s_pad_buf[n - 1] = 0;
    pad_refresh();
}

static void on_pad_cancel(lv_event_t *e)
{
    (void)e;
    pad_close();
}

static void on_pad_go(lv_event_t *e)
{
    uint32_t khz = pad_khz();

    (void)e;
    if (!khz || !rp_model.region || !en_region_on_grid(rp_model.region, khz))
        return;
    pad_close();
    rp_act_tune(khz);
    rp_ui_show(RP_SCREEN_NOW);
}

static void build_pad(lv_obj_t *parent)
{
    static const char *row[4][3] = {
        { "1", "2", "3" }, { "4", "5", "6" },
        { "7", "8", "9" }, { "<", "0", "X" },
    };
    int r, c;

    s_pad = panel(parent, 0, 0, RP_SCREEN_W, RP_SCREEN_H, C_BG);
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_HIDDEN);
    /* Clickable so a tap on the backdrop does not fall through to the dial
       underneath, which would tune something while the keypad is open. */
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_CLICKABLE);

    s_pad_entry = label(s_pad, "---", F_FREQ, C_TEXT);
    lv_obj_set_style_text_align(s_pad_entry, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_pad_entry, CONTENT_W);
    lv_obj_set_pos(s_pad_entry, MARGIN, 24);

    s_pad_note = label(s_pad, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_pad_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_pad_note, CONTENT_W);
    lv_obj_set_pos(s_pad_note, MARGIN, 90);

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 3; c++) {
            const char *t = row[r][c];
            int w = (CONTENT_W - 16) / 3;
            lv_obj_t *k = panel(s_pad, MARGIN + c * (w + 8),
                                122 + r * 58, w, 50, C_SURFACE);

            lv_obj_set_style_radius(k, 6, 0);
            lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(k, lv_color_hex(C_SURFACE_2),
                                      LV_STATE_PRESSED);
            lv_obj_center(label(k, t, F_TITLE, C_TEXT));

            if (t[0] == '<')
                lv_obj_add_event_cb(k, on_pad_back, LV_EVENT_CLICKED, 0);
            else if (t[0] == 'X')
                lv_obj_add_event_cb(k, on_pad_cancel, LV_EVENT_CLICKED, 0);
            else
                lv_obj_add_event_cb(k, on_pad_digit, LV_EVENT_CLICKED,
                                    (void *)(intptr_t)(t[0] - '0'));
        }
    }

    s_pad_go = panel(s_pad, MARGIN, 360, CONTENT_W, 52, C_SURFACE_2);
    lv_obj_set_style_radius(s_pad_go, 6, 0);
    lv_obj_add_flag(s_pad_go, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(label(s_pad_go, "Tune", F_BODY, C_TEXT_MUTE));
    lv_obj_add_event_cb(s_pad_go, on_pad_go, LV_EVENT_CLICKED, 0);
}

static void build_dial(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_DIAL];

    lv_obj_t *cap = label(s, "TUNE", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(cap, MARGIN, 10);
    hairline(s, 32);

    /* The readout is the way into the keypad. It is the largest thing on the
       screen and it is the number you want to change, so it is where a thumb
       goes first - a separate button would be a second place to look. */
    lv_obj_t *freqhit = panel(s, MARGIN, 46, CONTENT_W, 66, C_BG);
    lv_obj_add_flag(freqhit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(freqhit, on_pad_open, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(freqhit, lv_color_hex(C_SURFACE),
                              LV_STATE_PRESSED);

    s_dial_freq = label(freqhit, "--.-", F_FREQ, C_TEXT);
    lv_obj_set_style_text_align(s_dial_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_dial_freq, CONTENT_W);
    lv_obj_set_pos(s_dial_freq, 0, 6);

    s_dial_note = label(s, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_dial_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_dial_note, CONTENT_W);
    lv_obj_set_pos(s_dial_note, MARGIN, 118);

    /*
     * A strip of the band around where we are, and a way onto it.
     *
     * It was an orientation aid and nothing more, which was right when there
     * was no touchscreen. With one, a picture of the band that cannot be
     * touched is the most obviously missing control on the screen: tapping
     * along it tunes to the channel under the finger.
     */
    s_dial_grid = panel(s, 0, 150, RP_SCREEN_W, 46, C_BG);
    lv_obj_add_flag(s_dial_grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_dial_grid, on_strip_tap, LV_EVENT_CLICKED, 0);

    hairline(s, 210);

    /* Single steps, distinct from the seek on the Now screen: seek jumps to
       the next station, step moves one channel whether there is anything
       there or not. Both are wanted, and confusing them is annoying. */
    lv_obj_t *dn = panel(s, MARGIN, 240, CONTENT_W / 2 - 6, TAP_MIN + 18,
                         C_SURFACE);
    lv_obj_add_flag(dn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dn, on_step_down, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(dn, lv_color_hex(C_SURFACE_2), LV_STATE_PRESSED);
    lv_obj_center(label(dn, "-", F_TITLE, C_TEXT));

    lv_obj_t *up = panel(s, MARGIN + CONTENT_W / 2 + 6, 240,
                         CONTENT_W / 2 - 6, TAP_MIN + 18, C_SURFACE);
    lv_obj_add_flag(up, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(up, on_step_up, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(up, lv_color_hex(C_SURFACE_2), LV_STATE_PRESSED);
    lv_obj_center(label(up, "+", F_TITLE, C_TEXT));

    lv_obj_t *save = panel(s, MARGIN, 322, CONTENT_W, TAP_MIN + 18, C_SURFACE);
    lv_obj_add_flag(save, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(save, on_preset_here, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(C_SURFACE_2), LV_STATE_PRESSED);
    lv_obj_center(label(save, "Save as preset", F_BODY, C_TEXT));

    /* Last, so it covers everything above it when it is shown. */
    build_pad(s);
}

static void refresh_dial(void)
{
    /* Not built yet - see ensure_built. */
    if (!s_built[RP_SCREEN_DIAL]) return;
    char buf[64], n[16];
    fmt_mhz(buf, sizeof buf, rp_model.khz);
    lv_label_set_text(s_dial_freq, rp_model.khz ? buf : "--.-");

    buf[0] = 0;
    if (rp_model.region) {
        fmt_mhz(n, sizeof n, rp_model.region->low_khz);
        cat(buf, n, sizeof buf);
        cat(buf, " - ", sizeof buf);
        fmt_mhz(n, sizeof n, rp_model.region->high_khz);
        cat(buf, n, sizeof buf);
        cat(buf, " MHz", sizeof buf);
        cat(buf, "   ", sizeof buf);
        put_uint(n, rp_model.region->step_khz, 0);
        cat(buf, n, sizeof buf);
        cat(buf, " kHz", sizeof buf);

        /* Said once, where the picture is: a sweep that jumped between
           stations knows nothing about the space between them, and a strip
           that looked continuous would be claiming otherwise. */
        if (s_scan.profile_n && s_scan.phase != EN_SCAN_SWEEP)
            cat(buf, s_scan.profile_sparse ? "   stations only" : "   scanned",
                sizeof buf);
    }
    lv_label_set_text(s_dial_note, buf);

    /* Redraw the band strip: a tick per preset, and a marker where we are, so
       the strip says something rather than being decoration. */
    lv_obj_clean(s_dial_grid);
    if (!rp_model.region) return;

    uint32_t lo = rp_model.region->low_khz, hi = rp_model.region->high_khz;
    if (hi <= lo) return;

    /*
     * The band profile, if a scan has been run: one bar per channel, as tall
     * as the signal there was.
     *
     * Drawn under everything else and in the surface colour, so it reads as
     * ground rather than as data competing with the preset ticks. The bars
     * are one pixel wide and drawn only where the profile has something,
     * which after a seeking sweep is only at the stations - joining those up
     * would draw valleys nobody measured.
     */
    if (s_scan.profile_n) {
        /* Standing on the hairline at y = 22 and growing upward, so the
           landscape and the preset ticks share one baseline and read as one
           strip rather than two things at different heights. */
        const int base = 22, max_h = 18;
        for (uint16_t i = 0; i < s_scan.profile_n; i++) {
            uint8_t v = s_scan.profile[i];
            if (!v) continue;
            int h = (v * max_h) / 255;
            if (h < 1) h = 1;
            int x = MARGIN + (int)((uint32_t)i * (uint32_t)CONTENT_W /
                                   s_scan.profile_n);
            /* As wide as a channel is on screen, so the floor reads as a
               floor rather than as a dotted line with gaps in it. */
            int w = CONTENT_W / (int)s_scan.profile_n;
            if (w < 1) w = 1;
            panel(s_dial_grid, x, base - h, w, h,
                  s_scan.profile_sparse ? C_RDS : C_TEXT_MUTE);
        }
    }

    panel(s_dial_grid, MARGIN, 22, CONTENT_W, 1, C_HAIRLINE);

    for (uint8_t i = 0; i < rp_model.presets.count; i++) {
        uint32_t k = rp_model.presets.list[i].khz;
        if (k < lo || k > hi) continue;
        int x = MARGIN + (int)((uint64_t)(k - lo) * CONTENT_W / (hi - lo));
        panel(s_dial_grid, x, 14, 1, 9, C_RDS);
    }

    if (rp_model.khz >= lo && rp_model.khz <= hi) {
        int x = MARGIN + (int)((uint64_t)(rp_model.khz - lo) * CONTENT_W
                               / (hi - lo));
        panel(s_dial_grid, x - 1, 8, 3, 22, C_SIGNAL);
    }
}

/* ---- band scan ------------------------------------------------------------
 *
 * The state machine is in core/scan.c and knows nothing about tuners or
 * screens; this is the part that turns "it wants to be on 90.1" into a tune
 * and "it is 40% through" into a bar.
 *
 * Pumped from rp_ui_tick, which runs whatever screen is showing, so walking
 * away from the Presets screen mid-scan does not abandon it - which matters,
 * because the whole point of a scan is that you can stop watching it.
 */

static uint32_t  s_scan_last_ms;
static uint8_t   s_scan_added;
static uint8_t   s_scan_note_for;   /* refresh ticks the result stays up */

/* An RSSI a station has to beat. The tuner reports 0..255 and an empty
   channel on this chip sits in the low teens, so this is well clear of the
   noise floor without being so high that a weak local is missed. */
#define RP_SCAN_RSSI 90

static bool scan_running(void)
{
    return s_scan.phase == EN_SCAN_SWEEP || s_scan.phase == EN_SCAN_NAMING;
}

static void scan_pump(void)
{
    if (!scan_running()) return;

    uint32_t now = lv_tick_get();
    uint32_t dt = now - s_scan_last_ms;
    s_scan_last_ms = now;
    /* First tick after starting, or a clock that jumped. Charge nothing
       rather than a garbage interval that would skip a channel. */
    if (dt > 1000) dt = 0;

    uint32_t khz = 0;
    switch (en_scan_tick(&s_scan, dt, rp_model.khz, rp_model.rssi,
                         &rp_model.rds, &khz)) {
    case EN_SCAN_TUNE:
        rp_act_tune_quiet(khz);
        break;
    case EN_SCAN_SEEK:
        /* The chip crosses the empty band for us. If the platform cannot,
           say so once and the sweep steps the rest in software rather than
           waiting out a timeout on every station. */
        if (!rp_act_seek_quiet(true)) en_scan_seek_failed(&s_scan);
        break;
    default:
        break;
    }

    if (s_scan.phase == EN_SCAN_DONE) {
        rp_act_squelch(false);
        rp_model_set_fast(false);
        s_scan_added = en_scan_commit(&s_scan, &rp_model.presets);
        rp_act_presets_save();
        s_scan_note_for = 60;
        s_scan.phase = EN_SCAN_IDLE;
        /* Back where the user was, and through the saving path this time so
           the frequency they are left on is the one that persists. */
        rp_act_tune(s_scan.resume_khz);
    }
}

static void on_scan(lv_event_t *e)
{
    (void)e;
    if (scan_running()) {
        /* Cancel. Put the tuner back before anything else, so a cancelled
           scan costs nothing but the time it ran for. */
        rp_act_squelch(false);
        rp_model_set_fast(false);
        en_scan_stop(&s_scan);
        rp_act_tune(s_scan.resume_khz);
        s_scan_added = 0;
        s_scan_note_for = 0;
        return;
    }
    if (!rp_model.region) return;

    s_scan_last_ms = lv_tick_get();
    s_scan_added = 0;
    s_scan_note_for = 0;

    /*
     * Quiet for the duration.
     *
     * A sweep crosses two hundred channels of hiss on its way to twenty
     * stations, and the naming pass then hops between those every few seconds -
     * neither is anything anybody wants to hear. This mutes at the tuner's own
     * MANUAL_MUTE bit, which matters here specifically: muting by closing the
     * capture PCM would stop the IIS2 clock, and RDS is the entire point of the
     * naming pass.
     *
     * Both ways out of a scan clear it - completion above, cancel just up
     * there - and it is a separate flag from the user's own mute so that
     * finishing a scan cannot unmute a radio the user silenced before starting
     * one.
     */
    rp_act_squelch(true);

    /*
     * And a faster look at the tuner for the duration.
     *
     * The sweep decides a seek has landed by watching the frequency change,
     * and the ordinary poll is slower than the sweep's own settle time - so
     * it was reading RSSI for the previous channel and timing out seeks that
     * had already arrived. Turned off again at both exits below.
     */
    rp_model_set_fast(true);

    if (en_scan_start(&s_scan, rp_model.region, rp_model.khz, RP_SCAN_RSSI,
                      rp_model.can_seek))
        rp_act_tune_quiet(s_scan.khz);
    else {
        rp_act_squelch(false);   /* it never started; do not leave it muted */
        rp_model_set_fast(false);
    }
}

/* ---- Presets --------------------------------------------------------------- */

static void on_preset_pick(lv_event_t *e)
{
    uint32_t khz = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    rp_act_tune(khz);
    rp_ui_show(RP_SCREEN_NOW);
}

/* How long the "simple screen is full" note stays up, in refresh ticks. */
static uint8_t s_simple_full_for;

static lv_obj_t *s_scan_btn, *s_scan_label, *s_scan_fill;

static void on_preset_simple(lv_event_t *e)
{
    uint32_t khz = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    /* A refusal here means the grid is full, and it has to be said: a tick box
       that silently declines to tick is the most confusing control there is. */
    if (!rp_act_simple_toggle(khz)) s_simple_full_for = 10;

    refresh_presets();
    refresh_simple();
}

static void build_presets(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_PRESETS];
    lv_obj_t *cap = label(s, "PRESETS", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(cap, MARGIN, 10);
    hairline(s, 32);

    s_preset_list = panel(s, 0, 40, RP_SCREEN_W, RP_SCREEN_H - 152, C_BG);
    lv_obj_add_flag(s_preset_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_preset_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_preset_list, LV_SCROLLBAR_MODE_OFF);

    /* A line under the list for the one thing this screen has to say back. */
    s_preset_note = label(s, "", F_CAPTION, C_TA);
    lv_obj_set_size(s_preset_note, CONTENT_W, 18);
    lv_label_set_long_mode(s_preset_note, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_preset_note, MARGIN, RP_SCREEN_H - 108);

    /* Scan, across the bottom. Filling this list by hand is tuning to a
       station, deciding, saving, and doing that twenty times. */
    s_scan_btn = panel(s, MARGIN, RP_SCREEN_H - 86, CONTENT_W, TAP_MIN,
                       C_SURFACE);
    lv_obj_add_flag(s_scan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scan_btn, on_scan, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(s_scan_btn, lv_color_hex(C_SURFACE_2),
                              LV_STATE_PRESSED);
    s_scan_label = label(s_scan_btn, "Scan band", F_BODY, C_TEXT);
    lv_obj_center(s_scan_label);

    /* The progress bar is the button's own background growing across it,
       rather than a separate widget above it. There is no room on this screen
       for a bar and a button, and a button that fills up as it works says the
       same thing in the space of one. */
    s_scan_fill = panel(s_scan_btn, 0, 0, 0, TAP_MIN, C_SIGNAL);
    lv_obj_move_background(s_scan_fill);
}

static void refresh_presets(void)
{
    /* Not built yet - see ensure_built. */
    if (!s_built[RP_SCREEN_PRESETS]) return;
    lv_obj_clean(s_preset_list);

    if (!rp_model.presets.count) {
        lv_obj_t *e = label(s_preset_list, "No presets yet", F_BODY,
                            C_TEXT_MUTE);
        lv_obj_set_pos(e, MARGIN, 20);
        return;
    }

    int y = 0;
    char buf[64];
    for (uint8_t i = 0; i < rp_model.presets.count; i++) {
        const en_preset_t *p = &rp_model.presets.list[i];

        lv_obj_t *r = row(s_preset_list, y, 52, on_preset_pick,
                          (void *)(uintptr_t)p->khz);

        /* Frequency on the left in the same typeface as the dial, name on the
           right - so the column of numbers scans vertically. */
        fmt_mhz(buf, sizeof buf, p->khz);
        lv_obj_t *f = label(r, buf, F_TITLE, C_TEXT);
        lv_obj_set_pos(f, MARGIN + 34, 12);

        lv_obj_t *nm = label(r, p->name[0] ? p->name : "", F_BODY, C_RDS);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(nm, CONTENT_W - 110);
        lv_obj_set_pos(nm, MARGIN + 110, 8);

        lv_obj_t *ty = label(r, en_rds_pty_name(p->pty, p->rbds),
                             F_CAPTION, C_TEXT_MUTE);
        lv_obj_set_style_text_align(ty, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(ty, CONTENT_W - 110);
        lv_obj_set_pos(ty, MARGIN + 110, 30);

        /*
         * The simple-screen mark, on its own target at the left of the row.
         *
         * A separate object rather than a gesture on the row, because the row
         * already means "tune to this" and a long press that sometimes means
         * something else is a control you have to be told about. This one is
         * visible whether or not it is set, so the affordance is there before
         * you know what it does.
         */
        lv_obj_t *mark = panel(r, 0, 4, 34, 44, C_BG);
        lv_obj_add_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(mark, on_preset_simple, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)p->khz);
        lv_obj_t *dot = panel(mark, 11, 17, 11, 11,
                              p->simple ? C_SIGNAL : C_SURFACE_2);
        lv_obj_set_style_radius(dot, 6, 0);

        panel(r, MARGIN, 51, CONTENT_W, 1, C_HAIRLINE);
        y += 52;
    }

    /* The note line, in priority order: a refusal, then a scan in progress,
       then what a finished scan found. Only one of them at a time - it is one
       line, and the most recent thing the user did is the one worth saying. */
    if (s_simple_full_for) {
        s_simple_full_for--;
        lv_label_set_text(s_preset_note, "Simple screen is full - six at most");
    } else if (scan_running()) {
        char m[16];
        buf[0] = 0;
        fmt_mhz(m, sizeof m, s_scan.khz);
        cat(buf, m, sizeof buf);
        cat(buf, s_scan.phase != EN_SCAN_SWEEP ? "   reading names   "
                 : (s_scan.use_seek ? "   seeking   " : "   scanning   "),
            sizeof buf);
        put_uint(m, s_scan.n_hits, 0);
        cat(buf, m, sizeof buf);
        cat(buf, " found", sizeof buf);
        lv_label_set_text(s_preset_note, buf);
    } else if (s_scan_note_for) {
        s_scan_note_for--;
        buf[0] = 0;
        put_uint(buf, s_scan_added, 0);
        cat(buf, s_scan_added == 1 ? " station saved" : " stations saved",
            sizeof buf);
        if (s_scan.overflowed)
            cat(buf, " - list full", sizeof buf);
        lv_label_set_text(s_preset_note, buf);
    } else {
        lv_label_set_text(s_preset_note, "");
    }

    if (s_scan_btn) {
        lv_label_set_text(s_scan_label,
                          scan_running() ? "Stop" : "Scan band");
        int w = scan_running()
                    ? (CONTENT_W * en_scan_percent(&s_scan)) / 100
                    : 0;
        lv_obj_set_width(s_scan_fill, w);
    }
}

/* ---- the recording timer --------------------------------------------------
 *
 * The clock comes from RDS group 4A, which is the only one this device has:
 * there is no RTC. That is why a start time is offered with a caveat printed
 * next to it and a length is not - see core/timer.h.
 */

static lv_obj_t *s_rec_limit_btn, *s_rec_limit_lbl;
static lv_obj_t *s_rec_at_btn, *s_rec_at_lbl;
static lv_obj_t *s_rec_timer_note;

/* Minutes since local midnight, from the broadcast clock. The transmitted
   time is UTC with a local offset in half hours beside it, and the offset is
   signed - a station in Newfoundland sends -5 for a half-hour zone. */
static bool clock_minutes(int *out)
{
    if (!rp_model.rds.ct_valid) return false;
    int m = rp_model.rds.ct_hour * 60 + rp_model.rds.ct_minute;
    m += rp_model.rds.ct_offset * 30;
    while (m < 0) m += 24 * 60;
    m %= 24 * 60;
    *out = m;
    return true;
}

static void rectimer_pump(void)
{
    int now = 0;
    bool have = clock_minutes(&now);

    switch (en_rectimer_tick(&rp_model.rectimer, have, now,
                             rp_model.recording, rp_model.rec_ms)) {
    case EN_REC_START:
        if (!rp_model.recording) rp_act_record_toggle();
        break;
    case EN_REC_STOP:
        if (rp_model.recording) rp_act_record_toggle();
        break;
    default:
        break;
    }
}

static void on_rec_limit(lv_event_t *e)
{
    (void)e;
    uint8_t at = 0;
    for (uint8_t i = 0; i < EN_REC_LIMITS_COUNT; i++)
        if (EN_REC_LIMITS[i] == rp_model.rectimer.limit_min) { at = i; break; }
    at = (uint8_t)((at + 1) % EN_REC_LIMITS_COUNT);
    rp_act_set_rec_limit(EN_REC_LIMITS[at]);
}

/* Fifteen-minute steps, wrapping through the day and off the end back to
   "none". Arbitrary minutes would need a second control and this device has
   no keyboard; a quarter of an hour is the granularity broadcast schedules
   actually use. */
static void on_rec_at(lv_event_t *e)
{
    (void)e;
    int at = rp_model.rectimer.at_min;
    if (at == EN_REC_AT_NONE) at = 0;
    else {
        at += 15;
        if (at >= 24 * 60) at = EN_REC_AT_NONE;
    }
    rp_act_set_rec_at((int16_t)at);
}

static void on_rec_at_back(lv_event_t *e)
{
    (void)e;
    int at = rp_model.rectimer.at_min;
    if (at == EN_REC_AT_NONE) at = 24 * 60 - 15;
    else {
        at -= 15;
        if (at < 0) at = EN_REC_AT_NONE;
    }
    rp_act_set_rec_at((int16_t)at);
}

/* ---- Recordings ------------------------------------------------------------ */

static void on_library_pick(lv_event_t *e)
{
    rp_act_play_file((const char *)lv_event_get_user_data(e));
    rp_ui_show(RP_SCREEN_NOW);
}

static void build_library(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_LIBRARY];
    lv_obj_t *cap = label(s, "RECORDINGS", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(cap, MARGIN, 10);
    hairline(s, 32);

    s_library_list = panel(s, 0, 40, RP_SCREEN_W, RP_SCREEN_H - 176, C_BG);
    lv_obj_add_flag(s_library_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_library_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_library_list, LV_SCROLLBAR_MODE_OFF);

    hairline(s, RP_SCREEN_H - 132);

    lv_obj_t *tcap = label(s, "TIMER", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(tcap, MARGIN, RP_SCREEN_H - 126);

    /* Length on the left, start time on the right, both cycling on tap. The
       start time also steps backwards on its own narrow target, because
       reaching 07:45 by tapping forward ninety-six times is not a control. */
    s_rec_limit_btn = panel(s, MARGIN, RP_SCREEN_H - 104, CONTENT_W / 2 - 4,
                            TAP_MIN, C_SURFACE);
    lv_obj_add_flag(s_rec_limit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_rec_limit_btn, on_rec_limit, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(s_rec_limit_btn, lv_color_hex(C_SURFACE_2),
                              LV_STATE_PRESSED);
    s_rec_limit_lbl = label(s_rec_limit_btn, "", F_BODY, C_TEXT);
    lv_obj_center(s_rec_limit_lbl);

    s_rec_at_btn = panel(s, MARGIN + CONTENT_W / 2 + 4, RP_SCREEN_H - 104,
                         CONTENT_W / 2 - 4 - 30, TAP_MIN, C_SURFACE);
    lv_obj_add_flag(s_rec_at_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_rec_at_btn, on_rec_at, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(s_rec_at_btn, lv_color_hex(C_SURFACE_2),
                              LV_STATE_PRESSED);
    s_rec_at_lbl = label(s_rec_at_btn, "", F_BODY, C_TEXT);
    lv_obj_center(s_rec_at_lbl);

    lv_obj_t *back = panel(s, MARGIN + CONTENT_W - 26, RP_SCREEN_H - 104, 26,
                           TAP_MIN, C_SURFACE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, on_rec_at_back, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(C_SURFACE_2),
                              LV_STATE_PRESSED);
    lv_obj_center(label(back, "-", F_BODY, C_TEXT_DIM));

    /* Two lines: what will happen, and - when a start time is set - the
       caveat, because a scheduled recording on a device with no real-time
       wake is a promise this app cannot keep on its own. */
    s_rec_timer_note = label(s, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_size(s_rec_timer_note, CONTENT_W, 36);
    lv_obj_set_pos(s_rec_timer_note, MARGIN, RP_SCREEN_H - 54);
}

static void refresh_timer_controls(void)
{
    if (!s_rec_limit_btn) return;

    char buf[96], n[16];

    if (rp_model.rectimer.limit_min) {
        buf[0] = 0;
        put_uint(n, rp_model.rectimer.limit_min, 0);
        cat(buf, n, sizeof buf);
        cat(buf, " min", sizeof buf);
        lv_label_set_text(s_rec_limit_lbl, buf);
    } else {
        lv_label_set_text(s_rec_limit_lbl, "No limit");
    }

    int at = rp_model.rectimer.at_min;
    if (at == EN_REC_AT_NONE) {
        lv_label_set_text(s_rec_at_lbl, "No start");
    } else {
        buf[0] = 0;
        put_uint(n, (uint32_t)(at / 60), 0);
        if (at / 60 < 10) cat(buf, "0", sizeof buf);
        cat(buf, n, sizeof buf);
        cat(buf, ":", sizeof buf);
        put_uint(n, (uint32_t)(at % 60), 0);
        if (at % 60 < 10) cat(buf, "0", sizeof buf);
        cat(buf, n, sizeof buf);
        lv_label_set_text(s_rec_at_lbl, buf);
    }

    int now = 0;
    bool have = clock_minutes(&now);
    buf[0] = 0;
    if (at == EN_REC_AT_NONE) {
        cat(buf, rp_model.rectimer.limit_min
                     ? "Recordings stop at the length above."
                     : "Recordings run until you stop them.",
            sizeof buf);
    } else if (!have) {
        /* The honest version. The clock is the station's, not the device's. */
        cat(buf, "Waiting for the station clock. No RDS time, no start.",
            sizeof buf);
    } else {
        int until = en_rectimer_until(&rp_model.rectimer, have, now);
        put_uint(n, (uint32_t)(until < 0 ? 0 : until), 0);
        cat(buf, "Starts in ", sizeof buf);
        cat(buf, n, sizeof buf);
        cat(buf, " min. Radio+ must stay open.", sizeof buf);
    }
    lv_label_set_text(s_rec_timer_note, buf);
}

static void refresh_library(void)
{
    /* Not built yet - see ensure_built. */
    if (!s_built[RP_SCREEN_LIBRARY]) return;
    refresh_timer_controls();
    lv_obj_clean(s_library_list);

    if (!rp_model.library_count) {
        lv_obj_t *e = label(s_library_list, "Nothing recorded yet", F_BODY,
                            C_TEXT_MUTE);
        lv_obj_set_pos(e, MARGIN, 20);
        return;
    }

    int y = 0;
    for (uint8_t i = 0; i < rp_model.library_count; i++) {
        lv_obj_t *r = row(s_library_list, y, 46, on_library_pick,
                          rp_model.library[i]);

        /* One line, truncated. LV_LABEL_LONG_DOT needs a height as well as a
           width - given only a width it wraps instead, and the second line
           landed on whatever was below it. */
        lv_obj_t *nm = label(r, rp_model.library[i], F_BODY, C_TEXT);
        lv_obj_set_size(nm, CONTENT_W, 20);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(nm, MARGIN, 13);

        panel(r, MARGIN, 45, CONTENT_W, 1, C_HAIRLINE);
        y += 46;
    }
}

/* ---- Settings -------------------------------------------------------------- */

static void on_ta_toggle(lv_event_t *e)
{
    (void)e;
    rp_act_ta_record(!rp_model.ta_record);
}

static void on_af_toggle(lv_event_t *e)
{
    (void)e;
    rp_act_af_follow(!rp_model.af.enabled);
}

/* How long the AF-to-presets result stays on the row, in refresh ticks. */
static uint8_t s_af_saved_for;
static uint8_t s_af_saved_n;

/*
 * Turn the station's own alternate-frequency list into presets.
 *
 * A neat trick rather than a workaround: the station has just told us every
 * frequency it can be heard on, which is a better preset list for a journey
 * than anything built by turning a dial - and unlike a band scan it costs
 * nothing and takes no time, because the list already arrived.
 */
static void on_af_presets(lv_event_t *e)
{
    (void)e;
    s_af_saved_n = rp_act_af_to_presets();
    s_af_saved_for = 40;
}

static void on_simple_toggle(lv_event_t *e)
{
    (void)e;
    rp_act_show_simple(!rp_model.simple_screen);
    rebuild_order();
    refresh_settings();
    refresh_dots();
}

static void on_wide_toggle(lv_event_t *e)
{
    (void)e;
    rp_act_show_wide(!rp_model.wide_screen);
    rebuild_order();
    refresh_settings();
    refresh_dots();
}

static void on_open_advanced(lv_event_t *e)
{
    (void)e;
    rp_ui_show(RP_SCREEN_ADVANCED);
}

static void on_open_rds(lv_event_t *e)
{
    (void)e;
    rp_ui_show(RP_SCREEN_RDS);
}

static void on_open_clock(lv_event_t *e)
{
    (void)e;
    rp_ui_show(RP_SCREEN_CLOCK);
}

/*
 * Cycling the output.
 *
 * It moves to the next output that will actually take the audio rather than to
 * the next one in the list, because the Bluetooth legs can be unavailable -
 * they need an encoder reading the fifo at the far end - and stepping onto one
 * that cannot work, only to be told so, is a control that makes the user do
 * the searching.
 *
 * When nothing else will take it, the reason the first candidate refused is
 * shown under the row and left there for a few seconds. "Switching failed" is
 * no better than a button that does nothing; "nothing is listening on
 * Bluetooth" tells somebody what to go and start.
 */
static char     s_out_why[64];
static uint32_t s_out_why_until;
#define OUT_WHY_MS 4000u

static void on_output_next(lv_event_t *e)
{
    uint8_t n = rp_out_count();
    const char *first_why = NULL;
    uint8_t k;

    (void)e;
    if (n < 2) return;

    for (k = 1; k < n; k++) {
        uint8_t i = (uint8_t)((rp_model.output + k) % n);
        const char *why = rp_act_set_output(i);

        if (!why) {
            s_out_why[0] = 0;
            return;
        }
        if (!first_why) first_why = why;
    }

    s_out_why[0] = 0;
    cat(s_out_why, first_why ? first_why : "no other output",
        sizeof s_out_why);
    s_out_why_until = lv_tick_get() + OUT_WHY_MS;
}

static void on_volume_slider(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    rp_act_set_volume((uint8_t)lv_slider_get_value(sl));
}

/*
 * The codec's own level, which is not the volume above it.
 *
 * It is here rather than hidden away because it is the control that decides
 * whether the output clips, and it is a slider that stops at 88 rather than at
 * 100 because 88 is where the range ends - the top of it pins the peak at
 * 0.0 dBFS and is audible as fuzz. The caption says so; the slider still lets
 * anybody go there, because somebody measuring their own unit may want to.
 */
static void on_hp_slider(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    rp_act_set_hp_level((uint8_t)lv_slider_get_value(sl));
}

/* The two sliders share this, because a slider styled by hand in two places is
   a slider that stops matching. */
static lv_obj_t *thin_slider(lv_obj_t *r, int y, int32_t lo, int32_t hi,
                             int32_t at, lv_event_cb_t cb)
{
    lv_obj_t *sl = lv_slider_create(r);

    lv_obj_set_size(sl, CONTENT_W - 8, 6);
    lv_obj_set_pos(sl, MARGIN, y);
    lv_slider_set_range(sl, lo, hi);
    lv_slider_set_value(sl, at, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(sl, lv_color_hex(C_SURFACE_2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_SIGNAL), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_set_style_radius(sl, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 2, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(sl, 7, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 18);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, 0);
    return sl;
}

static void on_region_next(lv_event_t *e)
{
    (void)e;
    /* Cycle rather than open a picker: six entries is too few to justify a
       screen of its own, and cycling keeps the setting visible while it
       changes. */
    uint8_t at = 0;
    for (uint8_t i = 0; i < en_region_count; i++)
        if (&en_regions[i] == rp_model.region) { at = i; break; }
    rp_act_set_region(&en_regions[(at + 1u) % en_region_count]);
}

static lv_obj_t *setting_row(lv_obj_t *parent, int y, int h, const char *name,
                             const char *sub, lv_event_cb_t cb)
{
    lv_obj_t *r = row(parent, y, h, cb, 0);

    lv_obj_t *n = label(r, name, F_BODY, C_TEXT);
    /* Centred in a short row, but pinned to the top of a tall one - a tall
       row is tall because it carries a value below the name, and centring the
       name there puts it straight through that value. */
    lv_obj_set_pos(n, MARGIN, (sub || h > 56) ? 8 : (h - 20) / 2);

    if (sub) {
        lv_obj_t *sl = para(r, sub, F_CAPTION, C_TEXT_MUTE, CONTENT_W);
        lv_obj_set_pos(sl, MARGIN, 30);
    }

    /* Drawn from the height the row actually is. Deriving it from a default
       instead put the rule through the middle of the two tall rows. */
    panel(r, MARGIN, h - 1, CONTENT_W, 1, C_HAIRLINE);
    return r;
}

/* A value shown at the right of a row, on the name's line. */
static lv_obj_t *row_value(lv_obj_t *r, uint32_t colour)
{
    lv_obj_t *v = label(r, "", F_BODY, colour);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(v, CONTENT_W);
    lv_obj_set_pos(v, MARGIN, 8);
    return v;
}

static void build_settings(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_SETTINGS];
    lv_obj_t *cap = label(s, "SETTINGS", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(cap, MARGIN, 10);
    hairline(s, 32);

    /* Scrolls, because the content has outgrown the screen. The alternative
       was squeezing the two backend rows until the sysfs path truncated again,
       and a path that cannot be read is worth less than a scroll. */
    lv_obj_t *body = panel(s, 0, 40, RP_SCREEN_W, RP_SCREEN_H - 66, C_BG);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    s = body;

    int y = 4;

    lv_obj_t *r = setting_row(s, y, 48, "Region", 0, on_region_next);
    s_set_region = row_value(r, C_SIGNAL);
    lv_obj_set_pos(s_set_region, MARGIN, 14);
    y += 48;

    r = setting_row(s, y, 48, "RDS standard", 0, 0);
    s_set_std = row_value(r, C_RDS);
    lv_obj_set_pos(s_set_std, MARGIN, 14);
    y += 48;

    /*
     * Where the sound goes, and how loud - near the top because they are the
     * two settings anybody actually opens this screen for, and because on a
     * device that can play to headphones and Bluetooth at the same time
     * "which one is this going to" is a question people ask before any other.
     */
    r = setting_row(s, y, 62, "Output", 0, on_output_next);
    s_set_output = row_value(r, C_SIGNAL);
    lv_obj_set_pos(s_set_output, MARGIN, 14);
    /* Only ever a reason, never a description: this row is a control, and the
       device it resolved to is on the Playback row below with the other two
       backends, which are sized for a string that wraps. */
    s_set_output_note = para(r, "", F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(s_set_output_note, MARGIN, 32);
    y += 62;

    r = setting_row(s, y, 66, "Volume", 0, 0);
    s_set_vol_val = row_value(r, C_SIGNAL);
    lv_obj_set_pos(s_set_vol_val, MARGIN, 8);
    s_set_vol = thin_slider(r, 40, 0, 100, 100, on_volume_slider);
    y += 66;

    /* The codec's analog level. A separate row because it is a separate kind
       of thing: it applies to the headphone leg only, it is set once against a
       meter rather than adjusted while listening, and the top of its range
       clips - which is what the caption is for. */
    /* 94 and not 80: the caption below the slider wraps to two lines, and a
       row measured for one puts the second through its own rule. */
    r = setting_row(s, y, 94, "Headphone level", 0, 0);
    s_set_hp_val = row_value(r, C_TEXT_DIM);
    lv_obj_set_pos(s_set_hp_val, MARGIN, 8);
    s_set_hp = thin_slider(r, 40, 0, 88, 75, on_hp_slider);
    {
        lv_obj_t *n = para(r, "The codec's own volume. 75 is calibrated; above "
                              "about 80 it clips.", F_CAPTION, C_TEXT_MUTE,
                           CONTENT_W);
        lv_obj_set_pos(n, MARGIN, 54);
    }
    y += 94;

    /* These two keep their text because it is not a description of a control -
       it is which driver and which device, which is the answer to why the
       radio is or is not working and cannot be seen any other way. */
    r = setting_row(s, y, 88, "Tuner", 0, 0);
    s_set_backend = para(r, "", F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(s_set_backend, MARGIN, 30);
    y += 88;

    r = setting_row(s, y, 88, "Capture", 0, 0);
    s_set_capture = para(r, "", F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(s_set_capture, MARGIN, 30);
    y += 88;

    /* The other half of the audio path, which had no row at all - so "no audio
       out" was the whole of what could be learned about it, and a missing card
       and a missing /etc/asound.conf looked identical. */
    r = setting_row(s, y, 88, "Playback", 0, 0);
    s_set_play = para(r, "", F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(s_set_play, MARGIN, 30);
    y += 88;

    r = setting_row(s, y, 48, "Record traffic", 0, on_ta_toggle);
    s_set_ta = row_value(r, C_TA);
    lv_obj_set_pos(s_set_ta, MARGIN, 14);
    y += 48;

    /* Following the station across transmitters. Off by default, and the
       badge on Now Playing is the other way to turn it off - which is the one
       that matters, because the moment you want it off is the moment you are
       watching it do something wrong. */
    r = setting_row(s, y, 62, "Follow station (AF)", 0, on_af_toggle);
    s_set_af = row_value(r, C_RDS);
    lv_obj_set_pos(s_set_af, MARGIN, 14);
    s_set_af_note = para(r, "Only moves to a matching station ID, and only if "
                            "it is stronger.", F_CAPTION, C_TEXT_MUTE,
                         CONTENT_W);
    lv_obj_set_pos(s_set_af_note, MARGIN, 32);
    y += 62;

    r = setting_row(s, y, 48, "Save alternates as presets", 0, on_af_presets);
    s_set_af_save = row_value(r, C_SIGNAL);
    lv_obj_set_pos(s_set_af_save, MARGIN, 14);
    y += 48;

    /* The two optional screens. Both add a page to the swipe sequence, which
       is why they are a setting and not always on: a sequence you have learned
       should not grow a page because someone shipped a feature. */
    r = setting_row(s, y, 48, "Simple screen", 0, on_simple_toggle);
    s_set_simple = row_value(r, C_SIGNAL);
    lv_obj_set_pos(s_set_simple, MARGIN, 14);
    y += 48;

    r = setting_row(s, y, 48, "Landscape readout", 0, on_wide_toggle);
    s_set_wide = row_value(r, C_SIGNAL);
    lv_obj_set_pos(s_set_wide, MARGIN, 14);
    y += 48;

    /*
      * Two screens for the curious, above the register list because both are
      * safe to open and it is not.
      */
    r = setting_row(s, y, 48, "RDS inspector", 0, on_open_rds);
    lv_obj_t *chev_r = label(r, LV_SYMBOL_RIGHT, F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(chev_r, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(chev_r, CONTENT_W);
    lv_obj_set_pos(chev_r, MARGIN, 16);
    y += 48;

    r = setting_row(s, y, 62, "Set clock from FM", 0, on_open_clock);
    lv_obj_t *chev_c = label(r, LV_SYMBOL_RIGHT, F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(chev_c, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(chev_c, CONTENT_W);
    lv_obj_set_pos(chev_c, MARGIN, 16);
    {
        lv_obj_t *n = para(r, "Broadcasters transmit the time. This device has "
                              "no other way to learn it.", F_CAPTION,
                           C_TEXT_MUTE, CONTENT_W);
        lv_obj_set_pos(n, MARGIN, 32);
    }
    y += 62;

    r = setting_row(s, y, 48, "Advanced", 0, on_open_advanced);
    lv_obj_t *chev = label(r, LV_SYMBOL_RIGHT, F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(chev, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(chev, CONTENT_W);
    lv_obj_set_pos(chev, MARGIN, 16);

    /* Last item in the scroll rather than pinned to the bottom, so it cannot
       land on top of whatever the last row happens to be. */
    /* The real build stamp, not __DATE__: that expands when THIS file is
       compiled, so a binary relinked after a change elsewhere would carry a
       date older than itself. This one comes from the makefile and is the
       same string in every app, alongside the commit it was built from. */
    lv_obj_t *stamp = label(s, en_build_version(), F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(stamp, MARGIN, y + 60);
}

static void refresh_settings(void)
{
    /* Not built yet - see ensure_built. */
    if (!s_built[RP_SCREEN_SETTINGS]) return;
    lv_label_set_text(s_set_region,
                      rp_model.region ? rp_model.region->name : "-");
    lv_label_set_text(s_set_std, rp_model.rds.rbds ? "RBDS" : "RDS");

    lv_label_set_text(s_set_output, rp_out_label(rp_model.output));
    /* Dimmed when the chosen destination is not actually receiving anything,
       which on this device means the Bluetooth encoder has gone. The name
       stays, because it is still what was chosen. */
    lv_obj_set_style_text_color(s_set_output,
        lv_color_hex(rp_model.output_open ? C_SIGNAL : C_TEXT_MUTE), 0);

    if (s_out_why[0] && (int32_t)(lv_tick_get() - s_out_why_until) < 0) {
        lv_label_set_text(s_set_output_note, s_out_why);
    } else {
        s_out_why[0] = 0;
        lv_label_set_text(s_set_output_note,
                          rp_model.output_open ? "" : "not receiving audio");
        lv_obj_set_style_text_color(s_set_output_note,
            lv_color_hex(rp_model.output_open ? C_TEXT_MUTE : C_TA), 0);
    }

    lv_label_set_text(s_set_play,
                      rp_model.play_backend ? rp_model.play_backend
                                            : "not started");

    {
        char b[16];

        b[0] = 0;
        put_uint(b, rp_model.volume, 0);
        cat(b, "%", sizeof b);
        lv_label_set_text(s_set_vol_val, rp_model.volume_ok ? b : "-");
        /* Not while it is being dragged: writing the model's value back into a
           slider under a finger fights the finger. */
        if (!lv_obj_has_state(s_set_vol, LV_STATE_PRESSED))
            lv_slider_set_value(s_set_vol, rp_model.volume, LV_ANIM_OFF);

        b[0] = 0;
        put_uint(b, rp_model.hp_level, 0);
        cat(b, " / 88", sizeof b);
        lv_label_set_text(s_set_hp_val, rp_model.hp_level_ok ? b : "-");
        if (!lv_obj_has_state(s_set_hp, LV_STATE_PRESSED))
            lv_slider_set_value(s_set_hp, rp_model.hp_level, LV_ANIM_OFF);
    }

    lv_label_set_text(s_set_backend,
                      rp_model.backend ? rp_model.backend : "not detected");
    lv_label_set_text(s_set_capture,
                      rp_model.capture_backend ? rp_model.capture_backend
                                               : "not started");
    lv_label_set_text(s_set_ta, rp_model.ta_record ? "On" : "Off");

    /* The state text, not just on/off: "AF TRY" on a settings row is how you
       find out the radio is mid-attempt while you are looking at settings. */
    lv_label_set_text(s_set_af, rp_model.af.enabled
                                    ? en_af_state_text(&rp_model.af) : "Off");
    lv_obj_set_style_text_color(s_set_af,
        lv_color_hex(rp_model.af.enabled ? C_RDS : C_TEXT_MUTE), 0);

    if (s_af_saved_for) {
        char b[48];
        b[0] = 0;
        put_uint(b, s_af_saved_n, 0);
        cat(b, s_af_saved_n == 1 ? " added" : " added", sizeof b);
        lv_label_set_text(s_set_af_save, b);
        s_af_saved_for--;
    } else {
        char b[48];
        b[0] = 0;
        put_uint(b, rp_model.rds.af_count, 0);
        cat(b, rp_model.rds.af_count == 1 ? " on offer" : " on offer",
            sizeof b);
        lv_label_set_text(s_set_af_save,
                          rp_model.rds.af_count ? b : "none advertised");
    }
    lv_label_set_text(s_set_simple, rp_model.simple_screen ? "On" : "Off");
    lv_obj_set_style_text_color(s_set_simple,
        lv_color_hex(rp_model.simple_screen ? C_SIGNAL : C_TEXT_MUTE), 0);
    lv_label_set_text(s_set_wide, rp_model.wide_screen ? "On" : "Off");
    lv_obj_set_style_text_color(s_set_wide,
        lv_color_hex(rp_model.wide_screen ? C_SIGNAL : C_TEXT_MUTE), 0);
    lv_obj_set_style_text_color(s_set_ta,
                                lv_color_hex(rp_model.ta_record ? C_TA
                                                                : C_TEXT_MUTE),
                                0);
}

/* ---- Advanced: the register explorer --------------------------------------- */

static void on_register_pick(lv_event_t *e)
{
    rp_ui_open_register((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

static void on_advanced_back(lv_event_t *e)
{
    (void)e;
    rp_ui_show(RP_SCREEN_SETTINGS);
}

static void build_advanced(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_ADVANCED];

    lv_obj_t *back = panel(s, 0, 0, RP_SCREEN_W, 40, C_BG);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, on_advanced_back, LV_EVENT_CLICKED, 0);
    lv_obj_t *b = label(back, LV_SYMBOL_LEFT "  REGISTERS", F_CAPTION,
                        C_TEXT_MUTE);
    lv_obj_set_pos(b, MARGIN, 12);
    hairline(s, 40);

    /* No separate notice line: when raw access is unavailable the reason goes
       into the list itself, so an empty notice never leaves a gap above it. */
    s_adv_list = panel(s, 0, 50, RP_SCREEN_W, RP_SCREEN_H - 70, C_BG);
    lv_obj_add_flag(s_adv_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_adv_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_adv_list, LV_SCROLLBAR_MODE_OFF);
}

static void refresh_advanced(void)
{
    /* Not built yet - see ensure_built. */
    if (!s_built[RP_SCREEN_ADVANCED]) return;
    lv_obj_clean(s_adv_list);

    /* The whole screen is generated from the register table, which is why
       supporting every register is a table entry rather than a screen each. */
    if (!rp_model.can_raw) {
        lv_obj_t *no = label(s_adv_list, "Not available here", F_BODY, C_TA);
        lv_obj_set_pos(no, MARGIN, 8);
        lv_obj_t *why = para(s_adv_list,
            "Only where a driver owns the transport.",
            F_CAPTION, C_TEXT_MUTE, CONTENT_W);
        lv_obj_set_pos(why, MARGIN, 32);
        return;
    }

    int y = 0;
    char buf[80];
    for (uint8_t i = 0; i < en_fm_reg_count; i++) {
        const en_fm_reg_t *g = &en_fm_regs[i];
        lv_obj_t *r = row(s_adv_list, y, 54, on_register_pick,
                          (void *)(uintptr_t)g->addr);

        /* Address in hex, because that is how the datasheet and every other
           tool refer to it, and matching them is the whole point of this
           screen existing. */
        static const char hex[] = "0123456789ABCDEF";
        buf[0] = '0'; buf[1] = 'x';
        buf[2] = hex[(g->addr >> 4) & 0xF];
        buf[3] = hex[g->addr & 0xF];
        buf[4] = 0;
        lv_obj_t *a = label(r, buf, F_BODY, C_SIGNAL);
        lv_obj_set_pos(a, MARGIN, 8);

        lv_obj_t *nm = label(r, g->name, F_CAPTION, C_TEXT);
        lv_obj_set_width(nm, CONTENT_W - 52);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(nm, MARGIN + 52, 10);

        /* Access, and the honest caveats: a length nobody verified, and a
           register the documentation says must be read before it is written. */
        buf[0] = 0;
        cat(buf, (g->flags & EN_FM_R) ? "R" : "-", sizeof buf);
        cat(buf, (g->flags & EN_FM_W) ? "W" : "-", sizeof buf);
        if (g->flags & EN_FM_RMW) cat(buf, "  read-modify-write", sizeof buf);
        if (g->flags & EN_FM_TBD) cat(buf, "  length unverified", sizeof buf);

        lv_obj_t *fl = label(r, buf, F_CAPTION,
                             (g->flags & EN_FM_TBD) ? C_TA : C_TEXT_MUTE);
        lv_obj_set_pos(fl, MARGIN + 52, 30);

        panel(r, MARGIN, 53, CONTENT_W, 1, C_HAIRLINE);
        y += 54;
    }
}

/* ---- one register, field by field ------------------------------------------ */

/*
 * Every control here is generated from the field descriptors in fmreg.c. A
 * bitmap becomes a list of toggles named by its bits, an enum becomes a list of
 * options, and everything else becomes a slider bounded by the range the table
 * declares. Nothing about any particular register is written down twice.
 */

static void reg_push(void)
{
    if (!s_reg) return;
    rp_act_reg_write(s_reg->addr, s_reg_val, s_reg->write_len);
}

/* user_data packs which field and which bit, since a toggle needs both. */
#define PACK(f, b)  ((void *)(uintptr_t)(((uint32_t)(f) << 8) | (uint8_t)(b)))
#define UNPACK_F(p) ((uint8_t)(((uintptr_t)(p) >> 8) & 0xFF))
#define UNPACK_B(p) ((uint8_t)((uintptr_t)(p) & 0xFF))

static void on_bit_toggle(lv_event_t *e)
{
    void *u = lv_event_get_user_data(e);
    if (!s_reg) return;

    const en_fm_field_t *f = &s_reg->fields[UNPACK_F(u)];
    uint32_t mask = f->bits[UNPACK_B(u)].mask;

    int32_t v = en_fm_field_get(f, s_reg_val, s_reg->write_len);
    v = (int32_t)(((uint32_t)v) ^ mask);

    if (en_fm_field_set(f, s_reg_val, s_reg->write_len, v)) reg_push();
    rp_ui_open_register(s_reg->addr);      /* redraw with the new state */
}

static void on_enum_pick(lv_event_t *e)
{
    void *u = lv_event_get_user_data(e);
    if (!s_reg) return;

    const en_fm_field_t *f = &s_reg->fields[UNPACK_F(u)];
    int32_t v = (int32_t)f->vals[UNPACK_B(u)].value;

    if (en_fm_field_set(f, s_reg_val, s_reg->write_len, v)) reg_push();
    rp_ui_open_register(s_reg->addr);
}

static void on_slider(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    void *u = lv_event_get_user_data(e);
    if (!s_reg) return;

    const en_fm_field_t *f = &s_reg->fields[UNPACK_F(u)];
    if (en_fm_field_set(f, s_reg_val, s_reg->write_len,
                        (int32_t)lv_slider_get_value(sl)))
        reg_push();

    /* Only the readout is updated here. Rebuilding the screen mid-drag would
       destroy the slider under the finger. */
    lv_obj_t *val = lv_obj_get_child(lv_obj_get_parent(sl), 1);
    if (val) {
        char b[16];
        put_uint(b, (uint32_t)lv_slider_get_value(sl), 0);
        lv_label_set_text(val, b);
    }
}

static void on_reg_back(lv_event_t *e)
{
    (void)e;
    rp_ui_show(RP_SCREEN_ADVANCED);
}

static void on_reg_revert(lv_event_t *e)
{
    (void)e;
    if (!s_reg) return;
    rp_act_reg_revert(s_reg->addr);
    rp_ui_open_register(s_reg->addr);
}

/* A row whose right-hand chip is filled when the thing is on. Used for both
   bitmap bits and enum options, because "this one is selected" reads the same
   either way. */
static int reg_choice(lv_obj_t *parent, int y, const char *name, bool on,
                      lv_event_cb_t cb, void *user)
{
    lv_obj_t *r = row(parent, y, 44, cb, user);

    lv_obj_t *l = label(r, name, F_CAPTION, on ? C_TEXT : C_TEXT_DIM);
    lv_obj_set_width(l, CONTENT_W - 40);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(l, MARGIN + 8, 14);

    lv_obj_t *chip = panel(r, RP_SCREEN_W - MARGIN - 24, 15, 14, 14,
                           on ? C_SIGNAL : C_SURFACE_2);
    lv_obj_set_style_radius(chip, 3, 0);

    panel(r, MARGIN + 8, 43, CONTENT_W - 8, 1, C_HAIRLINE);
    return y + 44;
}

static int reg_slider(lv_obj_t *parent, int y, const en_fm_field_t *f,
                      uint8_t fi)
{
    lv_obj_t *r = row(parent, y, 66, 0, 0);

    lv_obj_t *n = label(r, f->name, F_CAPTION, C_TEXT);
    lv_obj_set_pos(n, MARGIN + 8, 8);

    /* Child index 1: on_slider finds the readout this way rather than caching
       a pointer per field. */
    int32_t cur = en_fm_field_get(f, s_reg_val, s_reg->write_len);
    char b[16];
    put_uint(b, (uint32_t)(cur < 0 ? -cur : cur), 0);
    lv_obj_t *val = label(r, b, F_CAPTION, C_SIGNAL);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(val, CONTENT_W - 16);
    lv_obj_set_pos(val, MARGIN + 8, 8);

    lv_obj_t *sl = lv_slider_create(r);
    lv_obj_set_size(sl, CONTENT_W - 16, 6);
    lv_obj_set_pos(sl, MARGIN + 8, 36);
    lv_slider_set_range(sl, f->min, f->max);
    lv_slider_set_value(sl, cur, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(sl, lv_color_hex(C_SURFACE_2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_SIGNAL), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_set_style_radius(sl, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 2, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(sl, 7, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 18);
    lv_obj_add_event_cb(sl, on_slider, LV_EVENT_VALUE_CHANGED, PACK(fi, 0));

    panel(r, MARGIN + 8, 65, CONTENT_W - 8, 1, C_HAIRLINE);
    return y + 66;
}

static void build_register(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_REGISTER];

    lv_obj_t *back = panel(s, 0, 0, RP_SCREEN_W - 76, 40, C_BG);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, on_reg_back, LV_EVENT_CLICKED, 0);
    lv_obj_t *b = label(back, LV_SYMBOL_LEFT "  REGISTERS", F_CAPTION,
                        C_TEXT_MUTE);
    lv_obj_set_pos(b, MARGIN, 12);

    /* Only shown when this register has actually been overridden - an always
       present Revert invites undoing something that was never done. */
    s_reg_revert = panel(s, RP_SCREEN_W - 76, 0, 76, 40, C_BG);
    lv_obj_add_flag(s_reg_revert, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_reg_revert, on_reg_revert, LV_EVENT_CLICKED, 0);
    lv_obj_t *rv = label(s_reg_revert, "REVERT", F_CAPTION, C_TA);
    lv_obj_set_pos(rv, 12, 12);

    /* The address in large type on the left - it is short, and it is what you
       match against a datasheet - with the raw value opposite it. */
    s_reg_title = label(s, "", F_TITLE, C_SIGNAL);
    lv_obj_set_pos(s_reg_title, MARGIN, 46);

    s_reg_hex = label(s, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_reg_hex, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_reg_hex, CONTENT_W);
    lv_obj_set_pos(s_reg_hex, MARGIN, 52);

    /* The name underneath, at a size that fits. Given a height as well as a
       width, so LV_LABEL_LONG_DOT truncates rather than wrapping. */
    s_reg_name = label(s, "", F_BODY, C_TEXT);
    lv_obj_set_size(s_reg_name, CONTENT_W, 20);
    lv_label_set_long_mode(s_reg_name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_reg_name, MARGIN, 76);

    s_reg_doc = para(s, "", F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(s_reg_doc, MARGIN, 102);

    hairline(s, 146);
    s_reg_body = panel(s, 0, 152, RP_SCREEN_W, RP_SCREEN_H - 162, C_BG);
    lv_obj_add_flag(s_reg_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_reg_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_reg_body, LV_SCROLLBAR_MODE_OFF);
}

void rp_ui_open_register(uint8_t addr)
{
    s_reg = en_fm_reg_find(addr);
    if (!s_reg) return;

    /* Start from what the chip currently holds, so editing one field of a
       packed register leaves its neighbours where they were. Registers that
       cannot be read start from an override if there is one, and from zero if
       not - which is stated on screen rather than left to be discovered. */
    for (uint8_t i = 0; i < 8; i++) s_reg_val[i] = 0;
    bool live = (s_reg->flags & EN_FM_R)
             && rp_act_reg_read(addr, s_reg_val,
                                s_reg->write_len ? s_reg->write_len
                                                 : s_reg->read_len);

    char buf[96];
    static const char hex[] = "0123456789ABCDEF";

    buf[0] = '0'; buf[1] = 'x';
    buf[2] = hex[(addr >> 4) & 0xF];
    buf[3] = hex[addr & 0xF];
    buf[4] = 0;
    lv_label_set_text(s_reg_title, buf);
    lv_label_set_text(s_reg_name, s_reg->name);

    /* The raw value, because anyone on this screen is cross-referencing a
       datasheet and wants the number the fields add up to. */
    buf[0] = 0;
    if (s_reg->write_len) {
        cat(buf, "= 0x", sizeof buf);
        char h[3] = { 0, 0, 0 };
        for (uint8_t i = 0; i < s_reg->write_len; i++) {
            h[0] = hex[(s_reg_val[i] >> 4) & 0xF];
            h[1] = hex[s_reg_val[i] & 0xF];
            cat(buf, h, sizeof buf);
        }
    }
    if (!live && (s_reg->flags & EN_FM_R)) cat(buf, "   (not read)", sizeof buf);
    lv_label_set_text(s_reg_hex, buf);
    lv_label_set_text(s_reg_doc, s_reg->doc ? s_reg->doc : "");

    if (rp_act_reg_overridden(addr))
        lv_obj_remove_flag(s_reg_revert, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_reg_revert, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clean(s_reg_body);
    int y = 0;

    if (!s_reg->nfields) {
        lv_obj_t *n = label(s_reg_body, "No fields described", F_CAPTION,
                            C_TEXT_MUTE);
        lv_obj_set_pos(n, MARGIN, 8);
    }

    for (uint8_t i = 0; i < s_reg->nfields; i++) {
        const en_fm_field_t *f = &s_reg->fields[i];

        /* A register that cannot be written gets its values shown and no
           controls - offering a slider that does nothing would be a lie. */
        bool editable = (s_reg->flags & EN_FM_W) != 0;

        if ((f->flags & EN_FMF_BITMAP) && f->bits) {
            lv_obj_t *h = label(s_reg_body, f->name, F_CAPTION, C_TEXT_MUTE);
            lv_obj_set_pos(h, MARGIN, y + 8);
            y += 30;

            int32_t v = en_fm_field_get(f, s_reg_val, s_reg->write_len
                                                    ? s_reg->write_len
                                                    : s_reg->read_len);
            for (uint8_t b = 0; b < f->nbits; b++)
                y = reg_choice(s_reg_body, y, f->bits[b].name,
                               (((uint32_t)v) & f->bits[b].mask) != 0,
                               editable ? on_bit_toggle : 0, PACK(i, b));

        } else if ((f->flags & EN_FMF_ENUM) && f->vals) {
            lv_obj_t *h = label(s_reg_body, f->name, F_CAPTION, C_TEXT_MUTE);
            lv_obj_set_pos(h, MARGIN, y + 8);
            y += 30;

            int32_t v = en_fm_field_get(f, s_reg_val, s_reg->write_len
                                                    ? s_reg->write_len
                                                    : s_reg->read_len);
            for (uint8_t k = 0; k < f->nvals; k++)
                y = reg_choice(s_reg_body, y, f->vals[k].name,
                               (uint32_t)v == f->vals[k].value,
                               editable ? on_enum_pick : 0, PACK(i, k));

        } else if (editable) {
            y = reg_slider(s_reg_body, y, f, i);

        } else {
            /* Read-only: the name and its value, nothing to touch. */
            lv_obj_t *r = row(s_reg_body, y, 44, 0, 0);
            lv_obj_t *n = label(r, f->name, F_CAPTION, C_TEXT_DIM);
            lv_obj_set_pos(n, MARGIN, 14);

            int32_t v = en_fm_field_get(f, s_reg_val, s_reg->read_len);
            char vb[16];
            if (v < 0) { vb[0] = '-'; put_uint(vb + 1, (uint32_t)(-v), 0); }
            else put_uint(vb, (uint32_t)v, 0);

            lv_obj_t *vl = label(r, vb, F_CAPTION, C_SIGNAL);
            lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_width(vl, CONTENT_W);
            lv_obj_set_pos(vl, MARGIN, 14);
            panel(r, MARGIN, 43, CONTENT_W, 1, C_HAIRLINE);
            y += 44;
        }
    }

    lv_screen_load(s_screen[RP_SCREEN_REGISTER]);
    s_current = RP_SCREEN_REGISTER;
}

/* ---- lifecycle ------------------------------------------------------------- */

static void build_screen_shell(rp_screen_t which)
{
    lv_obj_t *s = lv_obj_create(NULL);
    flat(s, C_BG);
    lv_obj_set_size(s, RP_SCREEN_W, RP_SCREEN_H);
    lv_obj_add_event_cb(s, on_gesture, LV_EVENT_GESTURE, 0);
    s_screen[which] = s;
}

/* ---- RDS inspector --------------------------------------------------------
 *
 * Every field RDS carries, on a screen, on an iPod.
 *
 * The decoder has always held far more than the four things Now Playing shows.
 * PI is not an opaque number - it is a country, a coverage area and a
 * programme reference, and in North America it spells the call sign outright.
 * The decoder identification bits say whether the transmission is stereo and
 * whether the programme type is being switched dynamically. The group counter
 * says what a station actually transmits, which is the difference between a
 * station that is running RadioText+ and one that has been announcing it in
 * group 3A for six years while sending nothing in the slot.
 *
 * None of that helps a listener and all of it helps somebody standing in front
 * of a transmitter. It costs one screen.
 *
 * Built once and updated by text rather than rebuilt each refresh: fifty-odd
 * labels reconstructed twice a second on this part is visibly slow, and a
 * value that changes twice an hour does not need its widget thrown away.
 */

static lv_obj_t *s_rds_body;
static lv_obj_t *s_rds_pi, *s_rds_call, *s_rds_cc, *s_rds_cov, *s_rds_ref;
static lv_obj_t *s_rds_ecc, *s_rds_pin, *s_rds_pty, *s_rds_ptyn;
static lv_obj_t *s_rds_flags, *s_rds_di;
static lv_obj_t *s_rds_ps, *s_rds_rt, *s_rds_rtp;
static lv_obj_t *s_rds_ct, *s_rds_ctutc, *s_rds_af;
static lv_obj_t *s_rds_oda[EN_RDS_ODA_MAX];
static lv_obj_t *s_rds_totals;
static lv_obj_t *s_rds_grp[32];

/* 0xC2B5, four digits, always. A PI with leading zeros stripped is a PI
   somebody has to count the digits of. */
static void put_hex4(char *out, uint16_t v)
{
    static const char H[] = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    out[2] = H[(v >> 12) & 0xF];
    out[3] = H[(v >> 8) & 0xF];
    out[4] = H[(v >> 4) & 0xF];
    out[5] = H[v & 0xF];
    out[6] = 0;
}

static void insp_head(lv_obj_t *p, int *y, const char *text)
{
    lv_obj_t *l = label(p, text, F_CAPTION, C_SIGNAL);
    lv_obj_set_pos(l, MARGIN, *y + 8);
    *y += 26;
    hairline(p, *y - 4);
}

/* Caption on the left, value on the right, one line each. The caption column
   is wide enough for the longest of them so the values line up, which is what
   makes a screen like this scannable rather than merely complete. */
#define INSP_CAP_W 88     /* the caption column; the value starts after it */

static lv_obj_t *insp_row(lv_obj_t *p, int *y, const char *cap)
{
    lv_obj_t *c = label(p, cap, F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(c, MARGIN, *y);
    /* Bounded, not trusted to be short. A caption is a string in this file and
       the value beside it starts at a fixed x, so an unbounded label is the
       same overlap the chip row had - the first long caption writes over its
       own value. */
    lv_obj_set_width(c, INSP_CAP_W);
    lv_label_set_long_mode(c, LV_LABEL_LONG_DOT);

    lv_obj_t *v = label(p, "-", F_CAPTION, C_TEXT);
    lv_obj_set_pos(v, MARGIN + 92, *y);
    lv_obj_set_width(v, CONTENT_W - 92);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    *y += 19;
    return v;
}

/* Caption above, value wrapped below, for the things that are text rather than
   a field: the programme service name, radio text, an alternate frequency
   list. */
static lv_obj_t *insp_block(lv_obj_t *p, int *y, const char *cap, int lines)
{
    lv_obj_t *c = label(p, cap, F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(c, MARGIN, *y);
    *y += 17;

    lv_obj_t *v = para(p, "", F_CAPTION, C_TEXT, CONTENT_W);
    lv_obj_set_pos(v, MARGIN, *y);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    *y += 17 * lines + 4;
    return v;
}

static void on_rds_back(lv_event_t *e)
{
    (void)e;
    rp_ui_show(RP_SCREEN_SETTINGS);
}

static void build_rds(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_RDS];
    int y = 4;
    uint8_t i;

    lv_obj_t *back = panel(s, 0, 0, RP_SCREEN_W, 40, C_BG);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, on_rds_back, LV_EVENT_CLICKED, 0);
    lv_obj_t *b = label(back, LV_SYMBOL_LEFT "  RDS INSPECTOR", F_CAPTION,
                        C_TEXT_MUTE);
    lv_obj_set_pos(b, MARGIN, 12);
    hairline(s, 40);

    s_rds_body = panel(s, 0, 44, RP_SCREEN_W, RP_SCREEN_H - 60, C_BG);
    lv_obj_add_flag(s_rds_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_rds_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_rds_body, LV_SCROLLBAR_MODE_OFF);

    insp_head(s_rds_body, &y, "IDENTIFICATION");
    s_rds_pi   = insp_row(s_rds_body, &y, "PI");
    s_rds_call = insp_row(s_rds_body, &y, "Call sign");
    s_rds_cc   = insp_row(s_rds_body, &y, "Country");
    s_rds_cov  = insp_row(s_rds_body, &y, "Coverage");
    s_rds_ref  = insp_row(s_rds_body, &y, "Reference");
    s_rds_ecc  = insp_row(s_rds_body, &y, "ECC");
    s_rds_pin  = insp_row(s_rds_body, &y, "PIN");

    insp_head(s_rds_body, &y, "PROGRAMME");
    s_rds_pty   = insp_row(s_rds_body, &y, "Type");
    s_rds_ptyn  = insp_row(s_rds_body, &y, "Type name");
    s_rds_flags = insp_row(s_rds_body, &y, "Flags");
    s_rds_di    = insp_row(s_rds_body, &y, "Decoder ID");

    insp_head(s_rds_body, &y, "TEXT");
    s_rds_ps  = insp_block(s_rds_body, &y, "Programme service", 1);
    s_rds_rt  = insp_block(s_rds_body, &y, "RadioText", 4);
    s_rds_rtp = insp_block(s_rds_body, &y, "RadioText+", 2);

    insp_head(s_rds_body, &y, "CLOCK");
    s_rds_ct    = insp_row(s_rds_body, &y, "Station time");
    s_rds_ctutc = insp_row(s_rds_body, &y, "UTC / offset");

    insp_head(s_rds_body, &y, "ALTERNATE FREQUENCIES");
    s_rds_af = insp_block(s_rds_body, &y, "Announced", 4);

    insp_head(s_rds_body, &y, "OPEN DATA APPLICATIONS");
    for (i = 0; i < EN_RDS_ODA_MAX; i++) {
        s_rds_oda[i] = label(s_rds_body, "", F_CAPTION, C_TEXT);
        lv_obj_set_pos(s_rds_oda[i], MARGIN, y);
        lv_obj_set_width(s_rds_oda[i], CONTENT_W);
        lv_label_set_long_mode(s_rds_oda[i], LV_LABEL_LONG_DOT);
        y += 18;
    }

    insp_head(s_rds_body, &y, "GROUPS RECEIVED");
    s_rds_totals = para(s_rds_body, "", F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(s_rds_totals, MARGIN, y);
    y += 36;

    /*
     * All thirty-two, hidden until one arrives.
     *
     * This is the part a broadcast engineer actually wants: what a station
     * transmits, in what proportion, as against what it claims. Building them
     * all and hiding the empties keeps the order stable, so a group appearing
     * for the first time slots into place rather than reshuffling the list
     * under somebody's finger.
     */
    for (i = 0; i < 32; i++) {
        s_rds_grp[i] = label(s_rds_body, "", F_CAPTION, C_TEXT);
        lv_obj_set_pos(s_rds_grp[i], MARGIN, y);
        lv_obj_set_width(s_rds_grp[i], CONTENT_W);
        lv_label_set_long_mode(s_rds_grp[i], LV_LABEL_LONG_DOT);
        lv_obj_add_flag(s_rds_grp[i], LV_OBJ_FLAG_HIDDEN);
        y += 18;
    }
}

static void refresh_rds(void)
{
    /* Not built yet - see ensure_built. */
    if (!s_built[RP_SCREEN_RDS]) return;
    const en_rds_t *r = &rp_model.rds;
    char buf[160], tmp[40];
    uint8_t i;

    /* Identification. */
    if (r->pi_valid) {
        put_hex4(buf, r->pi);
        lv_label_set_text(s_rds_pi, buf);

        if (en_rds_callsign(r->pi, buf, sizeof buf))
            lv_label_set_text(s_rds_call, buf);
        else
            lv_label_set_text(s_rds_call, "not encoded");

        put_hex4(buf, (uint16_t)en_rds_pi_country(r->pi));
        /* Only the nibble matters, so print it as one digit rather than as a
           four-digit word with three zeros in front of it. */
        buf[0] = '0'; buf[1] = 'x';
        buf[2] = "0123456789ABCDEF"[en_rds_pi_country(r->pi)];
        buf[3] = 0;
        lv_label_set_text(s_rds_cc, buf);

        lv_label_set_text(s_rds_cov,
                          en_rds_coverage_name(en_rds_pi_coverage(r->pi)));

        put_uint(buf, en_rds_pi_reference(r->pi), 0);
        lv_label_set_text(s_rds_ref, buf);
    } else {
        lv_label_set_text(s_rds_pi, "-");
        lv_label_set_text(s_rds_call, "-");
        lv_label_set_text(s_rds_cc, "-");
        lv_label_set_text(s_rds_cov, "-");
        lv_label_set_text(s_rds_ref, "-");
    }

    if (r->ecc_valid) {
        buf[0] = '0'; buf[1] = 'x';
        buf[2] = "0123456789ABCDEF"[(r->ecc >> 4) & 0xF];
        buf[3] = "0123456789ABCDEF"[r->ecc & 0xF];
        buf[4] = 0;
        lv_label_set_text(s_rds_ecc, buf);
    } else {
        lv_label_set_text(s_rds_ecc, "-");
    }

    if (r->pin_valid) {
        put_hex4(buf, r->pin);
        lv_label_set_text(s_rds_pin, buf);
    } else {
        lv_label_set_text(s_rds_pin, "-");
    }

    /* Programme. */
    buf[0] = 0;
    put_uint(tmp, r->pty, 0);
    cat(buf, tmp, sizeof buf);
    cat(buf, "  ", sizeof buf);
    cat(buf, en_rds_pty_name(r->pty, r->rbds), sizeof buf);
    lv_label_set_text(s_rds_pty, buf);

    lv_label_set_text(s_rds_ptyn, r->ptyn_valid ? r->ptyn : "-");

    buf[0] = 0;
    cat(buf, r->tp ? "TP " : "", sizeof buf);
    cat(buf, r->ta ? "TA " : "", sizeof buf);
    cat(buf, r->ms ? "Music" : "Speech", sizeof buf);
    lv_label_set_text(s_rds_flags, buf);

    en_rds_di_text(r->di, buf, sizeof buf);
    lv_label_set_text(s_rds_di, buf);

    /* Text. */
    lv_label_set_text(s_rds_ps, r->ps_valid ? r->ps : "-");
    lv_label_set_text(s_rds_rt, r->rt_valid ? r->rt : "-");

    if (r->rt_title_valid || r->rt_artist_valid) {
        buf[0] = 0;
        cat(buf, r->rt_title_valid ? r->rt_title : "?", sizeof buf);
        cat(buf, "\n", sizeof buf);
        cat(buf, r->rt_artist_valid ? r->rt_artist : "?", sizeof buf);
        lv_label_set_text(s_rds_rtp, buf);
    } else if (r->rtplus_known) {
        /* Announced and silent is a distinct and interesting state - it is the
           commonest RadioText+ fault there is. */
        buf[0] = 0;
        cat(buf, "announced in group ", sizeof buf);
        en_rds_group_label((uint8_t)(r->rtplus_group >> 1),
                           (r->rtplus_group & 1u) != 0, tmp, sizeof tmp);
        cat(buf, tmp, sizeof buf);
        cat(buf, ", nothing received", sizeof buf);
        lv_label_set_text(s_rds_rtp, buf);
    } else {
        lv_label_set_text(s_rds_rtp, "-");
    }

    /* Clock. */
    if (r->ct_valid) {
        buf[0] = 0;
        put_uint(tmp, r->ct_year, 4);      cat(buf, tmp, sizeof buf);
        cat(buf, "-", sizeof buf);
        put_uint(tmp, r->ct_month, 2);     cat(buf, tmp, sizeof buf);
        cat(buf, "-", sizeof buf);
        put_uint(tmp, r->ct_day, 2);       cat(buf, tmp, sizeof buf);
        cat(buf, " ", sizeof buf);
        put_uint(tmp, r->ct_hour, 2);      cat(buf, tmp, sizeof buf);
        cat(buf, ":", sizeof buf);
        put_uint(tmp, r->ct_minute, 2);    cat(buf, tmp, sizeof buf);
        lv_label_set_text(s_rds_ct, buf);

        buf[0] = 0;
        put_uint(tmp, r->ct_utc_hour, 2);  cat(buf, tmp, sizeof buf);
        cat(buf, ":", sizeof buf);
        put_uint(tmp, r->ct_utc_minute, 2); cat(buf, tmp, sizeof buf);
        cat(buf, r->ct_offset < 0 ? "  -" : "  +", sizeof buf);
        {
            int8_t half = r->ct_offset < 0 ? (int8_t)-r->ct_offset : r->ct_offset;
            put_uint(tmp, (uint32_t)(half / 2), 0);  cat(buf, tmp, sizeof buf);
            cat(buf, (half & 1) ? ":30" : ":00", sizeof buf);
        }
        cat(buf, "  x", sizeof buf);
        put_uint(tmp, r->ct_groups, 0);    cat(buf, tmp, sizeof buf);
        lv_label_set_text(s_rds_ctutc, buf);
    } else {
        lv_label_set_text(s_rds_ct, "-");
        lv_label_set_text(s_rds_ctutc, "-");
    }

    /* Alternate frequencies. */
    if (r->af_count) {
        buf[0] = 0;
        for (i = 0; i < r->af_count && i < EN_RDS_AF_MAX; i++) {
            fmt_mhz(tmp, (int)sizeof tmp, r->af[i]);
            cat(buf, tmp, sizeof buf);
            cat(buf, i + 1 < r->af_count ? "  " : "", sizeof buf);
        }
        lv_label_set_text(s_rds_af, buf);
    } else if (r->af_expected) {
        lv_label_set_text(s_rds_af, "a list was announced, none decoded yet");
    } else {
        lv_label_set_text(s_rds_af, "-");
    }

    /* Open data applications. */
    for (i = 0; i < EN_RDS_ODA_MAX; i++) {
        if (i >= r->oda_count) {
            show(s_rds_oda[i], false);
            continue;
        }
        show(s_rds_oda[i], true);

        buf[0] = 0;
        put_hex4(tmp, r->oda[i].aid);
        cat(buf, tmp, sizeof buf);
        cat(buf, "  ", sizeof buf);
        en_rds_group_label((uint8_t)(r->oda[i].group >> 1),
                           (r->oda[i].group & 1u) != 0, tmp, sizeof tmp);
        cat(buf, tmp, sizeof buf);
        cat(buf, "  ", sizeof buf);
        {
            const char *name = en_rds_oda_name(r->oda[i].aid);
            cat(buf, name ? name : "unregistered here", sizeof buf);
        }
        lv_label_set_text(s_rds_oda[i], buf);
    }
    if (!r->oda_count && s_rds_oda[0]) {
        show(s_rds_oda[0], true);
        lv_label_set_text(s_rds_oda[0], "none announced");
    }

    /* Totals, then the histogram. */
    buf[0] = 0;
    put_uint(tmp, r->groups, 0);
    cat(buf, tmp, sizeof buf);
    cat(buf, " groups, ", sizeof buf);
    put_uint(tmp, r->blocks_bad, 0);
    cat(buf, tmp, sizeof buf);
    cat(buf, " bad blocks", sizeof buf);
    if (r->groups) {
        /* Per mille rather than per cent: on a good signal the rate is well
           under one per cent and a rounded percentage reads as zero, which
           looks like the counter is broken. */
        uint32_t permille = (r->blocks_bad * 1000u) / (r->groups * 4u);
        cat(buf, "  (", sizeof buf);
        put_uint(tmp, permille / 10u, 0);
        cat(buf, tmp, sizeof buf);
        cat(buf, ".", sizeof buf);
        put_uint(tmp, permille % 10u, 0);
        cat(buf, tmp, sizeof buf);
        cat(buf, "%)", sizeof buf);
    }
    lv_label_set_text(s_rds_totals, buf);

    for (i = 0; i < 32; i++) {
        uint32_t n = r->group_count[i];

        if (!n) { show(s_rds_grp[i], false); continue; }
        show(s_rds_grp[i], true);

        buf[0] = 0;
        en_rds_group_label((uint8_t)(i >> 1), (i & 1u) != 0, tmp, sizeof tmp);
        cat(buf, tmp, sizeof buf);
        cat(buf, "  ", sizeof buf);
        put_uint(tmp, n, 0);
        cat(buf, tmp, sizeof buf);
        if (r->groups) {
            cat(buf, "  ", sizeof buf);
            put_uint(tmp, (n * 100u) / r->groups, 0);
            cat(buf, tmp, sizeof buf);
            cat(buf, "%  ", sizeof buf);
        } else {
            cat(buf, "  ", sizeof buf);
        }
        cat(buf, en_rds_group_name((uint8_t)(i >> 1), (i & 1u) != 0),
            sizeof buf);
        lv_label_set_text(s_rds_grp[i], buf);
    }
}

/* ---- setting the clock off the band ---------------------------------------
 *
 * This device has no network, no cell radio, and a hardware clock that has
 * never been set - so it boots into 1970 and every recording is filed under a
 * date that is off by fifty-odd years. There is no NTP here to fix it with.
 *
 * There is, however, a band full of broadcasters transmitting the date and the
 * time as part of their normal output, fed in most cases from the same
 * reference that times the transmitter. So: listen to them, show what each one
 * says, and let the listener pick which to believe.
 *
 * The part that makes this correct rather than approximate is the age. The
 * specification puts the minute edge at the start of the group carrying the
 * time, so a station's answer is exact when it lands and stale from then on -
 * a reading taken forty seconds ago sets the clock forty seconds slow. ctscan
 * timestamps every result and en_ctscan_now adds the elapsed time back, which
 * is why the list keeps updating while it is being read.
 */

static en_ctscan_t s_ct;
static lv_obj_t *s_ct_status, *s_ct_bar, *s_ct_fill, *s_ct_note;
static lv_obj_t *s_ct_rows[EN_CTSCAN_MAX];
static lv_obj_t *s_ct_btn_lbl;
static uint32_t s_ct_last_ms;
static char s_ct_result[64];
static uint8_t s_ct_result_for;

static bool ct_running(void)
{
    return s_ct.phase == EN_CTSCAN_RUNNING;
}

/* Seconds since the epoch, split into a printable local date and time using
   the offset the station transmitted. No timezone database, no environment,
   and nothing inferred from the device's own clock - which is the point, since
   the device's clock is the thing being corrected. */
static void fmt_stamp(char *out, int cap, int64_t utc, int8_t half_hours)
{
    int64_t local = utc + (int64_t)half_hours * 1800;
    int64_t days = local / 86400;
    int32_t rem = (int32_t)(local % 86400);
    uint32_t y = 1970, m;
    static const uint8_t MD[13] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
    char t[12];

    if (rem < 0) { rem += 86400; days--; }

    for (;;) {
        uint32_t len = (y % 4u == 0u && (y % 100u != 0u || y % 400u == 0u))
                       ? 366u : 365u;
        if (days < (int64_t)len) break;
        days -= (int64_t)len;
        y++;
    }
    for (m = 1; m <= 12; m++) {
        uint32_t len = MD[m];
        if (m == 2 && (y % 4u == 0u && (y % 100u != 0u || y % 400u == 0u)))
            len = 29;
        if (days < (int64_t)len) break;
        days -= (int64_t)len;
    }

    out[0] = 0;
    put_uint(t, y, 4);                     cat(out, t, cap);
    cat(out, "-", cap);
    put_uint(t, m, 2);                     cat(out, t, cap);
    cat(out, "-", cap);
    put_uint(t, (uint32_t)days + 1u, 2);   cat(out, t, cap);
    cat(out, "  ", cap);
    put_uint(t, (uint32_t)(rem / 3600), 2); cat(out, t, cap);
    cat(out, ":", cap);
    put_uint(t, (uint32_t)((rem / 60) % 60), 2); cat(out, t, cap);
    cat(out, ":", cap);
    put_uint(t, (uint32_t)(rem % 60), 2);  cat(out, t, cap);
}

/*
 * Which stations to try, strongest first.
 *
 * A band scan run this session is by far the best source: it has measured
 * signal strengths, and strength is what decides whether a station answers
 * inside the dwell or wastes seventy seconds of it. Failing that the presets,
 * which at least are stations somebody found worth keeping. Failing that,
 * whatever is tuned - one candidate is a perfectly good clock scan if it
 * happens to answer.
 */
static uint8_t ct_candidates(uint32_t *out, uint8_t max)
{
    uint8_t n = 0, i, j;

    if (s_scan.n_hits) {
        /* Selection sort by peak strength. Forty entries at most, once per
           scan, so the simplest thing that is obviously right. */
        uint8_t order[EN_SCAN_MAX_HITS];
        uint8_t cnt = s_scan.n_hits;
        if (cnt > EN_SCAN_MAX_HITS) cnt = EN_SCAN_MAX_HITS;
        for (i = 0; i < cnt; i++) order[i] = i;
        for (i = 0; i < cnt; i++) {
            for (j = (uint8_t)(i + 1); j < cnt; j++) {
                if (s_scan.hits[order[j]].rssi > s_scan.hits[order[i]].rssi) {
                    uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
                }
            }
        }
        for (i = 0; i < cnt && n < max; i++)
            out[n++] = s_scan.hits[order[i]].khz;
        return n;
    }

    for (i = 0; i < rp_model.presets.count && n < max; i++)
        out[n++] = rp_model.presets.list[i].khz;

    if (!n && rp_model.khz) out[n++] = rp_model.khz;
    return n;
}

static void ct_pump(void)
{
    uint32_t now = lv_tick_get();
    uint32_t dt = now - s_ct_last_ms;
    uint32_t khz = 0;

    if (s_ct.phase == EN_CTSCAN_IDLE) { s_ct_last_ms = now; return; }

    s_ct_last_ms = now;
    if (dt > 1000) dt = 0;

    if (en_ctscan_tick(&s_ct, dt, &rp_model.rds, &khz) == EN_CTSCAN_TUNE) {
        rp_act_tune_quiet(khz);
        /* The decoder belongs to whatever is tuned. Carrying one station's
           name or clock into the next one's row would be worse than a blank
           row, because it would look like an answer. */
        en_rds_init(&rp_model.rds,
                    rp_model.region ? rp_model.region->rbds : true);
    }

    if (s_ct_result_for) s_ct_result_for--;
}

static void on_clock_back(lv_event_t *e)
{
    (void)e;
    rp_ui_show(RP_SCREEN_SETTINGS);
}

static void on_clock_scan(lv_event_t *e)
{
    (void)e;

    if (ct_running()) {
        en_ctscan_stop(&s_ct);
        rp_act_tune(s_ct.resume_khz);
        return;
    }

    {
        uint32_t cand[EN_CTSCAN_MAX];
        uint8_t n = ct_candidates(cand, EN_CTSCAN_MAX);

        if (!n) return;
        s_ct_last_ms = lv_tick_get();
        s_ct_result[0] = 0;
        if (en_ctscan_start(&s_ct, cand, n, rp_model.khz)) {
            rp_act_tune_quiet(cand[0]);
            en_rds_init(&rp_model.rds,
                        rp_model.region ? rp_model.region->rbds : true);
        }
    }
}

/* Tapping a station sets the clock to what that station said, plus however
   long ago it said it. */
static void on_clock_pick(lv_event_t *e)
{
    uint8_t i = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    int64_t when;

    if (i >= s_ct.n_hits || !s_ct.hits[i].got) return;

    when = en_ctscan_now(&s_ct, i);
    if (!when) return;

    s_ct_result[0] = 0;
    cat(s_ct_result, rp_act_set_clock(when), (int)sizeof s_ct_result);
    s_ct_result_for = 120;      /* refreshes, not seconds; long enough to read */
}

static void build_clock(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_CLOCK];
    uint8_t i;
    int y;

    lv_obj_t *back = panel(s, 0, 0, RP_SCREEN_W, 40, C_BG);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, on_clock_back, LV_EVENT_CLICKED, 0);
    lv_obj_t *b = label(back, LV_SYMBOL_LEFT "  CLOCK FROM FM", F_CAPTION,
                        C_TEXT_MUTE);
    lv_obj_set_pos(b, MARGIN, 12);
    hairline(s, 40);

    s_ct_status = para(s, "", F_CAPTION, C_TEXT, CONTENT_W);
    lv_obj_set_pos(s_ct_status, MARGIN, 50);

    s_ct_bar = panel(s, MARGIN, 88, CONTENT_W, 3, C_SURFACE_2);
    s_ct_fill = panel(s_ct_bar, 0, 0, 0, 3, C_SIGNAL);

    lv_obj_t *btn = panel(s, MARGIN, 100, CONTENT_W, 38, C_SURFACE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_clock_scan, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_SURFACE_2), LV_STATE_PRESSED);
    s_ct_btn_lbl = label(btn, "Listen for the time", F_BODY, C_SIGNAL);
    lv_obj_center(s_ct_btn_lbl);

    s_ct_note = para(s, "", F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(s_ct_note, MARGIN, 144);

    y = 186;
    for (i = 0; i < EN_CTSCAN_MAX; i++) {
        s_ct_rows[i] = row(s, y, 40, on_clock_pick, (void *)(uintptr_t)i);
        lv_obj_t *l = label(s_ct_rows[i], "", F_CAPTION, C_TEXT);
        lv_obj_set_pos(l, MARGIN, 4);
        lv_obj_set_width(l, CONTENT_W);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);

        lv_obj_t *v = label(s_ct_rows[i], "", F_CAPTION, C_TEXT_MUTE);
        lv_obj_set_pos(v, MARGIN, 21);
        lv_obj_set_width(v, CONTENT_W);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);

        lv_obj_add_flag(s_ct_rows[i], LV_OBJ_FLAG_HIDDEN);
        y += 40;
    }
}

static void refresh_clock(void)
{
    /* Not built yet - see ensure_built. */
    if (!s_built[RP_SCREEN_CLOCK]) return;
    char buf[96], tmp[40];
    uint8_t i;

    lv_label_set_text(s_ct_btn_lbl,
                      ct_running() ? "Stop" : "Listen for the time");

    /* The device's own clock, which is the thing being judged. */
    buf[0] = 0;
    cat(buf, "Now: ", sizeof buf);
    fmt_stamp(tmp, (int)sizeof tmp, rp_model_now(), 0);
    cat(buf, tmp, sizeof buf);
    cat(buf, " UTC", sizeof buf);
    lv_label_set_text(s_ct_status, buf);

    lv_obj_set_width(s_ct_fill,
                     (CONTENT_W * en_ctscan_percent(&s_ct)) / 100);

    if (s_ct_result[0] && s_ct_result_for) {
        lv_label_set_text(s_ct_note, s_ct_result);
        lv_obj_set_style_text_color(s_ct_note, lv_color_hex(C_LIVE), 0);
    } else if (ct_running()) {
        buf[0] = 0;
        cat(buf, "Listening to ", sizeof buf);
        fmt_mhz(tmp, (int)sizeof tmp, s_ct.cand[s_ct.at]);
        cat(buf, tmp, sizeof buf);
        cat(buf, " - station ", sizeof buf);
        put_uint(tmp, (uint32_t)s_ct.at + 1u, 0);
        cat(buf, tmp, sizeof buf);
        cat(buf, " of ", sizeof buf);
        put_uint(tmp, s_ct.n_cand, 0);
        cat(buf, tmp, sizeof buf);
        cat(buf, ". The time comes about once a minute.", sizeof buf);
        lv_label_set_text(s_ct_note, buf);
        lv_obj_set_style_text_color(s_ct_note, lv_color_hex(C_TEXT_MUTE), 0);
    } else if (s_ct.phase == EN_CTSCAN_DONE) {
        lv_label_set_text(s_ct_note,
            "Tap a station to set the clock to its time.");
        lv_obj_set_style_text_color(s_ct_note, lv_color_hex(C_TEXT_MUTE), 0);
    } else {
        lv_label_set_text(s_ct_note,
            s_scan.n_hits
                ? "Stations from the last band scan, strongest first."
                : "Run a band scan first for the best results; otherwise this "
                  "uses your presets.");
        lv_obj_set_style_text_color(s_ct_note, lv_color_hex(C_TEXT_MUTE), 0);
    }

    for (i = 0; i < EN_CTSCAN_MAX; i++) {
        lv_obj_t *l, *v;

        if (i >= s_ct.n_hits) { show(s_ct_rows[i], false); continue; }
        show(s_ct_rows[i], true);

        l = lv_obj_get_child(s_ct_rows[i], 0);
        v = lv_obj_get_child(s_ct_rows[i], 1);

        buf[0] = 0;
        fmt_mhz(tmp, (int)sizeof tmp, s_ct.hits[i].khz);
        cat(buf, tmp, sizeof buf);
        cat(buf, "  ", sizeof buf);
        cat(buf, s_ct.hits[i].name[0] ? s_ct.hits[i].name : "", sizeof buf);
        lv_label_set_text(l, buf);

        if (s_ct.hits[i].got) {
            fmt_stamp(buf, (int)sizeof buf, en_ctscan_now(&s_ct, i),
                      s_ct.hits[i].offset);
            lv_label_set_text(v, buf);
            lv_obj_set_style_text_color(v, lv_color_hex(C_LIVE), 0);
        } else if (ct_running() && i == s_ct.at) {
            buf[0] = 0;
            cat(buf, "listening, ", sizeof buf);
            put_uint(tmp, s_ct.waited_ms / 1000u, 0);
            cat(buf, tmp, sizeof buf);
            cat(buf, "s", sizeof buf);
            lv_label_set_text(v, buf);
            lv_obj_set_style_text_color(v, lv_color_hex(C_SIGNAL), 0);
        } else if (ct_running() && i > s_ct.at) {
            lv_label_set_text(v, "waiting");
            lv_obj_set_style_text_color(v, lv_color_hex(C_TEXT_MUTE), 0);
        } else {
            lv_label_set_text(v, "no clock transmitted");
            lv_obj_set_style_text_color(v, lv_color_hex(C_TEXT_MUTE), 0);
        }
    }
}

void rp_ui_build_all(void)
{
    /*
     * The shells only - eleven empty containers, which is a few milliseconds.
     * What each one contains is built by ensure_built when it is first shown.
     */
    for (int i = 0; i < RP_SCREEN_COUNT; i++) build_screen_shell((rp_screen_t)i);

    rebuild_order();

    /* And the one that is about to be looked at. */
    ensure_built(RP_SCREEN_NOW);
}

/* ---- the boot screen ------------------------------------------------------ */

/*
 * Deliberately built by hand rather than through build_screen_shell and the
 * rest. It has to exist before anything else does - that is its entire job -
 * and anything it shared with the real screens would be one more thing that
 * has to work before the first pixel appears.
 */
static lv_obj_t *s_boot, *s_boot_step, *s_boot_fail, *s_boot_dots;
static uint8_t   s_boot_n;

void rp_ui_boot(const char *step)
{
    if (!s_boot) {
        s_boot = lv_obj_create(NULL);
        flat(s_boot, C_BG);
        lv_obj_remove_flag(s_boot, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = label(s_boot, "Radio+", F_STATION, C_TEXT);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(name, CONTENT_W);
        lv_obj_set_pos(name, MARGIN, 150);

        /* A rule under the name, so the screen reads as a screen rather than
           as two labels that happen to be there. */
        panel(s_boot, MARGIN + 60, 190, CONTENT_W - 120, 1, C_HAIRLINE);

        s_boot_step = label(s_boot, "", F_CAPTION, C_TEXT_DIM);
        lv_obj_set_style_text_align(s_boot_step, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_size(s_boot_step, CONTENT_W, 20);
        lv_label_set_long_mode(s_boot_step, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_boot_step, MARGIN, 206);

        /* Whatever went wrong first, kept under the running step. */
        s_boot_fail = label(s_boot, "", F_CAPTION, C_TA);
        lv_obj_set_style_text_align(s_boot_fail, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_size(s_boot_fail, CONTENT_W, 36);
        lv_label_set_long_mode(s_boot_fail, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(s_boot_fail, MARGIN, 232);

        /* Progress without a percentage. There is no way to know how long the
           tuner will take, and a bar that lies about it is worse than a row of
           dots that only claims something is still happening. */
        s_boot_dots = label(s_boot, "", F_CAPTION, C_SIGNAL);
        lv_obj_set_style_text_align(s_boot_dots, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_boot_dots, CONTENT_W);
        lv_obj_set_pos(s_boot_dots, MARGIN, 280);

        lv_screen_load(s_boot);
    }

    if (step) lv_label_set_text(s_boot_step, step);

    /* Wraps rather than saturating. It used to stop at twelve, which was
       fine when the boot screen lasted a second or two and reads as a hung
       machine now that it can legitimately wait twenty. */
    s_boot_n = (uint8_t)(s_boot_n % 12u + 1u);

    char dots[16];
    uint8_t i = 0;
    for (; i < s_boot_n && i < 12; i++) dots[i] = '.';
    dots[i] = 0;
    lv_label_set_text(s_boot_dots, dots);

    /* Synchronous. The caller is about to block for seconds inside a driver,
       and a frame queued behind that is a frame nobody sees. */
    lv_refr_now(NULL);
}

/*
 * Put the boot screen back in front, once.
 *
 * rp_ui_init() ends by loading Now Playing, which is right when startup is
 * over and wrong while it is still going on. The startup loop builds the
 * interface first - so the buttons work and the screens are ready the instant
 * the hardware arrives - and then calls this to cover it while it waits.
 *
 * Deliberately not called from rp_ui_boot(): once the listener has pressed
 * something and gone somewhere, the boot screen must not come clawing back on
 * the next progress update.
 */
void rp_ui_boot_show(void)
{
    if (s_boot) lv_screen_load(s_boot);
}

/* Whether the boot screen is still the one being looked at. False as soon as
   the listener navigates away, which is what lets a press end the wait. */
bool rp_ui_booting(void)
{
    return s_boot && lv_screen_active() == s_boot;
}

void rp_ui_boot_failed(const char *what)
{
    if (!s_boot_fail || !what) return;
    /* Only the first one. Later failures are usually consequences of it, and
       the screen has room for the cause rather than the cascade. */
    if (lv_label_get_text(s_boot_fail)[0]) return;
    lv_label_set_text(s_boot_fail, what);
    lv_refr_now(NULL);
}

/* ---- the one measurement this layout depends on --------------------------- */

/*
 * How much room is left over when every chip on the Now Playing row is showing
 * the longest thing it can show.
 *
 * The row is a flex container, so the chips cannot overlap each other any
 * more - but they can still run off the right-hand edge together, and that is
 * a function of six strings scattered across three files. Twice now this row
 * has broken because somebody lengthened one of them, and both times it was
 * found by looking at a picture of a case that happened to be rendered.
 *
 * So it is measured instead of estimated, and measured by building the actual
 * widget with the actual font rather than by counting characters: whatever
 * LVGL is going to draw is what gets weighed. The strings come from the same
 * functions the screen calls, so adding a stereo mode or an AF state is
 * automatically included.
 *
 * Positive is spare pixels. Negative means the row overflows and somebody has
 * to shorten something. The preview calls this and fails the run.
 */
static int measure_pill(const char *text)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *p = pill(scr, 0, 0, text, C_TEXT);
    int w;

    lv_obj_update_layout(scr);
    w = lv_obj_get_width(p);
    lv_obj_delete(scr);
    return w;
}

int rp_ui_pill_row_slack(void)
{
    /* Zeroed by initialiser rather than memset: this file has no <string.h>
       and does not need one for this. */
    en_affollow_t a = { 0 };
    int total = 2 * PILL_GAP;
    int best;
    uint8_t i;

    /* Stereo, whichever of its three labels is longest. */
    best = 0;
    for (i = 0; i < 3; i++) {
        int w = measure_pill(stereo_mode_text(i));
        if (w > best) best = w;
    }
    total += best;

    /* The traffic flag, which has only ever said one thing. */
    total += measure_pill("TA");

    /* AF, across every state it reports - including disabled, which is the
       short one, so this is a maximum rather than a sample. */
    best = measure_pill(en_af_state_text(&a));
    a.enabled = true;
    for (i = EN_AF_IDLE; i <= EN_AF_COOLDOWN; i++) {
        int w;
        a.phase = (en_af_phase_t)i;
        w = measure_pill(en_af_state_text(&a));
        if (w > best) best = w;
    }
    total += best;

    return CONTENT_W - total;
}

void rp_ui_init(void)
{
    /* Re-theme first. The default theme's blue would look like a different
       application next to this palette, and re-skinning every widget by hand
       afterwards is worse than setting it once. */
    lv_display_t *d = lv_display_get_default();
    if (d)
        lv_theme_default_init(d, lv_color_hex(C_SIGNAL),
                              lv_color_hex(C_RDS), true, F_BODY);

    rp_ui_build_all();
    rp_ui_show(RP_SCREEN_NOW);
}

/*
 * Build one screen, once.
 *
 * The dots go on with it rather than in a pass of their own, so a screen that
 * appears late still shows where it sits in the swipe order.
 */
static void ensure_built(rp_screen_t w)
{
    if (w >= RP_SCREEN_COUNT || s_built[w] || !s_screen[w])
        return;

    /* Set first: the build and the refresh below both run code that asks
       whether this screen exists, and it does by the time they look. */
    s_built[w] = true;

    switch (w) {
    case RP_SCREEN_SIMPLE:   build_simple();   refresh_simple();   break;
    case RP_SCREEN_NOW:      build_now();      refresh_now();      break;
    case RP_SCREEN_WIDE:     build_wide();     refresh_wide();     break;
    case RP_SCREEN_DIAL:     build_dial();     refresh_dial();     break;
    case RP_SCREEN_PRESETS:  build_presets();  refresh_presets();  break;
    case RP_SCREEN_LIBRARY:  build_library();  refresh_library();  break;
    case RP_SCREEN_SETTINGS: build_settings(); refresh_settings(); break;
    case RP_SCREEN_ADVANCED: build_advanced(); refresh_advanced(); break;
    case RP_SCREEN_REGISTER: build_register(); break;
    case RP_SCREEN_RDS:      build_rds();      break;
    case RP_SCREEN_CLOCK:    build_clock();    break;
    default: break;
    }

    if (w < RP_SWIPE_MAX)
        build_dots((int)w, s_screen[w]);
}

void rp_ui_show(rp_screen_t which)
{
    if (which >= RP_SCREEN_COUNT || !s_screen[which]) return;
    ensure_built(which);
    s_current = which;

    /* Restyle this screen's own row of dots. */
    if (which < RP_SWIPE_MAX) refresh_dots();

    lv_screen_load(s_screen[which]);
}

rp_screen_t rp_ui_current(void) { return s_current; }

void rp_ui_scan_start_for_preview(void) { on_scan(NULL); }

void rp_ui_tick(void)
{
    /* Before the screens, and not inside their cases: a scan and a timed
       recording both have to keep running when the user swipes away from the
       screen that set them going. */
    scan_pump();
    ct_pump();
    rectimer_pump();
    af_pump();

    switch (s_current) {
    case RP_SCREEN_SIMPLE:   refresh_simple();   break;
    case RP_SCREEN_NOW:      refresh_now();      break;
    case RP_SCREEN_WIDE:     refresh_wide();     break;
    case RP_SCREEN_DIAL:     refresh_dial();     break;
    case RP_SCREEN_PRESETS:  refresh_presets();  break;
    case RP_SCREEN_LIBRARY:  refresh_library();  break;
    case RP_SCREEN_SETTINGS: refresh_settings(); break;
    case RP_SCREEN_ADVANCED: break;   /* static once built */
    case RP_SCREEN_RDS:      refresh_rds();     break;
    case RP_SCREEN_CLOCK:    refresh_clock();   break;
    default: break;
    }
}
