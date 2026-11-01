/*
 * engine.c — see engine.h.
 */

#include "engine.h"

#include <string.h>

#include "core/render.h"
#include "core/osc.h"
#include "platform/audio.h"
#include "platform/sys.h"

/* Queue the next chunk this many seconds before the current one runs out. The
   device needs time to write a WAV and hand it to the loader; the host needs
   almost none. Being early costs nothing, being late costs a gap. */
#define EN_LOOKAHEAD_S 3.0

/* Program fades. Long enough to be a fade rather than a switch. */
#define EN_PROGRAM_FADE_S 2.0

/* How long the live-tune gesture must be still before the re-render starts.
   Re-rendering on every pixel of a drag would thrash; waiting for the gesture
   to settle costs nothing because the old loop is still playing. */
#define EN_RETUNE_SETTLE_MS 400

typedef struct {
    int16_t *buf;
    uint32_t frames;      /* total frames this job will produce */
    uint32_t advance;     /* where the next buffer takes over; 0 = all of it */
    uint32_t done;        /* frames rendered so far */
    uint32_t rate;
    bool     active;
    bool     loop;        /* submit as a loop rather than queueing a chunk */
    bool     first;       /* submit (replace) rather than queue */
    double   program_t0;  /* where this chunk sits in the program, seconds */
    char     key[64];
    en_segment_t seg;
} job_t;

static struct {
    en_source_kind_t source;
    int              index;
    en_user_program_t user;      /* valid when source is EN_SRC_USER */

    /* Valid when source is EN_SRC_FILE. Borrowed, not owned - see
       en_engine_play_segs for why, and for what the caller must guarantee. */
    const en_prog_seg_t *file_segs;
    int                  file_segs_n;

    /* what is playing */
    char        title[48];
    char        detail[96];
    en_mode_t   mode;
    en_loop_plan_t plan;         /* presets and live tune */
    double      total_s;         /* 0 for an endless preset */

    /* program cursor */
    double      render_pos_s;    /* how much of the program has been rendered */

    /* live tune */
    double      live_beat, live_carrier;
    double      base_beat, base_carrier;
    uint32_t    retune_at_ms;    /* 0 = nothing pending */
    bool        retuning;

    en_render_t rnd;
    job_t       job;

    uint32_t    sleep_timer_s;
    uint32_t    sleep_started_ms;
    bool        sleep_firing;
} E;

/* ---- streaming --------------------------------------------------------- *
 *
 * When the backend can take a continuous feed, everything above — the loop
 * planner's file budget, the chunk queue, the WAV cache — is bypassed. The
 * audio thread pulls frames and the renderer produces them on demand, so a
 * steady preset is one unbroken tone rather than a loop with a seam every
 * eighteen seconds, and a program's ramp is genuinely continuous.
 *
 * The audio thread reads parameters that the UI thread writes. Rather than a
 * lock the parameters are double-buffered and the slot index is flipped after
 * the write, so the puller always reads a slot nobody is writing. A torn
 * double here would be an audible blip, which is why it is not simply left to
 * chance. */

/* The rate the backend asks for. The DSP is rate-agnostic, so this follows the
   output path rather than the maths: 44.1k on a desktop, and half that on the
   iPod, where it halves both the render cost and the buffers for carriers that
   never go above a few hundred hertz. */
static uint32_t stream_rate(void)
{
    uint32_t r = en_audio_preferred_rate();
    return r ? r : 44100u;
}

typedef struct {
    double          carrier_hz;
    double          beat_hz;
    double          tone_level;
    double          noise_level;
    en_mode_t       mode;
    en_noise_kind_t noise;
} stream_params_t;

static stream_params_t  S_params[2];
static volatile int     S_slot;
static double           S_t;         /* audio thread only: seconds emitted */
static double           S_total;     /* 0 for an endless preset */
static en_render_t      S_rnd;       /* audio thread only */
static int              S_is_program;

static void publish_params(const stream_params_t *p)
{
    int next = S_slot ? 0 : 1;
    S_params[next] = *p;
    S_slot = next;
}

/* ---- small helpers ------------------------------------------------------ */

