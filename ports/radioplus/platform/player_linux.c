/*
 * player_linux.c — player.h through alsa-lib, to a device chosen by name.
 *
 * The tuner's audio arrives on IIS2 and the headphones are on IIS0. There is
 * no path between them inside the SoC, so something in userspace has to carry
 * one to the other; that carrying is this file, which is why the player is not
 * an optional extra. Without it the radio is silent however well it is tuned.
 *
 * Four things in here were measured rather than reasoned about, and each of
 * them is the difference between working audio and audio that sounds broken in
 * a way that points somewhere else.
 *
 *   IT OPENS A NAME, NOT A CARD. n31hp, n31bt and n31both are alsa-lib
 *   plugins from /etc/asound.conf: a softvol over a rate converter over either
 *   the codec, a fifo, or a tee into both. None of that has a card number, so
 *   the "card,device" pair this used to take could not reach any of it. Nor
 *   could tinyalsa, which talks to /dev/snd directly and cannot see a plugin
 *   at all - which is why the volume, the mute and the simultaneous output
 *   this file now provides were all impossible before.
 *
 *   IT STARTS ON A FULL BUFFER. snd_pcm_sw_params_set_start_threshold is the
 *   whole buffer size and not one period. Starting on a period means the
 *   stream begins essentially empty and has to be refilled in real time from
 *   then on, so any scheduling jitter at all underruns it. Measured on the
 *   recorded output of the identical path: with a one-period threshold, a
 *   third of all 20 ms windows came back more than 25 dB below the median,
 *   which is audible as constant breaking up; with a full-buffer threshold,
 *   none did. It costs one buffer of latency once, at the start.
 *
 *   THE OUTPUT BUFFER IS DEEPER THAN THE INPUT. Four times the periods. This
 *   is the side that must never run dry, and it is fed by a reader that can be
 *   late; the capture side can afford to be shallow because falling behind
 *   there costs a recorded sample, not a click.
 *
 *   A STATION CHANGE DOES NOT TOUCH IT. Writing fm_tune on a live stream
 *   re-runs the tuner registers and the audio route and nothing else - not
 *   RXCOM, not the PCM, not the DMA. Measured: no xruns, no restart, about
 *   60 ms of tuner mute across the hop. So retuning is not a reason to stop
 *   and reopen anything here, and nothing in this file listens for it.
 *
 * Everything runs on the player's own thread, including opening and closing
 * the device. That matters more than it looks: opening the Bluetooth leg can
 * block, and a switch that blocks the interface would be worse than one that
 * takes a moment.
 */

#include "player.h"
#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#include "../core/wav.h"

/* ---- where the audio can go ---------------------------------------------- */

/*
 * The three destinations, in the order the interface offers them.
 *
 * Headphones first because it is the one that always works: the jack is also
 * the antenna, so anybody listening to FM at all has something plugged into
 * it. Bluetooth and Both need an encoder running and can be unavailable, and
 * an unavailable option should not be the first thing on the list.
 */
static const en_play_out_t k_out[] = {
    { "n31hp",   "Headphones", false, EN_MIX_VOL_HP     },
    { "n31bt",   "Bluetooth",  true,  EN_MIX_VOL_BT     },
    { "n31both", "Both",       true,  EN_MIX_VOL_MASTER },
};
#define OUT_N ((uint8_t)(sizeof k_out / sizeof k_out[0]))

/*
 * The fifo the Bluetooth legs tee into, and the one property of it that
 * decides how this file is written: opening a fifo for writing BLOCKS until a
 * reader exists.
 *
 * So n31bt and n31both cannot simply be opened and allowed to fail. With no
 * encoder running they do not fail, they hang - inside snd_pcm_open, on
 * whatever thread called it, with the radio stopped and nothing to say why.
 * The probe below is what turns that into an answer.
 */
#define BT_FIFO_DEFAULT "/run/n31-bt.pcm"

static const char *bt_fifo(void)
{
    const char *e = getenv("RADIOPLUS_BT_FIFO");
    return (e && *e) ? e : BT_FIFO_DEFAULT;
}

