/*
 * render.c — see render.h.
 */

#include "render.h"

void en_render_init(en_render_t *r, uint32_t sample_rate,
                    en_noise_kind_t noise, uint32_t seed)
{
    en_osc_init();
    r->sample_rate = sample_rate;
    for (int j = 0; j < EN_MAX_LAYERS; j++) {
        r->lay[j].l.phase = 0;    r->lay[j].l.step = 0;
        r->lay[j].r.phase = 0;    r->lay[j].r.step = 0;
        r->lay[j].gate.phase = 0; r->lay[j].gate.step = 0;
    }
    en_noise_init(&r->noise, noise, seed);
}

double en_softclip(double x)
{
    if (x <= -1.5) return -1.0;
    if (x >=  1.5) return  1.0;
    return x - (4.0 / 27.0) * x * x * x;
}

static int16_t to_s16(double x)
{
    double v = en_softclip(x) * 32767.0;
    if (v >  32767.0) v =  32767.0;
    if (v < -32768.0) v = -32768.0;
    return (int16_t)(v < 0.0 ? v - 0.5 : v + 0.5);
}

/* Raised-cosine gain, rising over [0,1]. Zero slope at both ends, which is what
   keeps a fade from clicking at the moment it finishes. */
static double raised_cos(double t)
{
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return 0.5 - 0.5 * en_sin_turns(0.25 + t * 0.5);
}

static double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

/* The isochronic gate. A raised cosine at the beat rate rather than a square
   wave: a square gate is all harmonics and clicks twice per cycle, which is
   exactly the artefact this app cannot afford. */
static double gate_value(uint32_t phase)
{
    /* cos(x) = sin(x + quarter turn) */
    double c = (double)en_sine_lookup(phase + 0x40000000u) / 32767.0;
    return 0.5 - 0.5 * c;
}

uint8_t en_segment_layers(const en_segment_t *seg,
                          en_layer_t *start, en_layer_t *end)
{
    if (seg->layers) {
        uint8_t n = seg->layers;
        if (n > EN_MAX_LAYERS) n = EN_MAX_LAYERS;
        for (uint8_t j = 0; j < n; j++) {
            start[j] = seg->lstart[j];
            end[j]   = seg->lend[j];
        }
        return n;
    }

    /* Single-layer: the legacy fields describe layer zero at unity gain. The
       tone level is NOT folded in here — it stays the master gain applied to
       the whole mix below, which is what makes the two forms exactly
       equivalent for one layer and keeps tone_level meaningful for many. */
    start[0].mode       = seg->mode;
    start[0].carrier_hz = seg->start.carrier_hz;
    start[0].beat_hz    = seg->start.beat_hz;
    start[0].level      = 1.0;

    end[0].mode       = seg->mode;
    end[0].carrier_hz = seg->end.carrier_hz;
    end[0].beat_hz    = seg->end.beat_hz;
    end[0].level      = 1.0;
    return 1;
}

