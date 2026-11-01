/*
 * model_linux.c — fill rp_model from the real tuner and the real capture.
 *
 * The screens read rp_model and nothing else, so this is the only file that
 * knows both sides. Everything expensive is rate-limited rather than done per
 * frame: reading RSSI is a round trip through sysfs into a driver that talks
 * HCI, and doing that sixty times a second would keep the transport busy to
 * show a number that changes twice.
 *
 * Recording names carry the time, the station and the frequency, because a
 * directory of recordings called rec001 is useless a week later. The RDS
 * sidecar is written beside the audio under the same stem.
 */

#include "../model.h"
#include "../core/fmreg.h"
#include "tuner.h"
#include "capture.h"
#include "clock.h"
#include "mixer.h"
#include "player.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

rp_model_t rp_model;

static char s_home[256];
static char s_rec_dir[320];
static char s_presets_path[320];
static char s_settings_path[320];
static en_settings_t s_settings;

/* The recording in progress, and its sidecar. */
static FILE    *s_sidecar;
static en_sidecar_t s_sc;
static uint32_t s_rec_started_ms;
static bool     s_ta_prev;
static char     s_rec_stem[512];   /* directory plus a dated, named stem */

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + (uint32_t)(t.tv_nsec / 1000000));
}

/* ---- where things live ---------------------------------------------------- */

static void paths_init(void)
{
    const char *h = getenv("RADIOPLUS_HOME");
    /* Not /tmp/radioplus: that is where the binary is pushed, and mkdir then
       fails because the path is already a file. */
    if (!h || !*h) h = "/tmp/radioplus.d";
    snprintf(s_home, sizeof s_home, "%s", h);
    mkdir(s_home, 0755);

    snprintf(s_rec_dir, sizeof s_rec_dir, "%s/recordings", s_home);
    mkdir(s_rec_dir, 0755);

    snprintf(s_presets_path, sizeof s_presets_path, "%s/presets.json", s_home);
    snprintf(s_settings_path, sizeof s_settings_path, "%s/settings.json",
             s_home);
}

static void presets_load(void)
{
    FILE *f = fopen(s_presets_path, "rb");
    if (!f) {
        en_presets_init(&rp_model.presets,
                        rp_model.region ? rp_model.region->name : "");
        return;
    }
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;

    if (!en_presets_load(&rp_model.presets, buf, (uint32_t)n))
        en_presets_init(&rp_model.presets,
                        rp_model.region ? rp_model.region->name : "");
}

static void presets_save(void)
{
    static char buf[16384];
    uint32_t n = en_presets_save(&rp_model.presets, buf, sizeof buf);

    /* en_presets_save reports 0 rather than truncating. Writing a short file
       would look like a valid preset list with entries missing, which is worse
       than leaving the previous one alone. */
    if (!n) return;

    FILE *f = fopen(s_presets_path, "wb");
    if (!f) return;
    fwrite(buf, 1, n, f);
    fclose(f);
}

static void settings_load(void)
{
    en_settings_default(&s_settings);

    FILE *f = fopen(s_settings_path, "rb");
    if (!f) return;
    static char buf[2048];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    en_settings_load(&s_settings, buf, (uint32_t)n);
}

static void settings_save(void)
{
    /* Called whenever something worth remembering changes. Writing a few
       hundred bytes on a retune is cheaper than losing the station on a flat
       battery. */
    if (rp_model.region)
        snprintf(s_settings.region, sizeof s_settings.region, "%s",
                 rp_model.region->name);
    s_settings.khz = rp_model.khz;
    s_settings.rds_on = rp_model.rds_on;
    s_settings.ta_record = rp_model.ta_record;
    s_settings.stereo_mode = rp_model.stereo_mode;
    s_settings.simple_screen = rp_model.simple_screen;
    s_settings.wide_screen = rp_model.wide_screen;
    s_settings.volume = rp_model.volume;
    s_settings.hp_level = rp_model.hp_level;

    /* Stored by name, so adding an output later cannot move somebody's
       choice to a different one. */
    {
        const en_play_out_t *o = en_play_out(rp_model.output);
        snprintf(s_settings.output, sizeof s_settings.output, "%s",
                 o ? o->pcm : "");
    }

    char buf[2048];
    uint32_t n = en_settings_save(&s_settings, buf, sizeof buf);
    if (!n) return;

    FILE *f = fopen(s_settings_path, "wb");
    if (!f) return;
    fwrite(buf, 1, n, f);
    fclose(f);
}

/* Newest first, which is the order anyone wants a recordings list in. */
static void library_scan(void)
{
    rp_model.library_count = 0;

    DIR *d = opendir(s_rec_dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) && rp_model.library_count < 12) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".wav")) continue;

        /* Copied whole or not at all. A truncated name is worse than a missing
           row, because the row would be there and would not open - and names
           this app writes are about thirty characters, so anything that does
           not fit did not come from here. */
        if (len >= sizeof rp_model.library[0]) continue;
        memcpy(rp_model.library[rp_model.library_count], e->d_name, len + 1);
        rp_model.library_count++;
    }
    closedir(d);

    /* Names begin with the timestamp, so a reverse sort is newest first. */
    for (uint8_t i = 1; i < rp_model.library_count; i++) {
        char k[96];
        snprintf(k, sizeof k, "%s", rp_model.library[i]);
        int j = (int)i - 1;
        while (j >= 0 && strcmp(rp_model.library[j], k) < 0) {
            snprintf(rp_model.library[j + 1], sizeof rp_model.library[0],
                     "%s", rp_model.library[j]);
            j--;
        }
        snprintf(rp_model.library[j + 1], sizeof rp_model.library[0], "%s", k);
    }
}

/* ---- startup -------------------------------------------------------------- */

/* Where bring-up reports to. Null until something sets it, and every call is
   guarded, so nothing here depends on a UI existing. */
