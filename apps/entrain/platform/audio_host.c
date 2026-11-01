/*
 * audio_host.c — audio.h on a Linux desktop, over ALSA.
 *
 * The host has the luxury the device does not: a real streaming sink. A writer
 * thread pulls from the current buffer and pushes to ALSA, so a loop that is
 * seamless in memory is seamless in the air, and the click and seam tests
 * become audible rather than theoretical.
 *
 * Build without EN_HOST_ALSA and this becomes a silent backend that still
 * keeps time, so the UI can be developed and screenshotted on a machine with
 * no sound card.
 */

#include "audio.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

#ifdef EN_HOST_ALSA
#include <alsa/asoundlib.h>
#endif

typedef struct {
    int16_t *pcm;
    uint32_t frames;        /* the whole buffer, crossfade tail included */
    uint32_t advance;       /* where the next buffer takes over */
    uint32_t rate;
    bool     loop;
} slot_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       s_thread;
static int             s_thread_running;
static int             s_quit;

static slot_t   s_cur;
static slot_t   s_next;
static uint32_t s_pos;           /* frame cursor inside s_cur */
static uint64_t s_played;        /* frames emitted since the last submit */
static int      s_volume = 45;   /* deliberately not maximum by default */
static int      s_paused;
static en_audio_state_t s_state = EN_AUDIO_IDLE;

static double s_gain = 1.0;
static double s_gain_step;
static int    s_stop_when_silent;
static int    s_pause_when_silent;


static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

/* Advance the cursor by wall-clock time instead of by samples written. Used by
   the silent build, and by the ALSA build when there is no sound device — a
   missing sound card should make the app quiet, not frozen. */
static void advance_by_clock(uint64_t *last);

static void slot_free(slot_t *s)
{
    free(s->pcm);
    s->pcm = NULL;
    s->frames = 0;
}

/* Called with the lock held. Steps the gain ramp by one frame and acts on
   whatever the ramp was for once it reaches silence. */
