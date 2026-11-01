#ifndef DJ_RANDOM_H
#define DJ_RANDOM_H
#include <stdint.h>
typedef struct {uint32_t state[31];int front,rear;} DJRandom;
void dj_srandom(DJRandom *,uint32_t);
uint32_t dj_random(DJRandom *);
#endif