/*
 * One write end, held for the life of the process once it has ever been
 * obtained.
 *
 * The probe itself is O_WRONLY | O_NONBLOCK, which is the only call that asks
 * "is anybody listening" without committing to waiting for one: it returns
 * ENXIO when the fifo has no reader instead of blocking until it does.
 *
 * Holding the first successful one open is not laziness about closing it. A
 * fifo whose last writer closes gives its reader EOF, so a probe that opened
 * and closed would tell tinybtd the stream had ended - and so would switching
 * the output away from Bluetooth, every time. Keeping one write end open means
 * this app is always a writer and the encoder never sees the stream end
 * underneath it. Later probes open a second fd and close that one, which is
 * safe for the same reason.
 */
static int s_fifo_fd = -1;

static bool fifo_has_reader(void)
{
    int fd = open(bt_fifo(), O_WRONLY | O_NONBLOCK);

    if (fd < 0)
        return false;               /* ENXIO: nothing is reading it */

    if (s_fifo_fd < 0)
        s_fifo_fd = fd;             /* the one that is never closed */
    else
        close(fd);
    return true;
}

uint8_t en_play_out_count(void) { return OUT_N; }

const en_play_out_t *en_play_out(uint8_t i)
{
    return (i < OUT_N) ? &k_out[i] : NULL;
}

bool en_play_out_ready(uint8_t i)
{
    if (i >= OUT_N)
        return false;
    if (!k_out[i].via_fifo)
        return true;
    return fifo_has_reader();
}

/* ---- the stream ---------------------------------------------------------- */

/*
 * The rate everything runs at, which is the tuner's.
 *
 * The ring holds what the capture produced and the capture is clocked at
 * 32 kHz, so writing at anything else here would mean resampling twice: once
 * to whatever this asked for and again in the plug in front of the codec. The
 * output devices convert to the 48 kHz the codec and the fifo both run at, and
 * they are the right place for it.
 *
 * A recording is the exception: it carries its own rate in its header and the
 * stream is reopened at that rate, so a file written before the capture rate
 * was corrected still plays at the pitch it was recorded at.
 */
#define LIVE_RATE 32000u
#define CHANNELS  2u
#define BITS      16u
#define FRAME     (CHANNELS * (BITS / 8u))

#define PERIOD_FRAMES 512u
#define PERIOD_COUNT  16u        /* four times the capture's four */

/* How far behind the write head live playback sits. Two periods is about
   32 ms: enough that ordinary scheduling jitter never reaches the write head,
   and far too little to notice on a radio. */
#define LIVE_LAG_FRAMES (PERIOD_FRAMES * 2u)

/*
 * Inside this much, the listener is live.
 *
 * The distance between the write head and the read head is not a smooth
 * quantity: the capture thread appends a period at a time and the player
 * consumes one at a time, so it saws between roughly the lag and the lag plus
 * a period. Subtracting a constant lag from a sawtooth and asking whether the
 * result is exactly zero produces an answer that changes several times a
 * second - which is what made the home screen flicker between "live" and a
 * time behind, taking the pause and record buttons with it.
 *
 * A quarter of a second is far below anything a person can hear as a delay and
 * comfortably above the sawtooth, so the readout is steady and still honest.
 */
#define LIVE_DEADBAND_FRAMES (PERIOD_FRAMES * 16u)

/* When to stop believing an output is coming back. At one attempt every
   200 ms this is about a minute, which is long enough to ride out an encoder
   being restarted and short enough that a genuinely dead device is reported
   while somebody is still looking at the screen. */
#define FAIL_LIMIT 300u

/*
 * How long the writer waits for room before it decides the stream is dead.
 *
 * This exists because of the shape of a blocking write. snd_pcm_writei on a
 * full buffer waits for the hardware to consume some of it, and a device that
 * is open but not clocking never does - so the write does not fail, it simply
 * never returns. The thread is then unkillable: en_play_stop sets the flag and
 * joins, and the join waits on a thread that is inside the kernel with no
 * reason to come out. Radio+ stops responding to SIGTERM, which is what the
 * launcher sends for HOME, and the only way out of the app is the hard
 * power-off.
 *
 * So nothing here waits without a deadline. snd_pcm_wait carries one; 200 ms
 * is far longer than a period and short enough that quitting feels immediate.
 * Twenty-five of them in a row - five seconds with no room at all - is a
 * stream that is not going to move, and it is closed and reopened rather than
 * waited on.
 */
#define WAIT_MS      200
#define STALL_WAITS  25u

