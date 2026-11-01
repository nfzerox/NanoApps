/*
 * preview.c — render every screen headlessly and write it out as a BMP.
 *
 * No display, no SDL, no window server. LVGL draws into a plain memory buffer
 * and the buffer is written to disk, which means the whole interface can be
 * looked at from a build machine and, more importantly, that every screen can
 * be looked at *at once* rather than one at a time on hardware.
 *
 * That is the point of it. Laying out 240 x 432 by imagining it does not work.
 * A label that overflows its box, a row that collides with the one below it, a
 * hairline one pixel off - all of them are obvious in a picture and invisible
 * in source.
 *
 * BMP because it is twenty lines and no libraries; host/tobmp.py turns the
 * output into PNG with Pillow, which is already a dependency of ./start.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "../ui.h"
#include "../model.h"
#include "../core/fmreg.h"

static uint32_t s_fb[RP_SCREEN_W * RP_SCREEN_H];

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
    (void)area; (void)px;
    lv_display_flush_ready(d);
}

/* A 32-bit BMP, bottom-up, which is what an uncompressed BMP wants. */
static bool write_bmp(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    const uint32_t data = (uint32_t)(w * h * 4);
    const uint32_t off = 14 + 40;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof hdr);

    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t total = off + data;
    memcpy(hdr + 2, &total, 4);
    memcpy(hdr + 10, &off, 4);

    uint32_t ihdr = 40;
    memcpy(hdr + 14, &ihdr, 4);
    int32_t ww = w, hh = h;
    memcpy(hdr + 18, &ww, 4);
    memcpy(hdr + 22, &hh, 4);
    uint16_t planes = 1, bpp = 32;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data, 4);

    bool ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;

    /* Bottom-up: BMP stores the last row first. */
    for (int y = h - 1; y >= 0 && ok; y--)
        ok = fwrite(px + (size_t)y * w, 4, (size_t)w, f) == (size_t)w;

    fclose(f);
    return ok;
}

/*
 * A clock the preview controls.
 *
 * LVGL needs one, and so does the band scan, which measures its dwell in
 * milliseconds. Taking it from the wall clock would make the scan's progress
 * depend on how fast this machine renders - the screenshot would show a
 * different percentage every run, which is the one thing a screenshot used as
 * a regression test must not do. Advanced explicitly instead.
 */
static uint32_t s_ms;

static uint32_t preview_millis(void) { return s_ms; }

/* Let LVGL settle. One pass is not enough - layout, then draw, and anything
   that resizes to its content needs the round trip. */