static void render_inner(en_render_t *r, const en_segment_t *seg, int16_t *out,
                         int apply_fades)
{
    const uint32_t sr = seg->sample_rate ? seg->sample_rate : r->sample_rate;
    const uint32_t n = seg->frames;
    if (n == 0) return;

    const double fade_in_frames  = apply_fades ? seg->fade_in_s  * (double)sr : 0.0;
    const double fade_out_frames = apply_fades ? seg->fade_out_s * (double)sr : 0.0;
    const double inv_n = 1.0 / (double)n;

    en_layer_t ls[EN_MAX_LAYERS], le[EN_MAX_LAYERS];
    const uint8_t nlay = en_segment_layers(seg, ls, le);

    double level[EN_MAX_LAYERS];
    double master = seg->start.tone_level;
    double noise_level = seg->start.noise_level;

    /* The segment decides the noise kind, not whoever called en_render_init.
       Without this a preset with a pink bed renders through whatever generator
       the renderer happened to be initialised with — and if that was NONE, the
       bed is silent while the level says otherwise. Re-seed only on a real
       change, so a steady render keeps one continuous noise stream. */
    if (seg->noise != r->noise.kind)
        en_noise_init(&r->noise, seg->noise, 0x9E3779B9u);

    for (uint32_t i = 0; i < n; i += EN_CTRL_BLOCK) {
        uint32_t block = n - i;
        if (block > EN_CTRL_BLOCK) block = EN_CTRL_BLOCK;

        /* Control rate: recompute the ramped parameters and the phase steps
           once per block, then run the block with fixed steps. */
        double t = (double)i * inv_n;
        master      = lerp(seg->start.tone_level,  seg->end.tone_level,  t);
        noise_level = lerp(seg->start.noise_level, seg->end.noise_level, t);

        for (uint8_t j = 0; j < nlay; j++) {
            double carrier = lerp(ls[j].carrier_hz, le[j].carrier_hz, t);
            double beat    = lerp(ls[j].beat_hz,    le[j].beat_hz,    t);
            level[j]       = lerp(ls[j].level,      le[j].level,      t);

            switch (ls[j].mode) {
            case EN_MODE_BINAURAL:
            case EN_MODE_MONAURAL:
                en_osc_set_freq(&r->lay[j].l, carrier, sr);
                en_osc_set_freq(&r->lay[j].r, carrier + beat, sr);
                break;
            case EN_MODE_ISOCHRONIC:
                en_osc_set_freq(&r->lay[j].l, carrier, sr);
                en_osc_set_freq(&r->lay[j].gate, beat, sr);
                break;
            }
        }

        for (uint32_t k = 0; k < block; k++) {
            uint32_t idx = i + k;

            double env = 1.0;
            if (apply_fades) {
                if (fade_in_frames > 0.0 && (double)idx < fade_in_frames)
                    env = raised_cos((double)idx / fade_in_frames);
                if (fade_out_frames > 0.0) {
                    double left = (double)(n - 1 - idx);
                    if (left < fade_out_frames) {
                        double g = raised_cos(left / fade_out_frames);
                        if (g < env) env = g;
                    }
                }
            }

            /* Every layer is advanced every frame, including one whose gain has
               ramped to zero. Skipping a silent layer would stall its phase, and
               it would then re-enter at whatever phase it stopped at instead of
               where it would have been — a click at exactly the moment a layer
               fades back in, which is the join this renderer exists to avoid. */
            double l = 0.0, rr = 0.0;
            for (uint8_t j = 0; j < nlay; j++) {
                double a, b;
                switch (ls[j].mode) {
                case EN_MODE_BINAURAL:
                    a = (double)en_osc_next(&r->lay[j].l) / 32767.0;
                    b = (double)en_osc_next(&r->lay[j].r) / 32767.0;
                    l  += a * level[j];
                    rr += b * level[j];
                    break;
                case EN_MODE_MONAURAL: {
                    a = (double)en_osc_next(&r->lay[j].l) / 32767.0;
                    b = (double)en_osc_next(&r->lay[j].r) / 32767.0;
                    double m = 0.5 * (a + b) * level[j];
                    l  += m;
                    rr += m;
                    break;
                }
                case EN_MODE_ISOCHRONIC:
                default: {
                    a = (double)en_osc_next(&r->lay[j].l) / 32767.0;
                    double g = gate_value(r->lay[j].gate.phase);
                    r->lay[j].gate.phase += r->lay[j].gate.step;
                    double m = a * g * level[j];
                    l  += m;
                    rr += m;
                    break;
                }
                }
            }

            l  *= master * env;
            rr *= master * env;

            if (seg->noise != EN_NOISE_NONE && noise_level > 0.0) {
                /* One noise stream, same in both ears. Decorrelated noise would
                   widen the image and fight the binaural cue we are trying to
                   present cleanly. One bed for the whole mix, too, rather than
                   one per layer: three uncorrelated beds would be three times
                   the hiss for no added information. */
                double nz = en_noise_next(&r->noise) * noise_level * env;
                l  += nz;
                rr += nz;
            }

            out[2 * idx + 0] = to_s16(l  * EN_HEADROOM);
            out[2 * idx + 1] = to_s16(rr * EN_HEADROOM);
        }
    }
}

void en_render_segment(en_render_t *r, const en_segment_t *seg, int16_t *out)
{
    render_inner(r, seg, out, 1);
}

/* Fixed seed for looped renders, so a preset always renders to identical bytes
   and the render cache key stays honest. */
#define EN_LOOP_SEED 0x9E3779B9u

/* Frames of noise crossfade at the loop seam. 256 frames is 23 ms at 11025 Hz
   — long enough to hide the join, short enough that the buffer is 1 KB. */
#define EN_LOOP_XFADE 256

static void zero_phases(en_render_t *r)
{
    for (int j = 0; j < EN_MAX_LAYERS; j++) {
        r->lay[j].l.phase = 0;
        r->lay[j].r.phase = 0;
        r->lay[j].gate.phase = 0;
    }
}

