/*
 * mixer.h — the card's controls, by name.
 *
 * Level on this device is not one control, it is five, and which one matters
 * depends on where the audio is going. That is a consequence of how the audio
 * graph is built rather than an accident:
 *
 *   The CS42L81 has a real analog playback volume, so the headphone leg has
 *   one register that genuinely attenuates.
 *
 *   Nothing else does. The tuner has no volume anywhere in the recovered
 *   FM_RDS_Command map, and the Bluetooth leg is an SBC encoder in userspace
 *   with no gain stage at all. /etc/asound.conf supplies both with softvol,
 *   which applies the gain in the plugin and registers a genuine mixer control
 *   for it - so "Bluetooth Soft Volume" is a control on card 0 that behaves
 *   like any other, without a register existing anywhere behind it.
 *
 * So there is one softvol per output, and the one to move is the one belonging
 * to the device currently open. The codec's own volume sits underneath the
 * headphone leg and is a calibration rather than a knob: it is what decides
 * whether the output clips, and it is set once at start-up.
 *
 * Everything is looked up BY NAME. The numeric index of a control shifts as
 * controls are added to the machine driver or as softvol registers another
 * one, so an index cached at start-up is a control that silently becomes a
 * different control later.
 *
 * A control that is not there is not an error. softvol registers its control
 * when its PCM is first opened, so on a machine where nothing has opened
 * n31bt yet, "Bluetooth Soft Volume" genuinely does not exist - and it will,
 * later, without anything restarting. Callers ask again rather than latching
 * the first answer.
 */

#ifndef RADIOPLUS_MIXER_H
#define RADIOPLUS_MIXER_H

#include <stdbool.h>

typedef enum {
    /* The softvols from /etc/asound.conf, one per output device. */
    EN_MIX_VOL_MASTER = 0,   /* "Master Soft Volume"          n31both  */
    EN_MIX_VOL_HP,           /* "Headphone Soft Volume"       n31hp    */
    EN_MIX_VOL_BT,           /* "Bluetooth Soft Volume"       n31bt    */
    EN_MIX_VOL_FM,           /* "FM Capture Volume"           n31fm    */

    /* The codec's own, which is the one that can clip. */
    EN_MIX_VOL_CODEC,        /* "Headphones Playback Volume"  0..88    */
    EN_MIX_SW_CODEC,         /* "Headphones Playback Switch"           */

    /* The tuner's MANUAL_MUTE, published by the machine driver. */
    EN_MIX_SW_FM_MUTE,       /* "FM Tuner Mute"                        */

    EN_MIX_N
} en_mix_t;

/*
 * Open the card. Safe to call repeatedly and meant to be: it returns false
 * while the card is absent and true once it is there, so a caller polling
 * during bring-up gets a true answer each time rather than a latched one.
 */
bool en_mix_open(void);
void en_mix_close(void);

/* Whether the control exists right now. */
bool en_mix_present(en_mix_t c);

/*
 * Volume as a percentage of the control's own range, which is read from the
 * control rather than assumed - the softvols run 0..100 and the codec 0..88,
 * and a caller that had to know which is which would have to be corrected
 * every time a range changed.
 *
 * Returns -1 when the control is not there, which is not the same as zero and
 * must not be drawn as though it were.
 */
int  en_mix_get(en_mix_t c);
bool en_mix_set(en_mix_t c, int percent);

/*
 * The same controls in their own units.
 *
 * Percentages are right for a volume somebody drags, and wrong for the codec:
 * its range is 0..88, so a value that goes out as a percentage and comes back
 * as one lands a step or two from where it started, and a level that drifts
 * every time it is read is not a calibration. Anything with a range that is
 * not a hundred should be addressed in its own numbers.
 *
 * en_mix_get_raw returns -1 when the control is not there.
 */
int  en_mix_get_raw(en_mix_t c);
bool en_mix_set_raw(en_mix_t c, int value);

/* Booleans. en_mix_get_bool returns 1, 0, or negative for "cannot ask". */
int  en_mix_get_bool(en_mix_t c);
bool en_mix_set_bool(en_mix_t c, bool on);

/*
 * The state everything else assumes: the codec switched on, the tuner
 * unmuted, the softvols at full, and the codec's own volume just below where
 * it starts to clip.
 *
 * Applied to whichever of those controls exist, and reapplied cheaply, so a
 * control that appears later still gets set. Returns true when every control
 * it wanted was present, which is how the caller knows it can stop asking.
 */
bool en_mix_defaults(void);

/*
 * Where the codec is set, and where it must not be.
 *
 * Measured against RetailOS at full output, which reads about -12 dBFS on the
 * reference rig. 75 of 88 with the softvols at 100 gives roughly -20 dBFS RMS
 * and -9 dBFS peak on FM, which is the right neighbourhood. 88 clips outright:
 * the peak pins at 0.0 dBFS with a flat crest factor and it is audible as
 * fuzz, so the top of the codec's range is not somewhere to default to and is
 * not somewhere the UI should let anyone wander into by accident.
 */
#define EN_MIX_CODEC_SAFE 75
#define EN_MIX_CODEC_MAX  88

/*
 * Take alsa-lib's own diagnostics off stderr, and keep the last one.
 *
 * alsa-lib reports through a global handler that writes to stderr by default,
 * and it is chatty about a device it cannot resolve - several lines per failed
 * open. That is fine for a command line tool and wrong here twice over: this
 * app owns the framebuffer while it runs and the console may be pointed at the
 * same screen, and the retry loops mean a device that stays missing produces
 * those lines for as long as the app is running.
 *
 * snd_lib_error_set_handler is the interface for exactly this. Nothing is
 * thrown away - the last message is kept and is worth showing on the settings
 * screen, where "n31fm: Invalid argument" is the difference between a card
 * that is missing and an /etc/asound.conf that is.
 *
 * Call once, before anything else here opens anything.
 */
void        en_alsa_quiet(void);
const char *en_alsa_last_error(void);

const char *en_mix_backend(void);

#endif /* RADIOPLUS_MIXER_H */
