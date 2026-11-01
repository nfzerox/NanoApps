/*
 * tests.c — host unit tests for apps/entrain/core.
 *
 * These run on the development machine, not the iPod. core/ is pure C99 with
 * no LVGL and no SDK, precisely so that the DSP can be tested (and heard) here
 * rather than by squinting at a 240x432 screen over USB.
 *
 * The two that matter most are the click test and the loop-seam test. Every
 * other property can be a bit off and the app still sounds fine; a click at a
 * segment join or a loop wrap is the one artefact that makes the whole thing
 * feel broken, and it is exactly the kind of bug that creeps back in.
 *
 *     make -f Makefile.host test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../core/osc.h"
#include "../core/noise.h"
#include "../core/render.h"
#include "../core/program.h"
#include "../core/progfile.h"
#include "../core/freqset.h"
#include "../core/wavout.h"

static int g_checks, g_fails;

#define CHECK(cond, ...) do {                                                 \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_fails++;                                                        \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static void section(const char *name)
{
    printf("\n== %s\n", name);
}

/* ---- helpers ------------------------------------------------------------ */

/* Frequency of one channel by positive-going zero crossings, interpolated.
   Accurate to well under a millihertz on a clean sine over a few seconds. */
static double measure_freq(const int16_t *pcm, uint32_t frames, int channel,
                           uint32_t sample_rate)
{
    double first = -1.0, last = -1.0;
    long crossings = 0;

    for (uint32_t i = 1; i < frames; i++) {
        int16_t a = pcm[2 * (i - 1) + channel];
        int16_t b = pcm[2 * i + channel];
        if (a < 0 && b >= 0) {
            /* linear interpolation of the crossing instant */
            double frac = (double)(-a) / (double)(b - a);
            double t = ((double)(i - 1) + frac) / (double)sample_rate;
            if (first < 0.0) first = t;
            last = t;
            crossings++;
        }
    }
    if (crossings < 2) return 0.0;
    return (double)(crossings - 1) / (last - first);
}

/* Largest absolute sample-to-sample step in one channel over [from, to). */
static int max_step(const int16_t *pcm, uint32_t from, uint32_t to, int channel)
{
    int m = 0;
    for (uint32_t i = from + 1; i < to; i++) {
        int d = pcm[2 * i + channel] - pcm[2 * (i - 1) + channel];
        if (d < 0) d = -d;
        if (d > m) m = d;
    }
    return m;
}

static int peak_abs(const int16_t *pcm, uint32_t frames)
{
    int m = 0;
    for (uint32_t i = 0; i < frames * 2; i++) {
        int v = pcm[i];
        if (v < 0) v = -v;
        if (v > m) m = v;
    }
    return m;
}

/* ---- 1. oscillator and sine table --------------------------------------- */

static void test_sine(void)
{
    section("sine table + oscillator");
    en_osc_init();

    /* The series-based reference has to be right before the table can be
       judged against it. */
    CHECK(fabs(en_sin_turns(0.0)) < 1e-12, "sin(0)");
    CHECK(fabs(en_sin_turns(0.25) - 1.0) < 1e-9, "sin(pi/2) = %.12f",
          en_sin_turns(0.25));
    CHECK(fabs(en_sin_turns(0.5)) < 1e-9, "sin(pi) = %.12f", en_sin_turns(0.5));
    CHECK(fabs(en_sin_turns(0.75) + 1.0) < 1e-9, "sin(3pi/2)");

    double worst = 0.0;
    for (int i = 0; i < 100000; i++) {
        double turns = (double)i / 100000.0;
        double want = en_sin_turns(turns);
        double got = (double)en_sine_lookup((uint32_t)(turns * 4294967296.0))
                     / 32767.0;
        double e = fabs(want - got);
        if (e > worst) worst = e;
    }
    /* 2048 entries with linear interpolation: the interpolation error dominates
       and lands near 2e-6 of full scale. Allow a little slack for the 16-bit
       quantisation of the table itself. */
    printf("  worst LUT error: %.3e of full scale\n", worst);
    CHECK(worst < 1e-4, "LUT interpolation error %.3e too large", worst);

    /* The accumulator must not drift: after a whole number of cycles the phase
       must land back within rounding of where it started. */
    en_osc_t o = { 0, 0 };
    uint32_t sr = 11025;
    en_osc_set_freq(&o, 200.0, sr);
    for (uint32_t i = 0; i < sr; i++) o.phase += o.step;   /* exactly 1 second */
    double turns = en_osc_phase_turns(&o);
    double off = turns > 0.5 ? 1.0 - turns : turns;
    printf("  phase after 200 Hz x 1 s: %.9f turns from zero\n", off);
    CHECK(off < 1e-5, "accumulator drifted %.9f turns", off);
}

/* ---- 2. the loop planner ------------------------------------------------- */

static void test_planner(void)
{
    section("loop planner");

    en_loop_plan_t p;
    int ok = en_plan_loop(7.83, 200.0, EN_RATES, EN_RATES_COUNT,
                          EN_TARGET_BYTES, &p);
    CHECK(ok, "no plan for Schumann");
    if (ok) {
        printf("  Schumann: SR=%u N=%u T=%.6f s beat=%.9f Hz "
               "f_L=%.4f f_R=%.4f %u bytes\n",
               p.sample_rate, p.frames, p.t_seconds, p.beat_hz,
               p.f_l, p.f_r, p.bytes);
        /* This one has an exact solution and the planner must find it. */
        CHECK(fabs(p.beat_error) < 1e-9,
              "Schumann beat error %.9f Hz, expected exact", p.beat_error);
        CHECK(p.bytes <= EN_TARGET_BYTES, "plan is %u bytes, over budget",
              p.bytes);
        CHECK(p.bytes < EN_MAX_LOAD_BYTES,
              "plan would be rejected by the loader");
        /* Whole cycle counts are what make the seam work at all. */
        CHECK(fabs(p.f_l * p.t_seconds - (double)p.cycles_l) < 1e-6,
              "f_L is not a whole number of cycles");
        CHECK(fabs(p.f_r * p.t_seconds
                   - (double)(p.cycles_l + p.cycles_beat)) < 1e-6,
              "f_R is not a whole number of cycles");
        /* The carrier may move; the beat may not. */
        CHECK(fabs(p.f_l - 200.0) < 1.0,
              "carrier moved %.3f Hz, too far", fabs(p.f_l - 200.0));
    }

    /* Every shipped preset must plan, stay in budget, and land close on beat. */
    int n;
    const en_preset_t *ps = en_presets(&n);
    for (int i = 0; i < n; i++) {
        en_loop_plan_t q;
        int good = en_plan_loop(ps[i].beat_hz, ps[i].carrier_hz,
                                EN_RATES, EN_RATES_COUNT, EN_TARGET_BYTES, &q);
        CHECK(good, "%s: no plan", ps[i].name);
        if (!good) continue;
        printf("  %-14s %5u Hz  T=%6.3f s  beat %.6f (%+.6f)  %7u B\n",
               ps[i].name, q.sample_rate, q.t_seconds, q.beat_hz,
               q.beat_error, q.bytes);
        CHECK(fabs(q.beat_error) < 0.01,
              "%s: realised beat off by %.6f Hz", ps[i].name, q.beat_error);
        CHECK(q.bytes <= EN_TARGET_BYTES, "%s: %u bytes over budget",
              ps[i].name, q.bytes);
    }

    /* A beat too slow to fit a whole cycle in the budget must fail cleanly
       rather than return nonsense. */
    en_loop_plan_t tiny;
    CHECK(en_plan_loop(0.01, 200.0, EN_RATES, EN_RATES_COUNT, 4096, &tiny) == 0,
          "planner should refuse an impossible budget");
}

/* ---- 3. beat accuracy --------------------------------------------------- */