static void copy_str(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void append_num(char *dst, int cap, double v, int decimals)
{
    int n = 0;
    while (dst[n] && n < cap - 1) n++;

    long scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    long whole = (long)v;
    long frac = (long)((v - (double)whole) * (double)scale + 0.5);
    if (frac >= scale) { whole++; frac -= scale; }

    char tmp[24];
    int t = 0;
    if (whole == 0) tmp[t++] = '0';
    while (whole > 0 && t < (int)sizeof tmp) { tmp[t++] = (char)('0' + whole % 10); whole /= 10; }
    while (t > 0 && n < cap - 1) dst[n++] = tmp[--t];

    if (decimals > 0 && n < cap - 1) {
        dst[n++] = '.';
        for (int i = decimals - 1; i >= 0 && n < cap - 1; i--) {
            long d = frac;
            for (int k = 0; k < i; k++) d /= 10;
            dst[n++] = (char)('0' + (d % 10));
        }
    }
    dst[n] = 0;
}

/* Cache key: the realised parameters, not the requested ones. Two presets that
   plan to the same loop legitimately share a cached file. */
static void make_key(char *out, int cap, const en_loop_plan_t *p,
                     en_mode_t mode, en_noise_kind_t noise, double nlevel)
{
    out[0] = 0;
    copy_str(out, "l", cap);
    append_num(out, cap, (double)p->sample_rate, 0);
    copy_str(out + strlen(out), "_", cap - (int)strlen(out));
    append_num(out, cap, (double)p->frames, 0);
    copy_str(out + strlen(out), "_", cap - (int)strlen(out));
    append_num(out, cap, p->f_l, 3);
    copy_str(out + strlen(out), "_", cap - (int)strlen(out));
    append_num(out, cap, p->f_r, 3);
    copy_str(out + strlen(out), "_m", cap - (int)strlen(out));
    append_num(out, cap, (double)mode, 0);
    copy_str(out + strlen(out), "n", cap - (int)strlen(out));
    append_num(out, cap, (double)noise, 0);
    append_num(out, cap, nlevel * 100.0, 0);
}

/* ---- job management ------------------------------------------------------ */

static void job_cancel(void)
{
    en_sys_free(E.job.buf);
    E.job.buf = NULL;
    E.job.active = false;
    E.job.done = 0;
}

static bool job_start(const en_segment_t *seg, bool loop, bool first,
                      const char *key, double program_t0)
{
    job_cancel();
    if (seg->frames == 0) return false;

    /* The master buffer is the clean signal. Crossfading is the backend's job:
       the device plays this out in short windows and shapes each join itself,
       and the host streams it sample-exactly and needs no shaping at all.
       Fading here as well would shape the loop head twice. */
    E.job.buf = en_sys_alloc(seg->frames * 4u);
    if (!E.job.buf) return false;

    E.job.seg = *seg;
    E.job.frames = seg->frames;
    E.job.advance = 0;
    E.job.done = 0;
    E.job.rate = seg->sample_rate;
    E.job.loop = loop;
    E.job.first = first;
    E.job.active = true;
    E.job.program_t0 = program_t0;
    copy_str(E.job.key, key ? key : "", (int)sizeof E.job.key);

    if (loop) {
        /* A loop must start from a known phase and render deterministically,
           so it is produced in one go by the dedicated path rather than
           sliced. It is also the case where the user is waiting, so it wants
           to be quick, not smooth. */
        en_render_init(&E.rnd, seg->sample_rate, seg->noise, 1);
        en_render_loop(&E.rnd, seg, E.job.buf);
        E.job.done = E.job.frames;
    }
    return true;
}

/* Apply the program's overall fade-in and fade-out to a rendered chunk. The
   fades are short and the chunks are long, so a fade always lies inside one
   chunk; scaling after the fact keeps the slicing simple. */
static void apply_program_fades(int16_t *buf, uint32_t frames, uint32_t rate,
                                double t0, double total)
{
    if (total <= 0.0) return;
    for (uint32_t i = 0; i < frames; i++) {
        double t = t0 + (double)i / (double)rate;
        double g = 1.0;
        if (t < EN_PROGRAM_FADE_S)
            g = t / EN_PROGRAM_FADE_S;
        if (t > total - EN_PROGRAM_FADE_S) {
            double h = (total - t) / EN_PROGRAM_FADE_S;
            if (h < g) g = h;
        }
        if (g >= 1.0) continue;
        if (g < 0.0) g = 0.0;
        /* raised cosine, so the fade has no corner at either end */
        g = 0.5 - 0.5 * en_sin_turns(0.25 + g * 0.5);
        buf[2 * i + 0] = (int16_t)((double)buf[2 * i + 0] * g);
        buf[2 * i + 1] = (int16_t)((double)buf[2 * i + 1] * g);
    }
}

static void job_finish(void)
{
    if (E.source == EN_SRC_PROGRAM || E.source == EN_SRC_USER)
        apply_program_fades(E.job.buf, E.job.frames, E.job.rate,
                            E.job.program_t0, E.total_s);

    if (E.job.first)
        en_audio_submit(E.job.key, E.job.buf, E.job.frames, E.job.advance,
                        E.job.rate, E.job.loop);
    else
        en_audio_queue(E.job.key, E.job.buf, E.job.frames, E.job.advance,
                       E.job.rate);

    job_cancel();
    E.retuning = false;
}

/* Render one slice. Slicing works because the renderer carries its phase
   across calls, so a sub-segment picks up exactly where the last left off —
   the same property that makes program segment joins click-free. */
static double slerp(double a, double b, double u)
{
    return a + (b - a) * u;
}

static void job_step(void)
{
    if (!E.job.active) return;

    /* Only non-looped jobs are sliced; a loop is rendered whole in job_start,
       tail included, and arrives here already complete. */
    uint32_t left = E.job.frames - E.job.done;
    if (left == 0) { job_finish(); return; }

    uint32_t n = left < EN_RENDER_SLICE ? left : EN_RENDER_SLICE;

    double u0 = (double)E.job.done / (double)E.job.frames;
    double u1 = (double)(E.job.done + n) / (double)E.job.frames;

    const en_segment_t *src = &E.job.seg;

    /* Static rather than automatic. en_segment_t carries two arrays of
       EN_MAX_LAYERS layers, so at sixteen it is over a kilobyte, and this
       runs on the device's main thread where a kilobyte of stack per call is
       not worth spending to say `slice` is local. Nothing re-enters here. */
    static en_segment_t slice;
    slice = *src;
    slice.frames = n;
    slice.fade_in_s = 0.0;
    slice.fade_out_s = 0.0;

    /* Every ramped quantity is sampled at this slice's own two points in the
       chunk. tone_level and noise_level used to be copied whole, so each slice
       ramped them across the entire chunk's range rather than its own share -
       a sawtooth, had either ever actually ramped. Neither does today, which
       is why it was invisible; it is wrong all the same and a multi-layer
       program is the first thing here whose gains really do move. */
    slice.start.carrier_hz  = slerp(src->start.carrier_hz,  src->end.carrier_hz,  u0);
    slice.start.beat_hz     = slerp(src->start.beat_hz,     src->end.beat_hz,     u0);
    slice.start.tone_level  = slerp(src->start.tone_level,  src->end.tone_level,  u0);
    slice.start.noise_level = slerp(src->start.noise_level, src->end.noise_level, u0);
    slice.end.carrier_hz    = slerp(src->start.carrier_hz,  src->end.carrier_hz,  u1);
    slice.end.beat_hz       = slerp(src->start.beat_hz,     src->end.beat_hz,     u1);
    slice.end.tone_level    = slerp(src->start.tone_level,  src->end.tone_level,  u1);
    slice.end.noise_level   = slerp(src->start.noise_level, src->end.noise_level, u1);

    for (uint8_t j = 0; j < src->layers; j++) {
        slice.lstart[j].carrier_hz =
            slerp(src->lstart[j].carrier_hz, src->lend[j].carrier_hz, u0);
        slice.lstart[j].beat_hz =
            slerp(src->lstart[j].beat_hz, src->lend[j].beat_hz, u0);
        slice.lstart[j].level =
            slerp(src->lstart[j].level, src->lend[j].level, u0);
        slice.lend[j].carrier_hz =
            slerp(src->lstart[j].carrier_hz, src->lend[j].carrier_hz, u1);
        slice.lend[j].beat_hz =
            slerp(src->lstart[j].beat_hz, src->lend[j].beat_hz, u1);
        slice.lend[j].level =
            slerp(src->lstart[j].level, src->lend[j].level, u1);
    }

    en_render_segment(&E.rnd, &slice, E.job.buf + (size_t)E.job.done * 2);
    E.job.done += n;

    if (E.job.done >= E.job.frames) job_finish();
}

/* ---- program chunking ---------------------------------------------------- */

/* Parameters of the program at a given instant. */
/* Whichever timeline is playing, as a plain segment array. Built-in programs
   and user programs hold the same segments in different wrappers, and the
   engine has to evaluate both through one path or they drift apart. */
static const en_prog_seg_t *program_segs(int *n_out)
{
    if (E.source == EN_SRC_FILE) {
        *n_out = E.file_segs_n;
        return E.file_segs;
    }
    if (E.source == EN_SRC_USER) {
        *n_out = E.user.n_segs;
        return E.user.segs;
    }
    int count;
    const en_program_t *ps = en_programs(&count);
    *n_out = ps[E.index].n_segs;
    return ps[E.index].segs;
}

/* Evaluate the timeline at `t` into layer form. Returns the layer count. */
static uint8_t program_layers_at(double t, en_layer_t *lay,
                                 en_noise_kind_t *noise, double *nlevel)
{
    int n;
    const en_prog_seg_t *segs = program_segs(&n);
    return en_segs_layers_at(segs, n, t, E.mode, lay, noise, nlevel);
}

static void program_params_at(double t, double *beat, double *carrier,
                              en_noise_kind_t *noise, double *nlevel)
{
    en_layer_t lay[EN_MAX_LAYERS];
    program_layers_at(t, lay, noise, nlevel);
    /* Layer zero is the primary by convention — see program.h. */
    *beat = lay[0].beat_hz;
    *carrier = lay[0].carrier_hz;
}

/* Queue the next slice of the program timeline, if any is left. */
static bool start_next_chunk(bool first)
{
    if (E.render_pos_s >= E.total_s) return false;

    double t0 = E.render_pos_s;
    double dur = E.total_s - t0;
    if (dur > (double)EN_CHUNK_SECONDS) dur = (double)EN_CHUNK_SECONDS;

    uint32_t rate = EN_RATES[0];
    uint32_t frames = (uint32_t)(dur * (double)rate);
    if (frames == 0) return false;

    double nl0, nl1;
    en_noise_kind_t nk0, nk1;

    en_segment_t seg;
    memset(&seg, 0, sizeof seg);
    seg.sample_rate = rate;
    seg.mode = E.mode;
    seg.frames = frames;

    uint8_t n0 = program_layers_at(t0, seg.lstart, &nk0, &nl0);
    uint8_t n1 = program_layers_at(t0 + dur, seg.lend, &nk1, &nl1);
    seg.layers = n0 < n1 ? n0 : n1;

    seg.noise = nk0;
    seg.start.tone_level = 0.8;
    seg.start.noise_level = nl0;
    seg.end.tone_level = 0.8;
    seg.end.noise_level = nl1;
    seg.start.carrier_hz = seg.lstart[0].carrier_hz;
    seg.start.beat_hz = seg.lstart[0].beat_hz;
    seg.end.carrier_hz = seg.lend[0].carrier_hz;
    seg.end.beat_hz = seg.lend[0].beat_hz;

    /* Alternate between two keys. A program chunk is not worth caching — it is
       unique to its position in the timeline — but it does need a name of its
       own, because the backend skips rewriting a file that is already the right
       size and consecutive chunks are the same length. Sharing one name meant
       chunk N+1 was never written and the descriptor played chunk N again. Two
       names suffice: only two chunks are ever live. */
    static int chunk_parity;
    const char *chunk_key = chunk_parity ? "chunk1" : "chunk0";
    chunk_parity ^= 1;

    if (!job_start(&seg, false, first, chunk_key, t0)) return false;
    E.render_pos_s = t0 + dur;
    return true;
}

/* Build the segment describing [t, t + span) of whatever is playing. */
static void stream_segment(en_segment_t *seg, const stream_params_t *p,
                           double t, double span)
{
    memset(seg, 0, sizeof *seg);
    seg->sample_rate = stream_rate();
    seg->mode = p->mode;
    seg->noise = p->noise;

    if (S_is_program) {
        /* Evaluate the timeline across exactly this block, so the ramp stays
           continuous over every block boundary and every segment join. Both
           ends go through the layer evaluator, which is what carries a
           multi-layer program's per-layer gain ramps into the renderer. */
        double nl0, nl1;
        en_noise_kind_t nk0, nk1;
        uint8_t n0 = program_layers_at(t, seg->lstart, &nk0, &nl0);
        uint8_t n1 = program_layers_at(t + span, seg->lend, &nk1, &nl1);

        /* A block that straddles a join between segments with different layer
           counts takes the smaller: ramping a layer that only exists at one
           end would ramp it from uninitialised memory. Nothing shipped does
           this - the suites keep one layer count throughout - but a user
           program need not, and silence is the right failure. */
        seg->layers = n0 < n1 ? n0 : n1;

        seg->noise = nk0;
        seg->start.tone_level  = p->tone_level;
        seg->start.noise_level = nl0;
        seg->end.tone_level    = p->tone_level;
        seg->end.noise_level   = nl1;

        /* Kept in step for anything that still reads the scalar form. */
        seg->start.carrier_hz  = seg->lstart[0].carrier_hz;
        seg->start.beat_hz     = seg->lstart[0].beat_hz;
        seg->end.carrier_hz    = seg->lend[0].carrier_hz;
        seg->end.beat_hz       = seg->lend[0].beat_hz;
    } else {
        /* A preset, or one being tuned by hand. The renderer's control-rate
           interpolation turns a changed target into a glide, so live tuning
           needs no re-render here - it just moves. */
        seg->start.carrier_hz  = p->carrier_hz;
        seg->start.beat_hz     = p->beat_hz;
        seg->start.tone_level  = p->tone_level;
        seg->start.noise_level = p->noise_level;
        seg->end = seg->start;
    }
}

/* Called from whatever thread or tick the backend pulls on. No allocation, no
   locks, no LVGL. */
static uint32_t stream_pull(int16_t *dst, uint32_t frames,
                            uint32_t advance_frames, void *ctx)
{
    (void)ctx;

    if (S_total > 0.0 && S_t >= S_total) return 0;   /* the program is over */

    const uint32_t rate = stream_rate();
    /* advance_frames == 0 is a peek: render these frames from where the source
       stands and leave it standing there. The device backend renders a block
       long, looks for the best place to cut it, then asks again for exactly
       that many and commits. Both renders start from the same state, so the
       samples it keeps are the samples it measured. */
    if (advance_frames > frames) advance_frames = frames;

    stream_params_t p = S_params[S_slot];            /* snapshot */
    en_segment_t seg;

    /* The part that actually moves the source on. */
    if (advance_frames) {
        stream_segment(&seg, &p, S_t, (double)advance_frames / (double)rate);
        seg.frames = advance_frames;
        en_render_segment(&S_rnd, &seg, dst);
    }

    double t_after = S_t + (double)advance_frames / (double)rate;

    /* The overlap: rendered here as this window's tail, and again as the next
       window's head, so the renderer is rewound afterwards. en_render_t is a
       plain struct - oscillator phases and noise state - so a copy is a
       complete snapshot. Without the rewind the timeline would gain the length
       of every overlap and a 45 minute program would finish early. */
    uint32_t tail = frames - advance_frames;
    if (tail) {
        en_render_t saved = S_rnd;
        stream_segment(&seg, &p, t_after, (double)tail / (double)rate);
        seg.frames = tail;
        en_render_segment(&S_rnd, &seg, dst + (size_t)advance_frames * 2);
        S_rnd = saved;
    }

    if (S_is_program)
        apply_program_fades(dst, frames, rate, S_t, S_total);

    S_t = t_after;
    return frames;
}

/* Push the current parameters at the stream. Used both to start it and, on
   live tune, to move it. */
static void stream_publish_from_state(void)
{
    stream_params_t p;
    p.mode = E.mode;
    p.tone_level = 0.8;
    p.noise = EN_NOISE_NONE;
    p.noise_level = 0.0;
    p.carrier_hz = E.plan.f_l;
    p.beat_hz = E.plan.beat_hz;

    if (E.source == EN_SRC_PRESET || E.source == EN_SRC_LIVE) {
        int n;
        const en_preset_t *ps = en_presets(&n);
        if (E.index >= 0 && E.index < n) {
            p.noise = ps[E.index].noise;
            p.noise_level = ps[E.index].noise_level;
        }
    }
    publish_params(&p);
}

static bool stream_start(bool is_program, double total_s)
{
    S_is_program = is_program ? 1 : 0;
    S_total = total_s;
    S_t = 0.0;
    en_render_init(&S_rnd, stream_rate(),
                   is_program ? EN_NOISE_PINK : EN_NOISE_NONE, 1);
    stream_publish_from_state();
    return en_audio_start_stream(stream_rate(), stream_pull, NULL);
}

/* ---- starting things ----------------------------------------------------- */

static bool start_loop(double beat, double carrier, en_mode_t mode,
                       en_noise_kind_t noise, double nlevel)
{
    en_loop_plan_t plan;
    if (!en_plan_loop(beat, carrier, EN_RATES, EN_RATES_COUNT,
                      EN_TARGET_BYTES, &plan))
        return false;

    E.plan = plan;
    E.mode = mode;
    E.total_s = 0.0;   /* endless */

    en_segment_t seg;
    en_plan_to_segment(&plan, mode, noise, 0.8, nlevel, &seg);

    char key[64];
    make_key(key, (int)sizeof key, &plan, mode, noise, nlevel);
    return job_start(&seg, true, true, key, 0.0);
}

void en_engine_init(void)
{
    memset(&E, 0, sizeof E);
    en_osc_init();
    en_audio_init();
}

void en_engine_shutdown(void)
{
    /* Never leave a loop playing behind us. */
    en_audio_stop(0);
    en_audio_shutdown();
    job_cancel();
}

bool en_engine_play_preset(int index)
{
    int n;
    const en_preset_t *ps = en_presets(&n);
    if (index < 0 || index >= n) return false;

    E.source = EN_SRC_PRESET;
    E.index = index;
    copy_str(E.title, ps[index].name, (int)sizeof E.title);
    copy_str(E.detail, ps[index].detail, (int)sizeof E.detail);
    E.base_beat = E.live_beat = ps[index].beat_hz;
    E.base_carrier = E.live_carrier = ps[index].carrier_hz;
    E.retune_at_ms = 0;

    if (en_audio_can_stream()) {
        /* Plan anyway: the realised beat is what the UI displays, and it stays
           honest whether or not the loop it describes is ever written out. */
        en_loop_plan_t plan;
        if (en_plan_loop(ps[index].beat_hz, ps[index].carrier_hz, EN_RATES,
                         EN_RATES_COUNT, EN_TARGET_BYTES, &plan))
            E.plan = plan;
        E.mode = ps[index].mode;
        E.total_s = 0.0;
        return stream_start(false, 0.0);
    }

    return start_loop(ps[index].beat_hz, ps[index].carrier_hz, ps[index].mode,
                      ps[index].noise, ps[index].noise_level);
}

bool en_engine_play_program(int index)
{
    int n;
    const en_program_t *ps = en_programs(&n);
    if (index < 0 || index >= n) return false;

    E.source = EN_SRC_PROGRAM;
    E.index = index;
    copy_str(E.title, ps[index].name, (int)sizeof E.title);
    copy_str(E.detail, ps[index].detail, (int)sizeof E.detail);
    E.mode = ps[index].mode;
    E.total_s = (double)en_program_seconds(&ps[index]);
    E.render_pos_s = 0.0;

    if (en_audio_can_stream()) return stream_start(true, E.total_s);

    en_render_init(&E.rnd, EN_RATES[0], ps[index].segs[0].noise, 1);
    return start_next_chunk(true);
}

/*
 * Play a segment table the caller owns.
 *
 * The measured programs are parsed out of a file into a buffer the UI holds,
 * and the largest is thirty-odd kilobytes; copying it in here would mean two
 * copies of the biggest thing in the app for no gain. The pointer is borrowed,
 * so the caller has to keep it alive until something else plays - which the UI
 * does by parsing into a single static.
 */
bool en_engine_play_segs(const en_prog_seg_t *segs, int n_segs,
                         en_mode_t mode, const char *name, const char *detail)
{
    if (!segs || n_segs <= 0) return false;

    E.file_segs = segs;
    E.file_segs_n = n_segs;
    E.source = EN_SRC_FILE;
    E.index = -1;
    copy_str(E.title, name ? name : "", (int)sizeof E.title);
    copy_str(E.detail, detail ? detail : "", (int)sizeof E.detail);
    E.mode = mode;
    E.total_s = 0.0;
    for (int i = 0; i < n_segs; i++)
        E.total_s += (double)segs[i].seconds;
    E.render_pos_s = 0.0;

    if (en_audio_can_stream()) return stream_start(true, E.total_s);

    en_render_init(&E.rnd, EN_RATES[0], segs[0].noise, 1);
    return start_next_chunk(true);
}

bool en_engine_play_user(const en_user_program_t *up)
{
    if (!up || up->n_segs == 0) return false;

    E.user = *up;
    E.source = EN_SRC_USER;
    E.index = -1;
    copy_str(E.title, up->name, (int)sizeof E.title);
    E.detail[0] = 0;
    E.mode = up->mode;
    E.total_s = 0.0;
    for (int i = 0; i < up->n_segs; i++)
        E.total_s += (double)up->segs[i].seconds;
    E.render_pos_s = 0.0;

    if (en_audio_can_stream()) return stream_start(true, E.total_s);

    en_render_init(&E.rnd, EN_RATES[0], up->segs[0].noise, 1);
    return start_next_chunk(true);
}

void en_engine_stop(uint32_t fade_ms)
{
    job_cancel();
    en_audio_stop(fade_ms);
    E.sleep_firing = false;
    E.retuning = false;
}

void en_engine_toggle_pause(void)
{
    if (!en_engine_is_active()) return;
    en_audio_set_paused(en_audio_state() != EN_AUDIO_PAUSED);
}

bool en_engine_is_playing(void) { return en_audio_state() == EN_AUDIO_PLAYING; }
bool en_engine_is_paused(void)  { return en_audio_state() == EN_AUDIO_PAUSED; }

bool en_engine_is_active(void)
{
    en_audio_state_t s = en_audio_state();
    return s == EN_AUDIO_PLAYING || s == EN_AUDIO_PAUSED || E.job.active;
}

double en_engine_render_progress(void)
{
    if (!E.job.active || E.job.frames == 0) return -1.0;
    return (double)E.job.done / (double)E.job.frames;
}

/* ---- live tune ----------------------------------------------------------- */

void en_engine_live_adjust(double d_beat, double d_carrier)
{
    if (E.source != EN_SRC_PRESET && E.source != EN_SRC_LIVE) return;

    E.live_beat += d_beat;
    E.live_carrier += d_carrier;
    if (E.live_beat < 0.5) E.live_beat = 0.5;
    if (E.live_beat > 100.0) E.live_beat = 100.0;
    if (E.live_carrier < 60.0) E.live_carrier = 60.0;
    if (E.live_carrier > 600.0) E.live_carrier = 600.0;

    /* Re-plan straight away so the readout tells the truth immediately, even
       though the audio will not change for another moment. */
    en_loop_plan_t p;
    if (en_plan_loop(E.live_beat, E.live_carrier, EN_RATES, EN_RATES_COUNT,
                     EN_TARGET_BYTES, &p))
        E.plan = p;

    E.source = EN_SRC_LIVE;

    if (en_audio_can_stream()) {
        /* Nothing to re-render: publishing the new target is enough, and the
           renderer's control-rate interpolation glides to it. */
        stream_publish_from_state();
        E.retune_at_ms = 0;
        E.retuning = false;
        return;
    }
    E.retune_at_ms = en_sys_millis() + EN_RETUNE_SETTLE_MS;
}

void en_engine_live_reset(void)
{
    /* Only a preset has a baseline to return to. Reached from a program, the
       base fields are zero and resetting to them would drop the beat to the
       0.5 Hz clamp — which looked exactly like the reset "not working". */
    if (E.base_beat <= 0.0 || E.base_carrier <= 0.0) return;

    E.live_beat = E.base_beat;
    E.live_carrier = E.base_carrier;
    en_loop_plan_t p;
    if (en_plan_loop(E.live_beat, E.live_carrier, EN_RATES, EN_RATES_COUNT,
                     EN_TARGET_BYTES, &p))
        E.plan = p;
    if (en_audio_can_stream()) {
        stream_publish_from_state();
        E.retune_at_ms = 0;
        return;
    }
    E.retune_at_ms = en_sys_millis() + EN_RETUNE_SETTLE_MS;
}

bool en_engine_is_retuning(void) { return E.retuning || E.retune_at_ms != 0; }

/* ---- the tick ------------------------------------------------------------ */

void en_engine_tick(void)
{
    en_audio_tick();

    /* A stream needs no lookahead, no chunking and no re-arming: the audio
       thread pulls what it needs. Only the sleep timer still applies. */
    if (en_audio_can_stream()) {
        if (E.sleep_timer_s && !E.sleep_firing && en_engine_is_active()) {
            uint32_t el = (en_sys_millis() - E.sleep_started_ms) / 1000u;
            if (el >= E.sleep_timer_s) {
                E.sleep_firing = true;
                en_audio_stop(8000);
            }
        }
        return;
    }

    /* A settled live-tune gesture becomes a re-render. The currently playing
       loop is left alone until the new one is ready, so retuning is never a
       silence. */
    if (E.retune_at_ms && en_sys_millis() >= E.retune_at_ms && !E.job.active) {
        E.retune_at_ms = 0;
        E.retuning = true;
        int n;
        const en_preset_t *ps = en_presets(&n);
        en_noise_kind_t noise = EN_NOISE_NONE;
        double nlevel = 0.0;
        en_mode_t mode = EN_MODE_BINAURAL;
        if (E.index >= 0 && E.index < n) {
            noise = ps[E.index].noise;
            nlevel = ps[E.index].noise_level;
            mode = ps[E.index].mode;
        }
        if (!start_loop(E.live_beat, E.live_carrier, mode, noise, nlevel))
            E.retuning = false;
    }

    /* Keep the next program chunk one step ahead of playback. */
    if (!E.job.active &&
        (E.source == EN_SRC_PROGRAM || E.source == EN_SRC_USER) &&
        en_audio_wants_next() &&
        en_audio_remaining() < (double)EN_CHUNK_SECONDS + EN_LOOKAHEAD_S) {
        start_next_chunk(false);
    }

    job_step();

    /* Sleep timer: fade out over the last stretch rather than cutting. */
    if (E.sleep_timer_s && !E.sleep_firing && en_engine_is_active()) {
        uint32_t elapsed = (en_sys_millis() - E.sleep_started_ms) / 1000u;
        if (elapsed >= E.sleep_timer_s) {
            E.sleep_firing = true;
            en_audio_stop(8000);
        }
    }
}

/* ---- readouts ------------------------------------------------------------ */

en_source_kind_t en_engine_source(void) { return E.source; }
const char *en_engine_title(void)  { return E.title[0] ? E.title : "Nothing playing"; }
const char *en_engine_detail(void) { return E.detail; }
en_mode_t   en_engine_mode(void)   { return E.mode; }

double en_engine_beat(void)
{
    if (E.source == EN_SRC_PROGRAM || E.source == EN_SRC_USER) {
        double b, c, nl;
        en_noise_kind_t nk;
        program_params_at(en_engine_elapsed(), &b, &c, &nk, &nl);
        return b;
    }
    return E.plan.beat_hz;
}

double en_engine_carrier(void)
{
    if (E.source == EN_SRC_PROGRAM || E.source == EN_SRC_USER) {
        double b, c, nl;
        en_noise_kind_t nk;
        program_params_at(en_engine_elapsed(), &b, &c, &nk, &nl);
        return c;
    }
    return E.plan.f_l;
}

en_band_t en_engine_band(void) { return en_band_of(en_engine_beat()); }

double en_engine_elapsed(void) { return en_audio_elapsed(); }
double en_engine_total(void)   { return E.total_s; }

int en_engine_seg_count(void)
{
    if (E.source == EN_SRC_USER) return E.user.n_segs;
    if (E.source == EN_SRC_PROGRAM) {
        int n;
        const en_program_t *ps = en_programs(&n);
        return ps[E.index].n_segs;
    }
    return 0;
}

double en_engine_seg_start(int i)
{
    const en_prog_seg_t *segs;
    int n;
    if (E.source == EN_SRC_USER) { segs = E.user.segs; n = E.user.n_segs; }
    else if (E.source == EN_SRC_PROGRAM) {
        int count;
        const en_program_t *ps = en_programs(&count);
        segs = ps[E.index].segs;
        n = ps[E.index].n_segs;
    } else return 0.0;

    double acc = 0.0;
    for (int k = 0; k < i && k < n; k++) acc += (double)segs[k].seconds;
    return acc;
}

/* ---- sleep timer --------------------------------------------------------- */

void en_engine_set_sleep_timer(uint32_t seconds)
{
    E.sleep_timer_s = seconds;
    E.sleep_started_ms = en_sys_millis();
    E.sleep_firing = false;
}

uint32_t en_engine_sleep_timer(void) { return E.sleep_timer_s; }

uint32_t en_engine_sleep_remaining(void)
{
    if (!E.sleep_timer_s) return 0;
    uint32_t elapsed = (en_sys_millis() - E.sleep_started_ms) / 1000u;
    return elapsed >= E.sleep_timer_s ? 0 : E.sleep_timer_s - elapsed;
}