static rp_progress_t s_prog;

void rp_model_set_progress(const rp_progress_t *p)
{
    if (p) {
        s_prog = *p;
    } else {
        s_prog.step = 0;
        s_prog.failed = 0;
    }
}

static void step(const char *what)
{
    if (s_prog.step) s_prog.step(what);
}

static void failed(const char *why)
{
    if (s_prog.failed) s_prog.failed(why);
}

/* ---- bring-up, which is not a moment -------------------------------------- */

/*
 * Three things this app needs from the machine, none of which are guaranteed
 * to exist at the instant it starts.
 *
 *   The tuner    bcm2078-bt's sysfs, and hci0 UP underneath it.
 *   Capture      hw:0,1, which is IIS2 and is what clocks the tuner's audio.
 *   Playback     hw:0,0, the headphone codec.
 *
 * All three arrive from things happening in parallel with this process: the
 * sound modules are insmod'ed by a script, and hci0 is raised by another one.
 * This used to be written as though startup were a single instant - one
 * attempt at each, the answer latched into the model, and a driver that turned
 * up two seconds later was indistinguishable from one that never would. That
 * is what "no audio hardware" and then "no Bluetooth" on a machine where both
 * were about to work actually was.
 *
 * So each is a stage that can be attempted repeatedly and is idempotent, and
 * there are two cadences over them:
 *
 *   At startup, on the boot screen, every DEP_POLL_MS for up to DEP_WAIT_MS.
 *   The screen says which one it is waiting for, and the dots keep moving.
 *
 *   Afterwards, quietly, every DEP_RETRY_MS forever. A tuner that appears a
 *   minute in is configured and tuned exactly as it would have been at
 *   startup, and the interface simply starts working. Nothing needs restarting
 *   and nothing has to be latched.
 *
 * Both cadences are cheap: en_tuner_init() asks the controller its state with
 * one ioctl and does any actual bring-up on its own thread, and the two PCM
 * opens fail immediately when the card is absent.
 */
#define DEP_POLL_MS    250u
#define DEP_WAIT_MS  20000u
#define DEP_RETRY_MS  4000u

/* Defined with the actions, because that is where mute belongs. Declared
   here because bringing the mixer up has to reassert it - see try_mixer. */
static void mute_apply(void);

/*
 * The nearest real channel.
 *
 * Nothing enforced this, so any arithmetic slip anywhere upstream became a
 * frequency: 87645 kHz was tuned, displayed and written to the settings file,
 * and it is not a channel in any region this radio knows. An off-grid
 * frequency is not a station - it is noise between two of them - which is
 * exactly what a radio that "loads and then plays static" sounds like.
 *
 * Applied at the one place every tune goes through, rather than trusted to
 * each caller.
 */
static uint32_t snap_khz(uint32_t khz)
{
    const en_region_t *rg = rp_model.region;
    uint32_t off, v;

    if (!rg || !rg->step_khz || !khz) return khz;
    if (khz < rg->low_khz) return rg->low_khz;
    if (khz > rg->high_khz) return rg->high_khz;

    /* Nearest, not downward: a frequency half a channel high belongs to the
       channel above it, and truncating would always drag it back down. */
    off = (khz - rg->low_khz + rg->step_khz / 2u) / rg->step_khz;
    v = rg->low_khz + off * rg->step_khz;
    return v > rg->high_khz ? rg->high_khz : v;
}

static bool s_tuner_configured;

/*
 * Whether the tuner has had its turn - which is NOT whether it worked.
 *
 * The audio path must not open before a station has been tuned, because the
 * driver holds the audio route off until then and a capture opened early
 * carries silence that looks exactly like a broken card. That is an ordering
 * rule and this file briefly turned it into a dependency: capture and playback
 * were gated on the tune having RETURNED OK, so a device whose controller was
 * down - no hci0, no FM, nothing to tune - opened no sound card at all and
 * reported that it could not find one. The card was fine. Radio+ never asked
 * it.
 *
 * So there are two flags. s_tuner_configured means the tuner is set up and is
 * what the retry watches. This one means "we are past the point where opening
 * audio could beat the tune to it", and it is what the audio waits on: set as
 * soon as a tune has been ATTEMPTED, or once we have waited long enough for a
 * tuner that is never going to answer. A machine with no tuner still gets its
 * sound card, which is what lets it play back recordings.
 */
static bool s_tuner_settled;

/* How long a tuner that has not answered at all gets before the audio stops
   waiting for it. Short: there is nothing to be early for on a machine with no
   tuner, and the only cost of being wrong is a moment of silence on a machine
   where one turns up late. */
#define TUNE_GRACE_MS 3000u

/*
 * The frequency we asked for, and how long the chip has to catch up.
 *
 * rp_model.khz was two things at once: what the listener chose, and what the
 * chip last reported. The poll below runs every 400 ms and simply assigned the
 * second over the first - so pressing + on the dial tuned 88.1, and then the
 * next poll, arriving before the chip had retuned, wrote 87.9 back over it.
 * The display snapped back, the next press asked for 88.1 again, and the
 * kernel log filled with the same tune four times in a second and a half.
 * That is the whole of "changing channels does not work".
 *
 * So an intent is remembered. While one is outstanding the chip's report is
 * only believed when it agrees; the moment it agrees, or once the settle
 * window has passed, the chip is authoritative again. A tune that genuinely
 * fails therefore shows the truth after a beat rather than lying forever.
 *
 * A seek is the other way round - the chip picks the frequency, so there is
 * no intent to defend and the latch is dropped.
 */
static uint32_t s_want_khz;
static uint32_t s_want_until;
#define TUNE_SETTLE_MS 1500u

