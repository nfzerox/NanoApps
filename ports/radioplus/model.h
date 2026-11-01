/*
 * model.h — everything the UI draws, in one place.
 *
 * The screens read this and nothing else. That keeps the drawing free of any
 * knowledge of sysfs, HCI or tinyalsa, and it is what lets the whole interface
 * be rendered on a desktop with no tuner: the host preview fills the same
 * struct with a plausible station and every screen believes it.
 */

#ifndef RADIOPLUS_MODEL_H
#define RADIOPLUS_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "core/rds.h"
#include "core/region.h"
#include "core/affollow.h"
#include "core/store.h"

typedef struct {
    /* Tuner */
    uint32_t khz;
    uint8_t  rssi;           /* 0..255 as the chip reports it */
    int8_t   snr;
    bool     stereo;
    /* What we last asked the chip to do about stereo, which is not the same
       question as whether it is currently in stereo. */
    uint8_t  stereo_mode;    /* en_fm_stereo_t */
    bool     powered;
    bool     tuner_ok;
    const char *tuner_note;  /* why not, when tuner_ok is false */

    /*
     * Quiet, and who asked for it.
     *
     * Two things want the tuner muted and they must not undo each other: the
     * user pressing mute, and the band scan squelching a sweep across two
     * hundred channels of noise. Kept as two flags rather than one, because a
     * scan that ends by unmuting has just overridden a decision the user made
     * before it started.
     *
     * The chip is muted when either is set. `muted` is the one to draw.
     */
    bool     muted;          /* the user asked for it */
    bool     squelched;      /* something is sweeping the band */
    bool     mute_ok;        /* the card publishes the control at all */

    /* Region and decoded RDS */
    const en_region_t *region;
    en_rds_t rds;
    bool     rds_on;

    /* Capture, the live buffer and recording */
    bool     capture_ok;
    bool     recording;
    uint32_t rec_ms;
    uint32_t live_ms;        /* how much is buffered */
    uint32_t live_cap_ms;
    uint32_t live_pos_ms;    /* how far behind live we are; 0 is live */
    uint32_t overruns;

    /* Playback. The radio and a recording share one player, so "what is
       coming out of the headphones" is one question with one answer. */
    bool     play_file;          /* a recording rather than the radio */
    char     play_name[96];
    uint32_t play_pos_ms;
    uint32_t play_len_ms;
    bool     play_paused;
    uint32_t behind_ms;          /* how far behind live; 0 is live */
    uint32_t behind_max_ms;      /* how far back the buffer allows */
    bool     play_ok;

    /*
     * Where the audio is going, and how loud.
     *
     * `output` indexes the platform's own list rather than naming a device,
     * because the screens have no business knowing that "n31both" is a tee
     * over a rate converter. rp_out_label() turns it into words.
     *
     * `output_open` is not the same question as `play_ok`: the player can be
     * running with a destination it cannot currently open - Bluetooth with
     * nothing listening is exactly that - and an interface that showed those
     * as one state would claim audio was reaching somewhere it is not.
     *
     * `volume` is a percentage of whichever output's own control, which is a
     * different control per output. That is a property of how the audio graph
     * is built and not a complication invented here: each output has its own
     * softvol, and "Both" has one over the pair.
     */
    uint8_t  output;
    bool     output_open;
    uint8_t  volume;             /* 0..100 on the current output */
    bool     volume_ok;          /* the control exists to move */

    /*
     * The codec's own analog volume, 0..88, which is a calibration and not a
     * knob - the top of its range clips. Shown on the advanced screen only,
     * because somebody who has gone looking for it is the only person who
     * should find it.
     */
    uint8_t  hp_level;
    bool     hp_level_ok;

    bool     ta_record;          /* auto-record traffic announcements */

    /* The recording timer: stop after so long, start at a time of day. Here
       rather than private to the UI because it persists, and persistence is
       the platform's job. */
    en_rectimer_t rectimer;

    /* Following the station across transmitters. The whole state, not just
       the on/off, because the badge shows what it is doing. */
    en_affollow_t af;
    bool     ta_recording;       /* one is being recorded right now */

    /* Which optional screens are in the swipe order. Here rather than private
       to the UI because they persist, and persistence is the platform's job -
       the same reason ta_record lives here and not beside the checkbox. */
    bool     simple_screen;      /* big preset buttons and nothing else */
    bool     wide_screen;        /* the landscape readout */

    /* Presets and recordings */
    en_presets_t presets;
    uint8_t  library_count;
    char     library[12][96];   /* a dated, named recording is ~34 chars */

    bool     can_raw;        /* show the register explorer at all */

    /* Whether the tuner can seek for itself. The band scan asks the chip to
       jump from station to station when it can, and steps every channel in
       software when it cannot - and a seek that never lands would stall the
       sweep on every station, so this has to say what the platform actually
       does rather than what the chip is capable of. */
    bool     can_seek;
    const char *backend;
    const char *capture_backend;

    /*
     * And the same for the output, which had nowhere to say so.
     *
     * The capture row on the settings screen has always carried the reason it
     * could not open - "n31fm unavailable: No such file or directory" is a
     * different fault from "n31fm unavailable: Invalid argument", and one of
     * them is a missing card and the other a missing /etc/asound.conf. The
     * playback side had no such row, so "no audio out" was the whole of what a
     * person could learn about half the audio path.
     */
    const char *play_backend;
} rp_model_t;

