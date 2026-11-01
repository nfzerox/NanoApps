/*
 * noise.h — white, pink and brown noise for the bed under the tones.
 *
 * Deterministic: same seed, same samples, on host and device alike. The tests
 * depend on that, and so does the render cache — a preset must render to
 * byte-identical audio every time or the cache key lies.
 */

#ifndef ENTRAIN_NOISE_H
#define ENTRAIN_NOISE_H

#include <stdint.h>

typedef enum {
    EN_NOISE_NONE = 0,
    EN_NOISE_WHITE,
    EN_NOISE_PINK,
    EN_NOISE_BROWN
} en_noise_kind_t;

typedef struct {
    en_noise_kind_t kind;
    uint32_t rng;
    double b0, b1, b2;   /* pink: Kellett's 3-pole state */
    double brown;        /* brown: leaky integrator state */
} en_noise_t;

void en_noise_init(en_noise_t *n, en_noise_kind_t kind, uint32_t seed);

/* Next sample, roughly in [-1, 1]. Pink and brown are scaled so all three
   kinds sit at a similar loudness for the same level setting — otherwise the
   noise slider would mean something different per kind. */
double en_noise_next(en_noise_t *n);

const char *en_noise_name(en_noise_kind_t kind);

#endif /* ENTRAIN_NOISE_H */