/*
 * Faster state polling while something is sweeping the band.
 *
 * The scan ticks every frame and decides whether a seek has landed by
 * comparing the frequency it is handed against the one it started from. At
 * 400 ms that answer is up to four hundred milliseconds stale, which is longer
 * than the scan's own hundred-millisecond settle - so it judged RSSI on the
 * previous channel and called seeks failed that had already landed.
 */
static bool s_fast_poll;

static uint32_t s_bringup_ms;      /* when bring_up ran, for the cadence */
static bool s_bringup_reported;    /* the deadline message, at most once */

/*
 * The output the settings asked for, and whether that question is closed.
 *
 * Restoring it is not a single act, because the Bluetooth legs need an encoder
 * running at the far end and that can arrive after this app does. So a saved
 * choice of Bluetooth is retried through the startup window and then given up
 * on, rather than either failing immediately or switching the audio out from
 * under somebody a minute into listening.
 *
 * Anything the user does closes the question at once. A restore that overrode
 * a deliberate choice would be the worst of both.
 */
static uint8_t s_out_saved;
static bool    s_out_restored;

/* Volume changes are written to the settings lazily. A key held down produces
   twenty of them a second and each one would otherwise be a file write; this
   is when the pending one is due, or zero for none. */
static uint32_t s_save_due;
#define SAVE_DELAY_MS 1500u

/*
 * Everything the tuner needs told to it once, whenever it first answers.
 *
 * This is a separate function precisely because "whenever" is not "at
 * startup". A controller that comes up late gets the same power-on, region,
 * overrides, RDS state, stereo preference and remembered frequency as one that
 * was ready before this app was.
 */
static bool tuner_configure(void)
{
    en_tuner_power(true);
    en_tuner_set_region(rp_model.region);

    /* After the region, not before: a region write touches the control and
       audio registers, so replaying first would let it undo a deliberate
       override of either. */
    for (uint8_t i = 0; i < s_settings.overrides.count; i++) {
        const en_override_t *o = &s_settings.overrides.list[i];
        en_tuner_reg_write(o->addr, o->data, o->len);
    }

    en_tuner_rds_enable(rp_model.rds_on);

    /* Re-apply the stereo choice: it is a preference about a place - one weak
       station you always want in mono - so it has to survive a power cycle
       rather than reverting to auto every time the app starts.

       This ran before rp_model.stereo_mode had been loaded from the settings,
       so it always saw AUTO and the preference was never actually restored.
       The settings are applied to the model up in bring_up() now, before
       anything touches hardware. */
    if (rp_model.stereo_mode != EN_FM_STEREO_AUTO)
        rp_act_stereo_mode(rp_model.stereo_mode);

    /*
     * And back to where the listener was. rp_model.khz was decided during
     * bring-up from the settings and the presets, both of which are readable
     * with no hardware at all.
     *
     * Whether this worked is the return value, and it is the return value
     * because the audio path waits on it. The driver holds the audio route off
     * until a station is tuned, so a tune that did not go through means the
     * capture would open onto silence - and a tune that did not go through is
     * exactly the sort of thing that happens once while hci0 is settling and
     * works on the next attempt. Reporting it lets the caller try again
     * instead of latching a half-configured tuner.
     */
    return rp_model.khz && en_tuner_tune(rp_model.khz) == EN_TUNER_OK;
}

static bool try_tuner(void)
{
    en_tuner_err_t te = en_tuner_init();

    rp_model.backend = en_tuner_backend();
    rp_model.tuner_note = en_tuner_strerror(te);
    rp_model.tuner_ok = (te == EN_TUNER_OK);

    if (rp_model.tuner_ok && !s_tuner_configured) {
        rp_model.can_raw = en_tuner_can_raw();
        /* The driver exposes fm_seek, so the chip does the finding. */
        rp_model.can_seek = true;

        /* Set only when the tune landed. Everything in tuner_configure is
           idempotent, so a retry costs a few sysfs writes and buys a machine
           whose controller answered a moment before it was ready. */
        s_tuner_configured = tuner_configure();

        /* Attempted, which is all the audio path needs to know. */
        s_tuner_settled = true;
    }
    return rp_model.tuner_ok;
}

/*
 * A saved output name, turned back into a position in the platform's list.
 *
 * Names rather than positions is what makes adding an output later harmless:
 * a settings file that says "n31both" still means the same thing after the
 * list grows, where "the third one" would quietly become something else. A
 * name that is no longer offered falls back to the first output, which is the
 * headphones - the one destination that is always there.
 */
static uint8_t out_index(const char *pcm)
{
    if (pcm && *pcm) {
        for (uint8_t i = 0; i < en_play_out_count(); i++) {
            const en_play_out_t *o = en_play_out(i);
            if (o && strcmp(o->pcm, pcm) == 0) return i;
        }
    }
    return 0;
}

/*
 * Put the volume where the model says it should be.
 *
 * Which control that is depends on where the audio is going: each output has
 * its own softvol and "Both" has one over the pair. The output table names it,
 * so this does not have to know anything about the devices themselves.
 *
 * Silently doing nothing when the control is absent is deliberate. softvol
 * registers its control the first time its PCM is opened, so on a machine that
 * has not yet played to Bluetooth the Bluetooth volume genuinely does not
 * exist - and will, later, at which point try_mixer calls this again.
 */
static void volume_apply(void)
{
    const en_play_out_t *o = en_play_out(rp_model.output);

    if (!o) return;

    rp_model.volume_ok = en_mix_present(o->volume);
    if (rp_model.volume_ok)
        en_mix_set(o->volume, rp_model.volume);
}

/*
 * Capture is started with the tuner rather than with the record button: the
 * SoC side of IIS2 is clocked by the capture PCM, so nothing is audible until
 * this is open. It is how the radio makes sound, not just how it is recorded.
 *
 * And it is started AFTER the tuner has been tuned, which is the ordering the
 * whole audio path depends on and the one that is easiest to get wrong. The
 * driver deliberately leaves the audio route off until a station is tuned -
 * it logs "route off until tune" - because routing to a station the front end
 * has not reached yet puts noise on the PCM port. So a start that powers the
 * tuner on and opens the capture gets a perfectly working stream carrying pure
 * silence, which is indistinguishable from a broken audio path and has been
 * mistaken for one.
 *
 * audio_may_open() is the gate. Nothing here has to know the ordering; it
 * cannot be got wrong from this side.
 */
