/*
 * capture_linux.c — capture.h on the N31 Linux port, through alsa-lib.
 *
 * The tuner's digital audio arrives on IIS2, which is hw:0,1; the headphones
 * are on IIS0, which is hw:0,0. They are separate devices, so capture does not
 * contend with playback and a recording can run while listening.
 *
 * Three things about this path are load-bearing and each of them looks like
 * something else when it is wrong.
 *
 *   THE RATE IS 32000 AND NOTHING ELSE. The DAI advertises 32000 and 16000.
 *   This file asked for 44100 for a long time, which the port cannot be
 *   clocked at. Stock picks a different PCM bit clock per station - 8 MHz,
 *   1.6 MHz, 4.8 MHz - but always sets CLKDIV to bitclk/32000, so the frame
 *   rate is the same on every station and the PCM never has to be reopened
 *   because somebody retuned.
 *
 *   THE DEVICE IS OPENED BY NAME. n31fm, from /etc/asound.conf, is a softvol
 *   over a plug over hw:0,1. Opening it rather than the hardware device is
 *   what makes "FM Capture Volume" a real control over this source and what
 *   lets a reader ask for a rate the port cannot produce. tinyalsa, which this
 *   used before, talks to /dev/snd directly and cannot see an alsa-lib plugin
 *   at all - so with it there is no source gain, no mute and no conversion.
 *
 *   OPENING THIS IS NECESSARY FOR AUDIO AND NOT SUFFICIENT. The SoC side of
 *   IIS2 is clocked by the capture PCM, so tuner audio is inaudible until
 *   something opens it; and the audio then still has to be carried from IIS2
 *   to IIS0, which is the player's job. Capture starts with the tuner rather
 *   than with the record button because of the first half. It also must not be
 *   opened before a station has been tuned, because the driver leaves the
 *   audio route off until then - see the ordering note in model_linux.c.
 *
 * The reader runs on its own thread, feeding a ring in memory and, when one is
 * running, a WAV file. Neither consumer can stall the other: a file write that
 * blocks must not cost live audio, so a failed write stops the recording and
 * leaves the ring alone.
 */

#include "capture.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#include "../core/wav.h"

/*
 * The plugin device, and a way past it.
 *
 * n31fm is what should be opened. RADIOPLUS_PCM_IN exists for a machine whose
 * /etc/asound.conf is missing or older than this app: "hw:0,1" still works,
 * with no source volume and no conversion, which is worth having as a fallback
 * and is not worth making the default.
 */
#define CAP_PCM_DEFAULT "n31fm"

/*
 * 32000 because that is what the port is clocked at. See the note above; this
 * is not a preference and there is no other value that works.
 */
#define CAP_RATE     32000u
#define CAP_CHANNELS 2u
#define CAP_BITS     16u
#define CAP_FRAME    (CAP_CHANNELS * (CAP_BITS / 8u))

#define PERIOD_FRAMES 512u
#define PERIOD_COUNT  4u

/*
 * The reader waits with a deadline, for the same reason the player writes with
 * one: snd_pcm_readi on a device that is open but not clocking blocks forever,
 * and en_cap_stop() joins this thread on the way out of the app. A capture
 * that never produces a sample would make Radio+ unquittable - the process
 * would sit in pthread_join through SIGTERM, which is what HOME sends.
 *
 * 200 ms is several periods at 32 kHz and is short enough that quitting is
 * immediate to a person.
 */
#define WAIT_MS 200

static snd_pcm_t  *s_pcm;
static pthread_t   s_thread;
static bool        s_running;
static char        s_desc[128];

/* The ring, and everything the reader thread shares with the callers. */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t  *s_ring;
static uint32_t  s_ring_bytes;
static uint32_t  s_ring_used;      /* bytes currently held, up to s_ring_bytes */
static uint32_t  s_ring_head;      /* where the next byte is written */
static uint32_t  s_overruns;
static uint64_t  s_total;     /* frames captured since the stream began */

static FILE     *s_rec;
static uint32_t  s_rec_bytes;

