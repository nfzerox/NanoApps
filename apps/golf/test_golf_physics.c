/*
 * test_golf_physics.c — native host test harness for golf_physics.h.
 *
 * Compiles and runs on the dev machine with the host compiler (not the ARM
 * cross toolchain), against the exact same golf_physics.h shipped in
 * apps/golf/golf.c — not a reimplementation of its formulas.
 *
 * Build & run:
 *   cc -std=c11 -Wall -Wextra -O2 -o /tmp/test_golf_physics \
 *      apps/golf/test_golf_physics.c -lm && /tmp/test_golf_physics
 */
#include "golf_physics.h"
#include <stdio.h>
#include <math.h>

/* pi, spelled out locally: M_PI isn't part of standard C and isn't
   guaranteed available under a strict -std=c11 build. */
#define TEST_PI 3.14159265358979323846

/* Mirrored from golf.c's tuning constants (not finalized) — golf_physics.h
   intentionally carries no game-specific tuning values. */
#define A_GRASS_TEST   300.0f
#define A_SAND_TEST    1200.0f
#define VMAX_TEST      600.0f
#define DRAG_GAIN_TEST 7.5f
#define FIXED_STEP_US_TEST 8333u

static int g_fail = 0;

static void check(int cond, const char *what)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        g_fail++;
    }
}

static void check_close(const char *what, double got, double want, double tol)
{
    double d = fabs(got - want);
    if (d > tol) {
        printf("FAIL: %s (got %.6f, want %.6f, |diff|=%.6f > tol %.6f)\n",
               what, got, want, d, tol);
        g_fail++;
    }
}

/* ---- 1) Stop guarantee + no direction reversal, both signs, diagonals ---- */
static void test_stop_and_direction(void)
{
    const float STEP_S = FIXED_STEP_US_TEST * 1e-6f;
    struct { float vx0, vy0; float decel; const char *name; } cases[] = {
        {  10.0f,    0.0f, A_GRASS_TEST, "tiny +x, grass" },
        { -10.0f,    0.0f, A_GRASS_TEST, "tiny -x, grass" },
        {   0.0f,   10.0f, A_SAND_TEST,  "tiny +y, sand"  },
        {   0.0f,  -10.0f, A_SAND_TEST,  "tiny -y, sand"  },
        { 600.0f,    0.0f, A_GRASS_TEST, "max +x, grass"  },
        {-600.0f,    0.0f, A_GRASS_TEST, "max -x, grass"  },
        { 424.26f, 424.26f, A_GRASS_TEST, "max diagonal ++, grass" },
        {-424.26f,-424.26f, A_SAND_TEST,  "max diagonal --, sand"  },
        { 424.26f,-424.26f, A_GRASS_TEST, "max diagonal +-, grass" },
        {  60.0f,    0.0f, A_GRASS_TEST, "weakest legal drag (8px), grass" },
    };
    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        float x = 0, y = 0, vx = cases[c].vx0, vy = cases[c].vy0;
        int sx0 = (vx > 0) - (vx < 0), sy0 = (vy > 0) - (vy < 0);
        int reversed = 0;
        int iters = 0, max_iters = 100000;
        while ((vx != 0.0f || vy != 0.0f) && iters < max_iters) {
            phys_integrate_step(&x, &y, &vx, &vy, cases[c].decel, STEP_S);
            int sx = (vx > 0) - (vx < 0), sy = (vy > 0) - (vy < 0);
            if (sx != 0 && sx0 != 0 && sx != sx0) reversed = 1;
            if (sy != 0 && sy0 != 0 && sy != sy0) reversed = 1;
            iters++;
        }
        char buf[128];
        snprintf(buf, sizeof buf, "%s: reaches exact stop within %d iters", cases[c].name, max_iters);
        check(vx == 0.0f && vy == 0.0f, buf);
        snprintf(buf, sizeof buf, "%s: no direction reversal during braking", cases[c].name);
        check(!reversed, buf);
    }
}

/* ---- 2) Braking distance == v^2/(2a), exact integrator, step-size independent ---- */
static void test_distance_matches_formula(void)
{
    float steps_us[] = { 8333.0f, 16667.0f, 33333.0f }; /* ~120/60/30 Hz-equivalent */
    float speeds[]   = { 600.0f, 300.0f, 88.0f, 6.0f };
    float decels[]   = { 300.0f, 1200.0f };
    for (size_t si = 0; si < sizeof(steps_us)/sizeof(steps_us[0]); si++) {
        for (size_t vi = 0; vi < sizeof(speeds)/sizeof(speeds[0]); vi++) {
            for (size_t ai = 0; ai < sizeof(decels)/sizeof(decels[0]); ai++) {
                float h = steps_us[si] * 1e-6f;
                float v0 = speeds[vi], a = decels[ai];
                float x = 0, y = 0, vx = v0, vy = 0;
                int iters = 0, max_iters = 200000;
                while ((vx != 0.0f || vy != 0.0f) && iters < max_iters) {
                    phys_integrate_step(&x, &y, &vx, &vy, a, h);
                    iters++;
                }
                double analytical = (double)v0 * (double)v0 / (2.0 * (double)a);
                char buf[160];
                snprintf(buf, sizeof buf,
                         "distance v0=%.1f a=%.1f h=%.0fus: matches v^2/(2a)",
                         v0, a, steps_us[si]);
                /* Exact integrator: tolerance is float rounding only, not method error. */
                check_close(buf, x, analytical, 0.05);
            }
        }
    }
}