static bool audio_may_open(void)
{
    /* A tune has been attempted. Whether it worked is the tuner's problem. */
    if (s_tuner_settled)
        return true;

    /*
     * Or the tuner never turned up at all. Waiting forever for one would mean
     * a device with a broken controller has no sound card either, which is how
     * this went wrong the first time: there is nothing to be early for when
     * there is no tuner, and a recording still deserves somewhere to play.
     */
    return now_ms() - s_bringup_ms >= TUNE_GRACE_MS;
}
static bool try_capture(void)
{
    if (rp_model.capture_ok) return true;
    if (!audio_may_open()) return false;

    rp_model.capture_ok = (en_cap_start(s_settings.live_seconds) == EN_CAP_OK);
    rp_model.capture_backend = en_cap_backend();
    return rp_model.capture_ok;
}

/* And the other half of the audio path. Capture clocks IIS2; the player
   carries what arrives there to the codec, the Bluetooth fifo, or both.
   Without it the radio is silent however well it is tuned. */
static bool try_play(void)
{
    if (rp_model.play_ok) return true;
    if (!audio_may_open()) return false;

    rp_model.play_ok = (en_play_start() == EN_PLAY_OK);
    return rp_model.play_ok;
}

/*
 * The card's controls, which are neither the tuner's nor the PCM's.
 *
 * Kept apart from both because they arrive on their own schedule. The codec's
 * controls appear with the machine driver; the softvols appear when something
 * first opens the plugin that defines them, which for Bluetooth may be much
 * later or never. So this is attempted on every pass until it reports that
 * everything it wanted was there, and the volume is re-applied each time -
 * a control that appears late must still end up where the user left it.
 */
static bool s_mixer_ready;

static void try_mixer(void)
{
    if (s_mixer_ready) return;

    if (!en_mix_open()) return;

    s_mixer_ready = en_mix_defaults();

    /* The codec level from the settings rather than the safe default, for
       anyone who has calibrated their own unit against a meter. In its own
       0..88 rather than as a percentage, or it would land a step away from
       wherever it was left. */
    if (en_mix_present(EN_MIX_VOL_CODEC)) {
        rp_model.hp_level_ok = true;
        en_mix_set_raw(EN_MIX_VOL_CODEC, rp_model.hp_level);
    }

    volume_apply();

    /*
     * And the model's mute back over the top of it.
     *
     * en_mix_defaults() unmutes the tuner, deliberately: MANUAL_MUTE survives
     * a run of this app that was killed mid-sweep, and a radio that starts
     * silent for a reason nobody can see is the worst failure it can cause.
     * But this function runs until every control it wants has appeared, which
     * can be several seconds - long enough for somebody to press mute and have
     * it undone under them. The model is the authority on that, so it is
     * applied last.
     */
    mute_apply();
}

/*
 * What is still missing, named the way somebody looking at the screen would
 * name it. NULL when everything is here.
 *
 * Ordered by what blocks what: without the tuner there is nothing to hear,
 * without capture the tuner's audio is not even clocked, and without playback
 * it has nowhere to go. Reporting the first of those is reporting the cause
 * rather than the cascade.
 */
static const char *dep_missing(void)
{
    if (!rp_model.tuner_ok)   return "waiting for the tuner";
    /* Between the tuner answering and the tuner being tuned there is a real
       gap - the region write, the overrides and the tune itself are several
       round trips through HCI - and during it the audio deliberately has not
       been opened. Saying so beats "waiting for the sound card" on a machine
       whose sound card is fine.

       Reported on the ATTEMPT, not on success: a tune that keeps failing is a
       tuner fault and is reported as one below, not as a permanent "tuning"
       that never resolves. */
    if (!s_tuner_settled)     return "tuning";
    if (!rp_model.capture_ok) return "waiting for the sound card";
    if (!rp_model.play_ok)    return "waiting for audio out";
    return NULL;
}

static void try_all(void)
{
    /*
     * In this order and not another. The tuner has to answer and be tuned
     * before the capture is opened - see try_capture - and the mixer is asked
     * last only because nothing else waits on it.
     */
    try_tuner();
    try_capture();
    try_play();
    try_mixer();

    /*
     * The saved output, once there is a player to give it to.
     *
     * Bounded to the startup window on purpose: a Bluetooth encoder that
     * appears in the first few seconds was almost certainly started for this,
     * and one that appears two minutes later was not - moving the audio then
     * would be the app overruling somebody who is already listening.
     */
    if (!s_out_restored && rp_model.play_ok) {
        if (s_out_saved == rp_model.output) {
            s_out_restored = true;
        } else if (rp_out_ready(s_out_saved)
                   && en_play_set_output(s_out_saved) == NULL) {
            rp_model.output = s_out_saved;
            volume_apply();
            s_out_restored = true;
        } else if (now_ms() - s_bringup_ms >= DEP_WAIT_MS) {
            s_out_restored = true;
        }
    }

    /*
     * Whether the card publishes a mute control - asked rather than assumed,
     * and asked again while the answer is still no.
     *
     * It belongs here rather than with the tuner's one-time configuration
     * because it does not come from the tuner: "FM Tuner Mute" is published by
     * the machine driver that also provides the PCMs, so it can appear after
     * bcm2078-bt has answered. Probing it once, at whatever moment the tuner
     * first replied, would latch a no - which is the same bug as the rest of
     * this section, in the one place it would be least visible: a mute button
     * that is simply absent looks like a design decision.
     *
     * This stops being asked when the retry stops, which is when everything
     * else is up - and by then the card is present, so the answer is final.
     */
    if (rp_model.tuner_ok && !rp_model.mute_ok)
        rp_model.mute_ok = en_tuner_muted() >= 0;
}