static snd_pcm_t  *s_pcm;
static pthread_t   s_thread;
static bool        s_running;
static char        s_desc[128];

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static en_play_src_t s_src;
static bool     s_paused;
static uint64_t s_at;            /* live: absolute frame in the capture ring */
static uint32_t s_underruns;

/* Which output is wanted, which one is open, and at what rate. The writer
   thread compares the two at the top of every period and reopens when they
   have drifted apart - which is the only place a PCM is ever opened. */
static uint8_t  s_out_want;
static uint8_t  s_out_open = 0xFF;    /* 0xFF: nothing is open */
static uint32_t s_rate_want = LIVE_RATE;
static uint32_t s_rate_open;
static uint32_t s_fails;
static bool     s_failed;

static FILE    *s_file;
static uint32_t s_file_frames;   /* total, from the header */
static uint32_t s_file_pos;      /* frames played */
static uint32_t s_file_rate;     /* from the header, not assumed */
static char     s_file_name[96];

static uint32_t frames_to_ms(uint64_t f, uint32_t rate)
{
    return rate ? (uint32_t)(f * 1000u / rate) : 0;
}

static uint32_t ms_to_frames(uint32_t ms, uint32_t rate)
{
    return (uint32_t)((uint64_t)ms * rate / 1000u);
}

/* ---- opening it ---------------------------------------------------------- */

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

static int configure(snd_pcm_t *pcm, unsigned int rate,
                     snd_pcm_uframes_t *buffer_out)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    unsigned int r = rate;
    unsigned int periods = PERIOD_COUNT;
    snd_pcm_uframes_t period = PERIOD_FRAMES;
    snd_pcm_uframes_t buffer = 0, got = 0;
    int err;

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);

    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0) return err;
    if ((err = snd_pcm_hw_params_set_access(pcm, hw,
                    SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) return err;
    if ((err = snd_pcm_hw_params_set_format(pcm, hw,
                    SND_PCM_FORMAT_S16_LE)) < 0) return err;
    if ((err = snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS)) < 0)
        return err;
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &r, NULL)) < 0)
        return err;
    if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period,
                                                      NULL)) < 0) return err;
    if ((err = snd_pcm_hw_params_set_periods_near(pcm, hw, &periods,
                                                  NULL)) < 0) return err;
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) return err;

    if ((err = snd_pcm_get_params(pcm, &buffer, &got)) < 0) return err;

    if ((err = snd_pcm_sw_params_current(pcm, sw)) < 0) return err;
    /*
     * The whole buffer, which is the point of this function. See the note at
     * the top of the file: one period here is a third of every 20 ms window
     * more than 25 dB down.
     */
    if ((err = snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer)) < 0)
        return err;
    /* Wake the writer as soon as one period of room exists. */
    if ((err = snd_pcm_sw_params_set_avail_min(pcm, sw, got ? got : period)) < 0)
        return err;
    if ((err = snd_pcm_sw_params(pcm, sw)) < 0) return err;

    if (buffer_out) *buffer_out = buffer;
    return 0;
}

static void close_pcm(void)
{
    if (s_pcm) {
        snd_pcm_close(s_pcm);
        s_pcm = NULL;
    }
    s_out_open = 0xFF;
    s_rate_open = 0;
}

/*
 * Bring the stream in line with what has been asked for. Called from the
 * writer thread and nowhere else, so a PCM is only ever opened, written and
 * closed by one thread.
 *
 * Returns false when there is nothing to write to, and the caller waits rather
 * than spinning.
 */
