/*
 * audio.h — the whole platform audio interface, and deliberately tiny.
 *
 * The device and the desktop reach audio in completely different ways: the
 * iPod can only be handed a file to play (see AUDIO_NOTES.md — there is no
 * streaming path, and loads are capped at 1 MiB), while a Linux host streams
 * PCM to ALSA. The shared UI must not care which it is talking to, so the
 * interface is expressed in the one operation both can do:
 *
 *     "here is a seamless loop; play it until I say otherwise."
 *
 * Everything the device backend has to do — write a WAV, watch the heap, load
 * it, re-arm before the loop ends — lives behind en_audio_submit and
 * en_audio_tick. Everything the host backend has to do lives there too.
 */

#ifndef ENTRAIN_AUDIO_H
#define ENTRAIN_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    EN_AUDIO_IDLE = 0,
    EN_AUDIO_PREPARING,   /* writing / loading; nothing audible yet */
    EN_AUDIO_PLAYING,
    EN_AUDIO_PAUSED,
    EN_AUDIO_FAILED
} en_audio_state_t;

bool en_audio_init(void);
void en_audio_shutdown(void);

/* Two slots: what is playing now, and what plays after it.

   A steady preset is one buffer submitted with loop=true, and nothing else
   ever happens. A program is a chain of chunks — each one a ramp, so none of
   them can loop — submitted with loop=false and queued one ahead. Two slots is
   the least that expresses both, and it maps onto what the device can do: the
   expensive step there is loading a file, so the next chunk is loaded into a
   second descriptor while the current one plays, leaving only the cheap play
   call to happen at the join.

   `key` identifies the buffer for caching — the device backend keys its
   on-disk WAV cache by it, so replaying a preset is instant. The PCM is
   16-bit stereo interleaved and is copied or written out before the call
   returns; the caller may free it afterwards.

   Returns false if the buffer could not be accepted (no heap, disk full). */
/* `advance_frames` is how far to move before the NEXT buffer starts, which is
   not necessarily the whole buffer. A buffer may carry a short crossfade tail
   past its advance point: the tail repeats what the following buffer opens
   with, faded out, while that one fades in, and the two gains sum to one.

   That exists because a join can only be timed to the granularity of the UI
   tick, and a tone with a 16 ms hole in it clicks. Overlapping identical
   content cannot comb-filter, so being a few milliseconds early or late costs
   a slight amplitude ripple instead of an artefact.

   Pass 0 to mean "the whole buffer", which is the plain no-crossfade case. */
bool en_audio_submit(const char *key, const int16_t *pcm,
                     uint32_t frames, uint32_t advance_frames,
                     uint32_t sample_rate, bool loop);

/* Set the follow-on buffer, replacing any previously queued one. Only
   meaningful after a submit with loop=false. */
bool en_audio_queue(const char *key, const int16_t *pcm,
                    uint32_t frames, uint32_t advance_frames,
                    uint32_t sample_rate);

/* True when the next slot is empty and the engine should render into it. */
bool en_audio_wants_next(void);

/* Seconds left in the buffer currently playing. A loop never runs out, so this
   returns a large value for one. */
double en_audio_remaining(void);

/* ---- streaming, where the platform has a real PCM sink -------------------

   The two-slot model above exists because RetailOS can only be handed a file.
   A Linux target with ALSA has no such limit: it can be fed samples forever.
   Where that is true the engine skips the whole render-to-file path and
   generates straight into the sink, which removes every seam, every cache
   file, and the 1 MiB ceiling along with them.

   The pull callback runs on the audio thread. It must not allocate, block, or
   touch LVGL. */

/* Fill `frames` of interleaved stereo, but only ADVANCE the source by
   `advance_frames`. Those differ when the backend crossfades: it asks for a
   window plus the overlap the next window will also carry, and the source has
   to produce those overlapping frames twice — once as this window's tail and
   once as the next one's head. Counting them as consumed would gain that much
   time on every window.
   Where a backend streams contiguously, advance_frames == frames.
   Returns frames produced; 0 means the source is finished. */
typedef uint32_t (*en_audio_pull_fn)(int16_t *dst, uint32_t frames,
                                     uint32_t advance_frames, void *ctx);

/* True when en_audio_start_stream is usable. Backends that can only play
   files return false and the engine uses submit/queue instead. */
bool en_audio_can_stream(void);

/* Begin pulling. Replaces anything already playing. Returns false if the sink
   could not be opened, in which case nothing is playing and the caller should
   say so rather than pretending. */
bool en_audio_start_stream(uint32_t sample_rate, en_audio_pull_fn pull,
                           void *ctx);

/* The rate this backend would rather be fed. The DSP is rate-agnostic, so this
   is about what the output path likes and what the CPU can afford, not about
   the maths. */
uint32_t en_audio_preferred_rate(void);

/* Fade out over `fade_ms` and stop. Passing 0 stops immediately; anything the
   user can hear should pass at least 1000. */
void en_audio_stop(uint32_t fade_ms);

void en_audio_set_paused(bool paused);

/* 0..100. The app caps this well below maximum by default — long sessions at
   high SPL are the real hazard with this kind of audio. */
void en_audio_set_volume(int percent);
int  en_audio_get_volume(void);

/* Call once per UI frame. The device backend re-arms the loop from here, which
   is why it must keep being called even when the screen is blanked. */
void en_audio_tick(void);

en_audio_state_t en_audio_state(void);

/* Seconds of audio played since the current submit. Monotonic, survives
   loop wraps; the Now Playing progress ring reads this. */
double en_audio_elapsed(void);

/* Backend name for the About screen ("ALSA", "hb_audio"). */
const char *en_audio_backend_name(void);

#endif /* ENTRAIN_AUDIO_H */
