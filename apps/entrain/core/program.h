/*
 * program.h — bands, presets, programs, and the loop planner.
 *
 * The loop planner is the piece that makes the whole app possible on this
 * device. RetailOS refuses to load any sound file over 1 MiB (see
 * AUDIO_NOTES.md), so a steady preset has to fit its entire seamless loop into
 * that budget, and a loop is only seamless if it contains a whole number of
 * cycles of both carriers.
 *
 * The rule, and en_plan_loop implements exactly it:
 *
 *   - The beat is the point of the preset. Never move it to make the maths
 *     work.
 *   - The carrier is arbitrary within a wide range. 200.00 -> 200.05 Hz is
 *     inaudible. Move that instead.
 *   - Whatever comes out, store and display the REALISED beat, not the
 *     requested one.
 */

#ifndef ENTRAIN_PROGRAM_H
#define ENTRAIN_PROGRAM_H

#include <stdint.h>
#include "render.h"

/* ---- bands ------------------------------------------------------------- */

typedef enum {
    EN_BAND_DELTA = 0,   /* 0.5 - 4  Hz */
    EN_BAND_THETA,       /* 4   - 8  Hz */
    EN_BAND_ALPHA,       /* 8   - 13 Hz */
    EN_BAND_BETA,        /* 13  - 30 Hz */
    EN_BAND_GAMMA,       /* 30  - 100 Hz */
    EN_BAND_COUNT
} en_band_t;

en_band_t   en_band_of(double beat_hz);
const char *en_band_name(en_band_t band);
uint32_t    en_band_color(en_band_t band);   /* 0xRRGGBB */

/* ---- loop planning ------------------------------------------------------ */

/* The loader's hard ceiling. Anything at or over this is rejected outright. */
#define EN_MAX_LOAD_BYTES 1048576u
/* What we actually aim for. Every load allocates 1.2x the file plus 8 KB from
   the shared OS heap, through an allocator that panics rather than failing, so
   sitting right on the ceiling is not worth the risk. */
#define EN_TARGET_BYTES    786432u

typedef struct {
    uint32_t sample_rate;
    uint32_t frames;        /* N */
    uint32_t cycles_l;      /* whole cycles of f_l in the loop */
    uint32_t cycles_beat;   /* cycles_r - cycles_l */
    double   t_seconds;     /* frames / sample_rate */
    double   f_l;           /* realised left carrier */
    double   f_r;           /* realised right carrier */
    double   beat_hz;       /* realised beat = cycles_beat / t_seconds */
    double   beat_error;    /* realised - requested */
    uint32_t bytes;         /* the resulting file size */
} en_loop_plan_t;

/* Sample rates we are willing to ship, best first. Lower rates buy longer
   loops for the same bytes, and a 400 Hz carrier does not care: 11025 Hz still
   leaves a Nyquist of 5.5 kHz. Which of these the device actually accepts is
   what harness T1 measures. */
extern const uint32_t EN_RATES[];
extern const int      EN_RATES_COUNT;

/* Find the best cycle-exact loop for this beat within `max_bytes`.
   Returns 1 on success, 0 if nothing fits (a beat too slow for the budget).
   Tries every rate in `rates` and keeps the one with the smallest beat error,
   breaking ties toward the longest loop. */
int en_plan_loop(double beat_hz, double carrier_hz,
                 const uint32_t *rates, int n_rates,
                 uint32_t max_bytes, en_loop_plan_t *out);

/* Fill a segment that renders `plan` as a seamless loop. */
void en_plan_to_segment(const en_loop_plan_t *plan, en_mode_t mode,
                        en_noise_kind_t noise, double tone_level,
                        double noise_level, en_segment_t *out);

/* ---- presets (steady) --------------------------------------------------- */

typedef struct {
    const char     *name;
    const char     *detail;      /* what it does, technically. No claims. */
    double          beat_hz;     /* requested */
    double          carrier_hz;  /* requested */
    en_mode_t       mode;
    en_noise_kind_t noise;
    double          noise_level;
} en_preset_t;

const en_preset_t *en_presets(int *count);

/* ---- programs (timelines) ----------------------------------------------- */

/*
 * How a segment glides from its start value to its end value.
 *
 * LINEAR is what the hand-written programs have always done and is what a
 * segment gets when the field is left out, so nothing that existed before this
 * enum did behaves differently.
 *
 * SMOOTH is smoothstep: zero slope at both ends. It matters where a long
 * timeline is a chain of glides and holds, because a linear glide meeting a
 * hold has a corner in it — the beat stops moving all at once — and over a
 * forty-minute descent that corner is the one moment you notice. The imported
 * suites were authored with it and are ported with it.
 */
typedef enum {
    EN_INTERP_LINEAR = 0,
    EN_INTERP_SMOOTH
} en_interp_t;

