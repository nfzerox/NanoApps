/*
 * audio_linux.c — audio.h on the N31 Linux port, streaming through tinyalsa.
 *
 * This is the backend the DSP was written for. RetailOS could only be handed a
 * file, which is where the loop planner, the WAV cache and the 1 MiB ceiling
 * all come from; a real PCM sink has none of those problems. Here the engine
 * generates straight into the period buffer, so a steady preset is one
 * unbroken tone with no loop and no seam at all, and a program's ramp is
 * continuous rather than chunked.
 *
 * tinyalsa rather than alsa-lib on purpose: it is five source files with no
 * dependencies, which is what makes a fully static musl binary practical.
 *
 * The device's audio path is still being brought up, so this backend must not
 * be the reason the app fails to start. If the PCM device will not open it
 * falls back to a null sink that keeps time off the monotonic clock — the UI
 * then behaves exactly as it would with sound, which is what makes it useful
 * for checking the screen while the codec is still being fixed. Pass
 * --null-audio to select that deliberately.
 */

#include "audio.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include <tinyalsa/asoundlib.h>

/* Card 0, device 0 — "nano7gaudio", playback cs42l81-hifi-0. Overridable so a
   different card can be tried without a rebuild. */
#define EN_DEFAULT_CARD   0
#define EN_DEFAULT_DEVICE 0

/* 1024 frames at 44.1 kHz is 23 ms a period.
 *
 * Eight of them rather than four. The codec takes 60 ms to settle a rate
 * change inside its play-start, so a 93 ms buffer leaves under half a period
 * of slack for a restart on a device that is also compositing LVGL; 186 ms
 * leaves enough that one late wake-up is not an underrun. Four is still tried
 * if the driver will not give us eight - see pcm_try_open. */
#define EN_PERIOD_FRAMES 1024
#define EN_PERIOD_COUNT  8

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       s_thread;
static int             s_thread_running;
static int             s_quit;

static struct pcm *s_pcm;
static struct mixer     *s_mixer;
static struct mixer_ctl *s_vol_ctl;
static int               s_mixer_tried;
static int         s_null_sink;      /* forced, or fell back after a failure */
static int         s_card = EN_DEFAULT_CARD;
static int         s_device = EN_DEFAULT_DEVICE;

static en_audio_pull_fn s_pull;
static void            *s_pull_ctx;
static uint32_t         s_rate;
static uint64_t         s_frames_out;
static int              s_paused;
static int              s_volume = 45;
static en_audio_state_t s_state = EN_AUDIO_IDLE;

/* Start/stop/pause ramps. Never begin or end a tone on a step. */
static double s_gain, s_gain_step;
static int    s_stop_when_silent, s_pause_when_silent;

static int16_t s_buf[EN_PERIOD_FRAMES * 2];
static int16_t s_mix[EN_PERIOD_FRAMES * 2];

/* Frames of s_mix the device has not taken yet, counted from its end. A
   period is generated once and then handed over until it is gone; it is never
   dropped because the device was momentarily full. */
static unsigned s_pending;
static unsigned s_periods = EN_PERIOD_COUNT;   /* what the driver actually gave */
static unsigned long s_restarts;               /* stream restarts, for the log */

/* ---- configuration hooks ------------------------------------------------- */

void en_audio_linux_set_device(int card, int device)
{
    s_card = card;
    s_device = device;
}

void en_audio_linux_force_null(int on)
{
    s_null_sink = on ? 1 : 0;
}

/* ---- hardware volume ------------------------------------------------------
 *
 * Scaling the samples in software works, but it throws away bits and it leaves
 * the codec's own gain wherever it happened to be. The card has a real volume
 * control — the same one tinymix drives — so use it, and keep the software
 * gain only for the fade ramps, where it belongs.
 *
 * The control is found by name rather than hardcoded: this codec is a cs42l81
 * but the driver is still being worked on, and a name that is right today may
 * not be tomorrow. Anything that is an integer control with "volume" in its
 * name will do, preferring the ones that sound like a master output. */