extern rp_model_t rp_model;

/* Pull the current state in from whatever platform is underneath. Called from
   the frame callback a few times a second, not per frame - reading RSSI is a
   round trip through a driver and there is nothing to see at 60 Hz. */
void rp_model_refresh(void);

/*
 * Called as each stage of bring-up begins, and again with a reason when one
 * fails. Set before the first rp_model_refresh, which is what does the work.
 *
 * A hook rather than a direct call into the UI because core and platform know
 * nothing about screens - the host preview has no bring-up to report and the
 * device build should not gain a UI dependency to say what it is doing.
 */
typedef struct {
    void (*step)(const char *what);
    void (*failed)(const char *why);
} rp_progress_t;

void rp_model_set_progress(const rp_progress_t *p);

/*
 * What the platform is still waiting for, or NULL when it has stopped waiting.
 *
 * Called from the startup loop while the boot screen is up: as long as this
 * returns a string, show it and keep pumping. It stops returning one when
 * everything has arrived or when the platform has decided it has waited long
 * enough - and in the second case the platform keeps retrying in the
 * background, so a device that turns up late still works without a restart.
 *
 * The string is owned by the platform and is safe to display, not to keep.
 */
const char *rp_model_waiting(void);

/* Actions the UI invokes. Implemented per platform. */
void rp_act_tune(uint32_t khz);

/* Tune without persisting the frequency. The band scan moves two hundred
   times in half a minute and none of those are where the user wants to be
   left; rp_act_tune would write the settings file for every one of them. */
void rp_act_tune_quiet(uint32_t khz);

/* Ask the chip for the next station, without persisting. Returns false when
   the platform has no hardware seek, so the caller can fall back rather than
   wait for something that is never going to happen. */
bool rp_act_seek_quiet(bool up);

/*
 * Poll the tuner harder for a while.
 *
 * The band scan decides whether a seek has landed by watching the frequency,
 * and the ordinary cadence is slower than the scan's own settle time. Asked
 * for by whatever is sweeping and turned off when it stops - it costs a driver
 * round trip several times a second and there is nothing to see at that rate
 * the rest of the time.
 */
void rp_model_set_fast(bool on);

/* Persist the preset list. The scan writes many presets at once, so it does
   not go through rp_act_preset_toggle and has to ask for the save itself. */
void rp_act_presets_save(void);
void rp_act_step(bool up);
void rp_act_seek(bool up);
void rp_act_power(bool on);

/*
 * Mute, and squelch, which are the same bit in the chip and different
 * decisions above it. Both are safe to call when the platform has no mute:
 * they set the flag so the UI stays consistent and do nothing to the hardware.
 *
 * Neither persists. A radio that comes back silent because it was silent last
 * week is a radio nobody can fix.
 */
void rp_act_mute(bool on);
void rp_act_squelch(bool on);