static void test_beat_accuracy(void)
{
    section("beat accuracy");

    const uint32_t sr = 11025;
    const uint32_t frames = sr * 10;    /* 10 s */
    int16_t *pcm = malloc(frames * 2 * sizeof *pcm);

    struct { double carrier, beat; } cases[] = {
        { 200.0,  7.83 }, { 100.0, 2.0 }, { 250.0, 16.0 }, { 300.0, 40.0 },
    };

    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        en_render_t r;
        en_render_init(&r, sr, EN_NOISE_NONE, 1);

        en_segment_t seg;
        memset(&seg, 0, sizeof seg);
        seg.sample_rate = sr;
        seg.mode = EN_MODE_BINAURAL;
        seg.noise = EN_NOISE_NONE;
        seg.frames = frames;
        seg.start.carrier_hz = cases[c].carrier;
        seg.start.beat_hz = cases[c].beat;
        seg.start.tone_level = 0.8;
        seg.start.noise_level = 0.0;
        seg.end = seg.start;
        en_render_segment(&r, &seg, pcm);

        double fl = measure_freq(pcm, frames, 0, sr);
        double fr = measure_freq(pcm, frames, 1, sr);
        double beat = fr - fl;
        printf("  carrier %.2f beat %.2f -> L=%.5f R=%.5f measured beat %.5f\n",
               cases[c].carrier, cases[c].beat, fl, fr, beat);
        CHECK(fabs(beat - cases[c].beat) < 0.01,
              "beat measured %.5f, wanted %.5f", beat, cases[c].beat);
        CHECK(fabs(fl - cases[c].carrier) < 0.01,
              "left carrier measured %.5f, wanted %.5f", fl, cases[c].carrier);
    }

    free(pcm);
}

/* ---- 4. the click test -------------------------------------------------- */

/* Render a chain of segments through one renderer and assert that the joins
   are not visible in the waveform. This is the test that keeps the app feeling
   premium; if it ever fails, something is resetting phase between segments. */
static void test_clicks(void)
{
    section("click test: segment joins");

    const uint32_t sr = 11025;
    const uint32_t seg_frames = sr * 2;    /* 2 s each */
    const int n_segs = 5;
    const uint32_t total = seg_frames * n_segs;
    int16_t *pcm = malloc(total * 2 * sizeof *pcm);

    en_render_t r;
    en_render_init(&r, sr, EN_NOISE_NONE, 1);

    /* A deliberately awkward chain: the beat and the carrier both move at
       every join, which is the case a naive renderer gets wrong. */
    double beats[]    = { 10.0, 8.0, 6.0, 4.0, 2.0, 2.0 };
    double carriers[] = { 200.0, 190.0, 180.0, 160.0, 140.0, 140.0 };

    for (int s = 0; s < n_segs; s++) {
        en_segment_t seg;
        memset(&seg, 0, sizeof seg);
        seg.sample_rate = sr;
        seg.mode = EN_MODE_BINAURAL;
        seg.noise = EN_NOISE_NONE;
        seg.frames = seg_frames;
        seg.start.carrier_hz = carriers[s];
        seg.start.beat_hz    = beats[s];
        seg.start.tone_level = 0.8;
        seg.end.carrier_hz   = carriers[s + 1];
        seg.end.beat_hz      = beats[s + 1];
        seg.end.tone_level   = 0.8;
        seg.end.noise_level  = 0.0;
        en_render_segment(&r, &seg, pcm + (size_t)s * seg_frames * 2);
    }

    for (int ch = 0; ch < 2; ch++) {
        /* Baseline: the largest step anywhere well away from a join. */
        int baseline = 0;
        for (int s = 0; s < n_segs; s++) {
            uint32_t a = s * seg_frames + 64;
            uint32_t b = (s + 1) * seg_frames - 64;
            int m = max_step(pcm, a, b, ch);
            if (m > baseline) baseline = m;
        }
        for (int s = 1; s < n_segs; s++) {
            uint32_t j = s * seg_frames;
            int d = pcm[2 * j + ch] - pcm[2 * (j - 1) + ch];
            if (d < 0) d = -d;
            printf("  ch%d join %d: step %d (interior max %d)\n",
                   ch, s, d, baseline);
            CHECK(d <= baseline + 8,
                  "ch%d join %d steps %d against an interior max of %d "
                  "- that is a click", ch, s, d, baseline);
        }
    }

    free(pcm);
}

/* ---- 5. the loop-seam test ---------------------------------------------- */

static void test_loop_seams(void)
{
    section("loop seam: every shipped preset");

    int n;
    const en_preset_t *ps = en_presets(&n);

    for (int i = 0; i < n; i++) {
        en_loop_plan_t plan;
        if (!en_plan_loop(ps[i].beat_hz, ps[i].carrier_hz,
                          EN_RATES, EN_RATES_COUNT, EN_TARGET_BYTES, &plan)) {
            CHECK(0, "%s: no plan", ps[i].name);
            continue;
        }

        en_segment_t seg;
        en_plan_to_segment(&plan, ps[i].mode, ps[i].noise, 0.8,
                           ps[i].noise_level, &seg);

        int16_t *pcm = malloc((size_t)plan.frames * 2 * sizeof *pcm);
        en_render_t r;
        en_render_init(&r, plan.sample_rate, ps[i].noise, 1);
        en_render_loop(&r, &seg, pcm);

        int worst_seam = 0, worst_base = 0;
        for (int ch = 0; ch < 2; ch++) {
            /* Skip the noise crossfade region when taking the baseline, so the
               baseline reflects the steady state rather than the blend. */
            int baseline = max_step(pcm, 512, plan.frames - 2, ch);
            int seam = pcm[2 * 0 + ch] - pcm[2 * (plan.frames - 1) + ch];
            if (seam < 0) seam = -seam;
            if (seam > worst_seam) worst_seam = seam;
            if (baseline > worst_base) worst_base = baseline;
        }
        printf("  %-14s seam step %5d vs interior max %5d  (%.3f s loop)\n",
               ps[i].name, worst_seam, worst_base, plan.t_seconds);
        /* The wrap must look like any other sample transition. A little slack
           because a wrap can legitimately land on the steepest part of the
           waveform where an interior sample did not. */
        CHECK(worst_seam <= (worst_base * 5) / 4 + 8,
              "%s: loop seam steps %d against an interior max of %d",
              ps[i].name, worst_seam, worst_base);

        /* Rendering the same plan twice must give identical bytes, or the
           render cache is lying about what it holds. */
        int16_t *again = malloc((size_t)plan.frames * 2 * sizeof *again);
        en_render_t r2;
        en_render_init(&r2, plan.sample_rate, ps[i].noise, 1);
        en_render_loop(&r2, &seg, again);
        CHECK(memcmp(pcm, again, (size_t)plan.frames * 2 * sizeof *pcm) == 0,
              "%s: render is not deterministic", ps[i].name);

        free(again);
        free(pcm);
    }
}

/* A preset that asks for a noise bed must actually get one. The renderer used
   to take its noise kind from en_render_init rather than from the segment, so
   a pink-bed preset rendered through a NONE generator and the bed was silent
   while the level said otherwise. */
