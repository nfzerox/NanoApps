/*
 * noise.c — see noise.h.
 */

#include "noise.h"

void en_noise_init(en_noise_t *n, en_noise_kind_t kind, uint32_t seed)
{
    n->kind = kind;
    n->rng = seed ? seed : 0x1234567u;   /* xorshift dies on zero */
    n->b0 = n->b1 = n->b2 = 0.0;
    n->brown = 0.0;
}

/* xorshift32. Cheap, no multiply, and good enough for a noise bed. */
static double white(en_noise_t *n)
{
    uint32_t x = n->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    n->rng = x;
    /* to [-1, 1) */
    return (double)(int32_t)x * (1.0 / 2147483648.0);
}

double en_noise_next(en_noise_t *n)
{
    double w;
    switch (n->kind) {
    case EN_NOISE_WHITE:
        return white(n);

    case EN_NOISE_PINK:
        /* Paul Kellett's economy 3-pole filter: about -10 dB/decade across the
           audible band, which is close enough to true 1/f for a bed and costs
           three multiply-adds. */
        w = white(n);
        n->b0 = 0.99765 * n->b0 + w * 0.0990460;
        n->b1 = 0.96300 * n->b1 + w * 0.2965164;
        n->b2 = 0.57000 * n->b2 + w * 1.0526913;
        return (n->b0 + n->b1 + n->b2 + w * 0.1848) * 0.22;

    case EN_NOISE_BROWN:
        /* Leaky integrator. The leak keeps it from wandering off to a DC
           offset over a long render, which a pure integrator would. */
        w = white(n);
        n->brown = (n->brown + 0.02 * w) / 1.02;
        return n->brown * 12.0;

    case EN_NOISE_NONE:
    default:
        return 0.0;
    }
}

const char *en_noise_name(en_noise_kind_t kind)
{
    switch (kind) {
    case EN_NOISE_WHITE: return "White";
    case EN_NOISE_PINK:  return "Pink";
    case EN_NOISE_BROWN: return "Brown";
    default:             return "None";
    }
}
