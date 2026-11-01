/*
 * player.h — what comes out of the headphones.
 *
 * One player, two sources, because they are the same job. Tuner audio arrives
 * on IIS2 and the headphones are on IIS0, so something has to carry it across -
 * the n31-fm helper does it with arecord piped into aplay. That carrying is the
 * player, which is why it is not an optional extra: without it the radio is
 * silent even when perfectly tuned.
 *
 * Once the live audio is going through a player rather than straight out, three
 * things fall out for nothing:
 *
 *   Live is a position, not a mode. Playing the newest frame in the ring is
 *   live; playing an older one is being behind. There is no separate path.
 *
 *   Scrubbing back is moving that position, and catching up is moving it to the
 *   end - which is exactly what the LIVE button does.
 *
 *   A recording is the same player reading a file instead of the ring.
 *
 * The whole thing runs on its own thread and never blocks the UI.
 */

#ifndef RADIOPLUS_PLAYER_H
#define RADIOPLUS_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "mixer.h"

typedef enum {
    EN_PLAY_OK = 0,
    EN_PLAY_NO_DEVICE,
    EN_PLAY_NO_FILE,
    EN_PLAY_FAILED
} en_play_err_t;

typedef enum {
    EN_SRC_LIVE = 0,     /* out of the capture ring */
    EN_SRC_FILE          /* a recording */
} en_play_src_t;

typedef struct {
    bool          running;
    bool          paused;
    en_play_src_t source;

    /* Live: how far behind the newest captured audio we are. Zero is live. */
    uint32_t behind_ms;
    uint32_t behind_max_ms;   /* how far back the ring currently allows */

    /* File: where we are and how long it is. */
    uint32_t pos_ms;
    uint32_t len_ms;
    char     name[96];

    uint32_t underruns;

    /* Where it is going, as an index into the output table below, and whether
       the device is open at all. The two are different questions: a chosen
       output whose device cannot be opened is a real state and the interface
       has to be able to say so rather than showing a destination that is not
       receiving anything. */
    uint8_t  output;
    bool     output_open;
} en_play_state_t;

/*
 * Where the audio goes.
 *
 * Choosing an output is choosing an alsa-lib device name and nothing else -
 * there is no routing API on this hardware and nothing to program. The names
 * come from /etc/asound.conf:
 *
 *   n31hp    softvol -> plug 48 kHz -> hw:0,0, the codec and the jack
 *   n31bt    softvol -> plug 48 kHz -> a fifo the Bluetooth encoder reads
 *   n31both  softvol -> plug 48 kHz -> a tee into both of those at once
 *
 * "Both" is one softvol over a tee rather than two streams, so the pair shares
 * one volume. Two independent levels would need two writers, which is a
 * different feature and not one anybody has asked for.
 */
typedef struct {
    const char *pcm;        /* the alsa-lib device name */
    const char *label;      /* what to call it on screen */
    bool        via_fifo;   /* reaches Bluetooth through /run/n31-bt.pcm */

    /* Its volume, which is a different control per output because each output
       is a different softvol. Named here so that everything true of an output
       is stated in one place, rather than in a mapping somewhere else that
       has to be kept in step with this list. */
    en_mix_t    volume;
} en_play_out_t;

uint8_t              en_play_out_count(void);
const en_play_out_t *en_play_out(uint8_t i);
uint8_t              en_play_out_current(void);

/*
 * Whether output `i` could be opened right now.
 *
 * Worth asking before offering it, because of one property of the Bluetooth
 * leg: it tees into a fifo, and opening a fifo for writing BLOCKS until a
 * reader exists. With no encoder running, switching to Bluetooth would not
 * fail - it would hang, silently, inside the open, with the radio stopped and
 * nothing on screen to say why.
 *
 * So the fifo is probed first, with O_WRONLY | O_NONBLOCK, which is the one
 * call that answers "is anybody listening" without committing to anything: it
 * returns ENXIO when there is no reader instead of waiting for one.
 */
bool en_play_out_ready(uint8_t i);

/*
 * Send the audio somewhere else.
 *
 * Returns NULL when the switch was accepted, or a short phrase saying why not,
 * which the interface shows - "switching failed" with no reason is the same as
 * a button that does nothing.
 *
 * Safe while playing, and it takes effect at the next period rather than
 * instantly: a PCM's destination is fixed when it is opened, so moving the
 * audio means closing one stream and opening another however it is done. That
 * happens on the player's own thread, so nothing about it can block the
 * interface.
 */
const char *en_play_set_output(uint8_t i);

/* Open the headphone device and start carrying live audio. */
en_play_err_t en_play_start(void);
void          en_play_stop(void);

void en_play_state(en_play_state_t *out);
const char *en_play_backend(void);

/* ---- live ---------------------------------------------------------------- */

/* Move to `ms` behind the newest captured audio, clamped to what the ring
   still holds. Switches back to the live source if a file was playing. */
void en_play_seek_live(uint32_t behind_ms);

/* Jump to the front of the buffer. The one control that always means the same
   thing, whatever else is going on. */
void en_play_go_live(void);

/* Step back or forward by `ms`, clamped at both ends. */
void en_play_nudge(int32_t ms);

/* ---- recordings ---------------------------------------------------------- */

/* Play a WAV. Returns EN_PLAY_NO_FILE if it cannot be opened or is not a
   format this can play, which is reported rather than silently ignored. */
en_play_err_t en_play_file(const char *path);

/* Back to the radio. */
void en_play_close_file(void);

void en_play_pause(bool paused);
void en_play_seek_file(uint32_t ms);

const char *en_play_strerror(en_play_err_t e);

#endif /* RADIOPLUS_PLAYER_H */
