/*
 * mixer_linux.c — mixer.h through alsa-lib's control interface.
 *
 * Two choices in here are worth stating, because the obvious alternatives are
 * both wrong on this card.
 *
 * The control interface rather than the simple mixer. snd_mixer_* is the
 * "simple element" abstraction, and it only exposes controls whose names it
 * can parse - it strips a trailing " Playback Volume" or " Capture Switch" and
 * ignores anything left over. "FM Tuner Mute" ends in neither, so the simple
 * mixer does not expose it at all, and the app's mute button would be missing
 * on a card that publishes exactly the control it needs. snd_ctl_* asks for a
 * control by the name it actually has, which is also what tinymix does and
 * what every note about this hardware quotes.
 *
 * Nothing is cached but the connection. A softvol control does not exist until
 * something has opened its PCM, so "Bluetooth Soft Volume" is genuinely absent
 * on a machine where nothing has yet written to Bluetooth, and present a
 * moment later with nothing having restarted. A lookup is one ioctl; a cached
 * "no" is a control that never comes back.
 */

#include "mixer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

/*
 * The names, in the order of en_mix_t. Written out rather than derived,
 * because these are strings chosen by the machine driver and by
 * /etc/asound.conf and there is no rule connecting them to anything here.
 */
static const char *const k_name[EN_MIX_N] = {
    "Master Soft Volume",
    "Headphone Soft Volume",
    "Bluetooth Soft Volume",
    "FM Capture Volume",
    "Headphones Playback Volume",
    "Headphones Playback Switch",
    "FM Tuner Mute",
};

static snd_ctl_t *s_ctl;
static char       s_desc[96];
static char       s_alsa_err[160];

/*
 * alsa-lib's diagnostics, captured rather than printed.
 *
 * The signature is fixed by snd_lib_error_set_handler. `file` and `line` are
 * alsa-lib's own source position and are no use to anybody here; the function
 * name and the message are, and so is the errno it was reporting.
 */
static void alsa_error(const char *file, int line, const char *fn, int err,
                       const char *fmt, ...)
{
    char msg[112];
    va_list ap;

    (void)file;
    (void)line;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (err)
        snprintf(s_alsa_err, sizeof s_alsa_err, "%s: %s (%s)",
                 fn ? fn : "alsa", msg, snd_strerror(err));
    else
        snprintf(s_alsa_err, sizeof s_alsa_err, "%s: %s", fn ? fn : "alsa", msg);
}

void en_alsa_quiet(void)
{
    snd_lib_error_set_handler(alsa_error);
}

const char *en_alsa_last_error(void)
{
    return s_alsa_err[0] ? s_alsa_err : "";
}

/* The card, as a control name. Overridable for a machine where the audio card
   is not the first one, which costs nothing here and saves a rebuild there. */
static const char *card_name(void)
{
    const char *e = getenv("RADIOPLUS_CARD");
    return (e && *e) ? e : "hw:0";
}

bool en_mix_open(void)
{
    int err;

    if (s_ctl)
        return true;

    err = snd_ctl_open(&s_ctl, card_name(), 0);
    if (err < 0) {
        s_ctl = NULL;
        snprintf(s_desc, sizeof s_desc, "%s unavailable: %s",
                 card_name(), snd_strerror(err));
        return false;
    }
    snprintf(s_desc, sizeof s_desc, "alsa-lib %s", card_name());
    return true;
}

void en_mix_close(void)
{
    if (s_ctl) {
        snd_ctl_close(s_ctl);
        s_ctl = NULL;
    }
}

/* A card that has gone away is dropped rather than retried through a dead
   handle: the next call reopens, which is what happens when the sound modules
   are reloaded underneath a running app. */
static void drop_if_gone(int err)
{
    if (err == -ENODEV || err == -ENXIO || err == -EBADFD)
        en_mix_close();
}

/*
 * Resolve one control. `id` and `info` are the caller's, so nothing is held
 * across calls and there is nothing to invalidate.
 */