static int name_has(const char *hay, const char *needle)
{
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

static void mixer_find_volume(void)
{
    if (s_mixer_tried) return;
    s_mixer_tried = 1;

    s_mixer = mixer_open((unsigned)s_card);
    if (!s_mixer) return;

    unsigned n = mixer_get_num_ctls(s_mixer);
    int best_score = 0;

    for (unsigned i = 0; i < n; i++) {
        struct mixer_ctl *c = mixer_get_ctl(s_mixer, i);
        if (!c) continue;
        if (mixer_ctl_get_type(c) != MIXER_CTL_TYPE_INT) continue;

        const char *nm = mixer_ctl_get_name(c);
        if (!nm || !name_has(nm, "volume")) continue;

        /* Prefer a master/output control over, say, a mic gain that also has
           "volume" in its name. */
        int score = 1;
        if (name_has(nm, "master"))   score += 4;
        if (name_has(nm, "playback")) score += 3;
        if (name_has(nm, "output"))   score += 2;
        if (name_has(nm, "hp") || name_has(nm, "headphone")) score += 2;
        if (name_has(nm, "capture") || name_has(nm, "mic")) score -= 8;

        if (score > best_score) {
            best_score = score;
            s_vol_ctl = c;
        }
    }
}

static void mixer_apply_volume(int percent)
{
    mixer_find_volume();
    if (!s_vol_ctl) return;
    unsigned values = mixer_ctl_get_num_values(s_vol_ctl);
    for (unsigned i = 0; i < values; i++)
        mixer_ctl_set_percent(s_vol_ctl, i, percent);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- the sink ------------------------------------------------------------ */

static int pcm_try_open(uint32_t rate)
{
    /* Biggest buffer first, then down. A driver that will not give us eight
       periods should get four rather than nothing: falling back to the null
       sink means silence, which is a worse answer than a short buffer. */
    static const unsigned COUNTS[] = { EN_PERIOD_COUNT, 4 };

    for (unsigned i = 0; i < sizeof COUNTS / sizeof COUNTS[0]; i++) {
        struct pcm_config cfg;
        struct pcm *p;

        if (i && COUNTS[i] == COUNTS[i - 1])
            continue;

        memset(&cfg, 0, sizeof cfg);
        cfg.channels = 2;
        cfg.rate = rate;
        cfg.period_size = EN_PERIOD_FRAMES;
        cfg.period_count = COUNTS[i];
        cfg.format = PCM_FORMAT_S16_LE;
        /* Zero means "tinyalsa's defaults": start at half the buffer, stop at
           the whole of it. Which is what we want, and saying so by hand only
           risks disagreeing with the buffer we actually got. */
        cfg.start_threshold = 0;
        cfg.stop_threshold = 0;
        cfg.silence_threshold = 0;

        /* Non-blocking on purpose. The writer paces itself against the
           monotonic clock, so it never needs the sink to block — and a
           blocking write into a driver that is still being brought up is a
           good way to end up stuck in an uninterruptible wait that nothing
           can clear. */
        p = pcm_open((unsigned)s_card, (unsigned)s_device,
                     PCM_OUT | PCM_NONBLOCK, &cfg);
        if (p && pcm_is_ready(p)) {
            s_pcm = p;
            s_periods = COUNTS[i];
            return 1;
        }
        if (p)
            pcm_close(p);
    }
    return 0;
}

/* ---- gain ramp ----------------------------------------------------------- */

static void apply_gain(int16_t *dst, const int16_t *src, uint32_t frames)
{
    double vol = (double)s_volume / 100.0;
    for (uint32_t i = 0; i < frames; i++) {
        if (s_gain_step != 0.0) {
            s_gain += s_gain_step;
            if (s_gain <= 0.0) {
                s_gain = 0.0;
                s_gain_step = 0.0;
                if (s_stop_when_silent) {
                    s_pull = NULL;
                    s_state = EN_AUDIO_IDLE;
                    s_stop_when_silent = 0;
                } else if (s_pause_when_silent) {
                    s_paused = 1;
                    s_pause_when_silent = 0;
                }
            } else if (s_gain >= 1.0) {
                s_gain = 1.0;
                s_gain_step = 0.0;
            }
        }
        double g = vol * s_gain;
        dst[2 * i + 0] = (int16_t)((double)src[2 * i + 0] * g);
        dst[2 * i + 1] = (int16_t)((double)src[2 * i + 1] * g);
    }
}

/* Produce one period. Returns frames generated, 0 when the source is done. */
static uint32_t produce(void)
{
    en_audio_pull_fn pull;
    void *ctx;
    int paused;

    pthread_mutex_lock(&s_lock);
    pull = s_pull;
    ctx = s_pull_ctx;
    paused = s_paused;
    pthread_mutex_unlock(&s_lock);

    if (!pull) return 0;

    uint32_t got = 0;
    if (paused) {
        memset(s_buf, 0, sizeof s_buf);
        got = EN_PERIOD_FRAMES;
    } else {
        /* Streams contiguously - no crossfade, so the advance is the
           whole period. */
        got = pull(s_buf, EN_PERIOD_FRAMES, EN_PERIOD_FRAMES, ctx);
        if (got == 0) {
            pthread_mutex_lock(&s_lock);
            s_pull = NULL;
            s_state = EN_AUDIO_IDLE;
            pthread_mutex_unlock(&s_lock);
            return 0;
        }
        if (got < EN_PERIOD_FRAMES)
            memset(s_buf + got * 2, 0,
                   (EN_PERIOD_FRAMES - got) * 2 * sizeof(int16_t));
    }

    pthread_mutex_lock(&s_lock);
    apply_gain(s_mix, s_buf, EN_PERIOD_FRAMES);
    if (!paused) s_frames_out += EN_PERIOD_FRAMES;
    pthread_mutex_unlock(&s_lock);
    return EN_PERIOD_FRAMES;
}

static void *writer(void *arg)
{
    (void)arg;
    uint64_t next_ns = 0;

    while (!s_quit) {
        int have;
        pthread_mutex_lock(&s_lock);
        have = (s_pull != NULL);
        pthread_mutex_unlock(&s_lock);

        if (!have) {
            usleep(10000);
            next_ns = 0;
            continue;
        }

        /* Generate a period only when the last one has been handed over in
           full. What the device would not take is kept and offered again,
           rather than thrown away and replaced with the next period - which
           is a gap in the tone, on an app whose whole job is an unbroken
           one. */
        if (s_pending == 0) {
            if (!produce()) continue;
            s_pending = EN_PERIOD_FRAMES;
        }

        if (s_pcm) {
            const int16_t *from = s_mix + (EN_PERIOD_FRAMES - s_pending) * 2;
            int rc = pcm_writei(s_pcm, from, s_pending);

            if (rc > 0) {
                /* Short writes are legal and were being ignored: the frames
                   the device did not take were dropped along with the rest
                   of the period. */
                s_pending -= (unsigned)rc > s_pending ? s_pending : (unsigned)rc;
            } else if (rc < 0) {
                /*
                 * errno, not the return value.
                 *
                 * tinyalsa returns -1 and sets errno; it does not return
                 * -EAGAIN. So `rc != -EAGAIN` was true for every full
                 * buffer - the normal case whenever we are slightly ahead of
                 * real time - and every one of them went to pcm_prepare(),
                 * which throws away everything the device is holding and
                 * restarts the stream. That is a start/stop loop at close to
                 * period rate, generated here: on this codec each restart
                 * costs a 60 ms rate-change settle, so what comes out is a
                 * fragment of tone, silence, a fragment of tone.
                 *
                 * A full buffer is not an error. Keep the frames, let the
                 * pacing below absorb it, and offer them again. Only a real
                 * fault re-prepares.
                 */
                int e = errno;

                if (e != EAGAIN && e != EINTR) {
                    s_restarts++;
                    if (pcm_prepare(s_pcm) < 0) {
                        pcm_close(s_pcm);
                        s_pcm = NULL;
                        s_null_sink = 1;
                        s_pending = 0;
                    }
                }
            }
        } else {
            s_pending = 0;              /* null sink: nothing to hand over */
        }

        /* Never run ahead of real time.
         *
         * A PCM sink is normally its own clock: pcm_writei blocks until the
         * hardware has room, and that is what paces playback. Measured on this
         * device, with the audio path still being brought up, the write is
         * accepted and returns immediately — about 10x real time, which ran a
         * 45-minute program out in four minutes and made every readout on the
         * screen wrong.
         *
         * So the clock is not delegated to the sink. When the sink does block
         * properly this costs nothing: the deadline is already behind us and
         * the sleep is skipped. */
        uint64_t period_ns = (uint64_t)EN_PERIOD_FRAMES * 1000000000ull
                           / (s_rate ? s_rate : 44100);
        uint64_t now = now_ns();
        if (next_ns == 0 || now > next_ns + 10 * period_ns)
            next_ns = now;              /* first period, or after a long stall */
        next_ns += period_ns;
        if (next_ns > now) usleep((useconds_t)((next_ns - now) / 1000));
    }

    if (s_pcm) { pcm_close(s_pcm); s_pcm = NULL; }
    return NULL;
}

/* ---- the interface ------------------------------------------------------- */

bool en_audio_init(void)
{
    if (s_thread_running) return true;
    s_quit = 0;
    if (pthread_create(&s_thread, NULL, writer, NULL) != 0) return false;
    s_thread_running = 1;
    return true;
}

void en_audio_shutdown(void)
{
    if (!s_thread_running) return;
    s_quit = 1;
    pthread_join(s_thread, NULL);
    s_thread_running = 0;
    s_state = EN_AUDIO_IDLE;
}

uint32_t en_audio_preferred_rate(void) { return 44100u; }   /* what the cs42l81 path is configured for */

bool en_audio_can_stream(void) { return true; }

bool en_audio_start_stream(uint32_t sample_rate, en_audio_pull_fn pull,
                           void *ctx)
{
    if (!pull || !sample_rate) return false;

    pthread_mutex_lock(&s_lock);
    /* Reopen only when the rate actually changes: the codec does not need to
       be torn down every time a preset is picked. */
    if (!s_null_sink && (!s_pcm || sample_rate != s_rate)) {
        if (s_pcm) { pcm_close(s_pcm); s_pcm = NULL; }
        if (!pcm_try_open(sample_rate)) {
            /* Audio is still being brought up on this device. Keep going
               silently rather than refusing to run — the screen is the thing
               being checked. */
            s_null_sink = 1;
        }
    }
    s_rate = sample_rate;
    s_pull = pull;
    s_pull_ctx = ctx;
    s_frames_out = 0;
    s_paused = 0;
    s_stop_when_silent = 0;
    s_pause_when_silent = 0;
    s_gain = 0.0;
    s_gain_step = 1.0 / (double)sample_rate;   /* ~1 s ramp in */
    s_state = EN_AUDIO_PLAYING;
    pthread_mutex_unlock(&s_lock);
    return true;
}

/* The file-shaped half of the interface is unreachable here: the engine checks
   en_audio_can_stream() and takes the streaming path. They are defined so the
   backend satisfies audio.h in full. */

bool en_audio_submit(const char *key, const int16_t *pcm,
                     uint32_t frames, uint32_t advance_frames,
                     uint32_t sample_rate, bool loop)
{
    (void)key; (void)pcm; (void)frames; (void)advance_frames;
    (void)sample_rate; (void)loop;
    return false;
}

bool en_audio_queue(const char *key, const int16_t *pcm,
                    uint32_t frames, uint32_t advance_frames,
                    uint32_t sample_rate)
{
    (void)key; (void)pcm; (void)frames; (void)advance_frames;
    (void)sample_rate;
    return false;
}

bool en_audio_wants_next(void) { return false; }

double en_audio_remaining(void) { return s_pull ? 1e9 : 0.0; }

void en_audio_stop(uint32_t fade_ms)
{
    pthread_mutex_lock(&s_lock);
    if (s_pull) {
        if (fade_ms == 0) {
            s_pull = NULL;
            s_state = EN_AUDIO_IDLE;
        } else {
            double frames = (double)s_rate * (double)fade_ms / 1000.0;
            if (frames < 1.0) frames = 1.0;
            s_gain_step = -s_gain / frames;
            s_stop_when_silent = 1;
            s_pause_when_silent = 0;
        }
    }
    pthread_mutex_unlock(&s_lock);
}

void en_audio_set_paused(bool paused)
{
    pthread_mutex_lock(&s_lock);
    if (s_pull) {
        double frames = (double)s_rate * 0.25;
        if (frames < 1.0) frames = 1.0;
        s_stop_when_silent = 0;
        if (paused) {
            s_state = EN_AUDIO_PAUSED;
            s_gain_step = -s_gain / frames;
            s_pause_when_silent = 1;
        } else {
            s_state = EN_AUDIO_PLAYING;
            s_paused = 0;
            s_pause_when_silent = 0;
            s_gain_step = (1.0 - s_gain) / frames;
        }
    }
    pthread_mutex_unlock(&s_lock);
}

void en_audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    /* Drive the card's own control if there is one; otherwise the software
       gain in pull() carries it. */
    mixer_apply_volume(percent);

    pthread_mutex_lock(&s_lock);
    s_volume = s_vol_ctl ? 100 : percent;   /* no double attenuation */
    pthread_mutex_unlock(&s_lock);
}

int en_audio_get_volume(void) { return s_volume; }

void en_audio_tick(void) { /* the writer thread owns playback */ }

en_audio_state_t en_audio_state(void) { return s_state; }

double en_audio_elapsed(void)
{
    double v;
    pthread_mutex_lock(&s_lock);
    v = s_rate ? (double)s_frames_out / (double)s_rate : 0.0;
    pthread_mutex_unlock(&s_lock);
    return v;
}

const char *en_audio_backend_name(void)
{
    /* The buffer and the restart count go on the About screen, because a
       stream that keeps restarting sounds like a broken tone and there is
       nowhere else on this device to see the difference. */
    static char name[96];
    unsigned ms;

    if (s_null_sink) return "tinyalsa (null sink - no PCM device)";

    ms = (unsigned)((uint64_t)EN_PERIOD_FRAMES * s_periods * 1000u /
                    (s_rate ? s_rate : 44100u));
    if (s_restarts)
        snprintf(name, sizeof name, "tinyalsa, %u ms, %s vol, %lu restarts",
                 ms, s_vol_ctl ? "hw" : "sw", s_restarts);
    else
        snprintf(name, sizeof name, "tinyalsa, %u ms buffer, %s volume",
                 ms, s_vol_ctl ? "hardware" : "software");
    return name;
}

/* Which mixer control the volume ended up on, for the About screen and for
   working out why a device is silent. NULL if none was found. */
const char *en_audio_linux_volume_control(void)
{
    mixer_find_volume();
    return s_vol_ctl ? mixer_ctl_get_name(s_vol_ctl) : (const char *)0;
}