static void test_noise_bed(void)
{
    section("noise beds actually produce noise");

    const uint32_t sr = 11025;
    const uint32_t frames = sr * 2;
    int16_t *pcm = malloc(frames * 2 * sizeof *pcm);

    en_noise_kind_t kinds[] = { EN_NOISE_WHITE, EN_NOISE_PINK, EN_NOISE_BROWN };
    const char *names[] = { "white", "pink", "brown" };

    for (int k = 0; k < 3; k++) {
        en_render_t r;
        /* Deliberately initialise with the WRONG kind: the segment must win. */
        en_render_init(&r, sr, EN_NOISE_NONE, 1);

        en_segment_t seg;
        memset(&seg, 0, sizeof seg);
        seg.sample_rate = sr;
        seg.mode = EN_MODE_BINAURAL;
        seg.noise = kinds[k];
        seg.frames = frames;
        seg.start.carrier_hz = 200.0;
        seg.start.beat_hz = 10.0;
        seg.start.tone_level = 0.0;    /* noise only, so it is measurable */
        seg.start.noise_level = 0.5;
        seg.end = seg.start;
        en_render_segment(&r, &seg, pcm);

        int peak = peak_abs(pcm, frames);
        printf("  %-6s bed peak %d\n", names[k], peak);
        CHECK(peak > 500, "%s bed is silent (peak %d) - the segment's kind "
              "was ignored", names[k], peak);
    }

    /* And NONE must stay silent even when a level is set. */
    en_render_t r;
    en_render_init(&r, sr, EN_NOISE_PINK, 1);
    en_segment_t seg;
    memset(&seg, 0, sizeof seg);
    seg.sample_rate = sr;
    seg.mode = EN_MODE_BINAURAL;
    seg.noise = EN_NOISE_NONE;
    seg.frames = frames;
    seg.start.carrier_hz = 200.0;
    seg.start.beat_hz = 10.0;
    seg.start.tone_level = 0.0;
    seg.start.noise_level = 0.5;
    seg.end = seg.start;
    en_render_segment(&r, &seg, pcm);
    CHECK(peak_abs(pcm, frames) == 0, "EN_NOISE_NONE produced sound");

    free(pcm);
}

/* ---- how consecutive blocks are joined -----------------------------------
 *
 * The device hands the sound hardware one block at a time, because nothing can
 * silence a voice already sounding and the pool mixes rather than cuts. The
 * blocks are consecutive samples of one continuous signal - the renderer never
 * resets a phase - but WHEN the next voice actually starts is not ours to
 * choose, so the join lands early or late by up to a UI tick.
 *
 * Two versions shipped that tried to hide that with a crossfade, and both
 * sounded wrong. These prove why, and prove the replacement.
 */

/* A plain binaural pair: the hardest case for a join, because the two channels
   carry different frequencies and so only cross zero together once per beat. */
static void join_signal(int16_t *pcm, uint32_t frames, uint32_t sr)
{
    en_render_t r;
    en_render_init(&r, sr, EN_NOISE_NONE, 1);

    en_segment_t seg;
    memset(&seg, 0, sizeof seg);
    seg.sample_rate = sr;
    seg.mode = EN_MODE_BINAURAL;
    seg.noise = EN_NOISE_NONE;
    seg.frames = frames;
    seg.start.carrier_hz = 200.0;
    seg.start.beat_hz = 10.0;
    seg.start.tone_level = 0.8;
    seg.end = seg.start;
    en_render_segment(&r, &seg, pcm);
}

/* Why the crossfade had to go.
 *
 * Overlapping consecutive blocks does not blend two different sounds. It sums
 * one signal with a time-shifted copy of itself, which is a comb filter. At the
 * middle of any fade the two gains are equal, so a shift of half a carrier
 * period cancels the carrier outright - and the notch sweeps as the timing
 * drifts, which is exactly what the wobble was. */
static void test_crossfade_combs(void)
{
    section("crossfading consecutive blocks comb-filters (why it went)");

    const uint32_t sr = 22050;
    const uint32_t frames = sr * 2;
    int16_t *pcm = malloc((size_t)frames * 2 * sizeof *pcm);
    join_signal(pcm, frames, sr);

    /* Half a period of the 200 Hz carrier - well inside a UI tick, which is
       16 to 30 ms. */
    const uint32_t shift = sr / 400;
    printf("  carrier 200 Hz, half-period shift = %u frames (%.1f ms)\n",
           shift, 1000.0 * shift / sr);

    int ref_peak = 0, mix_peak = 0;
    for (uint32_t i = shift; i < frames; i++) {
        int a = pcm[2 * i];
        if (a < 0) a = -a;
        if (a > ref_peak) ref_peak = a;

        /* Equal gains, as at the midpoint of any crossfade. */
        int m = (pcm[2 * i] + pcm[2 * (i - shift)]) / 2;
        if (m < 0) m = -m;
        if (m > mix_peak) mix_peak = m;
    }
    int left = ref_peak ? (mix_peak * 100) / ref_peak : 0;
    printf("  crossfade midpoint keeps %d%% of the carrier\n", left);
    CHECK(left < 10, "expected near-total cancellation, got %d%%", left);

    free(pcm);
}

/* The block length has to survive a round trip through the OS's milliseconds.
 *
 * The OS is told how long each block is in milliseconds (descriptor+0x34) and
 * turns it into the voice's remaining-sample counter with
 * ms * output_rate / 1000, in integer arithmetic. That counter has to come out
 * holding exactly the number of frames we rendered. Too small and the chain
 * transition miscounts and clicks; too large and the voice pads with silence
 * once the PCM has run out, which sounded like a block of audio followed by a
 * block of nothing.
 *
 * The rate in that sum is the MIXER's, not ours, and it is read off the live
 * voice at run time — so the block length has to divide cleanly at every rate a
 * mixer plausibly runs at, not just at 22050. */
static void test_block_duration(void)
{
    section("block length converts to the OS sample count exactly");

    /* The shipped values from audio_device.c. */
    const uint32_t block_frames = 28224;
    const uint32_t quiet_frames = 7056;

    /* Every rate the mixer might be running at. */
    const uint32_t rates[] = { 22050, 44100, 48000 };
    const uint32_t blocks[] = { block_frames, quiet_frames };

    for (size_t b = 0; b < sizeof blocks / sizeof blocks[0]; b++) {
        for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
            uint32_t frames = blocks[b], rate = rates[r];

            /* What duration_ms() computes... */
            uint32_t num = frames * 1000u;
            CHECK(num % rate == 0,
                  "%u frames does not state exactly at %u Hz", frames, rate);
            uint32_t ms = num / rate;

            /* ...and what setSource makes of it. */
            uint32_t counter = (ms * rate) / 1000u;

            printf("  %5u frames @ %6u Hz -> %4u ms -> counter %5u\n",
                   frames, rate, ms, counter);
            CHECK(counter == frames,
                  "%u frames at %u Hz gives a counter of %u",
                  frames, rate, counter);
        }
    }

    /* And the two ways it went wrong on device, both reproduced here.

       Stating a block in its OWN milliseconds rather than against the mixer
       rate: 28224 frames is 1280 ms at 22050, but a 44100 mixer reads that as
       56448 samples — twice the buffer — so the voice pads the difference with
       silence. That was the block of sound, block of silence. */
    uint32_t own_ms = (block_frames * 1000u) / 22050u;
    uint32_t at_44k = (own_ms * 44100u) / 1000u;
    printf("  stating %u ms (its own rate) to a 44100 mixer -> %u samples,"
           " %u too many\n", own_ms, at_44k, at_44k - block_frames);
    CHECK(at_44k == block_frames * 2u,
          "expected exactly double, got %u", at_44k);

    /* And a length that does not divide: the counter lands short, and a
       counter that is short by even one sample is a click per block. */
    uint32_t odd = 26460;                   /* fine at 44.1k, not at 48k */
    uint32_t odd_ms = (odd * 1000u) / 48000u;
    uint32_t odd_back = (odd_ms * 48000u) / 1000u;
    printf("  %u frames @ 48000 Hz -> %u ms -> counter %u (%d out)\n",
           odd, odd_ms, odd_back, (int)odd_back - (int)odd);
    CHECK(odd_back != odd, "expected the unaligned case to lose samples");
}

/* ---- 6. phase continuity across a glide --------------------------------- */