static void step_gain(void)
{
    if (s_gain_step == 0.0) return;
    s_gain += s_gain_step;
    if (s_gain <= 0.0) {
        s_gain = 0.0;
        s_gain_step = 0.0;
        if (s_stop_when_silent) {
            slot_free(&s_cur);
            slot_free(&s_next);
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

/* Advance one frame, returning the sample pair. Lock held. */
static void advance(int16_t *l, int16_t *r)
{
    *l = *r = 0;
    if (!s_cur.pcm || !s_cur.frames || s_paused) {
        step_gain();
        return;
    }

    *l = s_cur.pcm[2 * s_pos + 0];
    *r = s_cur.pcm[2 * s_pos + 1];
    s_pos++;
    s_played++;

    if (s_pos >= s_cur.advance) {
        if (s_cur.loop) {
            /* Wrap at the advance. The tail past it is the crossfade partner
               for the head, and playing it here instead would be a dip. */
            s_pos = 0;
        } else if (s_next.pcm) {
            /* Hand over to the queued chunk. On this platform the join is
               sample-exact; on the device it is a short crossfade. */
            slot_free(&s_cur);
            s_cur = s_next;
            s_next.pcm = NULL;
            s_next.frames = 0;
            s_pos = 0;
        } else {
            /* Ran dry with nothing queued — the program is over. */
            slot_free(&s_cur);
            s_state = EN_AUDIO_IDLE;
            s_pos = 0;
        }
    }
    step_gain();
}

static void pull(int16_t *dst, uint32_t want)
{
    double vol = (double)s_volume / 100.0;
    for (uint32_t i = 0; i < want; i++) {
        int16_t l, r;
        advance(&l, &r);
        double g = vol * s_gain;
        dst[2 * i + 0] = (int16_t)((double)l * g);
        dst[2 * i + 1] = (int16_t)((double)r * g);
    }
}

static void advance_by_clock(uint64_t *last)
{
    uint64_t t = now_us();
    pthread_mutex_lock(&s_lock);
    if (s_cur.pcm && s_cur.rate) {
        uint32_t adv = (uint32_t)(((t - *last) * s_cur.rate) / 1000000ull);
        if (adv > s_cur.rate) adv = s_cur.rate;   /* clamp after a stall */
        for (uint32_t i = 0; i < adv && s_cur.pcm; i++) {
            int16_t l, r;
            advance(&l, &r);
        }
    }
    *last = t;
    pthread_mutex_unlock(&s_lock);
}

/* ---- ALSA ---------------------------------------------------------------- */

#define EN_CHUNK 512

#ifdef EN_HOST_ALSA

static snd_pcm_t *s_handle;
static uint32_t   s_open_rate;

static int alsa_open(uint32_t rate)
{
    if (s_handle && s_open_rate == rate) return 1;
    if (s_handle) { snd_pcm_close(s_handle); s_handle = NULL; }

    const char *dev = getenv("ENTRAIN_ALSA_DEVICE");
    if (!dev) dev = "default";
    if (snd_pcm_open(&s_handle, dev, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        s_handle = NULL;
        return 0;
    }
    if (snd_pcm_set_params(s_handle, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED, 2,
                           (unsigned int)rate, 1, 100000) < 0) {
        snd_pcm_close(s_handle);
        s_handle = NULL;
        return 0;
    }
    s_open_rate = rate;
    return 1;
}

static void *writer(void *arg)
{
    (void)arg;
    static int16_t buf[EN_CHUNK * 2];

    uint64_t last = now_us();

    while (!s_quit) {
        uint32_t rate;
        int have;

        pthread_mutex_lock(&s_lock);
        have = (s_cur.pcm != NULL);
        rate = s_cur.rate;
        pthread_mutex_unlock(&s_lock);

        if (!have) { usleep(10000); last = now_us(); continue; }

        if (!alsa_open(rate)) {
            /* No sound device — headless CI, a container, WSL. Keep time so the
               UI still plays through at the right pace instead of freezing on
               a progress ring that never moves. */
            usleep(10000);
            advance_by_clock(&last);
            continue;
        }

        pthread_mutex_lock(&s_lock);
        pull(buf, EN_CHUNK);
        pthread_mutex_unlock(&s_lock);

        snd_pcm_sframes_t n = snd_pcm_writei(s_handle, buf, EN_CHUNK);
        if (n < 0) snd_pcm_recover(s_handle, (int)n, 1);   /* underruns happen */
        last = now_us();
    }
    if (s_handle) { snd_pcm_close(s_handle); s_handle = NULL; }
    return NULL;
}

const char *en_audio_backend_name(void) { return "ALSA"; }

#else /* no ALSA: keep time, make no sound */

static void *writer(void *arg)
{
    (void)arg;
    uint64_t last = now_us();
    while (!s_quit) {
        usleep(10000);
        advance_by_clock(&last);
    }
    return NULL;
}

const char *en_audio_backend_name(void) { return "silent (built without ALSA)"; }

#endif

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
    pthread_mutex_lock(&s_lock);
    slot_free(&s_cur);
    slot_free(&s_next);
    s_state = EN_AUDIO_IDLE;
    pthread_mutex_unlock(&s_lock);
}

static int16_t *copy_pcm(const int16_t *pcm, uint32_t frames)
{
    int16_t *c = malloc((size_t)frames * 4);
    if (c) memcpy(c, pcm, (size_t)frames * 4);
    return c;
}

bool en_audio_submit(const char *key, const int16_t *pcm,
                     uint32_t frames, uint32_t advance_frames,
                     uint32_t sample_rate, bool loop)
{
    (void)key;   /* the host streams from memory; nothing to cache */
    if (!pcm || !frames) return false;

    int16_t *copy = copy_pcm(pcm, frames);
    if (!copy) return false;

    pthread_mutex_lock(&s_lock);
    slot_free(&s_cur);
    slot_free(&s_next);
    s_cur.pcm = copy;
    s_cur.frames = frames;
    s_cur.advance = (advance_frames && advance_frames <= frames)
                  ? advance_frames : frames;
    s_cur.rate = sample_rate;
    s_cur.loop = loop;
    s_pos = 0;
    s_played = 0;
    s_paused = 0;
    s_stop_when_silent = 0;
    s_pause_when_silent = 0;
    /* Never begin a tone at full amplitude: ramp in over about a second. */
    s_gain = 0.0;
    s_gain_step = 1.0 / (double)sample_rate;
    s_state = EN_AUDIO_PLAYING;
    pthread_mutex_unlock(&s_lock);
    return true;
}

bool en_audio_queue(const char *key, const int16_t *pcm,
                    uint32_t frames, uint32_t advance_frames,
                    uint32_t sample_rate)
{
    (void)key;
    if (!pcm || !frames) return false;

    int16_t *copy = copy_pcm(pcm, frames);
    if (!copy) return false;

    pthread_mutex_lock(&s_lock);
    slot_free(&s_next);
    s_next.pcm = copy;
    s_next.frames = frames;
    s_next.advance = (advance_frames && advance_frames <= frames)
                   ? advance_frames : frames;
    s_next.rate = sample_rate;
    s_next.loop = false;
    pthread_mutex_unlock(&s_lock);
    return true;
}

/* This backend plays buffers, not a continuous feed, so the engine takes the
   submit/queue path. */
uint32_t en_audio_preferred_rate(void) { return 44100u; }   /* the desktop can afford full rate */

bool en_audio_can_stream(void) { return false; }

bool en_audio_start_stream(uint32_t sample_rate, en_audio_pull_fn pull,
                           void *ctx)
{
    (void)sample_rate; (void)pull; (void)ctx;
    return false;
}

bool en_audio_wants_next(void)
{
    bool want;
    pthread_mutex_lock(&s_lock);
    want = (s_cur.pcm != NULL) && !s_cur.loop && (s_next.pcm == NULL);
    pthread_mutex_unlock(&s_lock);
    return want;
}

double en_audio_remaining(void)
{
    double v;
    pthread_mutex_lock(&s_lock);
    if (!s_cur.pcm || !s_cur.rate)   v = 0.0;
    else if (s_cur.loop)             v = 1e9;
    else v = (double)(s_cur.advance - s_pos) / (double)s_cur.rate;
    pthread_mutex_unlock(&s_lock);
    return v;
}

void en_audio_stop(uint32_t fade_ms)
{
    pthread_mutex_lock(&s_lock);
    if (s_cur.pcm) {
        if (fade_ms == 0) {
            slot_free(&s_cur);
            slot_free(&s_next);
            s_state = EN_AUDIO_IDLE;
        } else {
            double frames = (double)s_cur.rate * (double)fade_ms / 1000.0;
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
    if (s_cur.pcm) {
        /* Ramp rather than cut, in both directions. The cursor keeps moving
           through a pause fade so the ramp is actually heard; the writer
           freezes it once the gain reaches zero. */
        double frames = (double)s_cur.rate * 0.25;
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
    pthread_mutex_lock(&s_lock);
    s_volume = percent;
    pthread_mutex_unlock(&s_lock);
}

int en_audio_get_volume(void) { return s_volume; }

void en_audio_tick(void)
{
    /* Nothing to do here: the writer thread owns playback on this platform.
       This is where the device backend earns its keep. */
}

en_audio_state_t en_audio_state(void) { return s_state; }

double en_audio_elapsed(void)
{
    double v;
    pthread_mutex_lock(&s_lock);
    v = s_cur.rate ? (double)s_played / (double)s_cur.rate : 0.0;
    pthread_mutex_unlock(&s_lock);
    return v;
}
