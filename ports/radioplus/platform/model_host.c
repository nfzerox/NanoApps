/*
 * model_host.c — a plausible station, for looking at the interface.
 *
 * The screens read rp_model and nothing else, so filling it in here renders the
 * whole application on a desktop with no tuner, no iPod and no Bluetooth. That
 * is not a toy: laying out a 240 x 432 screen by imagining it does not work, and
 * the fastest way to find a label that overflows or a row that collides is to
 * render every screen and look at all of them at once.
 *
 * The data is chosen to be awkward on purpose. The station name is the full
 * eight characters, the radio text is longer than the box, the programme type
 * is one whose RBDS and RDS names differ, and there are enough presets to
 * scroll. A layout that survives this survives a real station.
 */

#include "../model.h"
#include "clock.h"

#include <stdio.h>
#include <string.h>

rp_model_t rp_model;

static bool s_recording;
static uint32_t s_rec_ms;

/* Build the RDS state the way it would actually arrive: through the decoder,
   group by group, rather than by writing the fields directly. That way the
   preview exercises the same code the device does, and a decoder bug shows up
   here rather than only on hardware. */
static void seed_rds(void)
{
    en_rds_init(&rp_model.rds, true);

    const char *ps = "CBC RAD";
    for (uint8_t seg = 0; seg < 4; seg++) {
        uint16_t b = (uint16_t)((0u << 12) | (1u << 10) | (22u << 5) | seg);
        char c0 = seg * 2 < 7 ? ps[seg * 2] : ' ';
        char c1 = seg * 2 + 1 < 7 ? ps[seg * 2 + 1] : ' ';
        uint16_t blk[4] = { 0xC2B5, b,
                            (uint16_t)((226u << 8) | 108u),
                            (uint16_t)(((uint8_t)c0 << 8) | (uint8_t)c1) };
        en_rds_group(&rp_model.rds, blk, EN_RDS_ALL);
    }

    const char *rt =
        "Q with Tom Power - Neil Young on Harvest Moon and thirty years of it  ";
    for (uint8_t seg = 0; seg < 16; seg++) {
        uint16_t b = (uint16_t)((2u << 12) | (1u << 10) | (22u << 5) | seg);
        uint16_t c = (uint16_t)(((uint8_t)rt[seg * 4] << 8)
                                | (uint8_t)rt[seg * 4 + 1]);
        uint16_t d = (uint16_t)(((uint8_t)rt[seg * 4 + 2] << 8)
                                | (uint8_t)rt[seg * 4 + 3]);
        uint16_t blk[4] = { 0xC2B5, b, c, d };
        en_rds_group(&rp_model.rds, blk, EN_RDS_ALL);
    }

    /*
     * One 4A, so the preview has a clock. Built as a real group and pushed
     * through the decoder like everything else here, rather than by writing
     * the ct_ fields directly: this way the modified-Julian-day conversion and
     * the half-hour offset are exercised by every preview run, and a screen
     * showing a time is evidence that the decoder produced one.
     *
     * MJD 61280 is 2026-08-28. 14:32 UTC at -3:30 (offset 7 half hours, west)
     * is 11:02 local, which is what the screens should show.
     */
    {
        const uint32_t mjd = 61280u;
        const uint8_t hour = 14u, minute = 32u;
        const uint8_t half_hours = 7u;      /* 3.5 h */

        uint16_t b = (uint16_t)((4u << 12) | (1u << 10) | (22u << 5)
                                | (uint16_t)((mjd >> 15) & 0x0003u));
        uint16_t c = (uint16_t)(((mjd & 0x7FFFu) << 1)
                                | (uint16_t)((hour >> 4) & 0x0001u));
        uint16_t d = (uint16_t)(((uint16_t)(hour & 0x0Fu) << 12)
                                | ((uint16_t)minute << 6)
                                | 0x0020u          /* west of Greenwich */
                                | half_hours);
        uint16_t blk[4] = { 0xC2B5, b, c, d };
        en_rds_group(&rp_model.rds, blk, EN_RDS_ALL);
    }
}

static void seed_presets(void)
{
    static const struct { uint32_t khz; const char *name; uint8_t pty; }
    seed[] = {
        {  88500, "CBC RAD", 22 },
        {  93500, "OZ FM",    5 },
        {  97500, "K-ROCK",   6 },
        {  99100, "HITS FM",  9 },
        { 102900, "VOCM FM", 11 },
        { 106900, "CHMR",    23 },
    };

    en_presets_init(&rp_model.presets, "Americas");
    for (unsigned i = 0; i < sizeof seed / sizeof seed[0]; i++) {
        en_preset_t e;
        memset(&e, 0, sizeof e);
        e.khz = seed[i].khz;
        e.pty = seed[i].pty;
        e.rbds = true;
        e.pi = (uint16_t)(0xC000 + i);
        strncpy(e.name, seed[i].name, sizeof e.name - 1);
        en_preset_add(&rp_model.presets, &e);
    }
}