static void settle(void)
{
    for (int i = 0; i < 8; i++) {
        lv_refr_now(NULL);
        lv_timer_handler();
    }
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "shots";

    lv_init();
    lv_tick_set_cb(preview_millis);

    lv_display_t *d = lv_display_create(RP_SCREEN_W, RP_SCREEN_H);
    lv_display_set_buffers(d, s_fb, NULL, sizeof s_fb,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush_cb);

    /* The boot screen, captured before anything else exists - which is the
       whole point of it. Done here rather than in the walk below, because by
       then the real screens have replaced it. Driven through a few steps and
       one failure, since that is the state worth looking at. */
    rp_ui_boot("tuner init");
    rp_ui_boot("audio capture");
    rp_ui_boot_failed("no capture device hw:0,1");
    rp_ui_boot("audio out");
    {
        char path[512];
        lv_refr_now(NULL);
        snprintf(path, sizeof path, "%s/0a-boot.bmp", out);
        if (write_bmp(path, s_fb, RP_SCREEN_W, RP_SCREEN_H))
            printf("  %s\n", path);
    }

    rp_model_refresh();
    rp_ui_init();

    /* Both optional screens are off by default, which is right on a device and
       wrong here: a screen nobody renders is a screen whose layout nobody
       checks. Turn them on for the shots. */
    rp_act_show_simple(true);
    rp_act_show_wide(true);

    /* And put a couple of stations on the simple screen, so it renders as it
       will look rather than as its empty state. The empty state gets its own
       shot below. */
    if (rp_model.presets.count) {
        en_preset_set_simple(&rp_model.presets, rp_model.presets.list[0].khz, true);
        if (rp_model.presets.count > 1)
            en_preset_set_simple(&rp_model.presets,
                                 rp_model.presets.list[1].khz, true);
        if (rp_model.presets.count > 2)
            en_preset_set_simple(&rp_model.presets,
                                 rp_model.presets.list[2].khz, true);
    }

    /* A timer set, so the Recordings shot shows the controls doing something
       rather than three defaults. Set through the actions, which is how the
       screen would set them. */
    /* Forced mono, so the stereo pill is shot as the control it now is
       rather than only as the readout it used to be. */
    rp_act_stereo_mode(EN_FM_STEREO_MONO);

    rp_act_set_rec_limit(60);
    rp_act_set_rec_at((int16_t)(7 * 60 + 45));

    /*
     * The row that has broken twice, measured rather than looked at.
     *
     * A screenshot only ever shows the combination that happened to be
     * rendered, and both times this row overflowed it was a combination
     * nobody had shot. This weighs every label each chip can display, so it
     * fails on the widest case whether or not anyone thought to draw it.
     */
    {
        int slack = rp_ui_pill_row_slack();

        printf("  pill row: %d px spare at its widest\n", slack);
        if (slack < 0) {
            printf("  FAILED the Now Playing chip row overflows by %d px\n",
                   -slack);
            return 1;
        }
    }

    /*
     * And a picture of it, because the number does not show alignment.
     *
     * Forced stereo, a traffic announcement in progress, and the AF follower
     * reporting a weak signal - three chips at their longest strings at the
     * same time. That combination is rare in use and is exactly the one that
     * has broken this row twice, so it gets a picture rather than an estimate.
     */
    {
        /* Its own buffer: the shared one is declared below, with the loop. */
        char wpath[512];
        uint8_t save_mode = rp_model.stereo_mode;
        bool save_ta = rp_model.rds.ta;
        en_affollow_t save_af = rp_model.af;

        rp_act_stereo_mode(EN_FM_STEREO_STEREO);
        rp_model.rds.ta = true;
        rp_model.af.enabled = true;
        rp_model.af.phase = EN_AF_WEAK;

        rp_ui_show(RP_SCREEN_NOW);
        rp_ui_tick();
        settle();
        snprintf(wpath, sizeof wpath, "%s/1c-now-widest.bmp", out);
        if (write_bmp(wpath, s_fb, RP_SCREEN_W, RP_SCREEN_H))
            printf("  %s\n", wpath);

        rp_model.af = save_af;
        rp_model.rds.ta = save_ta;
        rp_act_stereo_mode(save_mode);
    }

    static const struct { rp_screen_t s; const char *name; } shots[] = {
        { RP_SCREEN_SIMPLE,   "0-simple" },
        { RP_SCREEN_NOW,      "1-now" },
        { RP_SCREEN_WIDE,     "1b-wide" },
        { RP_SCREEN_DIAL,     "2-dial" },
        { RP_SCREEN_PRESETS,  "3-presets" },
        { RP_SCREEN_LIBRARY,  "4-recordings" },
        { RP_SCREEN_SETTINGS, "5-settings" },
        { RP_SCREEN_ADVANCED, "6-advanced" },
        { RP_SCREEN_RDS,      "6b-rds" },
        { RP_SCREEN_CLOCK,    "6c-clock" },
    };

    char path[512];
    int made = 0;

    for (unsigned i = 0; i < sizeof shots / sizeof shots[0]; i++) {
        rp_ui_show(shots[i].s);
        rp_ui_tick();
        settle();

        snprintf(path, sizeof path, "%s/%s.bmp", out, shots[i].name);
        if (write_bmp(path, s_fb, RP_SCREEN_W, RP_SCREEN_H)) {
            printf("  %s\n", path);
            made++;
        } else {
            printf("  FAILED %s\n", path);
        }
    }

    /* Three registers with different field shapes, because the editor is
       generated from the table and the shapes are what it has to get right:
       0x05 packs a range field above a bitmap, 0xFA is an enum, and 0xF9 is
       eight sliders of mixed sign. */
    static const struct { uint8_t addr; const char *name; } regs[] = {
        { 0x05, "8-reg-audio" },
        { 0xFA, "9-reg-antenna" },
        { 0xF9, "10-reg-blend" },
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) {
        rp_ui_open_register(regs[i].addr);
        settle();
        snprintf(path, sizeof path, "%s/%s.bmp", out, regs[i].name);
        if (write_bmp(path, s_fb, RP_SCREEN_W, RP_SCREEN_H)) {
            printf("  %s\n", path);
            made++;
        }
    }

    /* One more of Now Playing mid-recording, because the recording state
       changes several things at once and is worth seeing as a whole. */
    /* At the live edge, so the transport is in its record state rather than
       its playback one - the two are the same three buttons. */
    rp_act_play_live();
    rp_act_record_toggle();
    rp_model_refresh();
    rp_ui_show(RP_SCREEN_NOW);
    rp_ui_tick();
    settle();
    snprintf(path, sizeof path, "%s/7-now-recording.bmp", out);
    if (write_bmp(path, s_fb, RP_SCREEN_W, RP_SCREEN_H)) {
        printf("  %s\n", path);
        made++;
    }

    /*
      * Now Playing with the volume overlay up.
      *
      * It only appears when the volume changes and only for a second and a
      * half, so without a shot of its own the one thing on this screen that
      * covers other content is the one thing never rendered here. The nudge
      * has to come after a refresh that has already seen a volume, because the
      * first value observed is deliberately not treated as a change.
      */
    rp_act_record_toggle();          /* back out of the recording state */
    rp_ui_show(RP_SCREEN_NOW);
    rp_ui_tick();
    rp_act_nudge_volume(-6);
    rp_model_refresh();
    rp_ui_tick();
    settle();
    snprintf(path, sizeof path, "%s/1d-now-volume.bmp", out);
    if (write_bmp(path, s_fb, RP_SCREEN_W, RP_SCREEN_H)) {
        printf("  %s\n", path);
        made++;
    }

    /* And the Presets screen mid-scan. rp_ui_tick is what pumps the scan, so
       running it repeatedly is what advances it; the preview has no tuner, so
       the RSSI the model reports is whatever it was seeded with and the scan
       treats the whole band as one long station. That is fine for a picture
       of the bar - what is being checked is that it grows across the button
       and that the label changes to Stop. */
    rp_ui_show(RP_SCREEN_PRESETS);
    rp_ui_scan_start_for_preview();
    /* Twelve seconds of scanning at the frame rate the device runs at. Far
       enough in for the bar to be a bar rather than a sliver, and the same
       twelve seconds on every machine. */
    for (int i = 0; i < 12000 / 33; i++) { s_ms += 33; rp_ui_tick(); }
    settle();
    snprintf(path, sizeof path, "%s/3b-presets-scanning.bmp", out);
    if (write_bmp(path, s_fb, RP_SCREEN_W, RP_SCREEN_H)) {
        printf("  %s\n", path);
        made++;
    }

    /* Let the scan run to the end, then re-shoot the dial. The band
       profile only exists once a sweep has been over the band, and a
       dial with an empty strip is a picture of the feature not
       working rather than of the feature. */
    /* On the dial while it runs, not the preset list: every tick of the
       list rebuilds its rows, and forty seconds of that is most of the
       preview's runtime for a screen nobody is looking at. */
    rp_ui_show(RP_SCREEN_DIAL);
    for (int i = 0; i < 45000 / 33; i++) {
        s_ms += 33;
        rp_model_refresh();
        rp_ui_tick();
    }
    rp_ui_tick();
    settle();
    snprintf(path, sizeof path, "%s/2b-dial-scanned.bmp", out);
    if (write_bmp(path, s_fb, RP_SCREEN_W, RP_SCREEN_H)) {
        printf("  %s\n", path);
        made++;
    }

    printf("%d screens\n", made);
    return made ? 0 : 1;
}