static void test_glide(void)
{
    section("phase continuity across a parameter glide");

    const uint32_t sr = 11025;
    const uint32_t frames = sr * 4;
    int16_t *pcm = malloc(frames * 2 * sizeof *pcm);

    en_render_t r;
    en_render_init(&r, sr, EN_NOISE_NONE, 1);

    en_segment_t seg;
    memset(&seg, 0, sizeof seg);
    seg.sample_rate = sr;
    seg.mode = EN_MODE_BINAURAL;
    seg.noise = EN_NOISE_NONE;
    seg.frames = frames;
    seg.start.carrier_hz = 200.0;
    seg.start.beat_hz = 10.0;
    seg.start.tone_level = 0.8;
    seg.end.carrier_hz = 260.0;     /* a big, fast sweep */
    seg.end.beat_hz = 4.0;
    seg.end.tone_level = 0.8;
    seg.end.noise_level = 0.0;
    en_render_segment(&r, &seg, pcm);

    /* A frequency sweep should be smooth everywhere. The control blocks update
       the phase step every 64 samples; if that were done by resetting phase
       rather than by changing the increment, there would be a step at every
       block boundary. Compare the worst step at a block boundary against the
       worst step away from one. */
    for (int ch = 0; ch < 2; ch++) {
        int on_boundary = 0, off_boundary = 0;
        for (uint32_t i = 1; i < frames; i++) {
            int d = pcm[2 * i + ch] - pcm[2 * (i - 1) + ch];
            if (d < 0) d = -d;
            if (i % EN_CTRL_BLOCK == 0) {
                if (d > on_boundary) on_boundary = d;
            } else {
                if (d > off_boundary) off_boundary = d;
            }
        }
        printf("  ch%d worst step on a control boundary %d, elsewhere %d\n",
               ch, on_boundary, off_boundary);
        CHECK(on_boundary <= off_boundary + 8,
              "ch%d: control-block boundaries are audible (%d vs %d)",
              ch, on_boundary, off_boundary);
    }

    free(pcm);
}

/* ---- 7. levels, fades and clipping -------------------------------------- */

static void test_levels(void)
{
    section("levels, fades, soft clip");

    /* Soft clip: unity through zero, bounded, monotonic. */
    CHECK(fabs(en_softclip(0.0)) < 1e-12, "softclip(0)");
    CHECK(fabs(en_softclip(0.001) - 0.001) < 1e-6, "softclip near zero");
    CHECK(en_softclip(100.0) <= 1.0, "softclip does not bound above");
    CHECK(en_softclip(-100.0) >= -1.0, "softclip does not bound below");
    double prev = -2.0;
    for (double x = -3.0; x <= 3.0; x += 0.01) {
        double y = en_softclip(x);
        CHECK(y >= prev - 1e-9, "softclip is not monotonic at %.3f", x);
        prev = y;
    }

    const uint32_t sr = 11025;
    const uint32_t frames = sr * 4;
    int16_t *pcm = malloc(frames * 2 * sizeof *pcm);

    en_render_t r;
    en_render_init(&r, sr, EN_NOISE_PINK, 7);

    en_segment_t seg;
    memset(&seg, 0, sizeof seg);
    seg.sample_rate = sr;
    seg.mode = EN_MODE_BINAURAL;
    seg.noise = EN_NOISE_PINK;
    seg.frames = frames;
    seg.start.carrier_hz = 200.0;
    seg.start.beat_hz = 10.0;
    seg.start.tone_level = 1.0;
    seg.start.noise_level = 0.3;
    seg.end = seg.start;
    seg.fade_in_s = 1.5;
    seg.fade_out_s = 1.5;
    en_render_segment(&r, &seg, pcm);

    /* Nothing may wrap. A wrapped sample is the loudest click a 16-bit file
       can make, so this assertion is not a formality. */
    CHECK(peak_abs(pcm, frames) <= 32767, "output wrapped");

    /* The fades must actually start and end at silence. */
    CHECK(abs(pcm[0]) < 64 && abs(pcm[1]) < 64,
          "fade-in does not start from silence (%d, %d)", pcm[0], pcm[1]);
    CHECK(abs(pcm[2 * (frames - 1)]) < 64,
          "fade-out does not reach silence (%d)", pcm[2 * (frames - 1)]);

    /* ...and the middle must be at a sensible level, not crushed. */
    int mid = 0;
    for (uint32_t i = frames / 2 - 1000; i < frames / 2 + 1000; i++) {
        int v = abs(pcm[2 * i]);
        if (v > mid) mid = v;
    }
    printf("  peak %d, mid-segment peak %d\n", peak_abs(pcm, frames), mid);
    CHECK(mid > 8000, "mid-segment level is only %d, too quiet", mid);

    free(pcm);
}

/* ---- 8. WAV header ------------------------------------------------------ */

/* Magnitude of one frequency bin, by direct correlation. Used instead of the
   zero-crossing measure_freq() because a mix of three carriers has no useful
   zero crossings — the whole point of these tests is that several tones are
   present at once, which is exactly what that helper cannot see. Pick `frames`
   so every frequency completes a whole number of cycles and there is no
   leakage to worry about. */
static double bin_mag(const int16_t *pcm, uint32_t frames, int ch,
                      uint32_t sr, double hz)
{
    const double two_pi = 6.283185307179586;
    double re = 0.0, im = 0.0;
    for (uint32_t i = 0; i < frames; i++) {
        double t = (double)i / (double)sr;
        double a = (double)pcm[2 * i + ch] / 32767.0;
        re += a * cos(two_pi * hz * t);
        im += a * sin(two_pi * hz * t);
    }
    return 2.0 * sqrt(re * re + im * im) / (double)frames;
}

