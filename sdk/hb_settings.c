/*
 * hb_settings.c — read/write the OS preference store.
 *
 * Complete catalog of keys present in the 1.1.2 memory dump (64 total) +
 * inferred value formats. Named constants below are stable strings
 * (the OS reads its own keys from these exact addresses).
 *
 * Namespaces in use:
 *   General.*    16 keys — system-wide UI + backlight + locale
 *   Playback.*   16 keys — audio playback + EQ + shake + volume
 *   Temp.*       1 key   — temporary BacklightTimer override
 *   Photos.*     5 keys  — slideshow settings
 *   Trainer.*    25 keys — Nike+ workout settings
 *   Bluetooth.*  1 key   — Bluetooth.Enabled (0/1)
 *   Settings.*   1 key   — software version (read-only)
 */

#include "hb_sdk.h"

#define FN_THUMB(addr) ((addr) | 1u)

typedef void *(*prefs_get_t)(void);

#define ADDR_PREFS_INSTANCE 0x0841cfacu

static inline void *prefs_instance(void) {
    return ((prefs_get_t)FN_THUMB(ADDR_PREFS_INSTANCE))();
}

/* Read an integer setting. Returns the value, or `default_v` if the
   key is not set in the store. -3 appears to be unset. */
int32_t hb_settings_get_int(const char *key, int32_t default_v)
{
    void *prefs = prefs_instance();
    if (!prefs) return default_v;
    int32_t out = -3;
    /* vtbl+0x44 = GetInteger(this, key, &out) */
    void **vtbl = *(void ***)prefs;
    typedef void (*get_t)(void *this_, const char *key, int32_t *out);
    ((get_t)vtbl[0x44 / 4])(prefs, key, &out);
    if (out == -3) return default_v;
    return out;
}

/* Write an integer setting. Persists immediately (the OS calls the
   same vtable slot with no flush step). */
void hb_settings_set_int(const char *key, int32_t value)
{
    void *prefs = prefs_instance();
    if (!prefs) return;
    void **vtbl = *(void ***)prefs;
    typedef void (*set_t)(void *this_, const char *key, int32_t value);
    ((set_t)vtbl[0x34 / 4])(prefs, key, value);
}

/* Write a string setting. value must be a null-terminated UTF-8
   string. The third inline-arg 0 mirrors the OS callers (probably
   flags/notify). */
void hb_settings_set_str(const char *key, const char *value)
{
    void *prefs = prefs_instance();
    if (!prefs) return;
    void **vtbl = *(void ***)prefs;
    typedef void (*setstr_t)(void *this_, const char *key,
                             const char *value, int flag);
    ((setstr_t)vtbl[0x5c / 4])(prefs, key, value, 0);
}

/* Convenience: read current brightness. */
int32_t hb_settings_get_brightness(void)
{
    return hb_settings_get_int("General.Brightness", 2);
}

void hb_settings_set_brightness(int32_t level)
{
    hb_settings_set_int("General.Brightness", level);
}

/* Convenience: backlight timer in seconds. 0 = always on. */
int32_t hb_settings_get_backlight_timer(void)
{
    return hb_settings_get_int("General.BacklightTimer", 15);
}

void hb_settings_set_backlight_timer(int32_t seconds)
{
    hb_settings_set_int("General.BacklightTimer", seconds);
}

/* ----- Full key catalog -----
 *
 * Use these as the `key` argument to hb_settings_get_int/set_int/
 * set_str. All keys live in the same store. Types inferred from 
 * observed read/write patterns in the memory dump via SCSI.
 *
 * Naming: HB_PREF_<Namespace>_<Key>. The string after `=` is the
 * literal key value the OS uses. */

/* ----- General.* ----- */
const char *const HB_PREF_GENERAL_BACKLIGHT_TIMER     = "General.BacklightTimer";      /* int sec, 0=always-on */
const char *const HB_PREF_GENERAL_BRIGHTNESS          = "General.Brightness";          /* int 1..3 */
const char *const HB_PREF_GENERAL_CLICKER             = "General.Clicker";             /* int bitfield: 1=speaker, 2=headphones */
const char *const HB_PREF_GENERAL_FONT_SIZE           = "General.FontSize";            /* int 0=small, 1=med, 2=large */
const char *const HB_PREF_GENERAL_HOME_SCREEN         = "General.HomeScreen";          /* string — which screen to wake to */
const char *const HB_PREF_GENERAL_LANGUAGE            = "General.Language";            /* string locale id e.g. "en_US" */
const char *const HB_PREF_GENERAL_MUSIC_MENU          = "General.MusicMenu";           /* int bitfield: which items show in Music menu */
const char *const HB_PREF_GENERAL_PREVIEW_PANEL       = "General.PreviewPanel";        /* int bool */
const char *const HB_PREF_GENERAL_RADIO_LIVE_PAUSE    = "General.RadioLivePause";      /* int bool */
const char *const HB_PREF_GENERAL_REQUIRE_PASSCODE    = "General.RequirePasscodeLock"; /* int bool */
const char *const HB_PREF_GENERAL_ROTATE              = "General.Rotate";              /* int bool — manual rotate enabled */
const char *const HB_PREF_GENERAL_SLEEPWAKE_BUTTON    = "General.SleepWakeButton";     /* int enum (button mode) */
const char *const HB_PREF_GENERAL_SLEEPWAKE_BTN_ENAB  = "General.SleepWakeButtonEnabled"; /* int bool */
const char *const HB_PREF_GENERAL_SORT_CONTACTS       = "General.SortContacts";        /* int 0=first, 1=last */
const char *const HB_PREF_GENERAL_UI_ROTATION         = "General.UIRotation";          /* int 0/90/180/270 deg */
const char *const HB_PREF_GENERAL_WALLPAPER           = "General.Wallpaper";           /* string path to image */