static void bring_up(void)
{
    /* Before anything opens a PCM or a control: alsa-lib reports through a
       global handler that writes to stderr, and this app owns the screen the
       console may be pointed at. */
    en_alsa_quiet();

    step("reading settings");
    paths_init();

    settings_load();

    /* The region is stored by name, so a settings file survives the table
       gaining a row. An unknown name falls back rather than failing. */
    rp_model.region = en_region_find(s_settings.region);
    if (!rp_model.region) rp_model.region = en_region_find("Americas");

    rp_model.rds_on = s_settings.rds_on;
    en_rds_init(&rp_model.rds, rp_model.region ? rp_model.region->rbds : true);

    /* Preferences into the model before anything touches hardware, because
       tuner_configure() reads them back out. */
    rp_model.ta_record = s_settings.ta_record;
    rp_model.stereo_mode = s_settings.stereo_mode;
    rp_model.simple_screen = s_settings.simple_screen;
    rp_model.wide_screen = s_settings.wide_screen;

    /* Audio, before the player exists, so the first device it opens is
       already the right one. The name is resolved against the platform's own
       list: one that is no longer offered falls back to the first output
       rather than to nothing. */
    rp_model.volume = s_settings.volume;
    rp_model.hp_level = s_settings.hp_level;
    s_out_saved = out_index(s_settings.output);
    rp_model.output = 0;
    en_play_set_output(0);

    /* Files, which need no drivers and no waiting. Doing them first means the
       screen behind the boot overlay is already furnished when the hardware
       arrives, and it is what decides the frequency below. */
    presets_load();
    library_scan();

    /* Come back to where you were. Failing that the first preset, and failing
       that the bottom of the band - landing on a remembered station is the
       whole point of remembering one. */
    uint32_t start = s_settings.khz;
    if (!start || !en_region_on_grid(rp_model.region, start))
        start = rp_model.presets.count ? rp_model.presets.list[0].khz
                                       : (rp_model.region ? rp_model.region->low_khz : 0);
    rp_model.khz = start;

    step("tuner init");
    s_bringup_ms = now_ms();
    try_all();

    /*
     * And say where that got to, rather than leaving "tuner init" on screen.
     *
     * step() is only called from in here, and the startup loop only relabels
     * the boot screen while rp_model_waiting() has something to say. Between
     * those two, a bring-up that got far enough to stop the loop from running
     * left the last step it had announced sitting there - so a radio that had
     * finished starting read as one stuck on "tuner init".
     */
    {
        const char *left = dep_missing();

        step(left ? left : "ready");
    }

    /*
     * And that is all bring-up does. It does not wait.
     *
     * An earlier version of this sat in a sleep loop here until everything had
     * arrived, which fixed the race and introduced a worse bug: the caller is
     * the thread that reads the buttons, so for the length of the wait the
     * device ignored every one of them. Twenty seconds of a radio that cannot
     * be quit is not an improvement on a radio that started too early.
     *
     * The waiting is the caller's, through rp_model_waiting(). Everything
     * about how often to retry and how long to keep saying so stays here.
     */
}

/*
 * What the caller should put on the boot screen, or NULL to stop waiting.
 *
 * Two ways to stop: everything arrived, or the deadline passed. Past the
 * deadline this returns NULL and the interface opens anyway - the retry in
 * rp_model_refresh does not stop, so a tuner that turns up a minute later
 * still works, and a radio the user can hold is better than a boot screen with
 * a promise on it.
 */
const char *rp_model_waiting(void)
{
    const char *missing = dep_missing();

    if (!missing) return NULL;

    if (now_ms() - s_bringup_ms < DEP_WAIT_MS)
        return missing;

    /*
     * The deadline. Said once, as a state rather than a verdict: the note
     * names what is missing, and the retry is still running behind it.
     */
    if (!s_bringup_reported) {
        s_bringup_reported = true;
        if (!rp_model.tuner_ok && rp_model.tuner_note)
            failed(rp_model.tuner_note);
        else if (!rp_model.capture_ok)
            failed("no sound card yet - still trying");
        else
            failed("no audio out yet - still trying");
    }
    return NULL;
}

/* ---- the sidecar ---------------------------------------------------------- */

static void sidecar_open(void)
{
    char path[600];
    snprintf(path, sizeof path, "%s.rds.json", s_rec_stem);

    s_sidecar = fopen(path, "wb");
    if (!s_sidecar) return;

    char buf[2048];
    uint32_t n = en_sidecar_begin(&s_sc, buf, sizeof buf, rp_model.khz,
                                  rp_model.region ? rp_model.region->name : "",
                                  rp_model.rds.rbds, &rp_model.rds);
    if (n) fwrite(buf, 1, n, s_sidecar);
}

static void sidecar_close(uint32_t duration_ms)
{
    if (!s_sidecar) return;

    char buf[2048];
    uint32_t n = en_sidecar_end(&s_sc, buf, sizeof buf, duration_ms,
                                &rp_model.rds);
    if (n) fwrite(buf, 1, n, s_sidecar);
    fclose(s_sidecar);
    s_sidecar = 0;
}

/* ---- the refresh ---------------------------------------------------------- */

