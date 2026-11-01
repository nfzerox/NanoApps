/*
 * render.h — turn a parameter ramp into interleaved 16-bit stereo PCM.
 *
 * The renderer holds its oscillator phases across calls. That is the whole
 * reason it is a struct and not a function: a program is a chain of segments,
 * and if segment N+1 restarted its phase at zero, every join would click. Call
 * en_render_segment() repeatedly on one en_render_t and the joins are
 * continuous by construction.
 *
 * Parameters ramp linearly from `start` to `end` across the segment, updated
 * once per 64-sample control block — 5.8 ms at 11025 Hz, far finer than the
 * 50-200 ms glide the UI asks for and far coarser than recomputing a phase step
 * per sample.
 *
 * A segment may carry up to EN_MAX_LAYERS carrier pairs sounding together,
 * each with its own mode, beat and gain, over one shared noise bed. See the
 * layer block below. Every phase in every layer is held across calls for the
 * same reason the single-layer phases always were.
 */

#ifndef ENTRAIN_RENDER_H
#define ENTRAIN_RENDER_H

#include <stdint.h>
#include "osc.h"
#include "noise.h"

/* Control-block size. Anything from 16 to 256 is defensible; 64 keeps the
   glide smooth without recomputing steps every sample. */
#define EN_CTRL_BLOCK 64

/* Peak headroom for the mix. -3 dB, so two loops overlapping at a crossfade
   cannot sum past full scale and hit the soft clipper. */
#define EN_HEADROOM 0.707

typedef enum {
    /* Different frequency to each ear. The beat exists only in the listener;
       nothing in the signal oscillates at the beat rate. Headphones required. */
    EN_MODE_BINAURAL = 0,
    /* One carrier, gated at the beat rate. The modulation is in the signal, so
       it survives speakers. */
    EN_MODE_ISOCHRONIC,
    /* Both tones in both ears. The beat is a real amplitude beat in the air. */
    EN_MODE_MONAURAL
} en_mode_t;

typedef struct {
    double carrier_hz;    /* binaural: the left frequency. Others: the carrier */
    double beat_hz;
    double tone_level;    /* 0..1 */
    double noise_level;   /* 0..1 */
} en_params_t;

/* ---- layers -------------------------------------------------------------
 *
 * A layer is one carrier pair with its own mode, beat and gain. Most of what
 * this app plays is a single layer, but a long practice program is typically
 * two or three sounding at once: a main training carrier, a quiet warm one
 * below it, and a quieter support above. They are separate layers rather than
 * separate segments because they overlap in time and each has its own beat.
 *
 * `level` is an ABSOLUTE peak amplitude, not a share of a normalised mix.
 * Layers sum, and it is the caller's job to keep the sum sane — the source
 * material this exists to render specifies peak amplitudes directly (0.045,
 * 0.070, 0.018 and so on, summing well under one). Normalising here would
 * silently rescale a mix whose balance is the whole point of it. The soft
 * clipper remains as a backstop and nothing more.
 *
 * `start.tone_level` stays the master gain over the summed layers, so a
 * single-layer segment at unity layer gain is bit-for-bit what it always was,
 * and a multi-layer one is still subject to the same headroom discipline.
 */

/*
 * The ceiling, not a budget. It exists only so the arrays can be fixed-size in
 * a core that does not allocate; it is not a claim about how many layers are
 * worth having, and nothing may be discarded to fit under it.
 *
 * Measured material decides the number. The imported practice recordings carry
 * five concurrent carrier families, and a measurement that found six would
 * mean six. Sixteen is chosen to be past anything the analysis has turned up
 * rather than at it.
 *
 * The cost is bounded and per-segment, not per-ceiling: render_inner loops over
 * seg->layers, so a two-layer program does two layers' work whatever this says.
 * What this does set is the size of en_segment_t, which job_step copies - see
 * the note there.
 */
#define EN_MAX_LAYERS 16

typedef struct {
    en_mode_t mode;
    double    carrier_hz;   /* binaural: the left frequency */
    double    beat_hz;
    double    level;        /* absolute peak amplitude, 0..1; layers SUM */
} en_layer_t;

typedef struct {
    uint32_t        sample_rate;
    en_mode_t       mode;
    en_noise_kind_t noise;
    en_params_t     start;
    en_params_t     end;      /* equal to start for a steady segment */
    uint32_t        frames;
    double          fade_in_s;   /* raised cosine; 0 for a mid-program segment */
    double          fade_out_s;

    /* Multi-layer form. When `layers` is zero the segment is single-layer and
       `mode`, `start` and `end` above describe it exactly as they always have,
       so every existing caller keeps working untouched. When it is non-zero,
       `lstart`/`lend` describe the layers and `start.carrier_hz`,
       `start.beat_hz` and `start.tone_level` are ignored — only the noise
       levels are still read from `start`/`end`, because the bed is one stream
       shared by the whole mix rather than a property of any one layer. */
    uint8_t         layers;
    en_layer_t      lstart[EN_MAX_LAYERS];
    en_layer_t      lend[EN_MAX_LAYERS];
} en_segment_t;

typedef struct {
    uint32_t   sample_rate;
    /* One oscillator triple per layer: left/sole carrier, right carrier
       (binaural and monaural), and the isochronic amplitude gate. */
    struct {
        en_osc_t l, r, gate;
    } lay[EN_MAX_LAYERS];
    en_noise_t noise;
} en_render_t;

/* Expand whatever form the segment is in into the uniform layer arrays, and
   return the layer count (always at least 1). A single-layer segment maps to
   one layer at unity gain, its tone_level staying the master. Exposed because
   the tests assert that the legacy mapping renders identically to the explicit
   one-layer segment it produces. */
uint8_t en_segment_layers(const en_segment_t *seg,
                          en_layer_t *start, en_layer_t *end);

void en_render_init(en_render_t *r, uint32_t sample_rate,
                    en_noise_kind_t noise, uint32_t seed);

/* Render `seg->frames` frames into `out`, which must hold frames*2 int16.
   Advances the renderer's phases, so the next call continues seamlessly. */
void en_render_segment(en_render_t *r, const en_segment_t *seg, int16_t *out);

/* Render a seamless loop: no fades, and the phases are seeded from zero and
   land back on zero at the wrap, provided the caller passed cycle-exact
   frequencies (see program.h's loop planner). Leaves the renderer's phases
   where the loop started, so calling it twice gives identical bytes.

   Multi-layer segments render correctly here, but note what "cycle-exact"
   costs: EVERY carrier in EVERY layer has to complete a whole number of cycles
   in the same number of frames, and en_plan_loop solves that for one layer
   only. A looped multi-layer segment is therefore the caller's problem to make
   seamless. Nothing in the app asks for one: looping exists for the RetailOS
   backend, which can only be handed a file, and its presets are single-layer.
   Multi-layer material is streamed, where the question does not arise. */
void en_render_loop(en_render_t *r, const en_segment_t *seg, int16_t *out);

const char *en_mode_name(en_mode_t mode);

/* Cubic soft clip: unity slope through zero, saturating smoothly at +/-1.
   Exposed for the tests, which assert the output never wraps. */
double en_softclip(double x);

#endif /* ENTRAIN_RENDER_H */