/* ----- Temp.* (overrides cleared on reboot) ----- */
const char *const HB_PREF_TEMP_BACKLIGHT_TIMER        = "Temp.BacklightTimer";         /* int sec, supersedes General if set */

/* ----- Playback.* ----- */
const char *const HB_PREF_PLAYBACK_AIRPLANE_MODE      = "Playback.AirplaneMode";       /* int bool */
const char *const HB_PREF_PLAYBACK_AUDIOBOOK_SPEED    = "Playback.AudiobookSpeed";     /* int -1=slow, 0=normal, 1=fast */
const char *const HB_PREF_PLAYBACK_COMPILATIONS       = "Playback.Compilations";       /* int bool */
const char *const HB_PREF_PLAYBACK_CROSSFADE          = "Playback.Crossfade";          /* int bool */
const char *const HB_PREF_PLAYBACK_EQ                 = "Playback.EQ";                 /* int EQ preset index */
const char *const HB_PREF_PLAYBACK_ENERGY_SAVER       = "Playback.EnergySaver";        /* int bool */
const char *const HB_PREF_PLAYBACK_HEADSET_EQ         = "Playback.HeadsetEQ";          /* int EQ preset for headset */
const char *const HB_PREF_PLAYBACK_REPEAT             = "Playback.Repeat";             /* int 0=off, 1=one, 2=all */
const char *const HB_PREF_PLAYBACK_SHAKE              = "Playback.Shake";              /* int 0=off, 1=shuffle */
const char *const HB_PREF_PLAYBACK_SHUFFLE            = "Playback.Shuffle";            /* int 0=off, 1=songs, 2=albums */
const char *const HB_PREF_PLAYBACK_SOUND_CHECK        = "Playback.SoundCheck";         /* int bool */
const char *const HB_PREF_PLAYBACK_USE_REC_VOL_RANGE  = "Playback.UseRecommendedVolumeRange"; /* int bool */
const char *const HB_PREF_PLAYBACK_VOLUME_LIMIT       = "Playback.VolumeLimit";        /* int 0..0xb700 raw */

/* ----- Photos.* ----- */
const char *const HB_PREF_PHOTOS_MUSIC                = "Photos.Music";                /* string playlist id */
const char *const HB_PREF_PHOTOS_REPEAT               = "Photos.Repeat";               /* int bool */
const char *const HB_PREF_PHOTOS_SHUFFLE              = "Photos.Shuffle";              /* int bool */
const char *const HB_PREF_PHOTOS_TIME_PER_SLIDE       = "Photos.TimePerSlide";         /* int seconds */
const char *const HB_PREF_PHOTOS_TRANSITIONS          = "Photos.Transitions";          /* int enum (cross-fade/etc) */

/* ----- Bluetooth.* + Settings.* ----- */
const char *const HB_PREF_BLUETOOTH_ENABLED           = "Bluetooth.Enabled";           /* int bool */
const char *const HB_PREF_SETTINGS_SOFTWARE_VERSION   = "Settings.SoftwareVersion";    /* string (READ-ONLY) */

/* ----- Trainer.* (Nike+ fitness) — abbreviated; 25 keys total ----- */
const char *const HB_PREF_TRAINER_GENDER              = "Trainer.Gender";              /* int 0=male, 1=female */
const char *const HB_PREF_TRAINER_HEIGHT              = "Trainer.Height";              /* double cm (8B) */
const char *const HB_PREF_TRAINER_WEIGHT              = "Trainer.Weight";              /* double kg (8B) */
const char *const HB_PREF_TRAINER_BODY_LOCATION       = "Trainer.BodyLocation";        /* int enum */
const char *const HB_PREF_TRAINER_DISTANCE_UNITS      = "Trainer.DistanceUnits";       /* int 0=mi, 1=km */
const char *const HB_PREF_TRAINER_MEASUREMENT_SYSTEM  = "Trainer.MeasurementSystem";   /* int 0=US, 1=metric */
const char *const HB_PREF_TRAINER_PEDOMETER_STEP_GOAL = "Trainer.PedometerStepGoal";   /* int steps/day */
const char *const HB_PREF_TRAINER_WEIGHT_UNITS        = "Trainer.WeightUnits";         /* int 0=lb, 1=kg */
const char *const HB_PREF_TRAINER_POWER_SONG          = "Trainer.PowerSong";           /* string playlist id */
const char *const HB_PREF_TRAINER_VOICE_FEEDBACK      = "Trainer.VoiceFeedback";       /* int bool */