void rp_model_refresh(void)
{
    static bool up;
    static uint32_t last_slow, last_rds, last_dep, last_mix;

    if (!up) { up = true; bring_up(); }

    uint32_t t = now_ms();

    /*
     * Keep trying for anything that was not there at startup.
     *
     * This is what makes the startup wait a courtesy rather than a deadline: a
     * sound card or a controller that turns up a minute in is picked up here,
     * configured exactly as it would have been, and the interface starts
     * working with nothing restarted. capture_ok is re-derived from the
     * capture thread further down, so this also recovers a capture that died
     * rather than one that never started.
     */
    {
        /* Quick while the boot screen is still waiting on something, and
           unhurried once the interface is open - the fast cadence exists to
           make startup feel immediate, and after that a few seconds either way
           is invisible. */
        uint32_t every = (t - s_bringup_ms < DEP_WAIT_MS) ? DEP_POLL_MS
                                                          : DEP_RETRY_MS;

        /*
         * Three reasons to keep trying, not one.
         *
         * This used to stop the moment dep_missing() went quiet, which is the
         * moment the tuner, the capture and the player are all up - and that
         * is earlier than the other two are settled. The card's softvol
         * controls do not exist until something has opened the plugin that
         * defines them, so on a fast start the volume was still unrestored
         * when the retry stopped; the next read then took the control's own
         * default, which is full scale, and the saved level was replaced by
         * the loudest one. The saved output has the same shape of problem,
         * with an encoder that may not have started yet.
         */
        if (t - last_dep >= every
            && (dep_missing() || !s_mixer_ready || !s_out_restored)) {
            last_dep = t;
            try_all();
        }
    }

    /* Signal and frequency, a few times a second - and four times faster
       while a sweep is running, which needs to see a seek land. Each of these
       is a driver round trip and nothing else on screen changes faster. */
    if (rp_model.tuner_ok && t - last_slow >= (s_fast_poll ? 100u : 400u)) {
        last_slow = t;

        en_tuner_state_t st;
        if (en_tuner_state(&st) == EN_TUNER_OK) {
            if (st.khz) {
                /* An outstanding request is defended until the chip agrees
                   with it or the settle window runs out - see s_want_khz. */
                if (s_want_khz && (int32_t)(t - s_want_until) < 0) {
                    if (st.khz == s_want_khz) {
                        s_want_khz = 0;
                        rp_model.khz = st.khz;
                    }
                } else {
                    s_want_khz = 0;
                    rp_model.khz = st.khz;
                }
            }
            rp_model.rssi = st.rssi;
            rp_model.snr = st.snr;
            rp_model.stereo = st.stereo;
            rp_model.powered = st.powered;
        }
    }

    /* RDS more often, because groups arrive continuously and the FIFO is
       finite - polling too slowly loses them rather than merely delaying
       them. */
    if (rp_model.tuner_ok && rp_model.rds_on && t - last_rds >= 200u) {
        last_rds = t;

        uint16_t groups[8][4];
        uint8_t valid[8];
        int n = en_tuner_rds_poll(&rp_model.rds, groups, valid, 8);

        /* Every group also goes to the sidecar while recording, with its
           offset into the audio, so the display can be rebuilt against the
           recording afterwards. */
        if (s_sidecar && n > 0) {
            uint32_t at = t - s_rec_started_ms;
            char buf[512];
            for (int i = 0; i < n; i++) {
                uint32_t w = en_sidecar_group(&s_sc, buf, sizeof buf, at,
                                              groups[i], valid[i]);
                if (w) fwrite(buf, 1, w, s_sidecar);
            }
        }
    }

    /*
     * The card's controls, a few times a second and independently of the
     * tuner: the volume can be moved from a serial console with tinymix while
     * this is running, and a readout that only changed when this app changed
     * it would be a readout of its own intentions.
     */
    if (t - last_mix >= 400u) {
        const en_play_out_t *o = en_play_out(rp_model.output);
        int v;

        last_mix = t;

        if (o) {
            v = en_mix_get(o->volume);
            rp_model.volume_ok = (v >= 0);
            if (v >= 0) rp_model.volume = (uint8_t)v;
        }

        v = en_mix_get_raw(EN_MIX_VOL_CODEC);
        rp_model.hp_level_ok = (v >= 0);
        if (v >= 0) rp_model.hp_level = (uint8_t)v;
    }

    /* A volume change that has settled gets written out. Deferred rather than
       written on every step so that holding a key down is one file write and
       not forty. */
    if (s_save_due && (int32_t)(t - s_save_due) >= 0) {
        s_save_due = 0;
        settings_save();
    }

    en_play_state_t ps;
    en_play_state(&ps);
    rp_model.play_file = (ps.source == EN_SRC_FILE);
    rp_model.output = ps.output;
    rp_model.output_open = ps.output_open;
    rp_model.play_backend = en_play_backend();
    rp_model.play_paused = ps.paused;
    rp_model.play_pos_ms = ps.pos_ms;
    rp_model.play_len_ms = ps.len_ms;
    rp_model.behind_ms = ps.behind_ms;
    rp_model.behind_max_ms = ps.behind_max_ms;
    snprintf(rp_model.play_name, sizeof rp_model.play_name, "%s", ps.name);

    en_cap_state_t cs;
    en_cap_state(&cs);
    rp_model.capture_ok = cs.running;
    rp_model.recording = cs.recording;
    rp_model.rec_ms = cs.recorded_ms;
    rp_model.live_ms = cs.live_ms;
    rp_model.live_cap_ms = cs.live_cap_ms;
    rp_model.overruns = cs.overruns;

    /* A recording that stopped itself - a full disk, most likely - has to be
       noticed here, because the capture thread cannot close the sidecar. */
    if (s_sidecar && !cs.recording) sidecar_close(cs.recorded_ms);

    /*
     * Traffic announcements.
     *
     * The TA flag goes up for the duration of the announcement, so the edges
     * are the whole signal: start on the rising edge, stop on the falling one.
     * A recording already running by hand is left alone - the user pressing
     * record outranks the station raising a flag, and stopping their recording
     * because an announcement ended would be an unpleasant surprise.
     *
     * The prefill matters more here than anywhere else. By the time the flag is
     * seen the announcement has already begun, so without reaching back into
     * the buffer every automatic recording would start mid-sentence.
     */
    if (rp_model.ta_record && rp_model.tuner_ok) {
        bool ta = rp_model.rds.ta;
        if (ta && !s_ta_prev && !cs.recording) {
            rp_act_record_toggle();
            rp_model.ta_recording = rp_model.recording;
        } else if (!ta && s_ta_prev && rp_model.ta_recording) {
            rp_act_record_toggle();
            rp_model.ta_recording = false;
        }
        s_ta_prev = ta;
    } else {
        s_ta_prev = false;
    }
}