static const char *cap_pcm_name(void)
{
    const char *e = getenv("RADIOPLUS_PCM_IN");
    return (e && *e) ? e : CAP_PCM_DEFAULT;
}

static uint32_t bytes_to_ms(uint32_t b)
{
    return (uint32_t)((uint64_t)b * 1000u / (CAP_RATE * CAP_FRAME));
}

static uint32_t ms_to_bytes(uint32_t ms)
{
    uint64_t b = (uint64_t)ms * CAP_RATE * CAP_FRAME / 1000u;
    return (uint32_t)(b - (b % CAP_FRAME));      /* always whole frames */
}

/* ---- the ring ------------------------------------------------------------ */

static void ring_write(const uint8_t *p, uint32_t n)
{
    if (!s_ring || !s_ring_bytes) return;

    /* A block larger than the ring can only leave its own tail. */
    if (n >= s_ring_bytes) {
        p += n - s_ring_bytes;
        n = s_ring_bytes;
    }

    uint32_t first = s_ring_bytes - s_ring_head;
    if (first > n) first = n;
    memcpy(s_ring + s_ring_head, p, first);
    if (n > first) memcpy(s_ring, p + first, n - first);

    s_ring_head = (s_ring_head + n) % s_ring_bytes;
    s_ring_used += n;
    if (s_ring_used > s_ring_bytes) s_ring_used = s_ring_bytes;
    s_total += n / CAP_FRAME;
}

/* Copy the most recent `want` bytes out, oldest first. Returns how many. */
static uint32_t ring_tail(uint8_t *out, uint32_t want)
{
    if (!s_ring || !s_ring_used) return 0;
    if (want > s_ring_used) want = s_ring_used;

    uint32_t start = (s_ring_head + s_ring_bytes - want) % s_ring_bytes;
    uint32_t first = s_ring_bytes - start;
    if (first > want) first = want;

    memcpy(out, s_ring + start, first);
    if (want > first) memcpy(out + first, s_ring, want - first);
    return want;
}

/* ---- the device ---------------------------------------------------------- */

/*
 * -EPIPE on a capture is an overrun: the reader lost the race and audio is
 * genuinely gone. -ESTRPIPE is a suspend, which has to be resumed or, failing
 * that, prepared again. Both are recoverable and neither is a reason to stop
 * the radio; anything else is real.
 */
static int recover(snd_pcm_t *pcm, int err)
{
    if (err == -EPIPE)
        return snd_pcm_prepare(pcm);

    if (err == -ESTRPIPE) {
        int r;
        while ((r = snd_pcm_resume(pcm)) == -EAGAIN)
            usleep(10000);
        if (r < 0)
            r = snd_pcm_prepare(pcm);
        return r;
    }
    return err;
}

static int configure(snd_pcm_t *pcm, snd_pcm_uframes_t *buffer_out)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    unsigned int rate = CAP_RATE;
    unsigned int periods = PERIOD_COUNT;
    snd_pcm_uframes_t period = PERIOD_FRAMES;
    snd_pcm_uframes_t buffer = 0, got_period = 0;
    int err;

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);

    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0) return err;
    if ((err = snd_pcm_hw_params_set_access(pcm, hw,
                    SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) return err;
    if ((err = snd_pcm_hw_params_set_format(pcm, hw,
                    SND_PCM_FORMAT_S16_LE)) < 0) return err;
    if ((err = snd_pcm_hw_params_set_channels(pcm, hw, CAP_CHANNELS)) < 0)
        return err;
    /* _near rather than exact: the plugin will hand back what it can give, and
       a stream at a rate this file did not ask for is a fault worth reporting
       rather than an open that fails with nothing said. */
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL)) < 0)
        return err;
    if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period,
                                                      NULL)) < 0) return err;
    if ((err = snd_pcm_hw_params_set_periods_near(pcm, hw, &periods,
                                                  NULL)) < 0) return err;
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) return err;

    if ((err = snd_pcm_sw_params_current(pcm, sw)) < 0) return err;
    /* Capture starts as soon as there is anything at all; the reader is what
       decides when to take it. This is the opposite of the playback side, and
       for the same reason - see the start-threshold note in player_linux.c. */
    if ((err = snd_pcm_sw_params_set_start_threshold(pcm, sw, 1)) < 0)
        return err;
    if ((err = snd_pcm_sw_params_set_avail_min(pcm, sw, period)) < 0)
        return err;
    if ((err = snd_pcm_sw_params(pcm, sw)) < 0) return err;

    if ((err = snd_pcm_get_params(pcm, &buffer, &got_period)) < 0) return err;

    if (rate != CAP_RATE)
        fprintf(stderr, "radioplus: capture opened at %u Hz, not %u - "
                        "timings and recordings will be wrong\n",
                rate, CAP_RATE);

    if (buffer_out) *buffer_out = buffer;
    return 0;
}

