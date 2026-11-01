/*
 * osc.h — phase-accumulator oscillators.
 *
 * 32-bit accumulators: the phase wraps naturally on overflow, so there is no
 * drift and no conditional in the inner loop, and the frequency resolution is
 * sr / 2^32 — about 2.6e-6 Hz at 11025 Hz. Well past what anyone can hear, and
 * well past what the loop-planner needs.
 *
 * Sine comes from a 2048-entry int16 table with linear interpolation. The table
 * is built at init from a Taylor series, so this file needs no libm and no
 * generated data — it compiles the same on the host and on the device, which is
 * the whole point of keeping core/ dependency-free.
 */

#ifndef ENTRAIN_OSC_H
#define ENTRAIN_OSC_H

#include <stdint.h>

#define EN_SINE_BITS 11
#define EN_SINE_SIZE (1 << EN_SINE_BITS)   /* 2048 */

/* Build the shared sine table. Idempotent; call before any oscillator runs. */
void en_osc_init(void);

/* sin(2*pi*turns) for turns in [0,1), from the table. Exposed for tests and for
   the gate/envelope code, which wants a sine without owning an oscillator. */
int32_t en_sine_lookup(uint32_t phase);

/* sin(2*pi*turns) in double, computed directly (no table). Used where accuracy
   matters more than speed: building the table, and the host test suite. */
double en_sin_turns(double turns);

typedef struct {
    uint32_t phase;   /* current position, full 32-bit turn */
    uint32_t step;    /* added per sample */
} en_osc_t;

/* Phase step for `hz` at `sample_rate`. Rounds to nearest. */
uint32_t en_osc_step(double hz, uint32_t sample_rate);

void en_osc_set_freq(en_osc_t *o, double hz, uint32_t sample_rate);

/* Place the oscillator at an exact fraction of a turn. Only used to seed a
   render; never call it mid-stream, that is what a click sounds like. */
void en_osc_set_phase_turns(en_osc_t *o, double turns);

/* Current position as a fraction of a turn, [0,1). */
double en_osc_phase_turns(const en_osc_t *o);

/* Advance one sample, returning the sample in [-32768, 32767]. */
static inline int32_t en_osc_next(en_osc_t *o)
{
    int32_t s = en_sine_lookup(o->phase);
    o->phase += o->step;
    return s;
}

#endif /* ENTRAIN_OSC_H */