/* ---- actions -------------------------------------------------------------- */

void rp_act_tune(uint32_t khz)
{
    khz = snap_khz(khz);

    /* Whatever round was in progress is void: the listener has chosen a
       station, and that station is home now. */
    en_af_retuned(&rp_model.af, khz);
    rp_model.khz = khz;
    s_want_khz = khz;
    s_want_until = now_ms() + TUNE_SETTLE_MS;
    if (rp_model.tuner_ok) en_tuner_tune(khz);
    settings_save();

    /* A new station is a new set of RDS state; carrying the old name over a
       retune is worse than showing nothing for a second. */
    en_rds_init(&rp_model.rds, rp_model.region ? rp_model.region->rbds : true);
}

void rp_act_tune_quiet(uint32_t khz)
{
    /* Deliberately not rp_act_tune: this is a retune made ON the listener's
       behalf - by the band scan, or by the station follower - so it must not
       write the settings file two hundred times in a sweep, and it must not
       tell the follower that the listener has chosen a new home. The follower
       retunes through here, and treating its own move as a manual retune
       would cancel the round it had just started. */
    khz = snap_khz(khz);
    rp_model.khz = khz;
    s_want_khz = khz;
    s_want_until = now_ms() + TUNE_SETTLE_MS;
    if (rp_model.tuner_ok) en_tuner_tune(khz);
    en_rds_init(&rp_model.rds, rp_model.region ? rp_model.region->rbds : true);
}

void rp_model_set_fast(bool on) { s_fast_poll = on; }

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

/*
 * Seek, and take the answer straight away.
 *
 * The chip decides where this lands, so there is no intent to defend and the
 * latch is dropped. The state is read back here rather than left to the next
 * poll: the band scan asks "has it moved yet" on the very next tick, and an
 * answer that is four hundred milliseconds old makes a landed seek look like
 * one that never went anywhere.
 */
static bool seek_and_adopt(bool up)
{
    en_tuner_state_t st;

    if (!rp_model.tuner_ok) return false;
    if (en_tuner_seek(up) != EN_TUNER_OK) return false;

    s_want_khz = 0;
    if (en_tuner_state(&st) == EN_TUNER_OK && st.khz)
        rp_model.khz = st.khz;

    /* The station changed underneath the decoder, so what it had belongs to
       the previous one. */
    en_rds_init(&rp_model.rds, rp_model.region ? rp_model.region->rbds : true);
    return true;
}

bool rp_act_seek_quiet(bool up)
{
    return seek_and_adopt(up);
}

void rp_act_stereo_mode(uint8_t mode)
{
    rp_model.stereo_mode = mode;
    settings_save();

    if (!rp_model.tuner_ok || !rp_model.can_raw) return;

    uint8_t ctrl = 0;
    if (en_tuner_reg_read(EN_FM_CTRL_ADDR, &ctrl, 1) != EN_TUNER_OK) return;
    ctrl = en_fm_ctrl_set_stereo(ctrl, (en_fm_stereo_t)mode);
    en_tuner_reg_write(EN_FM_CTRL_ADDR, &ctrl, 1);
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
            snprintf(e.name, sizeof e.name, "%s", rp_model.rds.ps);
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

void rp_act_seek(bool up)
{
    /*
     * A seek the listener asked for, so its result is theirs: remembered,
     * and treated as a new home for the follower. Neither happened before -
     * the frequency a seek landed on was never written to the settings, so a
     * radio restarted after seeking came back where it had been before.
     */
    if (seek_and_adopt(up)) {
        en_af_retuned(&rp_model.af, rp_model.khz);
        settings_save();
    } else {
        /* No hardware seek, or it refused. Stepping is the honest fallback -
           silently doing nothing is what this used to do when it failed. */
        rp_act_step(up);
    }
}

void rp_act_power(bool on)
{
    rp_model.powered = on;
    if (rp_model.tuner_ok) en_tuner_power(on);
}

/*
 * One bit in the chip, two reasons to set it.
 *
 * The flags are updated whether or not the hardware takes it, so the UI shows
 * what was asked for on a card that has no such control. That is the honest
 * way round: the alternative is a mute button that silently does not latch.
 */
static void mute_apply(void)
{
    if (rp_model.mute_ok)
        en_tuner_mute(rp_model.muted || rp_model.squelched);
}

const char *rp_act_set_clock(int64_t utc)
{
    return en_clock_strerror(en_clock_set_unix(utc));
}

int64_t     rp_model_now(void)           { return en_clock_now(); }
const char *rp_model_clock_backend(void) { return en_clock_backend(); }

void rp_act_mute(bool on)
{
    rp_model.muted = on;
    mute_apply();
}

/* ---- where the audio goes, and how loud ---------------------------------- */

uint8_t rp_out_count(void) { return en_play_out_count(); }

const char *rp_out_label(uint8_t i)
{
    const en_play_out_t *o = en_play_out(i);
    return o ? o->label : "";
}

bool rp_out_ready(uint8_t i) { return en_play_out_ready(i); }

const char *rp_act_set_output(uint8_t i)
{
    const char *why;

    /* Whatever the settings wanted, the user has now said otherwise. */
    s_out_restored = true;

    why = en_play_set_output(i);
    if (why) return why;

    rp_model.output = i;

    /* The new output has its own volume control, which may be at a different
       level or may not exist yet - so the level the user is looking at is
       pushed onto it rather than read from it. Arriving at a different volume
       because the destination changed would be a surprise, and on this
       hardware it would be a loud one. */
    volume_apply();
    settings_save();
    return NULL;
}

void rp_act_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    rp_model.volume = percent;
    volume_apply();

    /* Deferred: see s_save_due. */
    s_save_due = now_ms() + SAVE_DELAY_MS;
    if (!s_save_due) s_save_due = 1;      /* 0 means "nothing pending" */
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
    if (level > EN_MIX_CODEC_MAX) level = EN_MIX_CODEC_MAX;
    rp_model.hp_level = level;

    if (en_mix_present(EN_MIX_VOL_CODEC)) {
        rp_model.hp_level_ok = true;
        en_mix_set_raw(EN_MIX_VOL_CODEC, level);
    }
    settings_save();
}

