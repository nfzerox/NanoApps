/*
 * engine.h — what to render, when to render it, and what to hand the audio
 * backend. Everything between core/ (pure DSP) and platform/ (pure output).
 *
 * Two shapes of playback, because the loader's 1 MiB ceiling forces them:
 *
 *   Steady preset  -> one cycle-exact loop, submitted once, looping forever.
 *                     Zero CPU while it plays, which is the whole battery
 *                     story for a 45-minute session.
 *   Program        -> a chain of fixed-length chunks, each a parameter ramp.
 *                     Chunk N+1 is rendered while N plays and queued into the
 *                     backend's second slot, so the join costs only a play
 *                     call.
 *
 * Rendering is sliced across UI frames rather than done in one blocking call.
 * A ten-second chunk is a hundred thousand frames of double-precision DSP; on
 * a 800 MHz Cortex-A8 that is long enough to drop frames, and a stutter in the
 * UI at the moment audio starts is exactly the wrong first impression. Slicing
 * also gives the progress ring something honest to show.
 */

#ifndef ENTRAIN_ENGINE_H
#define ENTRAIN_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "core/program.h"

/* Seconds of audio per program chunk. Bounded by the loader ceiling (a chunk
   must fit in EN_TARGET_BYTES) and by the OS heap, which has to hold the
   render buffer and the loader's 1.2x copy at the same time. */
#define EN_CHUNK_SECONDS 10

/* Frames rendered per UI frame. Small enough to stay invisible at 60 Hz on the
   device, large enough that a chunk completes in well under a second. */
#define EN_RENDER_SLICE 4096

typedef enum {
    EN_SRC_NONE = 0,
    EN_SRC_PRESET,
    EN_SRC_PROGRAM,
    EN_SRC_USER,
    EN_SRC_FILE,     /* a multi-layer program loaded from a file */
    EN_SRC_LIVE      /* a preset the user is tuning by hand */
} en_source_kind_t;

void en_engine_init(void);
void en_engine_shutdown(void);

/* Call once per UI frame. Drives sliced rendering, chunk queueing, the sleep
   timer, and the audio backend's own tick. */
void en_engine_tick(void);

bool en_engine_play_preset(int index);
bool en_engine_play_program(int index);
bool en_engine_play_user(const en_user_program_t *up);

/* Play a segment table the caller owns and keeps alive until something else
   plays. Used by the measured programs, which are parsed out of a file into a
   buffer the UI holds rather than copied in here. */
bool en_engine_play_segs(const en_prog_seg_t *segs, int n_segs,
                         en_mode_t mode, const char *name, const char *detail);

void en_engine_stop(uint32_t fade_ms);
void en_engine_toggle_pause(void);

bool en_engine_is_playing(void);   /* audible right now */
bool en_engine_is_paused(void);
bool en_engine_is_active(void);    /* playing, paused, or preparing */

/* 0..1 while a render is in flight, so the UI can show a determinate ring
   rather than a spinner that means nothing. Negative when nothing is
   rendering. */
double en_engine_render_progress(void);

/* ---- live tune ---------------------------------------------------------- */

/* Nudge the beat and carrier. The realised values are re-planned immediately
   so the readout is truthful, but the re-render is deferred until the gesture
   settles — and the old loop keeps playing throughout. There is never a silent
   gap while retuning. */
void en_engine_live_adjust(double d_beat, double d_carrier);
void en_engine_live_reset(void);
bool en_engine_is_retuning(void);

/* ---- readouts ----------------------------------------------------------- */

en_source_kind_t en_engine_source(void);
const char *en_engine_title(void);
const char *en_engine_detail(void);
double      en_engine_beat(void);       /* realised, at the current instant */
double      en_engine_carrier(void);
en_band_t   en_engine_band(void);
en_mode_t   en_engine_mode(void);

double   en_engine_elapsed(void);       /* seconds */
double   en_engine_total(void);         /* 0 for an endless preset */
int      en_engine_seg_count(void);
double   en_engine_seg_start(int i);    /* seconds from the program's start */

/* ---- sleep timer -------------------------------------------------------- */

void     en_engine_set_sleep_timer(uint32_t seconds);   /* 0 turns it off */
uint32_t en_engine_sleep_timer(void);
uint32_t en_engine_sleep_remaining(void);

#endif /* ENTRAIN_ENGINE_H */