/*
 * A plausible band, for the preview only.
 *
 * A fixed reading at every frequency makes a scan of the desktop build come
 * out perfectly flat, which is a picture of the drawing code working and of
 * nothing else. Six stations of different strengths over a noise floor is
 * what the dial's band strip exists to show.
 */
static uint8_t fake_band_rssi(uint32_t khz)
{
    static const struct { uint32_t khz; uint8_t rssi; } ON_AIR[] = {
        {  88100, 210 }, {  93500, 150 }, {  97500, 235 },
        {  99100,  96 }, { 102900, 180 }, { 106900, 120 },
    };
    for (unsigned i = 0; i < sizeof ON_AIR / sizeof ON_AIR[0]; i++) {
        uint32_t d = khz > ON_AIR[i].khz ? khz - ON_AIR[i].khz
                                         : ON_AIR[i].khz - khz;
        /* A transmitter is not a spike: it spreads over a few channels, which
           is what makes the strip look like a band rather than a barcode. */
        if (d <= 400) {
            uint32_t v = ON_AIR[i].rssi;
            v = v - (v * d) / 600u;
            return (uint8_t)v;
        }
    }
    /* The floor wanders a little with frequency, so the empty parts of the
       band are not a ruled line. Derived from the frequency rather than from
       a random number, so every run draws the same picture. */
    return (uint8_t)(14 + (khz / 300u) % 9u);
}

void rp_model_refresh(void)
{
    static bool seeded;
    if (!seeded) {
        seeded = true;

        rp_model.region = en_region_find("Americas");
        rp_model.khz = 98500;
        rp_model.rssi = 62;
        rp_model.snr = 24;
        rp_model.stereo = true;
        rp_model.powered = true;
        rp_model.tuner_ok = true;
        rp_model.rds_on = true;
        rp_model.capture_ok = true;
        rp_model.live_ms = 21000;
        rp_model.live_cap_ms = 30000;
        rp_model.behind_ms = 8000;      /* mid-scrub, to see the control */
        rp_model.behind_max_ms = 30000;
        rp_model.play_ok = true;
        rp_model.can_raw = true;
        /* The preview exists to show the layout the device would draw, and the
           mute button hides itself on a card that has no such control - so
           without this the one screen that could have caught a collision in it
           never renders it. */
        rp_model.mute_ok = true;

        /* Audio out, in the state the device would be in a minute after
           starting: playing to both, at a level somebody has turned down a
           little, with the codec where the app leaves it. */
        rp_model.output = 2;
        rp_model.output_open = true;
        rp_model.volume = 82;
        rp_model.volume_ok = true;
        rp_model.hp_level = 75;
        rp_model.hp_level_ok = true;

        rp_model.play_backend =
            "alsa-lib n31both (Both)  32000 Hz 2 ch  8192 frames, starts full";
        rp_model.output_open = true;
        rp_model.backend = "bcm2078-bt at /sys/devices/platform/soc/bcm2078";
        rp_model.capture_backend =
            "alsa-lib n31fm  32000 Hz 2 ch  2048 frame buffer  30s live";

        rp_model.library_count = 3;
        strcpy(rp_model.library[0], "2026-08-28 09-14 CBC RAD 98.5.wav");
        strcpy(rp_model.library[1], "2026-08-27 18-02 K-ROCK 97.5.wav");
        strcpy(rp_model.library[2], "2026-08-26 21-47 traffic 98.5.wav");

        seed_rds();
        seed_presets();
    }

    rp_model.recording = s_recording;
    rp_model.rec_ms = s_rec_ms;
    if (s_recording) s_rec_ms += 100;
    /* Follows the dial, so a scan of the preview sees a band. */
    rp_model.rssi = fake_band_rssi(rp_model.khz);

}

/* ---- actions ------------------------------------------------------------- */

void rp_act_tune(uint32_t khz)
{
    /* A station the listener chose is the follower's new home. */
    en_af_retuned(&rp_model.af, khz);
    rp_model.khz = khz;
}

void rp_act_tune_quiet(uint32_t khz)
{
    /* See the note in model_linux.c: a retune made on the listener's behalf
       is not the listener choosing a station, and the follower must not treat
       its own move as one. */
    rp_model.khz = khz;
}

