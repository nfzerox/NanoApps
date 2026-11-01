/*
 * model_device.c — the model on RetailOS.
 *
 * What works here is persistence and the interface. What does not, yet, is the
 * tuner and the audio, and the reason is worth stating plainly because it is a
 * protocol constraint rather than missing work.
 *
 * The tuner is reached by HCI vendor command 0xFC15, and on this device the OS
 * Bluetooth stack owns that transport. HCI is flow-controlled: the controller
 * grants command credits in Command Complete events, and a host is expected to
 * account for them. Injecting a command underneath a running stack breaks that
 * accounting in both directions - our completion can be consumed as the OS's,
 * or the OS can see a credit returned for a command it never sent - and the
 * failure mode is its command queue stalling, which takes Bluetooth and FM with
 * it. That is why this backend does not send commands, and why it is not simply
 * a matter of being careful with reads.
 *
 * The supported route is the OS's own tuner object, ISL::TRFTuner, whose vtable
 * is located (0x087f0050, with the concrete RTXC implementation at 0x087ed41c)
 * and whose slots are mapped by shape but not by meaning. Calling a slot whose
 * name is a guess would be worse than not calling it, so this reports the tuner
 * unavailable and says why. See RADIO_NOTES.md.
 *
 * Everything above the platform layer is unaffected: the screens read rp_model,
 * settings and presets persist, and the parts that cannot work are absent
 * rather than broken.
 */

#include "../model.h"
#include "clock.h"
#include "../core/fmreg.h"

#include "hb_sdk.h"

#include <string.h>

/* The device build is freestanding and has no formatted printing. A bounded
   copy is all this file needed it for. */
static void copy_str(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!cap) return;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

rp_model_t rp_model;

#define RP_DIR       "/Data/RadioPlus"
#define RP_SETTINGS  RP_DIR "/settings.json"
#define RP_PRESETS   RP_DIR "/presets.json"
#define RP_RECORDS   RP_DIR "/Recordings"

static en_settings_t s_settings;

/* ---- files ---------------------------------------------------------------- */

static void paths_init(void)
{
    if (!hb_fs_exists(RP_DIR)) hb_fs_mkdir(RP_DIR);
    if (!hb_fs_exists(RP_RECORDS)) hb_fs_mkdir(RP_RECORDS);
}

static void settings_load(void)
{
    en_settings_default(&s_settings);

    static char buf[4096];
    uint32_t n = hb_fs_read(RP_SETTINGS, buf, sizeof buf - 1);
    if (!n) return;
    buf[n] = 0;
    en_settings_load(&s_settings, buf, n);
}

static void settings_save(void)
{
    if (rp_model.region)
        copy_str(s_settings.region, sizeof s_settings.region, rp_model.region->name);
    s_settings.khz = rp_model.khz;
    s_settings.rds_on = rp_model.rds_on;
    s_settings.ta_record = rp_model.ta_record;
    s_settings.stereo_mode = rp_model.stereo_mode;

    static char buf[4096];
    uint32_t n = en_settings_save(&s_settings, buf, sizeof buf);

    /* Zero means it would have been truncated, and a short settings file looks
       valid with fields missing. Leaving the previous one is the better half of
       a bad choice. */
    if (n) hb_fs_write(RP_SETTINGS, buf, n);
}

static void presets_load(void)
{
    static char buf[16384];
    uint32_t n = hb_fs_read(RP_PRESETS, buf, sizeof buf - 1);
    if (!n) {
        en_presets_init(&rp_model.presets,
                        rp_model.region ? rp_model.region->name : "");
        return;
    }
    buf[n] = 0;
    if (!en_presets_load(&rp_model.presets, buf, n))
        en_presets_init(&rp_model.presets,
                        rp_model.region ? rp_model.region->name : "");
}

static void presets_save(void)
{
    static char buf[16384];
    uint32_t n = en_presets_save(&rp_model.presets, buf, sizeof buf);
    if (n) hb_fs_write(RP_PRESETS, buf, n);
}

