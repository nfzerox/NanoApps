/*
 * capture.h — FM audio capture, the live buffer, and recording.
 *
 * On this hardware the tuner's digital audio arrives on a separate I2S
 * controller from the one driving the headphones, which is what makes recording
 * while listening possible at all rather than a trick: capture and playback are
 * different devices and neither has to yield to the other. RetailOS does the
 * same thing for its Live Pause, and the captured DMA state shows both channels
 * armed in every state including playing-from-buffer.
 *
 * Two consumers hang off one capture:
 *
 *   The live buffer is a ring in memory that is always filling while the tuner
 *   is on. It is what makes it possible to jump back to something that has
 *   already been said, and it costs nothing when unused.
 *
 *   A recording is a WAV file written as it arrives. Starting one does not
 *   disturb the live buffer, and a recording can begin with the contents of
 *   the live buffer, so "record this" can reach backwards to before the button
 *   was pressed - which is the only useful moment to press it.
 *
 * The capture runs on its own thread. Nothing here blocks the UI.
 */

#ifndef RADIOPLUS_CAPTURE_H
#define RADIOPLUS_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EN_CAP_OK = 0,
    EN_CAP_NO_DEVICE,
    EN_CAP_BUSY,
    EN_CAP_NO_MEMORY,
    EN_CAP_IO,
    EN_CAP_FAILED
} en_cap_err_t;

typedef struct {
    uint32_t rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t live_ms;        /* how much audio the ring currently holds */
    uint32_t live_cap_ms;    /* how much it can hold */
    uint32_t recorded_ms;
    bool     running;
    bool     recording;
    uint32_t overruns;       /* capture stalls; a non-zero count is a real fault */
} en_cap_state_t;

/* Open the capture device and start filling the live buffer.
   `live_seconds` of ring is allocated up front - failing here is better than
   failing later with the tuner already playing. */
en_cap_err_t en_cap_start(uint32_t live_seconds);
void         en_cap_stop(void);

void en_cap_state(en_cap_state_t *out);
const char *en_cap_backend(void);

/*
 * Begin recording to `path`.
 *
 * `prefill_ms` takes that much audio from the live buffer first, so a recording
 * can include what was already said before the button was pressed. Pass 0 for
 * a recording that starts now.
 */
en_cap_err_t en_cap_record_start(const char *path, uint32_t prefill_ms);
en_cap_err_t en_cap_record_stop(void);

/* Write the last `ms` of the live buffer to a WAV file, without disturbing
   either the buffer or a recording in progress. */
en_cap_err_t en_cap_save_live(const char *path, uint32_t ms);

/*
 * Playing out of the ring.
 *
 * Opening the capture device clocks IIS2, but the tuner's audio still has to be
 * carried from IIS2 to IIS0 for anything to be heard - the n31-fm helper does
 * it with arecord piped into aplay. Here the player does it, reading through
 * these, which is why the player is not an optional extra.
 *
 * Positions are absolute frame counts rather than "behind live". A reader that
 * asks for an offset from a moving target drifts, because the target moves
 * while the read is in flight; a cursor the reader advances itself cannot.
 */

/* Frames captured since the stream started. The newest sample is at
   en_cap_total_frames() - 1. */
uint64_t en_cap_total_frames(void);

/* Copy up to `frames` starting at absolute frame `at`. Returns how many were
   available; a reader that has fallen out of the window gets what is left and
   should re-anchor. */
uint32_t en_cap_read_from(uint64_t at, void *buf, uint32_t frames);

/* The oldest frame the ring still holds. */
uint64_t en_cap_oldest_frame(void);

const char *en_cap_strerror(en_cap_err_t e);

#endif /* RADIOPLUS_CAPTURE_H */