/* ---- the reader ---------------------------------------------------------- */

static void *reader(void *arg)
{
    (void)arg;
    const uint32_t chunk = PERIOD_FRAMES * CAP_FRAME;
    uint8_t *buf = malloc(chunk);
    if (!buf) return 0;

    while (s_running) {
        snd_pcm_sframes_t got;
        int w = snd_pcm_wait(s_pcm, WAIT_MS);

        /*
         * Nothing arrived. Not an overrun and not counted as one - a silent
         * tuner is a normal thing to be pointed at. Going round again is where
         * s_running is re-read, which is what makes this thread joinable.
         */
        if (w == 0)
            continue;

        if (w < 0) {
            pthread_mutex_lock(&s_lock);
            s_overruns++;
            pthread_mutex_unlock(&s_lock);
            if (recover(s_pcm, w) < 0)
                usleep(50000);
            continue;
        }

        got = snd_pcm_readi(s_pcm, buf, PERIOD_FRAMES);

        if (got < 0) {
            /*
             * An overrun means the thread lost the race and audio is gone.
             * Counting them matters: a recording with a non-zero count has
             * holes in it and the user should be told rather than left to
             * notice.
             */
            pthread_mutex_lock(&s_lock);
            s_overruns++;
            pthread_mutex_unlock(&s_lock);

            if (recover(s_pcm, (int)got) < 0) {
                /* Not an xrun. The device has gone, and spinning on it would
                   burn the battery to no purpose. */
                usleep(50000);
            }
            continue;
        }

        if (got == 0)
            continue;

        pthread_mutex_lock(&s_lock);
        ring_write(buf, (uint32_t)got * CAP_FRAME);

        if (s_rec) {
            size_t want = (size_t)got * CAP_FRAME;
            if (fwrite(buf, 1, want, s_rec) != want) {
                /* Out of space, or the card went away. Close the recording so
                   what was captured stays playable, and leave the live buffer
                   running - losing the radio because a write failed would be
                   the wrong trade. */
                fclose(s_rec);
                s_rec = 0;
            } else {
                s_rec_bytes += (uint32_t)want;
            }
        }
        pthread_mutex_unlock(&s_lock);
    }
    free(buf);
    return 0;
}

/* ---- the interface ------------------------------------------------------- */

