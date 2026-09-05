/*
 * golf_physics.h — pure math core for the Golf ball's physics.
 *
 * No SDK/LVGL dependency on purpose: this header is compiled both into the
 * app (apps/golf/golf.c, ARM target) and, unmodified, into a native host
 * test harness (apps/golf/test_golf_physics.c), so the same code that ships
 * on-device is exercised by the native tests too.
 */
#ifndef GOLF_PHYSICS_H
#define GOLF_PHYSICS_H

#include <stdint.h>

/*
 * Self-contained sqrtf, no libm call.
 *
 * The standard <math.h> sqrtf does not link against this app's build flags
 * (sdk/hb_app.mk: -fno-builtin -ffreestanding -nostdlib, with only libgcc
 * on the link line — no libm), so this replacement uses only basic float
 * ops (add/mul/div): the classic IEEE-754 bit-trick for an initial guess at
 * 1/sqrt(x), refined by three Newton-Raphson iterations to full float32
 * precision, then x*(1/sqrt(x)). Kept local to apps/golf/ — no change to
 * the shared SDK.
 */
static inline float phys_sqrtf(float x)
{
    if (x <= 0.0f) return 0.0f;
    union { float f; uint32_t i; } conv;
    conv.f = x;
    conv.i = 0x5f3759dfu - (conv.i >> 1);
    float y = conv.f;
    float xhalf = 0.5f * x;
    y = y * (1.5f - xhalf * y * y);
    y = y * (1.5f - xhalf * y * y);
    y = y * (1.5f - xhalf * y * y);
    return x * y;
}

/* Rounds a float to the nearest int (ties away from zero) without a libm
 * call. Used only for drawing/collision-test coordinates; never write the
 * rounded value back into the float state. */
static inline int phys_round(float v)
{
    return (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

/*
 * Advances one fixed physics step under constant-magnitude deceleration.
 * Direction is preserved (scaled, never subtracted per-axis), so speed can
 * only shrink monotonically toward exactly zero, never overshoot past it or
 * flip sign. When the step would cross zero speed partway through, the
 * partial-step time-to-stop is computed exactly and the residual distance
 * (speed^2 / (2*decel)) is applied for that fraction only — the remainder of
 * the step is correctly left at rest, not skipped and not over-advanced.
 *
 * Because deceleration is constant, v(t) is linear within the step, so the
 * trapezoidal (start+end)/2 average speed used for the position update is
 * exact (not an approximation) — total distance over a full stop matches
 * v0^2/(2*decel) up to float rounding only.
 */
static inline void phys_integrate_step(float *x, float *y, float *vx, float *vy,
                                        float decel, float h)
{
    float speed = phys_sqrtf((*vx) * (*vx) + (*vy) * (*vy));
    if (speed <= 0.0f) {
        *vx = 0.0f; *vy = 0.0f;
        return;
    }
    float dirx = *vx / speed, diry = *vy / speed;
    float t_stop = speed / decel;
    if (t_stop >= h) {
        float new_speed = speed - decel * h;
        float avg = (speed + new_speed) * 0.5f;
        *x += dirx * avg * h;
        *y += diry * avg * h;
        *vx = dirx * new_speed;
        *vy = diry * new_speed;
    } else {
        float avg = speed * 0.5f;
        *x += dirx * avg * t_stop;
        *y += diry * avg * t_stop;
        *vx = 0.0f;
        *vy = 0.0f;
    }
}

/* Clamps the length of (vx,vy) to max_speed, preserving direction. */
static inline void phys_clamp_speed(float *vx, float *vy, float max_speed)
{
    float speed = phys_sqrtf((*vx) * (*vx) + (*vy) * (*vy));
    if (speed > max_speed && speed > 0.0f) {
        float s = max_speed / speed;
        *vx *= s;
        *vy *= s;
    }
}

/*
 * Maps a drag vector (px, ball->release) to a launch velocity (px/s):
 * direction opposite the drag, magnitude gain*|drag| clamped to max_speed —
 * a limit on the VECTOR LENGTH, so shot power doesn't depend on aim angle.
 * Caller is responsible for the dead-zone check (reject drags with
 * dx*dx+dy*dy < 64 before calling this).
 */
static inline void phys_launch_from_drag(float dragx, float dragy, float gain,
                                          float max_speed, float *vx, float *vy)
{
    float len = phys_sqrtf(dragx * dragx + dragy * dragy);
    if (len <= 0.0f) { *vx = 0.0f; *vy = 0.0f; return; }
    float speed = gain * len;
    if (speed > max_speed) speed = max_speed;
    *vx = -(dragx / len) * speed;
    *vy = -(dragy / len) * speed;
}

/*
 * Advances the fixed-step accumulator by one real-time sample. Classifies
 * a gap above pause_threshold_us as a pause (app backgrounded / stall) and
 * drops it entirely (no catch-up steps); otherwise adds it to the budget
 * and clamps the budget to max_accum_us (bounded catch-up for ordinary
 * jitter, e.g. a missed render frame) — excess is discarded, never carried
 * forward, so a burst of catch-up steps across future frames cannot build
 * up. uint32 subtraction for dt is correct modulo 2^32 across a single
 * counter wrap (~71 min at 1 tick/us), which a single real-time sample
 * capped by pause_threshold_us can never approach.
 *
 * Returns the resulting accumulator value; caller drains it with
 * `while (accum >= FIXED_STEP_US) { accum -= FIXED_STEP_US; step(...); }`,
 * subtracting the step BEFORE running it so a mid-loop reset of *accum_us
 * (e.g. the step sinks the ball or the ball stops) is never followed by a
 * stray post-call subtraction that would underflow the unsigned counter.
 */
static inline uint32_t phys_clock_tick(uint32_t *last_now_us, uint32_t *accum_us,
                                        uint32_t now_us, uint32_t pause_threshold_us,
                                        uint32_t max_accum_us)
{
    uint32_t dt = now_us - *last_now_us;
    *last_now_us = now_us;
    if (dt > pause_threshold_us) {
        *accum_us = 0;
    } else {
        *accum_us += dt;
        if (*accum_us > max_accum_us) *accum_us = max_accum_us;
    }
    return *accum_us;
}

/*
 * Advances to the next hole after a sink (state SUNK, on_press's tap to
 * continue). Returns 1 (and leaves *hole_idx unchanged) when the hole just
 * played was the last one, so the caller can switch to the tour-end state
 * WITHOUT ever indexing s_holes past its last valid element (s_holes has
 * n_holes elements, valid indices 0..n_holes-1). Returns 0 after
 * incrementing *hole_idx to the next hole to load.
 *
 * Pure function, no globals, so it's testable directly — see
 * test_golf_physics.c.
 */
static inline int phys_advance_hole(int *hole_idx, int n_holes)
{
    if (*hole_idx + 1 >= n_holes) return 1;
    (*hole_idx)++;
    return 0;
}

#endif /* GOLF_PHYSICS_H */