/* Nothing to persist to on the desktop: the preview keeps its presets in
   memory and is thrown away with the process. */
/* Nothing here polls a tuner, so there is no cadence to change. */
void rp_model_set_fast(bool on) { (void)on; }

void rp_act_presets_save(void) { }

void rp_act_set_rec_limit(uint16_t minutes)
{
    rp_model.rectimer.limit_min = minutes;

}

void rp_act_set_rec_at(int16_t minutes)
{
    rp_model.rectimer.at_min = minutes;
    /* Clear the fired flag with the time itself, or setting a new one inside
       the same minute the last fired in would do nothing. */
    rp_model.rectimer.fired = false;

}

/* No tuner on the desktop, so nothing to seek with. */
bool rp_act_seek_quiet(bool up) { (void)up; return false; }

/* No tuner: the preview remembers the choice so the control can be seen
   working, and nothing else happens. */
void rp_act_stereo_mode(uint8_t mode) { rp_model.stereo_mode = mode; }

void rp_act_af_follow(bool on)
{
    uint32_t back = 0;
    if (en_af_enable(&rp_model.af, on, &back) == EN_AF_GOTO)
        rp_act_tune(back);

}

uint8_t rp_act_af_to_presets(void)
{
    /* Whatever the station is advertising right now. The PI and programme
       type come from what is being received, so the presets arrive named as
       far as RDS has got - and the frequency is the point regardless. */
    uint8_t n = 0;
    for (uint8_t i = 0; i < rp_model.rds.af_count; i++) {
        uint32_t khz = rp_model.rds.af[i];
        if (!khz) continue;
        if (en_preset_find(&rp_model.presets, khz) >= 0) continue;

        en_preset_t e;
        memset(&e, 0, sizeof e);
        e.khz = khz;
        e.pi = rp_model.rds.pi;
        e.pty = rp_model.rds.pty;
        e.rbds = rp_model.rds.rbds;
        if (rp_model.rds.ps_valid)
            snprintf(e.name, sizeof e.name, "%s", rp_model.rds.ps);
        if (en_preset_add(&rp_model.presets, &e)) n++;
    }
    if (n) {
        en_preset_sort(&rp_model.presets);

    }
    return n;
}

void rp_act_step(bool up)
{
    rp_model.khz = en_region_step(rp_model.region, rp_model.khz, up);
}

void rp_act_seek(bool up) { rp_act_step(up); }
/* Nothing here is brought up, so nothing is ever waited for. */
const char *rp_model_waiting(void) { return NULL; }

/* And nothing here sets a clock; en_clock's stub says so for both. */
const char *rp_act_set_clock(int64_t utc)
{
    return en_clock_strerror(en_clock_set_unix(utc));
}
int64_t     rp_model_now(void)           { return en_clock_now(); }
const char *rp_model_clock_backend(void) { return en_clock_backend(); }

void rp_act_power(bool on) { rp_model.powered = on; }

/* No tuner behind this build, so these are the flags and nothing else - which
   is enough for the preview to draw the state it would be in. */
void rp_act_mute(bool on)    { rp_model.muted = on; }
void rp_act_squelch(bool on) { rp_model.squelched = on; }

/* ---- where the audio goes, and how loud ---------------------------------- */

/*
 * The same three the device offers, so the preview draws the real thing.
 *
 * Named here rather than shared with the Linux player because there is nothing
 * to share: on the device these are alsa-lib device names, and this build has
 * no ALSA at all. What the two have in common is the words on the screen,
 * which is exactly what this list is.
 */
static const char *const k_out_label[] = { "Headphones", "Bluetooth", "Both" };
#define OUT_N ((uint8_t)(sizeof k_out_label / sizeof k_out_label[0]))

uint8_t rp_out_count(void) { return OUT_N; }

const char *rp_out_label(uint8_t i)
{
    return i < OUT_N ? k_out_label[i] : "";
}

/*
 * Everything is available here, which is the honest answer for a build with no
 * hardware: the reason an output can be unavailable on the device is that
 * nothing is reading the Bluetooth fifo, and there is no fifo.
 */
bool rp_out_ready(uint8_t i) { return i < OUT_N; }

const char *rp_act_set_output(uint8_t i)
{
    if (i >= OUT_N) return "no such output";
    rp_model.output = i;
    rp_model.output_open = true;
    return NULL;
}

void rp_act_set_volume(uint8_t percent)
{
    rp_model.volume = percent > 100 ? 100 : percent;
    rp_model.volume_ok = true;
}