static bool elem(en_mix_t c, snd_ctl_elem_id_t *id, snd_ctl_elem_info_t *info)
{
    int err;

    if ((unsigned)c >= (unsigned)EN_MIX_N)
        return false;
    if (!en_mix_open())
        return false;

    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, k_name[c]);
    snd_ctl_elem_info_set_id(info, id);

    err = snd_ctl_elem_info(s_ctl, info);
    if (err < 0) {
        drop_if_gone(err);
        return false;
    }

    /* Take the resolved id back out: it now carries the numid the kernel
       assigned, which is what a read or a write has to be addressed with. */
    snd_ctl_elem_info_get_id(info, id);
    return true;
}

bool en_mix_present(en_mix_t c)
{
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    return elem(c, id, info);
}

/* ---- integers ------------------------------------------------------------ */

int en_mix_get(en_mix_t c)
{
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *val;
    long min, max, v;
    int err;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    if (!elem(c, id, info))
        return -1;
    if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_INTEGER)
        return -1;

    min = snd_ctl_elem_info_get_min(info);
    max = snd_ctl_elem_info_get_max(info);
    if (max <= min)
        return -1;

    snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_value_set_id(val, id);
    err = snd_ctl_elem_read(s_ctl, val);
    if (err < 0) {
        drop_if_gone(err);
        return -1;
    }

    /* Channel zero. These controls are stereo pairs moved together; reporting
       one side is honest as long as nothing here ever sets them apart, and
       nothing does. */
    v = snd_ctl_elem_value_get_integer(val, 0);
    if (v < min) v = min;
    if (v > max) v = max;

    /* Rounded rather than truncated, so a value set through this comes back as
       the same percentage it went in as. */
    return (int)(((v - min) * 100 + (max - min) / 2) / (max - min));
}

bool en_mix_set(en_mix_t c, int percent)
{
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *val;
    long min, max, raw;
    unsigned int n, i;
    int err;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    if (!elem(c, id, info))
        return false;
    if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_INTEGER)
        return false;

    min = snd_ctl_elem_info_get_min(info);
    max = snd_ctl_elem_info_get_max(info);
    if (max <= min)
        return false;

    raw = min + ((max - min) * percent + 50) / 100;

    n = snd_ctl_elem_info_get_count(info);
    if (!n) n = 1;

    snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_value_set_id(val, id);
    /* Every channel, not just the first: a stereo control with one side left
       where it was is a balance the user did not ask for. */
    for (i = 0; i < n; i++)
        snd_ctl_elem_value_set_integer(val, i, raw);

    err = snd_ctl_elem_write(s_ctl, val);
    if (err < 0) {
        drop_if_gone(err);
        return false;
    }
    return true;
}

int en_mix_get_raw(en_mix_t c)
{
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *val;
    int err;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    if (!elem(c, id, info))
        return -1;
    if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_INTEGER)
        return -1;

    snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_value_set_id(val, id);
    err = snd_ctl_elem_read(s_ctl, val);
    if (err < 0) {
        drop_if_gone(err);
        return -1;
    }
    return (int)snd_ctl_elem_value_get_integer(val, 0);
}

bool en_mix_set_raw(en_mix_t c, int value)
{
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *val;
    long min, max;
    unsigned int n, i;
    int err;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    if (!elem(c, id, info))
        return false;
    if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_INTEGER)
        return false;

    /* Clamped to the control's own range rather than refused. A caller asking
       for more than the hardware has wants the loudest it can give, and the
       range is the control's business to know. */
    min = snd_ctl_elem_info_get_min(info);
    max = snd_ctl_elem_info_get_max(info);
    if (value < min) value = (int)min;
    if (value > max) value = (int)max;

    n = snd_ctl_elem_info_get_count(info);
    if (!n) n = 1;

    snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_value_set_id(val, id);
    for (i = 0; i < n; i++)
        snd_ctl_elem_value_set_integer(val, i, value);

    err = snd_ctl_elem_write(s_ctl, val);
    if (err < 0) {
        drop_if_gone(err);
        return false;
    }
    return true;
}

