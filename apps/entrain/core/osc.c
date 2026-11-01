/*
 * osc.c — see osc.h.
 */

#include "osc.h"

#define EN_TWO_PI 6.283185307179586476925286766559

/* One extra entry so interpolation at the last index can read [i+1] without a
   wrap test in the inner loop. */
static int16_t s_sine[EN_SINE_SIZE + 1];
static int     s_ready;

/* sin(x) for x in [0, pi/2] by Taylor series. Seven terms leaves an error under
   1e-9 across the quadrant — three orders of magnitude finer than the 16-bit
   table it fills, and it keeps core/ free of libm. */
static double sin_quadrant(double x)
{
    double x2 = x * x;
    double t = x;      /* x^1 / 1! */
    double sum = t;
    t *= -x2 / (2 * 3);   sum += t;
    t *= -x2 / (4 * 5);   sum += t;
    t *= -x2 / (6 * 7);   sum += t;
    t *= -x2 / (8 * 9);   sum += t;
    t *= -x2 / (10 * 11); sum += t;
    t *= -x2 / (12 * 13); sum += t;
    return sum;
}

double en_sin_turns(double turns)
{
    /* Fold to [0,1) first so large inputs stay accurate. */
    double f = turns - (double)(int64_t)turns;
    if (f < 0.0) f += 1.0;

    /* Then to a quadrant, so the series is only ever evaluated on [0, pi/2]. */
    if (f <= 0.25) return  sin_quadrant(f * EN_TWO_PI);
    if (f <= 0.50) return  sin_quadrant((0.5 - f) * EN_TWO_PI);
    if (f <= 0.75) return -sin_quadrant((f - 0.5) * EN_TWO_PI);
    return                -sin_quadrant((1.0 - f) * EN_TWO_PI);
}

void en_osc_init(void)
{
    if (s_ready) return;
    for (int i = 0; i <= EN_SINE_SIZE; i++) {
        double v = en_sin_turns((double)i / (double)EN_SINE_SIZE) * 32767.0;
        s_sine[i] = (int16_t)(v < 0.0 ? v - 0.5 : v + 0.5);
    }
    s_ready = 1;
}

int32_t en_sine_lookup(uint32_t phase)
{
    uint32_t idx  = phase >> (32 - EN_SINE_BITS);          /* 0..2047 */
    uint32_t frac = (phase >> (16 - EN_SINE_BITS)) & 0xFFFFu;
    int32_t  a = s_sine[idx];
    int32_t  b = s_sine[idx + 1];
    return a + (((b - a) * (int32_t)frac) >> 16);
}

uint32_t en_osc_step(double hz, uint32_t sample_rate)
{
    if (sample_rate == 0) return 0;
    double s = hz * 4294967296.0 / (double)sample_rate;
    /* Negative frequencies are meaningless here and would alias oddly; clamp. */
    if (s < 0.0) s = 0.0;
    if (s > 4294967295.0) s = 4294967295.0;
    return (uint32_t)(s + 0.5);
}

void en_osc_set_freq(en_osc_t *o, double hz, uint32_t sample_rate)
{
    o->step = en_osc_step(hz, sample_rate);
}

void en_osc_set_phase_turns(en_osc_t *o, double turns)
{
    double f = turns - (double)(int64_t)turns;
    if (f < 0.0) f += 1.0;
    o->phase = (uint32_t)(f * 4294967296.0);
}

double en_osc_phase_turns(const en_osc_t *o)
{
    return (double)o->phase / 4294967296.0;
}