static void library_scan(void)
{
    rp_model.library_count = 0;

    hb_dir_t d;
    if (!hb_fs_dir_open(&d, RP_RECORDS, false)) return;

    char name[128];
    bool is_dir = false;
    while (hb_fs_dir_next(&d, name, sizeof name, &is_dir)
           && rp_model.library_count < 12) {
        if (is_dir) continue;

        uint32_t len = 0;
        while (name[len]) len++;
        if (len < 5 || name[len - 4] != '.' || name[len - 3] != 'w'
            || name[len - 2] != 'a' || name[len - 1] != 'v') continue;

        /* Whole or not at all: a truncated name would give a row that cannot
           be opened, which is worse than a row that is not there. */
        if (len >= sizeof rp_model.library[0]) continue;
        memcpy(rp_model.library[rp_model.library_count], name, len + 1);
        rp_model.library_count++;
    }
    hb_fs_dir_close(&d);
}

/* ---- the model ------------------------------------------------------------ */

static void bring_up(void)
{
    paths_init();
    settings_load();

    rp_model.region = en_region_find(s_settings.region);
    if (!rp_model.region) rp_model.region = en_region_find("Americas");

    rp_model.rds_on = s_settings.rds_on;
    rp_model.ta_record = s_settings.ta_record;
    rp_model.stereo_mode = s_settings.stereo_mode;
    en_rds_init(&rp_model.rds, rp_model.region ? rp_model.region->rbds : true);

    presets_load();
    library_scan();

    /* Stated rather than left blank. A radio that shows nothing and explains
       nothing is indistinguishable from a broken one. */
    rp_model.tuner_ok = false;
    rp_model.backend = "not wired: the OS owns the HCI transport";
    rp_model.tuner_note =
        "The tuner is reached by an HCI vendor command, and the system "
        "Bluetooth stack owns that transport. HCI is flow controlled, so "
        "injecting commands underneath it can stall its command queue. The "
        "supported route is the OS tuner object, whose vtable is located but "
        "whose methods are not yet identified.";

    rp_model.capture_ok = false;
    rp_model.capture_backend = "not wired: FM capture buffer not located";
    rp_model.play_ok = false;

    /* Not offered at all rather than offered and failing. */
    rp_model.can_raw = false;

    rp_model.khz = s_settings.khz;
    if (!rp_model.khz && rp_model.region) rp_model.khz = rp_model.region->low_khz;
}

void rp_model_refresh(void)
{
    static bool up;
    if (!up) { up = true; bring_up(); }

    /* Nothing to poll. The screens redraw from what is already here, which on
       this platform is the settings, the presets and the recordings. */
}

/* ---- actions -------------------------------------------------------------- */

/*
 * The tuner actions are accepted and remembered rather than refused. A user
 * setting a region or saving a preset on this build is making a choice that
 * will be correct the moment the tuner is wired, and throwing it away because
 * the radio is not working yet would lose real intent.
 */

void rp_act_tune(uint32_t khz)
{
    /* Whatever round was in progress is void: the listener has chosen a
       station, and that station is home now. */
    en_af_retuned(&rp_model.af, khz);
    rp_model.khz = khz;
    en_rds_init(&rp_model.rds, rp_model.region ? rp_model.region->rbds : true);
    settings_save();
}

void rp_act_tune_quiet(uint32_t khz)
{
    rp_model.khz = khz;
    en_rds_init(&rp_model.rds, rp_model.region ? rp_model.region->rbds : true);
}

/* Nothing here polls a tuner, so there is no cadence to change. */
void rp_model_set_fast(bool on) { (void)on; }

void rp_act_presets_save(void) { presets_save(); }

void rp_act_set_rec_limit(uint16_t minutes)
{
    rp_model.rectimer.limit_min = minutes;
    settings_save();
}

void rp_act_set_rec_at(int16_t minutes)
{
    rp_model.rectimer.at_min = minutes;
    /* Clear the fired flag with the time itself, or setting a new one inside
       the same minute the last fired in would do nothing. */
    rp_model.rectimer.fired = false;
    settings_save();
}