/* ---- booleans ------------------------------------------------------------ */

int en_mix_get_bool(en_mix_t c)
{
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *val;
    int err;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    if (!elem(c, id, info))
        return -1;
    if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_BOOLEAN)
        return -1;

    snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_value_set_id(val, id);
    err = snd_ctl_elem_read(s_ctl, val);
    if (err < 0) {
        drop_if_gone(err);
        return -1;
    }
    return snd_ctl_elem_value_get_boolean(val, 0) ? 1 : 0;
}

bool en_mix_set_bool(en_mix_t c, bool on)
{
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *val;
    unsigned int n, i;
    int err;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    if (!elem(c, id, info))
        return false;
    if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_BOOLEAN)
        return false;

    n = snd_ctl_elem_info_get_count(info);
    if (!n) n = 1;

    snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_value_set_id(val, id);
    for (i = 0; i < n; i++)
        snd_ctl_elem_value_set_boolean(val, i, on ? 1 : 0);

    err = snd_ctl_elem_write(s_ctl, val);
    if (err < 0) {
        drop_if_gone(err);
        return false;
    }
    return true;
}

/* ---- the state everything else assumes ----------------------------------- */

bool en_mix_defaults(void)
{
    bool all = true;

    /*
     * The codec first, and only if it is still where it powers up.
     *
     * This is a calibration, not a preference: 88 clips and 75 does not, so
     * the app puts it somewhere safe on a card it has not seen before. But it
     * must not overwrite a level somebody set on purpose - either through this
     * app's own advanced screen or with tinymix from a serial console - so a
     * value that is already below the safe point is left alone. Only a card
     * sitting at the top of its range, which is where the driver leaves it, is
     * brought down.
     */
    if (en_mix_present(EN_MIX_VOL_CODEC)) {
        int now = en_mix_get_raw(EN_MIX_VOL_CODEC);

        if (now < 0 || now > EN_MIX_CODEC_SAFE)
            en_mix_set_raw(EN_MIX_VOL_CODEC, EN_MIX_CODEC_SAFE);
    } else {
        all = false;
    }

    /* The switch has to be on or the analog path is muted whatever else is
       set, and it is the one control here whose wrong state looks exactly like
       a dead audio path. */
    if (en_mix_present(EN_MIX_SW_CODEC))
        en_mix_set_bool(EN_MIX_SW_CODEC, true);
    else
        all = false;

    /*
     * The tuner unmuted.
     *
     * Deliberately not conditional on anything: MANUAL_MUTE survives a power
     * cycle of the app but not of the chip, so it can be left set by a
     * previous run that was killed mid-sweep, and a radio that starts silent
     * for a reason nobody can see is the worst failure this file can cause.
     * The app's own mute is applied afterwards, from the model.
     */
    if (en_mix_present(EN_MIX_SW_FM_MUTE))
        en_mix_set_bool(EN_MIX_SW_FM_MUTE, false);
    else
        all = false;

    /*
     * The source at full.
     *
     * FM Capture Volume attenuates before anything downstream sees it, so
     * anything taken off here cannot be recovered by an output volume and is
     * lost from the recording as well. The place to be quiet is the output.
     */
    if (en_mix_present(EN_MIX_VOL_FM))
        en_mix_set(EN_MIX_VOL_FM, 100);
    else
        all = false;

    /* The output softvols are the user's volume and are restored from the
       settings by the model, not here - this only reports whether they have
       appeared yet. */
    if (!en_mix_present(EN_MIX_VOL_HP))
        all = false;

    return all;
}

const char *en_mix_backend(void)
{
    return s_desc[0] ? s_desc : "alsa-lib (card not opened)";
}