void en_render_loop(en_render_t *r, const en_segment_t *seg, int16_t *out)
{
    /* A loop must render identically every time and must wrap onto itself, so
       it starts from a known phase rather than wherever the last segment left
       off. The caller guarantees whole cycle counts; see en_plan_loop. */
    zero_phases(r);
    en_noise_init(&r->noise, seg->noise, EN_LOOP_SEED);
    render_inner(r, seg, out, 0);

    /* The tones wrap exactly — whole cycle counts see to that. The noise does
       not: sample N-1 and sample 0 come from unrelated points in the stream,
       and for brown noise that is an audible step, not a hiss.
       Fix it by crossfading the stream's natural continuation past the end
       into the first few frames. After the blend, frame N-1 is followed by
       what the generator would genuinely have produced next, so the wrap is
       as continuous as any interior sample. */
    if (seg->noise != EN_NOISE_NONE && seg->start.noise_level > 0.0 &&
        seg->frames > 2 * EN_LOOP_XFADE) {

        float cont[EN_LOOP_XFADE];
        for (int k = 0; k < EN_LOOP_XFADE; k++)
            cont[k] = (float)en_noise_next(&r->noise);   /* continues past N */

        /* Regenerate the head of the stream — deterministic from the seed. */
        en_noise_t head;
        en_noise_init(&head, seg->noise, EN_LOOP_SEED);

        const uint32_t sr = seg->sample_rate ? seg->sample_rate : r->sample_rate;
        const double nlvl = seg->start.noise_level;
        const double master = seg->start.tone_level;

        en_layer_t ls[EN_MAX_LAYERS], le[EN_MAX_LAYERS];
        const uint8_t nlay = en_segment_layers(seg, ls, le);

        uint32_t step_l[EN_MAX_LAYERS], step_r[EN_MAX_LAYERS];
        uint32_t step_g[EN_MAX_LAYERS];
        for (uint8_t j = 0; j < nlay; j++) {
            step_l[j] = en_osc_step(ls[j].carrier_hz, sr);
            step_r[j] = en_osc_step(ls[j].carrier_hz + ls[j].beat_hz, sr);
            step_g[j] = en_osc_step(ls[j].beat_hz, sr);
        }

        for (int k = 0; k < EN_LOOP_XFADE; k++) {
            double w = (double)k / (double)EN_LOOP_XFADE;   /* 0 -> 1 */
            double nz = ((double)cont[k] * (1.0 - w)
                         + en_noise_next(&head) * w) * nlvl;

            /* Recompute this frame's tone. A loop is steady, so the phase at
               frame k is exactly k * step and no ramp state is needed. */
            double l = 0.0, rr = 0.0;
            for (uint8_t j = 0; j < nlay; j++) {
                double a, b;
                switch (ls[j].mode) {
                case EN_MODE_BINAURAL:
                    a = (double)en_sine_lookup((uint32_t)k * step_l[j]) / 32767.0;
                    b = (double)en_sine_lookup((uint32_t)k * step_r[j]) / 32767.0;
                    l  += a * ls[j].level;
                    rr += b * ls[j].level;
                    break;
                case EN_MODE_MONAURAL: {
                    a = (double)en_sine_lookup((uint32_t)k * step_l[j]) / 32767.0;
                    b = (double)en_sine_lookup((uint32_t)k * step_r[j]) / 32767.0;
                    double m = 0.5 * (a + b) * ls[j].level;
                    l  += m;
                    rr += m;
                    break;
                }
                case EN_MODE_ISOCHRONIC:
                default: {
                    a = (double)en_sine_lookup((uint32_t)k * step_l[j]) / 32767.0;
                    double m = a * gate_value((uint32_t)k * step_g[j]) * ls[j].level;
                    l  += m;
                    rr += m;
                    break;
                }
                }
            }

            out[2 * k + 0] = to_s16((l  * master + nz) * EN_HEADROOM);
            out[2 * k + 1] = to_s16((rr * master + nz) * EN_HEADROOM);
        }
    }

    zero_phases(r);
}

const char *en_mode_name(en_mode_t mode)
{
    switch (mode) {
    case EN_MODE_BINAURAL:   return "Binaural";
    case EN_MODE_ISOCHRONIC: return "Isochronic";
    case EN_MODE_MONAURAL:   return "Monaural";
    default:                 return "?";
    }
}