static bool ensure_open(void)
{
    uint8_t want;
    uint32_t rate;
    snd_pcm_uframes_t buffer = 0;
    int err;

    pthread_mutex_lock(&s_lock);
    want = s_out_want;
    rate = s_rate_want;
    pthread_mutex_unlock(&s_lock);

    if (s_pcm && want == s_out_open && rate == s_rate_open)
        return true;

    close_pcm();

    if (want >= OUT_N)
        want = 0;

    /*
     * The probe, before the open that could block. A fifo output with nothing
     * reading it is reported rather than waited for.
     */
    if (k_out[want].via_fifo && !fifo_has_reader()) {
        snprintf(s_desc, sizeof s_desc,
                 "%s: nothing is reading %s", k_out[want].label, bt_fifo());
        pthread_mutex_lock(&s_lock);
        s_fails++;
        if (s_fails > FAIL_LIMIT) s_failed = true;
        pthread_mutex_unlock(&s_lock);
        return false;
    }

    err = snd_pcm_open(&s_pcm, k_out[want].pcm, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        s_pcm = NULL;
        snprintf(s_desc, sizeof s_desc, "%s unavailable: %s",
                 k_out[want].pcm, snd_strerror(err));
        pthread_mutex_lock(&s_lock);
        s_fails++;
        if (s_fails > FAIL_LIMIT) s_failed = true;
        pthread_mutex_unlock(&s_lock);
        return false;
    }

    err = configure(s_pcm, rate, &buffer);
    if (err < 0) {
        snprintf(s_desc, sizeof s_desc, "%s cannot be configured: %s",
                 k_out[want].pcm, snd_strerror(err));
        close_pcm();
        pthread_mutex_lock(&s_lock);
        s_fails++;
        if (s_fails > FAIL_LIMIT) s_failed = true;
        pthread_mutex_unlock(&s_lock);
        return false;
    }

    s_out_open = want;
    s_rate_open = rate;

    pthread_mutex_lock(&s_lock);
    s_fails = 0;
    s_failed = false;
    pthread_mutex_unlock(&s_lock);

    /* The settings screen shows this, and on a device with three possible
       destinations "which one, and how deep" is the first thing worth
       knowing. */
    snprintf(s_desc, sizeof s_desc,
             "alsa-lib %s (%s)  %u Hz %u ch  %lu frames, starts full",
             k_out[want].pcm, k_out[want].label, rate, CHANNELS,
             (unsigned long)buffer);
    return true;
}

/* ---- the thread ----------------------------------------------------------- */

static void *writer(void *arg)
{
    (void)arg;
    const uint32_t chunk = PERIOD_FRAMES * FRAME;
    uint8_t *buf = malloc(chunk);
    if (!buf) return 0;

    while (s_running) {
        uint32_t got = 0;
        snd_pcm_uframes_t left;
        uint8_t *at_bytes;
        unsigned stalled;

        if (!ensure_open()) {
            /* Nothing to write to. Waiting rather than spinning, and still
               looping, because the encoder that is missing may well be started
               a moment from now and nothing should have to be restarted when
               it is. */
            usleep(200000);
            continue;
        }

        pthread_mutex_lock(&s_lock);
        bool paused = s_paused;

        if (!paused && s_src == EN_SRC_FILE && s_file) {
            size_t n = fread(buf, 1, chunk, s_file);
            got = (uint32_t)(n / FRAME);
            s_file_pos += got;

            /* A recording that has run out stops rather than looping, and hands
               the headphones back to the radio - which is what anyone expects
               when a clip ends. */
            if (got < PERIOD_FRAMES) {
                fclose(s_file);
                s_file = 0;
                s_src = EN_SRC_LIVE;
                s_at = 0;                 /* re-anchor below */
                s_rate_want = LIVE_RATE;
            }
        }
        pthread_mutex_unlock(&s_lock);

        if (!paused && !got) {
            /* Live. Anchor on first use, and re-anchor whenever the cursor has
               fallen outside what the ring still holds - which happens after a
               stall, and is better than playing a gap nobody asked for. */
            uint64_t total = en_cap_total_frames();
            uint64_t oldest = en_cap_oldest_frame();

            pthread_mutex_lock(&s_lock);
            if (s_src != EN_SRC_LIVE) s_src = EN_SRC_LIVE;
            if (!s_at || s_at < oldest || s_at > total)
                s_at = (total > LIVE_LAG_FRAMES) ? total - LIVE_LAG_FRAMES : 0;
            uint64_t at = s_at;
            pthread_mutex_unlock(&s_lock);

            got = at ? en_cap_read_from(at, buf, PERIOD_FRAMES) : 0;

            pthread_mutex_lock(&s_lock);
            s_at = at + got;
            pthread_mutex_unlock(&s_lock);

            /*
             * An empty ring before the capture has produced anything is not an
             * underrun, it is the first second of the app. Counting it as one
             * put a hundred underruns on the diagnostics screen of a machine
             * where nothing had gone wrong, which makes the number useless for
             * the case it exists for.
             */
            if (!got && total > 0) {
                pthread_mutex_lock(&s_lock);
                s_underruns++;
                pthread_mutex_unlock(&s_lock);
            }
        }

        /* Silence rather than nothing. The PCM has to keep being fed or it
           underruns and the next real audio starts with a click. */
        if (got < PERIOD_FRAMES)
            memset(buf + got * FRAME, 0, (PERIOD_FRAMES - got) * FRAME);

        /*
         * Write the whole period, however many calls that takes. snd_pcm_writei
         * can return a short count, and treating a short write as a failed one
         * would drop the tail of every period it happened on - which is audible
         * and looks exactly like a source problem.
         */
        left = PERIOD_FRAMES;
        at_bytes = buf;
        stalled = 0;
        while (left && s_running) {
            snd_pcm_sframes_t put;
            int w = snd_pcm_wait(s_pcm, WAIT_MS);

            if (w == 0) {
                /*
                 * No room yet. Going round again is the whole point: it is
                 * where s_running is re-read, so a stop that arrives while the
                 * sink is full is acted on within a fifth of a second instead
                 * of never.
                 */
                if (++stalled < STALL_WAITS)
                    continue;

                /* Five seconds without room. Not a slow sink - a dead one. */
                snprintf(s_desc, sizeof s_desc, "%s stopped accepting audio",
                         k_out[s_out_open < OUT_N ? s_out_open : 0].label);
                close_pcm();
                pthread_mutex_lock(&s_lock);
                s_fails++;
                if (s_fails > FAIL_LIMIT) s_failed = true;
                pthread_mutex_unlock(&s_lock);
                break;
            }
            stalled = 0;

            if (w < 0) {
                if (recover(s_pcm, w) < 0) {
                    close_pcm();
                    break;
                }
                continue;
            }

            put = snd_pcm_writei(s_pcm, at_bytes, left);

            if (put < 0) {
                pthread_mutex_lock(&s_lock);
                s_underruns++;
                pthread_mutex_unlock(&s_lock);

                if (recover(s_pcm, (int)put) < 0) {
                    /*
                     * Not an xrun. Usually the reader on the fifo has gone, or
                     * the card has. Dropped so the next pass reopens it, which
                     * is how a Bluetooth encoder that is restarted comes back
                     * on its own.
                     */
                    close_pcm();
                    pthread_mutex_lock(&s_lock);
                    s_fails++;
                    if (s_fails > FAIL_LIMIT) s_failed = true;
                    pthread_mutex_unlock(&s_lock);
                    break;
                }
                continue;
            }
            left -= (snd_pcm_uframes_t)put;
            at_bytes += (size_t)put * FRAME;
        }
    }
    free(buf);
    return 0;
}