void rp_act_squelch(bool on)
{
    rp_model.squelched = on;
    mute_apply();
}

void rp_act_record_toggle(void)
{
    if (!rp_model.capture_ok) return;

    if (rp_model.recording) {
        en_cap_record_stop();
        sidecar_close(rp_model.rec_ms);
        library_scan();
        return;
    }

    /* Name it so it is identifiable a week later: when, what, and where on the
       dial. A directory of rec001.wav is not a library. */
    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);

    char station[24];
    snprintf(station, sizeof station, "%s",
             rp_model.rds.ps_valid ? rp_model.rds.ps : "FM");
    for (char *p = station; *p; p++)
        if (*p == '/' || *p == ' ') *p = '-';

    snprintf(s_rec_stem, sizeof s_rec_stem,
             "%s/%04d-%02d-%02d %02d-%02d %s %u.%u",
             s_rec_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, station,
             rp_model.khz / 1000u, (rp_model.khz % 1000u) / 100u);

    char wav[600];
    snprintf(wav, sizeof wav, "%s.wav", s_rec_stem);

    /* Ten seconds of prefill, so pressing record keeps what was just said as
       well as what comes next - which is the only useful moment to press it. */
    if (en_cap_record_start(wav, 10000) == EN_CAP_OK) {
        s_rec_started_ms = now_ms();
        sidecar_open();
    }
}

void rp_act_save_live(uint32_t ms)
{
    if (!rp_model.capture_ok) return;

    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);

    char path[600];
    snprintf(path, sizeof path, "%s/%04d-%02d-%02d %02d-%02d live %u.%u.wav",
             s_rec_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min,
             rp_model.khz / 1000u, (rp_model.khz % 1000u) / 100u);

    if (en_cap_save_live(path, ms) == EN_CAP_OK) library_scan();
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
        snprintf(e.name, sizeof e.name, "%s", rp_model.rds.ps);

    if (en_preset_find(&rp_model.presets, e.khz) >= 0)
        en_preset_remove(&rp_model.presets, e.khz);
    else
        en_preset_add(&rp_model.presets, &e);

    en_preset_sort(&rp_model.presets);
    presets_save();
}

/* ---- playback and the live buffer ----------------------------------------- */

void rp_act_play_file(const char *name)
{
    if (!name || !*name) return;

    char path[600];
    snprintf(path, sizeof path, "%s/%s", s_rec_dir, name);
    en_play_file(path);
}

void rp_act_play_live(void)
{
    /* One button, one meaning, whatever is playing: back to the live edge of
       the radio. From a recording that also closes the file. */
    en_play_go_live();
}

void rp_act_nudge(int32_t ms)
{
    en_play_nudge(ms);
}

void rp_act_pause_toggle(void)
{
    en_play_state_t ps;
    en_play_state(&ps);
    en_play_pause(!ps.paused);
}

void rp_act_ta_record(bool on)
{
    rp_model.ta_record = on;
    s_settings.ta_record = on;
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

    bool want = !rp_model.presets.list[at].simple;
    if (!en_preset_set_simple(&rp_model.presets, khz, want)) return false;

    presets_save();
    return true;
}

/* ---- the register explorer ------------------------------------------------ */

bool rp_act_reg_read(uint8_t addr, uint8_t *buf, uint8_t len)
{
    if (!rp_model.tuner_ok || !buf || !len) return false;
    return en_tuner_reg_read(addr, buf, len) == EN_TUNER_OK;
}

void rp_act_reg_write(uint8_t addr, const uint8_t *buf, uint8_t len)
{
    if (!buf || !len) return;

    if (rp_model.tuner_ok) en_tuner_reg_write(addr, buf, len);

    /* Remembered even if the write failed. The user asked for this value, and
       a transport that is down now may be up on the next run - dropping the
       intent because of a transient failure would be the wrong call. */
    en_override_set(&s_settings.overrides, addr, buf, len);
    settings_save();
}

void rp_act_reg_revert(uint8_t addr)
{
    en_override_clear(&s_settings.overrides, addr);
    settings_save();

    /* The chip is not put back here. There is no record of what it held before
       the first override - the only honest reset is a power cycle, and saying
       so is better than writing a zero and calling it the default. */
}

bool rp_act_reg_overridden(uint8_t addr)
{
    return en_override_find(&s_settings.overrides, addr) != 0;
}

void rp_act_set_region(const en_region_t *rg)
{
    if (!rg) return;

    rp_model.region = rg;
    rp_model.rds.rbds = rg->rbds;
    snprintf(rp_model.presets.region, sizeof rp_model.presets.region,
             "%s", rg->name);

    if (rp_model.tuner_ok) en_tuner_set_region(rg);

    /* A frequency that was on the old grid may not be on the new one. */
    if (!en_region_on_grid(rg, rp_model.khz))
        rp_act_tune(en_region_step(rg, rp_model.khz, true));

    presets_save();
    settings_save();
}