en_cap_err_t en_cap_start(uint32_t live_seconds)
{
    const char *name = cap_pcm_name();
    snd_pcm_uframes_t buffer = 0;
    int err;

    if (s_running) return EN_CAP_OK;
    if (!live_seconds) live_seconds = 30;

    err = snd_pcm_open(&s_pcm, name, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        snprintf(s_desc, sizeof s_desc, "%s unavailable: %s",
                 name, snd_strerror(err));
        s_pcm = 0;
        return EN_CAP_NO_DEVICE;
    }

    err = configure(s_pcm, &buffer);
    if (err < 0) {
        snprintf(s_desc, sizeof s_desc, "%s cannot be configured: %s",
                 name, snd_strerror(err));
        snd_pcm_close(s_pcm);
        s_pcm = 0;
        return EN_CAP_NO_DEVICE;
    }

    /* Allocated up front. Failing here, before the tuner is playing, is much
       better than failing later with the radio already on. */
    s_ring_bytes = live_seconds * CAP_RATE * CAP_FRAME;
    s_ring = malloc(s_ring_bytes);
    if (!s_ring) {
        snd_pcm_close(s_pcm);
        s_pcm = 0;
        s_ring_bytes = 0;
        return EN_CAP_NO_MEMORY;
    }
    s_ring_used = s_ring_head = s_overruns = 0;
    s_total = 0;

    /* Explicit rather than relying on the first read to trigger it, so the
       stream is running before the thread exists and the first period is not
       also the first thing that could go wrong. */
    err = snd_pcm_start(s_pcm);
    if (err < 0)
        fprintf(stderr, "radioplus: capture start: %s\n", snd_strerror(err));

    s_running = true;
    if (pthread_create(&s_thread, 0, reader, 0) != 0) {
        s_running = false;
        free(s_ring); s_ring = 0; s_ring_bytes = 0;
        snd_pcm_close(s_pcm); s_pcm = 0;
        return EN_CAP_FAILED;
    }

    snprintf(s_desc, sizeof s_desc,
             "alsa-lib %s  %u Hz %u ch  %lu frame buffer  %us live",
             name, CAP_RATE, CAP_CHANNELS, (unsigned long)buffer,
             live_seconds);
    return EN_CAP_OK;
}

void en_cap_stop(void)
{
    if (!s_running) return;
    s_running = false;
    pthread_join(s_thread, 0);

    en_cap_record_stop();

    pthread_mutex_lock(&s_lock);
    free(s_ring);
    s_ring = 0;
    s_ring_bytes = s_ring_used = s_ring_head = 0;
    pthread_mutex_unlock(&s_lock);

    if (s_pcm) { snd_pcm_close(s_pcm); s_pcm = 0; }
}

void en_cap_state(en_cap_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);

    out->rate = CAP_RATE;
    out->channels = CAP_CHANNELS;
    out->bits = CAP_BITS;
    out->running = s_running;

    pthread_mutex_lock(&s_lock);
    out->live_ms = bytes_to_ms(s_ring_used);
    out->live_cap_ms = bytes_to_ms(s_ring_bytes);
    out->recorded_ms = bytes_to_ms(s_rec_bytes);
    out->recording = s_rec != 0;
    out->overruns = s_overruns;
    pthread_mutex_unlock(&s_lock);
}

uint64_t en_cap_total_frames(void)
{
    pthread_mutex_lock(&s_lock);
    uint64_t t = s_total;
    pthread_mutex_unlock(&s_lock);
    return t;
}

uint64_t en_cap_oldest_frame(void)
{
    pthread_mutex_lock(&s_lock);
    uint64_t oldest = s_total - (s_ring_used / CAP_FRAME);
    pthread_mutex_unlock(&s_lock);
    return oldest;
}

uint32_t en_cap_read_from(uint64_t at, void *buf, uint32_t frames)
{
    if (!buf || !frames) return 0;

    pthread_mutex_lock(&s_lock);

    uint32_t held = s_ring_used / CAP_FRAME;
    uint64_t oldest = s_total - held;

    /* A reader that has fallen behind the window gets the oldest audio still
       held rather than silence or a fault: the alternative is a gap the reader
       cannot see and cannot fix. */
    if (at < oldest) at = oldest;
    if (at >= s_total) { pthread_mutex_unlock(&s_lock); return 0; }

    uint64_t avail = s_total - at;
    if (frames > avail) frames = (uint32_t)avail;

    uint32_t back = (uint32_t)((s_total - at) * CAP_FRAME);
    uint32_t start = (s_ring_head + s_ring_bytes - back) % s_ring_bytes;
    uint32_t want = frames * CAP_FRAME;

    uint32_t first = s_ring_bytes - start;
    if (first > want) first = want;
    memcpy(buf, s_ring + start, first);
    if (want > first) memcpy((uint8_t *)buf + first, s_ring, want - first);

    pthread_mutex_unlock(&s_lock);
    return frames;
}

const char *en_cap_backend(void)
{
    return s_desc[0] ? s_desc : "alsa-lib (not started)";
}

/* Write a header and, optionally, a prefill from the ring. Caller holds the
   lock, because the ring must not move underneath the prefill. */