static void test_layers(void)
{
    section("layers");

    const uint32_t sr = 11025;
    const uint32_t frames = sr;          /* one second: every carrier is exact */
    int16_t *a = malloc(frames * 2 * sizeof *a);
    int16_t *b = malloc(frames * 2 * sizeof *b);

    /* ---- 1. the legacy form and an explicit one-layer form are the same
       bytes. This is the check that lets every existing caller stay untouched:
       if it ever fails, single-layer material has silently changed. */
    en_segment_t legacy;
    memset(&legacy, 0, sizeof legacy);
    legacy.sample_rate = sr;
    legacy.mode = EN_MODE_BINAURAL;
    legacy.noise = EN_NOISE_NONE;
    legacy.frames = frames;
    legacy.start.carrier_hz = 200.0;
    legacy.start.beat_hz = 10.0;
    legacy.start.tone_level = 0.8;
    legacy.start.noise_level = 0.0;
    legacy.end = legacy.start;

    en_segment_t explicit_one = legacy;
    explicit_one.layers = 1;
    explicit_one.lstart[0].mode = EN_MODE_BINAURAL;
    explicit_one.lstart[0].carrier_hz = 200.0;
    explicit_one.lstart[0].beat_hz = 10.0;
    explicit_one.lstart[0].level = 1.0;
    explicit_one.lend[0] = explicit_one.lstart[0];

    en_render_t r;
    en_render_init(&r, sr, EN_NOISE_NONE, 1);
    en_render_segment(&r, &legacy, a);
    en_render_init(&r, sr, EN_NOISE_NONE, 1);
    en_render_segment(&r, &explicit_one, b);
    CHECK(memcmp(a, b, frames * 2 * sizeof *a) == 0,
          "one explicit layer is not bit-identical to the legacy form");

    /* en_segment_layers agrees, and reports one layer for the legacy form. */
    en_layer_t ls[EN_MAX_LAYERS], le[EN_MAX_LAYERS];
    CHECK(en_segment_layers(&legacy, ls, le) == 1, "legacy is not one layer");
    CHECK(ls[0].level == 1.0, "legacy layer gain is not unity");
    CHECK(ls[0].carrier_hz == 200.0, "legacy carrier did not map through");

    /* ---- 2. layers sum, at the gains asked for. Three carriers in the left
       ear at 4:2:1, measured back out of the rendered PCM. */
    en_segment_t mix;
    memset(&mix, 0, sizeof mix);
    mix.sample_rate = sr;
    mix.noise = EN_NOISE_NONE;
    mix.frames = frames;
    mix.start.tone_level = 1.0;
    mix.start.noise_level = 0.0;
    mix.end = mix.start;
    mix.layers = 3;
    const double carr[3] = { 200.0, 300.0, 500.0 };
    const double lvl[3]  = { 0.20, 0.10, 0.05 };
    for (int j = 0; j < 3; j++) {
        mix.lstart[j].mode = EN_MODE_BINAURAL;
        mix.lstart[j].carrier_hz = carr[j];
        mix.lstart[j].beat_hz = 4.0;
        mix.lstart[j].level = lvl[j];
        mix.lend[j] = mix.lstart[j];
    }

    en_render_init(&r, sr, EN_NOISE_NONE, 1);
    en_render_segment(&r, &mix, a);

    for (int j = 0; j < 3; j++) {
        double got = bin_mag(a, frames, 0, sr, carr[j]);
        double want = lvl[j] * EN_HEADROOM;
        CHECK(fabs(got - want) < want * 0.04,
              "layer %d at %.0f Hz measured %.4f, wanted %.4f",
              j, carr[j], got, want);
    }
    /* And nothing appeared where no layer was put. */
    CHECK(bin_mag(a, frames, 0, sr, 400.0) < 0.004,
          "energy at 400 Hz where no layer sounds");

    /* ---- 3. a layer at zero gain still advances. Skipping silent layers is
       the obvious optimisation and it is wrong: the phase would stall and the
       layer would re-enter wherever it stopped, which is a click at the exact
       moment it fades back in. */
    en_segment_t silent = mix;
    silent.lstart[1].level = 0.0;
    silent.lend[1].level = 0.0;

    en_render_init(&r, sr, EN_NOISE_NONE, 1);
    en_render_segment(&r, &silent, a);
    uint32_t after_silent = r.lay[1].l.phase;

    en_render_init(&r, sr, EN_NOISE_NONE, 1);
    en_render_segment(&r, &mix, b);
    uint32_t after_audible = r.lay[1].l.phase;

    CHECK(after_silent == after_audible,
          "a silent layer did not advance its phase (0x%08x vs 0x%08x)",
          after_silent, after_audible);
    /* It really was silent, though. */
    CHECK(bin_mag(a, frames, 0, sr, 300.0) < 0.004,
          "the muted layer is still audible");

    /* ---- 4. more layers than the renderer holds are clamped, not overrun. */
    en_segment_t too_many = mix;
    too_many.layers = EN_MAX_LAYERS + 3;
    CHECK(en_segment_layers(&too_many, ls, le) == EN_MAX_LAYERS,
          "layer count was not clamped to EN_MAX_LAYERS");

    /* ---- 5. a multi-layer loop still wraps without a step. Both carriers are
       cycle-exact in one second at this rate, which is what the seam needs. */
    en_segment_t loop = mix;
    loop.layers = 2;
    loop.lstart[0].beat_hz = 0.0; loop.lend[0].beat_hz = 0.0;
    loop.lstart[1].beat_hz = 0.0; loop.lend[1].beat_hz = 0.0;

    en_render_init(&r, sr, EN_NOISE_NONE, 1);
    en_render_loop(&r, &loop, a);

    int32_t seam = (int32_t)a[0] - (int32_t)a[2 * (frames - 1)];
    int32_t worst = 0;
    for (uint32_t i = 1; i < frames; i++) {
        int32_t d = (int32_t)a[2 * i] - (int32_t)a[2 * (i - 1)];
        if (d < 0) d = -d;
        if (d > worst) worst = d;
    }
    if (seam < 0) seam = -seam;
    printf("  two-layer loop seam %d, worst interior step %d\n", seam, worst);
    CHECK(seam <= worst, "the two-layer loop seam is bigger than any interior step");

    free(a);
    free(b);
}

static void test_freqset(void)
{
    /* A header, a comment, a blank line and three sets: the shipped bundle's
       shape, small enough to assert every number in. */
    static const char text[] =
        "# a bundle\n"
        "\n"
        "Alpha Sweep|3|100,200.5,300\n"
        "# not a set\n"
        "9 Volt|10|440,880\n"
        "Too High|3|1000,90000,2000\n";
    uint32_t len = (uint32_t)(sizeof text - 1);

    CHECK(en_freqset_count(text, len) == 3,
          "expected 3 sets, got %d", en_freqset_count(text, len));

    char name[EN_FREQSET_NAME_MAX + 1];
    int steps = 0;
    uint32_t dwell = 0;

    CHECK(en_freqset_head(text, len, 0, name, sizeof name, &steps, &dwell),
          "head 0 failed");
    CHECK(strcmp(name, "Alpha Sweep") == 0, "name 0 was '%s'", name);
    CHECK(steps == 3, "steps 0 was %d", steps);
    CHECK(dwell == 3, "dwell 0 was %u", (unsigned)dwell);

    CHECK(en_freqset_head(text, len, 1, name, sizeof name, &steps, &dwell),
          "head 1 failed");
    CHECK(strcmp(name, "9 Volt") == 0, "name 1 was '%s'", name);
    CHECK(steps == 2 && dwell == 10, "head 1 was %d/%u", steps,
          (unsigned)dwell);

    CHECK(!en_freqset_head(text, len, 3, name, sizeof name, 0, 0),
          "head past the end should fail");

    /* Expansion: one flat segment per step, at the file's dwell. */
    static en_prog_seg_t segs[EN_FREQSET_MAX_STEPS];
    int dropped = -1;
    int n = en_freqset_segs(text, len, 0, 0, 0.0, segs,
                            EN_FREQSET_MAX_STEPS, &dropped);
    CHECK(n == 3, "expected 3 segments, got %d", n);
    CHECK(dropped == 0, "expected nothing dropped, got %d", dropped);
    CHECK(segs[0].carrier_hz == 100.0 && segs[1].carrier_hz == 200.5 &&
          segs[2].carrier_hz == 300.0,
          "carriers were %g/%g/%g", segs[0].carrier_hz, segs[1].carrier_hz,
          segs[2].carrier_hz);
    CHECK(segs[0].seconds == 3 && segs[2].seconds == 3,
          "dwell was %u/%u", (unsigned)segs[0].seconds,
          (unsigned)segs[2].seconds);
    CHECK(segs[0].beat_start == 0.0 && segs[0].beat_end == 0.0 &&
          segs[0].layers == 0 && segs[0].noise == EN_NOISE_NONE,
          "a step should be one flat tone with no beat and no noise");

    /* The dwell override, which is what a longer run off the same set is. */
    n = en_freqset_segs(text, len, 1, 25, 0.0, segs, EN_FREQSET_MAX_STEPS, 0);
    CHECK(n == 2 && segs[0].seconds == 25 && segs[1].seconds == 25,
          "dwell override gave %d segments at %u s", n,
          (unsigned)segs[0].seconds);

    /* Above Nyquist is skipped and counted, never rendered: it would come out
       as a different frequency, which for a signal generator is a lie. */
    n = en_freqset_segs(text, len, 2, 0, 22050.0, segs,
                        EN_FREQSET_MAX_STEPS, &dropped);
    CHECK(n == 2 && dropped == 1, "got %d segments, %d dropped", n, dropped);
    CHECK(segs[0].carrier_hz == 1000.0 && segs[1].carrier_hz == 2000.0,
          "the surviving carriers were %g and %g",
          segs[0].carrier_hz, segs[1].carrier_hz);

    /* A set too long for the buffer is refused whole. Half a sweep would look
       like a sweep and play as something else. */
    n = en_freqset_segs(text, len, 0, 0, 0.0, segs, 2, &dropped);
    CHECK(n == 0, "a set that does not fit should be refused, got %d", n);

    /* And the real bundle, when it is there to read. */
    /* The platform layer is not linked into the tests, so the bundle is found
       the two ways it can be from here: where the device would export it, or
       where it lives in the tree when running from host/. */
    const char *env = getenv("ENTRAIN_DATA");
    char path[512];
    if (env && *env) snprintf(path, sizeof path, "%s/frequencies.set", env);
    else             snprintf(path, sizeof path,
                              "../../../data/Entrain/frequencies.set");
    FILE *f = fopen(path, "rb");
    if (f) {
        static char buf[262144];
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[got] = 0;
        int sets = en_freqset_count(buf, (uint32_t)got);
        CHECK(sets > 0, "the shipped bundle parsed to %d sets", sets);
        for (int i = 0; i < sets; i++) {
            CHECK(en_freqset_head(buf, (uint32_t)got, i, name, sizeof name,
                                  &steps, &dwell),
                  "bundle set %d has no head", i);
            CHECK(name[0] != 0, "bundle set %d has no name", i);
            CHECK(steps > 0 && steps <= EN_FREQSET_MAX_STEPS,
                  "bundle set %d '%s' has %d steps, past the %d cap",
                  i, name, steps, EN_FREQSET_MAX_STEPS);
            CHECK(dwell > 0, "bundle set %d '%s' has no dwell", i, name);
        }
        printf("  (bundle: %d sets, all within the cap)\n", sets);
    } else {
        printf("  (no bundle at %s, skipped)\n", path);
    }
}

