/*
 * benchmark — runs LVGL's lv_demo_benchmark on the relocatable surface runtime,
 * and writes the per-scene results to /Data/Bench/bench_*.csv so they can be
 * pulled off the device and compared across rendering changes (e.g. an GPU-2D /
 * hardware-accelerated draw backend later).
 *
 * Needs LV_PERF := 1 (Makefile) — the benchmark reads FPS/CPU from the sysmon
 * perf subject, which -DHB_LV_PERF enables.
 */
#include "hb_sdk.h"
#include "hb_silver_app.h"
#include "lvgl/lvgl.h"
#include "../../lvgl/demos/benchmark/lv_demo_benchmark.h"

extern void lv_demo_benchmark(void);
extern void lv_demo_benchmark_set_end_cb(lv_demo_benchmark_on_end_cb_t cb);
extern void lv_demo_benchmark_summary_display(const lv_demo_benchmark_summary_t *summary);

/* Called once the benchmark has cycled every scene. Dump a CSV of the results to
 * a timestamped file so multiple runs accumulate for comparison (e.g. before vs
 * after a hardware-accelerated draw backend). */
static void on_benchmark_end(const lv_demo_benchmark_summary_t *s)
{
    static char buf[4096];
    char path[64];
    hb_rtc_time_t t;
    int n = 0;
    const lv_demo_benchmark_scene_dsc_t *sc;

    hb_rtc_read(&t);
    hb_fs_mkdir("/Data/Bench");   /* mkdir -p; no-op if it already exists */
    lv_snprintf(path, sizeof path,
                "/Data/Bench/bench_%04u%02u%02u_%02u%02u%02u.csv",
                (unsigned)t.year, (unsigned)t.month, (unsigned)t.day_of_month,
                (unsigned)t.hours, (unsigned)t.minutes, (unsigned)t.seconds);

    n += lv_snprintf(buf + n, (size_t)(sizeof buf - n),
                     "scene,fps,cpu_pct,render_us,flush_us\n");
    for (sc = s->scenes; sc && sc->create_cb; sc++) {
        if (sc->measurement_cnt == 0)
            continue;
        n += lv_snprintf(buf + n, (size_t)(sizeof buf - n),
                         "%s,%" LV_PRIu32 ",%" LV_PRIu32 ",%" LV_PRIu32 ",%" LV_PRIu32 "\n",
                         sc->name ? sc->name : "?", sc->fps_avg, sc->cpu_avg_usage,
                         sc->render_avg_time, sc->flush_avg_time);
    }
    if (s->valid_scene_cnt > 0) {
        n += lv_snprintf(buf + n, (size_t)(sizeof buf - n),
                         "SUMMARY,%" LV_PRId32 ",%" LV_PRId32 ",%" LV_PRId32 ",%" LV_PRId32 "\n",
                         s->total_avg_fps / s->valid_scene_cnt,
                         s->total_avg_cpu / s->valid_scene_cnt,
                         s->total_avg_render_time / s->valid_scene_cnt,
                         s->total_avg_flush_time / s->valid_scene_cnt);
    }
    hb_fs_write(path, buf, (uint32_t)n);
    hb_trace_log("BENCHEND",
                 (uint32_t)(s->valid_scene_cnt > 0 ? s->total_avg_fps / s->valid_scene_cnt : 0),
                 (uint32_t)s->valid_scene_cnt);

    /* Setting an end_cb makes the benchmark SKIP its own on-screen summary table
     * (it assumes the callback owns the display). Show it ourselves so the device
     * still presents the results table after writing the CSV. */
    lv_demo_benchmark_summary_display(s);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_wake_lock(true);   /* keep the panel lit through the touch-less run */
    lv_demo_benchmark_set_end_cb(on_benchmark_end);
    lv_demo_benchmark();
}