/*
 * Where the audio goes.
 *
 * The list is the platform's, because what a machine can do with sound is not
 * something the screens can work out. rp_out_ready() is asked before offering
 * one: the Bluetooth legs need an encoder running at the far end, and an
 * option that cannot work should be visibly unavailable rather than silently
 * ineffective.
 *
 * rp_act_set_output returns NULL when it worked, or a short phrase saying why
 * not, which the interface shows. A control that refuses without saying why is
 * indistinguishable from one that is broken.
 */
uint8_t     rp_out_count(void);
const char *rp_out_label(uint8_t i);
bool        rp_out_ready(uint8_t i);
const char *rp_act_set_output(uint8_t i);

/*
 * Volume, 0..100, on whichever output is selected.
 *
 * Persisted, unlike mute: coming back at the level you left it is what every
 * other radio does, and unlike silence a level is visible on the screen the
 * moment you look at it.
 */
void rp_act_set_volume(uint8_t percent);
void rp_act_nudge_volume(int delta);

/*
 * The codec's analog volume, 0..88.
 *
 * Separate from the volume above because it is a different kind of thing: it
 * decides whether the output clips rather than how loud it is, it applies to
 * the headphone leg only, and the top of its range is measurably wrong. The
 * app sets it once to a safe value at start-up and this exists so somebody
 * calibrating against a meter can move it.
 */
void rp_act_set_hp_level(uint8_t level);

/*
 * Set the clock, from a time collected off the band.
 *
 * Takes UTC, because that is what a hardware clock holds and what a station
 * transmits; the local offset is a display matter. Returns a short phrase
 * saying what happened, never NULL, because "set" and "set until you reboot"
 * are different outcomes and the difference is the whole point of the
 * feature.
 */
const char *rp_act_set_clock(int64_t utc);

/* Seconds since the epoch, and what hardware clock is behind it. */
int64_t     rp_model_now(void);
const char *rp_model_clock_backend(void);

/* Auto, forced mono, or forced stereo. Forced mono is the useful one: a weak
   station is steadier in mono than blending in and out of it. */
void rp_act_stereo_mode(uint8_t mode);

/* Turn station-following on or off. Off is the default and has to stay one
   tap away: a misconfigured transmitter nearby is exactly the case where you
   want out immediately. */
void rp_act_af_follow(bool on);

/* Save every alternate frequency this station advertises as a preset. The
   station has just told us where else it can be heard; that is a better
   preset list than one built by turning a dial. Returns how many were added
   or updated. */
uint8_t rp_act_af_to_presets(void);
void rp_act_record_toggle(void);
void rp_act_save_live(uint32_t ms);
void rp_act_preset_toggle(void);
void rp_act_set_region(const en_region_t *rg);

/*
 * Raw register access from the explorer.
 *
 * A write is also remembered: the tuner forgets everything when it powers down,
 * so an override that is not replayed at start-up lasts only as long as the
 * chip stays on, and the explorer would be a toy rather than a settings screen.
 * rp_act_reg_revert drops the override and puts the register back to whatever
 * the chip powers up with.
 */
bool rp_act_reg_read(uint8_t addr, uint8_t *buf, uint8_t len);
void rp_act_reg_write(uint8_t addr, const uint8_t *buf, uint8_t len);
void rp_act_reg_revert(uint8_t addr);
bool rp_act_reg_overridden(uint8_t addr);

/* Playback and the live buffer. */
void rp_act_play_file(const char *name);
void rp_act_play_live(void);        /* jump to the front of the buffer */
void rp_act_nudge(int32_t ms);      /* negative goes back */
void rp_act_pause_toggle(void);
void rp_act_ta_record(bool on);

/* Length to stop a recording after, in minutes; 0 runs until stopped. */
void rp_act_set_rec_limit(uint16_t minutes);
/* Minute of the day to start one at, or EN_REC_AT_NONE. */
void rp_act_set_rec_at(int16_t minutes);

/* Turn the optional screens on or off, and remember it. */
void rp_act_show_simple(bool on);
void rp_act_show_wide(bool on);

/* Flag or unflag the tuned station for the simple screen, saving the presets.
   Returns false when the grid is full, which the UI says out loud. */
bool rp_act_simple_toggle(uint32_t khz);

#endif /* RADIOPLUS_MODEL_H */