static bool open_wav_locked(FILE **fp, const char *path, uint32_t prefill_ms,
                            uint32_t *written)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    uint8_t hdr[EN_WAV_HDR_BYTES];
    en_wav_header(hdr, sizeof hdr, CAP_RATE, CAP_CHANNELS, CAP_BITS, 0);
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return false; }

    uint32_t n = 0;
    if (prefill_ms) {
        uint32_t want = ms_to_bytes(prefill_ms);
        if (want > s_ring_used) want = s_ring_used;
        if (want) {
            uint8_t *tmp = malloc(want);
            if (tmp) {
                uint32_t got = ring_tail(tmp, want);
                if (fwrite(tmp, 1, got, f) == got) n = got;
                free(tmp);
            }
        }
    }
    *fp = f;
    *written = n;
    return true;
}

en_cap_err_t en_cap_record_start(const char *path, uint32_t prefill_ms)
{
    if (!path) return EN_CAP_FAILED;
    if (!s_running) return EN_CAP_NO_DEVICE;

    pthread_mutex_lock(&s_lock);
    if (s_rec) { pthread_mutex_unlock(&s_lock); return EN_CAP_BUSY; }

    FILE *f = 0;
    uint32_t pre = 0;
    bool ok = open_wav_locked(&f, path, prefill_ms, &pre);
    if (ok) { s_rec = f; s_rec_bytes = pre; }
    pthread_mutex_unlock(&s_lock);

    return ok ? EN_CAP_OK : EN_CAP_IO;
}

en_cap_err_t en_cap_record_stop(void)
{
    pthread_mutex_lock(&s_lock);
    FILE *f = s_rec;
    uint32_t n = s_rec_bytes;
    s_rec = 0;
    s_rec_bytes = 0;
    pthread_mutex_unlock(&s_lock);

    if (!f) return EN_CAP_OK;

    /* Patch the two length fields now that the length is known. Written last
       deliberately: until this happens the file is still playable by anything
       that tolerates a zero-length data chunk, and en_wav_repair-style recovery
       from the file size remains possible if the app never gets here. */
    uint8_t hdr[EN_WAV_HDR_BYTES];
    en_wav_header(hdr, sizeof hdr, CAP_RATE, CAP_CHANNELS, CAP_BITS, n);

    bool ok = fseek(f, 0, SEEK_SET) == 0
           && fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    fclose(f);
    return ok ? EN_CAP_OK : EN_CAP_IO;
}

en_cap_err_t en_cap_save_live(const char *path, uint32_t ms)
{
    if (!path) return EN_CAP_FAILED;
    if (!s_running) return EN_CAP_NO_DEVICE;

    pthread_mutex_lock(&s_lock);
    uint32_t want = ms ? ms_to_bytes(ms) : s_ring_used;
    if (want > s_ring_used) want = s_ring_used;

    uint8_t *tmp = want ? malloc(want) : 0;
    uint32_t got = tmp ? ring_tail(tmp, want) : 0;
    pthread_mutex_unlock(&s_lock);

    if (!tmp) return want ? EN_CAP_NO_MEMORY : EN_CAP_FAILED;

    FILE *f = fopen(path, "wb");
    if (!f) { free(tmp); return EN_CAP_IO; }

    uint8_t hdr[EN_WAV_HDR_BYTES];
    en_wav_header(hdr, sizeof hdr, CAP_RATE, CAP_CHANNELS, CAP_BITS, got);

    bool ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr
           && fwrite(tmp, 1, got, f) == got;
    fclose(f);
    free(tmp);
    return ok ? EN_CAP_OK : EN_CAP_IO;
}

const char *en_cap_strerror(en_cap_err_t e)
{
    switch (e) {
    case EN_CAP_OK:        return "ok";
    case EN_CAP_NO_DEVICE: return "no capture device";
    case EN_CAP_BUSY:      return "already recording";
    case EN_CAP_NO_MEMORY: return "out of memory";
    case EN_CAP_IO:        return "write failed";
    default:               return "failed";
    }
}
