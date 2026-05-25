/*
 * hb_accel.c — read 3-axis accelerometer samples.
 *
 * The OS runs a high-priority accelerometer task that polls the
 * hardware and stores each sample into a global accel struct at
 * 0x08a91e50 (3 × int32_t = X, Y, Z raw). The scale factor
 * 1/65536 ≈ 1.52588e-5 converts raw counts to g-units.
 *
 * We just memcpy from that global — no syscalls, no locking. Sampling
 * lag is at most one period of the OS task (~10ms for normal motion,
 * ~1ms during shake-detect mode). Concurrent writes are atomic at
 * word granularity (Cortex-A8 + 32-bit aligned stores).
 */

#include "hb_sdk.h"

#define ACCEL_SAMPLE_GLOBAL 0x08a91e50u

void hb_accel_read_raw(int32_t out[3])
{
    volatile int32_t *p = (volatile int32_t *)ACCEL_SAMPLE_GLOBAL;
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
}

/* Scaled to milli-g (1g = 1000). Avoids float math but stays human-
   readable: stationary device flat-on-back reads roughly z=+1000.

   Conversion: raw * (1000 / 65536). Implemented as
   `(raw * 1000) >> 16` which keeps full int32 precision and avoids
   any floating-point ops. */
void hb_accel_read_milli_g(int32_t out[3])
{
    int32_t raw[3];
    hb_accel_read_raw(raw);
    out[0] = (raw[0] * 1000) >> 16;
    out[1] = (raw[1] * 1000) >> 16;
    out[2] = (raw[2] * 1000) >> 16;
}