/* The bare-metal path drives the chip through the OS's own tuner calls and
   has no seek of its own; the scan steps instead. */
bool rp_act_seek_quiet(bool up) { (void)up; return false; }

void rp_act_stereo_mode(uint8_t mode)
{
    rp_model.stereo_mode = mode;
    settings_save();
}

void rp_act_af_follow(bool on)
{
    uint32_t back = 0;
    if (en_af_enable(&rp_model.af, on, &back) == EN_AF_GOTO)
        rp_act_tune(back);
    settings_save();
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
            copy_str(e.name, sizeof e.name, rp_model.rds.ps);
        if (en_preset_add(&rp_model.presets, &e)) n++;
    }
    if (n) {
        en_preset_sort(&rp_model.presets);
        presets_save();
    }
    return n;
}

void rp_act_step(bool up)
{
    rp_act_tune(en_region_step(rp_model.region, rp_model.khz, up));
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


void rp_act_set_region(const en_region_t *rg)
{
    if (!rg) return;
    rp_model.region = rg;
    rp_model.rds.rbds = rg->rbds;
    copy_str(rp_model.presets.region, sizeof rp_model.presets.region, rg->name);

    if (!en_region_on_grid(rg, rp_model.khz))
        rp_act_tune(en_region_step(rg, rp_model.khz, true));

    presets_save();
    settings_save();
}

void rp_act_preset_toggle(void)
{
    en_preset_t e;
    memset(&e, 0, sizeof e);
    e.khz = rp_model.khz;
    e.pty = rp_model.rds.pty;
    e.rbds = rp_model.rds.rbds;
    e.pi = rp_model.rds.pi;
    if (rp_model.rds.ps_valid)
        copy_str(e.name, sizeof e.name, rp_model.rds.ps);

    if (en_preset_find(&rp_model.presets, e.khz) >= 0)
        en_preset_remove(&rp_model.presets, e.khz);
    else
        en_preset_add(&rp_model.presets, &e);

    en_preset_sort(&rp_model.presets);
    presets_save();
}

void rp_act_ta_record(bool on)
{
    rp_model.ta_record = on;
    settings_save();
}

void rp_act_show_simple(bool on)
{
    rp_model.simple_screen = on;
    settings_save();
}

void rp_act_show_wide(bool on)
{
    rp_model.wide_screen = on;
    settings_save();
}

bool rp_act_simple_toggle(uint32_t khz)
{
    int at = en_preset_find(&rp_model.presets, khz);
    if (at < 0) return false;
    if (!en_preset_set_simple(&rp_model.presets, khz,
                              !rp_model.presets.list[at].simple))
        return false;
    presets_save();
    return true;
}

/* Audio. Nothing to do without a capture path, and doing nothing quietly is
   correct here: the buttons that would drive these are not shown, because the
   model reports capture and playback unavailable. */
void rp_act_record_toggle(void) { }
void rp_act_save_live(uint32_t ms) { (void)ms; }
void rp_act_play_file(const char *name) { (void)name; }
void rp_act_play_live(void) { }
void rp_act_nudge(int32_t ms) { (void)ms; }
void rp_act_pause_toggle(void) { }

/* Raw register access is not offered on this platform - can_raw is false, so
   the explorer shows why instead of a list of controls that would fail. */
bool rp_act_reg_read(uint8_t addr, uint8_t *buf, uint8_t len)
{
    (void)addr; (void)buf; (void)len;
    return false;
}

void rp_act_reg_write(uint8_t addr, const uint8_t *buf, uint8_t len)
{
    (void)addr; (void)buf; (void)len;
}

void rp_act_reg_revert(uint8_t addr) { (void)addr; }
bool rp_act_reg_overridden(uint8_t addr) { (void)addr; return false; }

/* RetailOS brings up through its own path; nothing to report yet. */
void rp_model_set_progress(const rp_progress_t *p) { (void)p; }