static void test_progfile(void)
{
    section("programs loaded from a file");

    static const char text[] =
        "# Entrain measured programs, v1\n"
        "\n"
        "@Alpha Thing|600\n"
        "C102.5,300.25\n"
        "S120|4.0,4.5,1.0,0.8|3.8,3.8,0.0,0.25\n"
        "S480|4.5,2.0,0.8,0.2|3.8,3.7,0.25,0.5\n"
        "\n"
        "@Beta Thing|60\n"
        "C200\n"
        "S60|10.0,12.0,1.0,1.0\n";
    const uint32_t len = (uint32_t)(sizeof text - 1);

    CHECK(en_progfile_count(text, len) == 2,
          "expected 2 programs, got %d", en_progfile_count(text, len));

    char name[64];
    uint32_t secs = 0;
    CHECK(en_progfile_head(text, len, 0, name, sizeof name, &secs),
          "head 0 failed");
    CHECK(strcmp(name, "Alpha Thing") == 0, "name 0 is '%s'", name);
    CHECK(secs == 600, "seconds 0 is %u", secs);

    CHECK(en_progfile_head(text, len, 1, name, sizeof name, &secs),
          "head 1 failed");
    CHECK(strcmp(name, "Beta Thing") == 0, "name 1 is '%s'", name);
    CHECK(secs == 60, "seconds 1 is %u", secs);

    CHECK(!en_progfile_head(text, len, 2, name, sizeof name, &secs),
          "a third program was reported");

    static en_progfile_t pf;
    CHECK(en_progfile_load(text, len, 0, &pf), "load 0 failed");
    CHECK(pf.n_segs == 2, "expected 2 segments, got %d", pf.n_segs);
    CHECK(pf.layers == 2, "expected 2 layers, got %u", pf.layers);

    CHECK(pf.segs[0].seconds == 120, "seg 0 is %u s", pf.segs[0].seconds);
    CHECK(fabs(pf.segs[0].layer[0].carrier_hz - 102.5) < 1e-9,
          "layer 0 carrier %.4f", pf.segs[0].layer[0].carrier_hz);
    CHECK(fabs(pf.segs[0].layer[1].carrier_hz - 300.25) < 1e-9,
          "layer 1 carrier %.4f", pf.segs[0].layer[1].carrier_hz);
    CHECK(fabs(pf.segs[0].layer[1].level_start - 0.0) < 1e-9,
          "a layer silent at the start was not read as silent");
    CHECK(fabs(pf.segs[0].layer[1].level_end - 0.25) < 1e-9,
          "layer 1 end gain %.4f", pf.segs[0].layer[1].level_end);

    /* The scalar fields mirror layer zero, because the readout reads those. */
    CHECK(fabs(pf.segs[0].beat_start - 4.0) < 1e-9, "beat_start not mirrored");
    CHECK(fabs(pf.segs[1].beat_end - 2.0) < 1e-9, "beat_end not mirrored");

    /* Fields the format does not carry must be their documented defaults, not
       whatever the previous parse left behind. */
    CHECK(pf.segs[0].noise == EN_NOISE_NONE, "noise was not defaulted");
    CHECK(pf.segs[0].interp == EN_INTERP_SMOOTH, "interp was not set");

    /* And it evaluates like any other program. */
    en_layer_t lay[EN_MAX_LAYERS];
    en_noise_kind_t nk;
    double nl;
    uint8_t n = en_segs_layers_at(pf.segs, pf.n_segs, 0.0, EN_MODE_BINAURAL,
                                  lay, &nk, &nl);
    CHECK(n == 2, "evaluated %u layers", n);
    CHECK(fabs(lay[0].beat_hz - 4.0) < 1e-9, "beat at t=0 is %.3f",
          lay[0].beat_hz);

    CHECK(en_progfile_load(text, len, 1, &pf), "load 1 failed");
    CHECK(pf.n_segs == 1 && pf.layers == 1,
          "second program is %d segs / %u layers", pf.n_segs, pf.layers);

    /* Malformed input is refused rather than half-parsed. */
    static const char broken[] = "@No Carriers|10\nS10|1.0,1.0,1.0,1.0\n";
    CHECK(!en_progfile_load(broken, (uint32_t)(sizeof broken - 1), 0, &pf),
          "a segment before its carrier line was accepted");
}

static void test_wav_header(void)
{
    section("wav header");

    uint8_t h[EN_WAV_HEADER_BYTES];
    uint32_t total = en_wav_header(h, 11025, 2, 122500);

    CHECK(memcmp(h + 0, "RIFF", 4) == 0, "no RIFF tag");
    CHECK(memcmp(h + 8, "WAVE", 4) == 0, "no WAVE tag");
    CHECK(memcmp(h + 12, "fmt ", 4) == 0, "no fmt chunk");
    CHECK(memcmp(h + 36, "data", 4) == 0, "data chunk is not at offset 36");

    uint32_t data_bytes = (uint32_t)h[40] | ((uint32_t)h[41] << 8)
                        | ((uint32_t)h[42] << 16) | ((uint32_t)h[43] << 24);
    CHECK(data_bytes == 122500u * 4u, "data size %u wrong", data_bytes);
    CHECK(total == 44u + 122500u * 4u, "total %u wrong", total);
    CHECK(total == en_wav_size(122500, 2), "en_wav_size disagrees");

    uint32_t rate = (uint32_t)h[24] | ((uint32_t)h[25] << 8)
                  | ((uint32_t)h[26] << 16) | ((uint32_t)h[27] << 24);
    CHECK(rate == 11025u, "sample rate %u wrong", rate);
    CHECK(h[22] == 2 && h[23] == 0, "channel count wrong");
    CHECK(h[34] == 16 && h[35] == 0, "bit depth wrong");
}

/* ---- 9. programs and the user-program parser ---------------------------- */