/* ---- lifecycle ------------------------------------------------------------ */

en_play_err_t en_play_start(void)
{
    if (s_running) return EN_PLAY_OK;

    /*
     * A fifo whose reader dies turns the next write into SIGPIPE, and the
     * default for that is to kill the process. Losing the whole radio because
     * a Bluetooth encoder exited is not a trade worth making; the write
     * returns EPIPE instead and the loop above reopens.
     */
    signal(SIGPIPE, SIG_IGN);

    s_src = EN_SRC_LIVE;
    s_at = 0;
    s_paused = false;
    s_underruns = 0;
    s_fails = 0;
    s_failed = false;
    s_rate_want = LIVE_RATE;

    s_running = true;
    if (pthread_create(&s_thread, 0, writer, 0) != 0) {
        s_running = false;
        return EN_PLAY_FAILED;
    }

    /*
     * Reported as started even though nothing is open yet.
     *
     * The thread owns the device, and it opens on its first pass a few
     * milliseconds from now. Waiting for it here would mean blocking the
     * caller on an open that can itself block - which is exactly the thing
     * this file is arranged to avoid.
     */
    if (!s_desc[0])
        snprintf(s_desc, sizeof s_desc, "alsa-lib %s (%s), opening",
                 k_out[s_out_want < OUT_N ? s_out_want : 0].pcm,
                 k_out[s_out_want < OUT_N ? s_out_want : 0].label);
    return EN_PLAY_OK;
}

void en_play_stop(void)
{
    if (!s_running) return;
    s_running = false;
    pthread_join(s_thread, 0);

    pthread_mutex_lock(&s_lock);
    if (s_file) { fclose(s_file); s_file = 0; }
    pthread_mutex_unlock(&s_lock);

    close_pcm();

    /* The held write end goes with the process and not with the stream: see
       the note on s_fifo_fd. Closed here only because the process is on its
       way out and a reader seeing EOF at that point is correct. */
    if (s_fifo_fd >= 0) { close(s_fifo_fd); s_fifo_fd = -1; }
}