void rp_act_nudge_volume(int delta)
{
    int v = (int)rp_model.volume + delta;

    if (v < 0) v = 0;
    if (v > 100) v = 100;
    rp_act_set_volume((uint8_t)v);
}

void rp_act_set_hp_level(uint8_t level)
{
    rp_model.hp_level = level > 88 ? 88 : level;
    rp_model.hp_level_ok = true;
}


void rp_act_record_toggle(void)
{
    s_recording = !s_recording;
    if (s_recording) s_rec_ms = 0;
}

void rp_act_save_live(uint32_t ms) { (void)ms; }

void rp_act_preset_toggle(void)
{
    en_preset_t e;
    memset(&e, 0, sizeof e);
    e.khz = rp_model.khz;
    e.pty = rp_model.rds.pty;
    e.rbds = rp_model.rds.rbds;
    e.pi = rp_model.rds.pi;
    if (rp_model.rds.ps_valid) strncpy(e.name, rp_model.rds.ps, sizeof e.name - 1);

    if (en_preset_find(&rp_model.presets, e.khz) >= 0)
        en_preset_remove(&rp_model.presets, e.khz);
    else
        en_preset_add(&rp_model.presets, &e);
    en_preset_sort(&rp_model.presets);
}

/* Playback, faked just enough that the controls have something to move. */
void rp_act_play_file(const char *name)
{
    if (!name) return;
    rp_model.play_file = true;
    rp_model.play_paused = false;
    rp_model.play_pos_ms = 0;
    rp_model.play_len_ms = 214000;
    snprintf(rp_model.play_name, sizeof rp_model.play_name, "%s", name);
}

void rp_act_play_live(void)
{
    rp_model.play_file = false;
    rp_model.behind_ms = 0;
    rp_model.play_name[0] = 0;
}

void rp_act_nudge(int32_t ms)
{
    if (rp_model.play_file) {
        int64_t p = (int64_t)rp_model.play_pos_ms + ms;
        if (p < 0) p = 0;
        if (p > (int64_t)rp_model.play_len_ms) p = rp_model.play_len_ms;
        rp_model.play_pos_ms = (uint32_t)p;
        return;
    }
    int64_t b = (int64_t)rp_model.behind_ms - ms;
    if (b < 0) b = 0;
    if (b > (int64_t)rp_model.behind_max_ms) b = rp_model.behind_max_ms;
    rp_model.behind_ms = (uint32_t)b;
}

void rp_act_pause_toggle(void) { rp_model.play_paused = !rp_model.play_paused; }
void rp_act_ta_record(bool on) { rp_model.ta_record = on; }
void rp_act_show_simple(bool on) { rp_model.simple_screen = on; }
void rp_act_show_wide(bool on) { rp_model.wide_screen = on; }

bool rp_act_simple_toggle(uint32_t khz)
{
    int at = en_preset_find(&rp_model.presets, khz);
    if (at < 0) return false;
    return en_preset_set_simple(&rp_model.presets, khz,
                                !rp_model.presets.list[at].simple);
}

/* A shadow of the chip, so the register editor can be driven in the preview.
   Values persist for the run, which is enough to see a toggle stick. */
static uint8_t s_shadow[256][8];
static en_overrides_t s_over;

bool rp_act_reg_read(uint8_t addr, uint8_t *buf, uint8_t len)
{
    if (!buf || !len) return false;
    for (uint8_t i = 0; i < len && i < 8; i++) buf[i] = s_shadow[addr][i];
    return true;
}

void rp_act_reg_write(uint8_t addr, const uint8_t *buf, uint8_t len)
{
    if (!buf || !len) return;
    for (uint8_t i = 0; i < len && i < 8; i++) s_shadow[addr][i] = buf[i];
    en_override_set(&s_over, addr, buf, len);
}

void rp_act_reg_revert(uint8_t addr)
{
    en_override_clear(&s_over, addr);
    for (uint8_t i = 0; i < 8; i++) s_shadow[addr][i] = 0;
}

bool rp_act_reg_overridden(uint8_t addr)
{
    return en_override_find(&s_over, addr) != 0;
}

void rp_act_set_region(const en_region_t *rg)
{
    if (!rg) return;
    rp_model.region = rg;
    rp_model.rds.rbds = rg->rbds;
    if (!en_region_on_grid(rg, rp_model.khz))
        rp_model.khz = en_region_step(rg, rp_model.khz, true);
}

/* The preview has no bring-up to report. */
void rp_model_set_progress(const rp_progress_t *p) { (void)p; }