static void test_programs(void)
{
    section("programs");

    int n;
    const en_program_t *ps = en_programs(&n);
    CHECK(n == 16, "expected 16 built-in programs, got %d", n);

    for (int i = 0; i < n; i++) {
        uint32_t secs = en_program_seconds(&ps[i]);
        printf("  %-20s %2u:%02u  %s  %s\n", ps[i].name, secs / 60, secs % 60,
               en_band_name(en_program_band(&ps[i])), ps[i].detail);
        CHECK(secs > 0, "%s has zero duration", ps[i].name);
        CHECK(ps[i].n_segs > 0, "%s has no segments", ps[i].name);

        /* The beat must be continuous across segment boundaries, or the audio
           would have to jump. */
        for (int s = 1; s < ps[i].n_segs; s++) {
            CHECK(fabs(ps[i].segs[s].beat_start - ps[i].segs[s - 1].beat_end)
                      < 1e-9,
                  "%s: beat jumps from %.3f to %.3f at segment %d",
                  ps[i].name, ps[i].segs[s - 1].beat_end,
                  ps[i].segs[s].beat_start, s);
        }

        /* Sampling the timeline must agree with the segment table. */
        CHECK(fabs(en_program_beat_at(&ps[i], 0.0) - ps[i].segs[0].beat_start)
                  < 1e-9, "%s: beat at t=0 wrong", ps[i].name);
        CHECK(en_program_seg_at(&ps[i], 0.0) == 0, "%s: seg at t=0", ps[i].name);
        CHECK(en_program_seg_at(&ps[i], (double)secs + 1.0) == -1,
              "%s: seg past the end should be -1", ps[i].name);

        /* Multi-layer tables carry the primary beat twice — once in the
           segment and once in layer zero — because the readout reads one and
           the renderer the other. They have to agree, and nothing but a test
           can keep two hand-written columns of numbers in step. */
        for (int s = 0; s < ps[i].n_segs; s++) {
            const en_prog_seg_t *sg = &ps[i].segs[s];
            if (!sg->layers) continue;
            CHECK(fabs(sg->layer[0].beat_start - sg->beat_start) < 1e-9,
                  "%s seg %d: layer 0 starts at %.3f, segment says %.3f",
                  ps[i].name, s, sg->layer[0].beat_start, sg->beat_start);
            CHECK(fabs(sg->layer[0].beat_end - sg->beat_end) < 1e-9,
                  "%s seg %d: layer 0 ends at %.3f, segment says %.3f",
                  ps[i].name, s, sg->layer[0].beat_end, sg->beat_end);

            /* Every layer's gain and carrier must be continuous too, or a
               layer steps in level at a join — which is a click, and the whole
               reason the source authored these as breakpoint curves. */
            if (s > 0) {
                const en_prog_seg_t *pv = &ps[i].segs[s - 1];
                CHECK(pv->layers == sg->layers,
                      "%s: layer count changes at segment %d", ps[i].name, s);
                for (uint8_t j = 0; j < sg->layers && j < pv->layers; j++) {
                    CHECK(fabs(sg->layer[j].level_start - pv->layer[j].level_end)
                              < 1e-9,
                          "%s seg %d layer %d: level jumps %.4f -> %.4f",
                          ps[i].name, s, j, pv->layer[j].level_end,
                          sg->layer[j].level_start);
                    CHECK(fabs(sg->layer[j].carrier_hz - pv->layer[j].carrier_hz)
                              < 1e-9,
                          "%s seg %d layer %d: carrier moves", ps[i].name, s, j);
                }
            }
        }
    }

    /* ---- the imported suites, against the numbers they were ported from --- */

    section("imported suites");

    const en_program_t *xp = 0, *st1 = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(ps[i].name, "Extended Practice"))      xp  = &ps[i];
        if (!strcmp(ps[i].name, "Stage 1"))                st1 = &ps[i];
    }
    CHECK(xp != 0, "Extended Practice is missing");
    CHECK(st1 != 0, "Stage 1 is missing");

    if (xp) {
        CHECK(en_program_seconds(xp) == 6000,
              "extended practice is %u s, the source is 6000",
              en_program_seconds(xp));

        /* The source's own cue table, checked at every cue. These are the
           numbers the port exists to reproduce; if one moves, the port is
           wrong however good the audio sounds. */
        const struct { double t, beat; } cues[] = {
            {    0.0, 10.0 }, {  300.0, 10.0 }, {  900.0,  7.0 },
            { 1800.0,  4.0 }, { 2220.0,  4.0 }, { 2400.0,  7.0 },
            { 2700.0,  4.0 }, { 4680.0,  4.0 }, { 5100.0,  7.0 },
            { 5940.0, 12.0 },
        };
        for (size_t c = 0; c < sizeof cues / sizeof cues[0]; c++) {
            double got = en_program_beat_at(xp, cues[c].t);
            CHECK(fabs(got - cues[c].beat) < 1e-6,
                  "extended practice at %.0f s: %.4f Hz, source says %.4f",
                  cues[c].t, got, cues[c].beat);
        }

        /* Two layers, and the second is 13.7 dB down through the main block —
           the one relationship in the piece that a listener would notice. */
        en_layer_t lay[EN_MAX_LAYERS];
        en_noise_kind_t nk;
        double nl;
        uint8_t nlay = en_segs_layers_at(xp->segs, xp->n_segs, 3000.0,
                                         xp->mode, lay, &nk, &nl);
        CHECK(nlay == 2, "extended practice has %u layers at 50:00", nlay);
        CHECK(fabs(lay[0].carrier_hz - 200.0) < 1e-9, "primary is not 200 Hz");
        CHECK(fabs(lay[1].carrier_hz - 260.0) < 1e-9, "secondary is not 260 Hz");
        CHECK(fabs(lay[1].beat_hz - 7.0) < 1e-9,
              "the secondary layer should hold 7 Hz, got %.3f", lay[1].beat_hz);
        double db = 20.0 * log10(lay[1].level / lay[0].level);
        CHECK(fabs(db + 13.67) < 0.05,
              "secondary is %.2f dB down, the source says -13.67", db);

        /* Before Part B the secondary is silent, not merely quiet. */
        en_segs_layers_at(xp->segs, xp->n_segs, 600.0, xp->mode, lay, &nk, &nl);
        CHECK(lay[1].level == 0.0,
              "the secondary layer sounds before 40:00 (level %.4f)",
              lay[1].level);

        /* End to end: build a segment the way the engine's stream path does and
           render it, then look for both carriers in the output. Everything
           above tests the table; this tests that the table reaches the audio.
           A layer lost between the two would pass every check before this one. */
        const uint32_t sr = 11025;
        const uint32_t frames = sr;
        int16_t *pcm = malloc(frames * 2 * sizeof *pcm);

        en_segment_t seg;
        memset(&seg, 0, sizeof seg);
        seg.sample_rate = sr;
        seg.mode = xp->mode;
        seg.frames = frames;
        seg.start.tone_level = 0.8;
        seg.end.tone_level = 0.8;
        seg.layers = en_segs_layers_at(xp->segs, xp->n_segs, 3000.0, xp->mode,
                                       seg.lstart, &seg.noise,
                                       &seg.start.noise_level);
        en_segs_layers_at(xp->segs, xp->n_segs, 3001.0, xp->mode, seg.lend,
                          &nk, &seg.end.noise_level);
        /* No bed for this one: pink noise across the whole spectrum would sit
           under both bins and blunt exactly what is being measured. */
        seg.noise = EN_NOISE_NONE;
        seg.start.noise_level = seg.end.noise_level = 0.0;

        en_render_t rr;
        en_render_init(&rr, sr, EN_NOISE_NONE, 1);
        en_render_segment(&rr, &seg, pcm);

        double m200 = bin_mag(pcm, frames, 0, sr, 200.0);
        double m260 = bin_mag(pcm, frames, 0, sr, 260.0);
        printf("  rendered at 50:00 — 200 Hz %.4f, 260 Hz %.4f\n", m200, m260);
        CHECK(m200 > 0.3, "the primary carrier is not in the rendered audio");
        CHECK(m260 > 0.05, "the secondary carrier is not in the rendered audio");
        double rdb = 20.0 * log10(m260 / m200);
        CHECK(fabs(rdb + 13.67) < 0.6,
              "rendered layer balance is %.2f dB, the table says -13.67", rdb);

        free(pcm);
    }

    if (st1) {
        CHECK(en_program_seconds(st1) == 2160,
              "stage one is %u s, the source is 2160",
              en_program_seconds(st1));

        en_layer_t lay[EN_MAX_LAYERS];
        en_noise_kind_t nk;
        double nl;
        uint8_t nlay = en_segs_layers_at(st1->segs, st1->n_segs, 1500.0,
                                         st1->mode, lay, &nk, &nl);
        CHECK(nlay == 3, "stage one has %u layers at 25:00", nlay);
        CHECK(fabs(lay[0].carrier_hz - 300.0) < 1e-9, "main carrier is not 300 Hz");
        CHECK(fabs(lay[1].carrier_hz - 104.0) < 1e-9, "warm carrier is not 104 Hz");
        CHECK(fabs(lay[2].carrier_hz - 496.0) < 1e-9, "support is not 496 Hz");

        /* All three share one beat curve, which is how the source builds a
           stage — the support layers are the same beat at another carrier. */
        CHECK(fabs(lay[1].beat_hz - lay[0].beat_hz) < 1e-9,
              "the warm layer carries a different beat");
        CHECK(fabs(lay[2].beat_hz - lay[0].beat_hz) < 1e-9,
              "the support layer carries a different beat");

        /* Layer gains are normalised to the main carrier's peak amplitude, so
           a gain of 1.0 on the warm layer means 0.045/0.070 here. */
        en_segs_layers_at(st1->segs, st1->n_segs, 1000.0, st1->mode, lay,
                          &nk, &nl);
        CHECK(lay[0].level > lay[1].level && lay[1].level > lay[2].level,
              "stage one layer balance is not main > warm > support "
              "(%.4f, %.4f, %.4f)", lay[0].level, lay[1].level, lay[2].level);

        /* And the whole mix still fits under full scale once the master gain
           and the headroom are applied. This is what stops a three-layer sum
           from living permanently in the soft clipper. */
        for (int s = 0; s < st1->n_segs; s++) {
            double sum0 = 0.0, sum1 = 0.0;
            for (uint8_t j = 0; j < st1->segs[s].layers; j++) {
                sum0 += st1->segs[s].layer[j].level_start;
                sum1 += st1->segs[s].layer[j].level_end;
            }
            double peak = (sum0 > sum1 ? sum0 : sum1) * 0.8 * EN_HEADROOM;
            CHECK(peak < 1.0,
                  "stage one seg %d peaks at %.3f of full scale", s, peak);
        }
    }

    section("user program parser");

    const char *good =
        "# a user program\n"
        "name    Evening Slide\n"
        "mode    isochronic\n"
        "carrier 180\n"
        "noise   pink 0.2\n"
        "seg     10 6 900\n"
        "seg     6 2.5 1200\n";

    en_user_program_t up;
    int line = 0;
    int rc = en_parse_program(good, (uint32_t)strlen(good), &up, &line);
    CHECK(rc == EN_PARSE_OK, "parse failed: %s at line %d",
          en_parse_error_text(rc), line);
    CHECK(strcmp(up.name, "Evening Slide") == 0, "name is '%s'", up.name);
    CHECK(up.mode == EN_MODE_ISOCHRONIC, "mode wrong");
    CHECK(fabs(up.carrier_hz - 180.0) < 1e-9, "carrier %.3f", up.carrier_hz);
    CHECK(up.noise == EN_NOISE_PINK, "noise kind wrong");
    CHECK(fabs(up.noise_level - 0.2) < 1e-9, "noise level %.3f", up.noise_level);
    CHECK(up.n_segs == 2, "%d segments", up.n_segs);
    CHECK(fabs(up.segs[1].beat_end - 2.5) < 1e-9, "fractional beat not parsed");
    CHECK(up.segs[0].seconds == 900, "seconds wrong");

    /* A file with no name still loads, under a default. */
    const char *nameless = "seg 10 10 600\n";
    rc = en_parse_program(nameless, (uint32_t)strlen(nameless), &up, &line);
    CHECK(rc == EN_PARSE_OK, "nameless program should parse");
    CHECK(up.name[0] != 0, "nameless program got no default name");

    /* Bad input must be rejected with the offending line, not silently
       half-accepted. */
    struct { const char *text; int want; } bad[] = {
        { "name X\nwibble 3\nseg 10 10 60\n", EN_PARSE_BAD_KEY },
        { "mode sideways\nseg 10 10 60\n",    EN_PARSE_BAD_VALUE },
        { "carrier 5\nseg 10 10 60\n",        EN_PARSE_BAD_VALUE },
        { "seg 10 10\n",                      EN_PARSE_BAD_VALUE },
        { "name Empty\n",                     EN_PARSE_NO_SEGS },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        rc = en_parse_program(bad[i].text, (uint32_t)strlen(bad[i].text),
                              &up, &line);
        CHECK(rc == bad[i].want, "case %u: got %d (%s), wanted %d",
              i, rc, en_parse_error_text(rc), bad[i].want);
    }

    /* Too many segments must be refused rather than overrun the array. */
    char many[2048];
    int off = 0;
    off += sprintf(many + off, "name Many\n");
    for (int i = 0; i < EN_USER_MAX_SEGS + 4; i++)
        off += sprintf(many + off, "seg 10 10 60\n");
    rc = en_parse_program(many, (uint32_t)off, &up, &line);
    CHECK(rc == EN_PARSE_TOO_MANY_SEGS, "overlong program was accepted (%d)",
          rc);
}