/* ---- choosing an output --------------------------------------------------- */

uint8_t en_play_out_current(void)
{
    uint8_t i;

    pthread_mutex_lock(&s_lock);
    i = s_out_want;
    pthread_mutex_unlock(&s_lock);
    return i < OUT_N ? i : 0;
}

const char *en_play_set_output(uint8_t i)
{
    if (i >= OUT_N)
        return "no such output";

    /*
     * Refused here rather than discovered by the thread, so the answer arrives
     * in the same gesture that asked the question. The thread checks again
     * when it opens, because a reader can disappear in between - but by then
     * there is no user waiting for a reply.
     */
    if (k_out[i].via_fifo && !fifo_has_reader())
        return "nothing is listening on Bluetooth";

    pthread_mutex_lock(&s_lock);
    s_out_want = i;
    /* A previous output's failures say nothing about this one. */
    s_fails = 0;
    s_failed = false;
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

void en_play_state(en_play_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);

    uint64_t total = en_cap_total_frames();
    uint64_t oldest = en_cap_oldest_frame();

    pthread_mutex_lock(&s_lock);
    out->running = s_running;
    out->paused = s_paused;
    out->source = s_src;
    out->underruns = s_underruns;
    out->output = s_out_want;
    /*
     * s_pcm belongs to the writer thread and is not covered by this lock,
     * which is deliberate: making the writer take a mutex around every open
     * and close so that a status read could be exact would be paying in audio
     * for a readout. One period out of date is a truer answer than a late
     * one.
     */
    out->output_open = (s_pcm != NULL) && !s_failed;

    if (s_src == EN_SRC_LIVE) {
        /* The lag is deliberate and constant, so it is subtracted out - showing
           "0:00 behind" while sitting two periods back is the honest reading of
           what the listener experiences. */
        uint64_t at = s_at ? s_at : (total > LIVE_LAG_FRAMES
                                     ? total - LIVE_LAG_FRAMES : 0);
        uint64_t behind = (total > at) ? total - at : 0;
        behind = (behind > LIVE_LAG_FRAMES) ? behind - LIVE_LAG_FRAMES : 0;
        /* Snapped to live inside the deadband, so the readout does not
           oscillate with the period boundary - see LIVE_DEADBAND_FRAMES. */
        if (behind < LIVE_DEADBAND_FRAMES) behind = 0;
        out->behind_ms = frames_to_ms(behind, LIVE_RATE);
        out->behind_max_ms = frames_to_ms(total > oldest ? total - oldest : 0,
                                          LIVE_RATE);
    } else {
        uint32_t r = s_file_rate ? s_file_rate : LIVE_RATE;
        out->pos_ms = frames_to_ms(s_file_pos, r);
        out->len_ms = frames_to_ms(s_file_frames, r);
        snprintf(out->name, sizeof out->name, "%s", s_file_name);
    }
    pthread_mutex_unlock(&s_lock);
}

const char *en_play_backend(void)
{
    return s_desc[0] ? s_desc : "alsa-lib (not started)";
}

/* ---- live ----------------------------------------------------------------- */

void en_play_seek_live(uint32_t behind_ms)
{
    uint64_t total = en_cap_total_frames();
    uint64_t oldest = en_cap_oldest_frame();

    uint64_t back = ms_to_frames(behind_ms, LIVE_RATE) + LIVE_LAG_FRAMES;
    uint64_t at = (total > back) ? total - back : oldest;
    if (at < oldest) at = oldest;

    pthread_mutex_lock(&s_lock);
    /* Seeking the radio while a recording is playing means the user wants the
       radio, so the file is closed rather than left half-played. */
    if (s_file) { fclose(s_file); s_file = 0; }
    s_src = EN_SRC_LIVE;
    s_at = at;
    s_paused = false;
    s_rate_want = LIVE_RATE;
    pthread_mutex_unlock(&s_lock);
}

void en_play_go_live(void) { en_play_seek_live(0); }