/* ---- 3) Equal drag length -> equal launch speed at any angle; V_MAX clamp ---- */
static void test_drag_mapping_isotropic(void)
{
    float angles_deg[] = { 0, 30, 45, 60, 90, 137, 180, 233, 270, 315 };
    float drag_len = 50.0f;
    double first_speed = -1;
    for (size_t i = 0; i < sizeof(angles_deg)/sizeof(angles_deg[0]); i++) {
        double rad = angles_deg[i] * TEST_PI / 180.0;
        float dragx = (float)(drag_len * cos(rad));
        float dragy = (float)(drag_len * sin(rad));
        float vx, vy;
        phys_launch_from_drag(dragx, dragy, DRAG_GAIN_TEST, VMAX_TEST, &vx, &vy);
        double speed = sqrt((double)vx*vx + (double)vy*vy);
        if (first_speed < 0) first_speed = speed;
        char buf[128];
        snprintf(buf, sizeof buf, "drag angle %.0f deg: same speed as angle 0", angles_deg[i]);
        check_close(buf, speed, first_speed, 1e-3);
        /* direction must be exactly opposite the drag vector */
        double dot = (double)vx*dragx + (double)vy*dragy;
        char buf2[128];
        snprintf(buf2, sizeof buf2, "drag angle %.0f deg: launch opposite drag direction", angles_deg[i]);
        check(dot < 0.0, buf2);
    }
    /* V_MAX clamp: a long drag must not exceed VMAX_TEST */
    float vx, vy;
    phys_launch_from_drag(200.0f, 0.0f, DRAG_GAIN_TEST, VMAX_TEST, &vx, &vy);
    check_close("long drag clamps to V_MAX", sqrt((double)vx*vx+(double)vy*vy), VMAX_TEST, 1e-3);
}

/* ---- 4) phys_clock_tick: pause vs jitter, budget cap, wraparound, fps-independence ---- */
static void test_clock_accumulator(void)
{
    /* Pause: a 5s gap must reset the accumulator to zero, not clamp-and-drain. */
    {
        uint32_t last = 0, accum = 0;
        uint32_t r = phys_clock_tick(&last, &accum, 5000000u, 200000u, 99996u);
        check(r == 0, "pause (5s gap): accumulator resets to 0, no catch-up");
        check(last == 5000000u, "pause: last_now_us updated to now");
    }
    /* Ordinary jitter: accumulator never exceeds the budget cap. */
    {
        uint32_t last = 0, accum = 0, now = 0;
        for (int i = 0; i < 50; i++) {
            now += 33333u; /* ~30fps */
            phys_clock_tick(&last, &accum, now, 200000u, 99996u);
            check(accum <= 99996u, "jitter: accumulator never exceeds MAX_ACCUM_US budget");
        }
    }
    /* uint32 wraparound: dt computed correctly across a counter wrap. */
    {
        uint32_t last = 0xFFFFFFF0u, accum = 0;
        uint32_t now = 0x10u; /* wrapped: true elapsed = 0x20 = 32us */
        phys_clock_tick(&last, &accum, now, 200000u, 99996u);
        check(accum == 32u, "counter wraparound: dt computed correctly (32us)");
    }
    /* fps-independence: same total real time -> same number of physics steps
       (within 1), whether sampled at a steady 60fps or irregular jittery spacing. */
    {
        uint32_t last_a = 0, accum_a = 0, now_a = 0, steps_a = 0;
        while (now_a < 1000000u) {
            now_a += 16667u; /* steady 60fps */
            phys_clock_tick(&last_a, &accum_a, now_a, 200000u, 99996u);
            while (accum_a >= 8333u) { accum_a -= 8333u; steps_a++; }
        }
        uint32_t last_b = 0, accum_b = 0, now_b = 0, steps_b = 0;
        uint32_t pattern[] = { 9000u, 24334u }; /* irregular, same 16667 average */
        int p = 0;
        while (now_b < 1000000u) {
            now_b += pattern[p % 2]; p++;
            phys_clock_tick(&last_b, &accum_b, now_b, 200000u, 99996u);
            while (accum_b >= 8333u) { accum_b -= 8333u; steps_b++; }
        }
        char buf[128];
        long diff = (long)steps_a - (long)steps_b;
        if (diff < 0) diff = -diff;
        snprintf(buf, sizeof buf, "fps-independence: steady vs jittery spacing over 1s (a=%u b=%u)",
                 steps_a, steps_b);
        check(diff <= 1, buf);
    }
}

/* ---- 5) phys_advance_hole: tour-end boundary, no out-of-bounds hole index ----
 * s_holes has 9 elements (valid indices 0..8); on_press() calls this exact
 * function when advancing after a sink, so the game's hole index can never
 * go out of range. This calls the same function directly, not a
 * reimplementation of its logic. */
static void test_advance_hole_bounds(void)
{
    const int N = 9;
    /* Advancing through holes 0..7 must move to the next hole (0 returned). */
    for (int i = 0; i < N - 1; i++) {
        int idx = i;
        int ended = phys_advance_hole(&idx, N);
        char buf[96];
        snprintf(buf, sizeof buf, "advance from hole %d: not tour-end", i);
        check(!ended, buf);
        snprintf(buf, sizeof buf, "advance from hole %d: moves to hole %d", i, i + 1);
        check(idx == i + 1, buf);
    }
    /* Advancing from the LAST hole (index N-1 = 8, "hole 9") must report
       tour-end and must NOT touch hole_idx (never becomes 9 == N, which
       would be an out-of-bounds index into s_holes). */
    {
        int idx = N - 1;
        int ended = phys_advance_hole(&idx, N);
        check(ended, "advance from last hole: reports tour-end");
        check(idx == N - 1, "advance from last hole: hole_idx left in-bounds (unchanged, not N)");
    }
}

int main(void)
{
    test_stop_and_direction();
    test_distance_matches_formula();
    test_drag_mapping_isotropic();
    test_clock_accumulator();
    test_advance_hole_bounds();
    if (g_fail == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_fail);
    return 1;
}