/*
 * How many layers a stored program table can carry.
 *
 * Separate from EN_MAX_LAYERS, which is what the renderer can sound, because
 * this one sits inside every segment of every table and so is paid for in
 * flash 173 times over. At the renderer's sixteen that is 120 KB of program
 * tables; at eight it is 65 KB, on a 465 KB binary.
 *
 * This is NOT a budget that anything gets dropped to fit. The measured
 * material uses five concurrent carrier families, this is sized above that
 * with room, and the assert below fails the build rather than silently
 * truncating if a table ever needs more. If analysis finds a sixth or a
 * ninth, raise this.
 */
#define EN_PROG_MAX_LAYERS 8

#if EN_PROG_MAX_LAYERS > EN_MAX_LAYERS
#error "a program cannot carry more layers than the renderer can sound"
#endif

/* One layer's contribution across a segment. Carrier is fixed for the segment;
   beat and level ramp. See render.h for what `level` means — absolute peak
   amplitude, relative to a primary layer conventionally carried at 1.0. */
typedef struct {
    double carrier_hz;
    double beat_start,  beat_end;
    double level_start, level_end;
} en_prog_layer_t;

typedef struct {
    double          beat_start;
    double          beat_end;
    double          carrier_hz;
    uint32_t        seconds;
    en_noise_kind_t noise;
    double          noise_level;

    /* Zero layers means the four fields above describe a single layer, which
       is how every hand-written program and every user program is expressed.
       Non-zero means `layer` describes the mix and those fields are ignored
       except that beat_start/beat_end stay the segment's PRIMARY beat — the
       one the Now Playing readout shows and en_program_band colours by. Keep
       them in step with layer[0] when writing a multi-layer table. */
    en_interp_t     interp;
    uint8_t         layers;
    en_prog_layer_t layer[EN_PROG_MAX_LAYERS];
} en_prog_seg_t;

/*
 * Which shelf a program sits on in the library.
 *
 * Not a cosmetic split. The hand-written timelines are a handful of numbers
 * each and are meant to be read, changed and argued with; the imported suites
 * are ported schedules that should stay exactly as their source authored them.
 * Mixing them in one list would invite editing the second kind as casually as
 * the first.
 */
typedef enum {
    EN_GROUP_PROGRAM = 0,   /* the hand-written timelines */
    EN_GROUP_SUITE          /* imported multi-layer suites */
} en_group_t;

typedef struct {
    const char          *name;
    const char          *detail;
    en_mode_t            mode;
    const en_prog_seg_t *segs;
    int                  n_segs;
    en_group_t           group;
} en_program_t;

/* Programs in `group`, in table order. Pass a null `out` to count only.
   Returns how many there are; fills at most `cap` indices. */
int en_programs_in_group(en_group_t group, int *out, int cap);

const en_program_t *en_programs(int *count);

uint32_t  en_program_seconds(const en_program_t *p);
/* Band of the program's longest segment — what the list chip should colour. */
en_band_t en_program_band(const en_program_t *p);
/* Beat frequency at `t` seconds in, for the Now Playing readout. */
double    en_program_beat_at(const en_program_t *p, double t_seconds);
/* Index of the segment covering `t`, or -1 past the end. */
int       en_program_seg_at(const en_program_t *p, double t_seconds);

/*
 * Evaluate a timeline at `t` seconds into renderer layer form, and report the
 * noise bed in force there. Returns the layer count, always at least one.
 *
 * Takes the segment array rather than an en_program_t because a user program
 * holds the same segments in a different wrapper, and the engine has to
 * evaluate both through one path or the two would drift apart.
 *
 * `mode` is the program's mode and is stamped onto every layer: a program is
 * binaural or isochronic as a whole, never half of each — mixing them would
 * put a beat in the signal and a beat in the listener at once, which is two
 * different beats claiming to be the same one.
 */
uint8_t en_segs_layers_at(const en_prog_seg_t *segs, int n_segs, double t,
                          en_mode_t mode, en_layer_t *out,
                          en_noise_kind_t *noise, double *noise_level);

/* ---- user programs loaded from disk ------------------------------------- */

#define EN_USER_MAX_SEGS 16
#define EN_NAME_MAX      32

typedef struct {
    char            name[EN_NAME_MAX];
    en_mode_t       mode;
    double          carrier_hz;
    en_noise_kind_t noise;
    double          noise_level;
    en_prog_seg_t   segs[EN_USER_MAX_SEGS];
    int             n_segs;
} en_user_program_t;

typedef enum {
    EN_PARSE_OK = 0,
    EN_PARSE_BAD_KEY = -1,
    EN_PARSE_BAD_VALUE = -2,
    EN_PARSE_TOO_MANY_SEGS = -3,
    EN_PARSE_NO_SEGS = -4
} en_parse_result_t;

/* Parse the user-program text format documented in README.md. Read-only over
   `text`; no allocation, no stdio. On failure returns a negative code and sets
   *err_line (1-based) to the offending line. */
int en_parse_program(const char *text, uint32_t len,
                     en_user_program_t *out, int *err_line);

const char *en_parse_error_text(int code);

#endif /* ENTRAIN_PROGRAM_H */