void en_play_nudge(int32_t ms)
{
    en_play_state_t st;
    en_play_state(&st);

    if (st.source == EN_SRC_FILE) {
        int64_t p = (int64_t)st.pos_ms + ms;
        if (p < 0) p = 0;
        if ((uint32_t)p > st.len_ms) p = st.len_ms;
        en_play_seek_file((uint32_t)p);
        return;
    }

    /* Negative ms means going further back, which is a larger "behind". */
    int64_t behind = (int64_t)st.behind_ms - ms;
    if (behind < 0) behind = 0;
    if (behind > (int64_t)st.behind_max_ms) behind = st.behind_max_ms;
    en_play_seek_live((uint32_t)behind);
}

/* ---- recordings ----------------------------------------------------------- */

/*
 * The sample rate a WAV header claims.
 *
 * Read here rather than added to core/wav.h because it is the player that
 * needs it and only the player: everything else in this app deals in the one
 * capture rate. Offset 24 in a canonical 44-byte header, little-endian, which
 * is the layout en_wav_header writes.
 *
 * It matters because the capture rate was wrong for a while. Recordings made
 * then carry 44100 in the header, and playing them at 32000 would be a quarter
 * slow - so the rate comes out of the file rather than out of an assumption,
 * and the stream is reopened at whatever it says.
 */
static uint32_t wav_rate(const uint8_t *hdr)
{
    return (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8)
         | ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
}

en_play_err_t en_play_file(const char *path)
{
    if (!path) return EN_PLAY_NO_FILE;

    FILE *f = fopen(path, "rb");
    if (!f) return EN_PLAY_NO_FILE;

    uint8_t hdr[EN_WAV_HDR_BYTES];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        fclose(f);
        return EN_PLAY_NO_FILE;
    }

    uint32_t rate = wav_rate(hdr);
    /* A rate outside anything a sound card runs at means this is not a header
       we wrote, and playing the bytes anyway is worse than declining. */
    if (rate < 8000 || rate > 192000) {
        fclose(f);
        return EN_PLAY_NO_FILE;
    }

    uint32_t bytes = en_wav_data_len(hdr, sizeof hdr);
    if (!bytes) {
        /* A recording interrupted before its length was patched. The audio is
           still good; the length is simply whatever is there. */
        long here = ftell(f);
        if (fseek(f, 0, SEEK_END) == 0) {
            long end = ftell(f);
            if (end > here) bytes = (uint32_t)(end - here);
            fseek(f, here, SEEK_SET);
        }
    }
    if (!bytes) { fclose(f); return EN_PLAY_NO_FILE; }

    pthread_mutex_lock(&s_lock);
    if (s_file) fclose(s_file);
    s_file = f;
    s_file_frames = bytes / FRAME;
    s_file_pos = 0;
    s_file_rate = rate;
    s_rate_want = rate;              /* the writer reopens if it differs */
    s_src = EN_SRC_FILE;
    s_paused = false;

    const char *base = path, *p = path;
    for (; *p; p++) if (*p == '/') base = p + 1;
    snprintf(s_file_name, sizeof s_file_name, "%s", base);
    pthread_mutex_unlock(&s_lock);

    return EN_PLAY_OK;
}

void en_play_close_file(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_file) { fclose(s_file); s_file = 0; }
    s_src = EN_SRC_LIVE;
    s_at = 0;                 /* re-anchors to live on the next period */
    s_paused = false;
    s_rate_want = LIVE_RATE;
    pthread_mutex_unlock(&s_lock);
}

void en_play_pause(bool paused)
{
    pthread_mutex_lock(&s_lock);
    s_paused = paused;

    /* Pausing the radio is really "start being behind", so resuming picks up
       where it stopped rather than jumping to live. The cursor is left alone
       and the ring keeps filling underneath it. */
    pthread_mutex_unlock(&s_lock);
}

void en_play_seek_file(uint32_t ms)
{
    pthread_mutex_lock(&s_lock);
    if (s_file) {
        uint32_t frame = ms_to_frames(ms, s_file_rate ? s_file_rate
                                                      : LIVE_RATE);
        if (frame > s_file_frames) frame = s_file_frames;
        if (fseek(s_file, (long)(EN_WAV_HDR_BYTES + (size_t)frame * FRAME),
                  SEEK_SET) == 0)
            s_file_pos = frame;
    }
    pthread_mutex_unlock(&s_lock);
}

const char *en_play_strerror(en_play_err_t e)
{
    switch (e) {
    case EN_PLAY_OK:        return "ok";
    case EN_PLAY_NO_DEVICE: return "no playback device";
    case EN_PLAY_NO_FILE:   return "cannot play that file";
    default:                return "failed";
    }
}