/* ---- 10. bands ---------------------------------------------------------- */

static void test_bands(void)
{
    section("bands");
    CHECK(en_band_of(2.0)  == EN_BAND_DELTA, "2 Hz is not Delta");
    CHECK(en_band_of(6.0)  == EN_BAND_THETA, "6 Hz is not Theta");
    CHECK(en_band_of(7.83) == EN_BAND_THETA, "7.83 Hz is not Theta");
    CHECK(en_band_of(10.0) == EN_BAND_ALPHA, "10 Hz is not Alpha");
    CHECK(en_band_of(16.0) == EN_BAND_BETA,  "16 Hz is not Beta");
    CHECK(en_band_of(40.0) == EN_BAND_GAMMA, "40 Hz is not Gamma");
    /* Boundaries land in the upper band. */
    CHECK(en_band_of(4.0)  == EN_BAND_THETA, "4 Hz should be Theta");
    CHECK(en_band_of(8.0)  == EN_BAND_ALPHA, "8 Hz should be Alpha");
    CHECK(en_band_of(13.0) == EN_BAND_BETA,  "13 Hz should be Beta");
    CHECK(en_band_of(30.0) == EN_BAND_GAMMA, "30 Hz should be Gamma");

    for (int b = 0; b < EN_BAND_COUNT; b++) {
        CHECK(en_band_color((en_band_t)b) != 0, "band %d has no colour", b);
        CHECK(en_band_name((en_band_t)b)[0] != '?', "band %d has no name", b);
    }
}

/* ---- main --------------------------------------------------------------- */

int main(void)
{
    printf("entrain core tests\n");

    test_sine();
    test_planner();
    test_beat_accuracy();
    test_clicks();
    test_loop_seams();
    test_noise_bed();
    test_crossfade_combs();
    test_block_duration();
    test_glide();
    test_levels();
    test_layers();
    test_progfile();
    test_freqset();
    test_wav_header();
    test_programs();
    test_bands();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
