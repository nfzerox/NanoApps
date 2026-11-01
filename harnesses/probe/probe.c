/*
 * probe — measures CPU and memory resources available to larger apps:
 *   - OS heap: total free and the largest contiguous block (pure reads), plus one
 *     real allocation of that block, freed immediately
 *   - CPU clock estimate: a timed chain of dependent single-cycle adds
 *   - memcpy bandwidth
 * Results show on screen (heap numbers refresh every second) and are written to
 * /Data/Probe/probe.txt. Tap to re-run the CPU/memory tests and rewrite the file.
 */
#include "hb_raw_surface.h"
#include "hb_sdk.h"
#include "hb_heap.h"
#include <string.h>

#define OUT_DIR  "/Data/Probe"
#define OUT_PATH "/Data/Probe/probe.txt"

static uint32_t *s_fb;
static int s_w, s_h;

/* ---- text ---- */

static void text(int x, int y, const char *str, int scale, uint32_t fg)
{
    fg |= 0xff000000u;
    for (; *str; str++, x += 8*scale) {
        const uint8_t *g = hb_font8x8_glyph(*str);
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++) {
                if (!(g[r] & (0x80 >> c))) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + c*scale + sx, py = y + r*scale + sy;
                        if (px >= 0 && px < s_w && py >= 0 && py < s_h) s_fb[py*s_w + px] = fg;
                    }
            }
    }
}

/* tiny formatter: appends decimal u (with optional one decimal place from tenths) */
static char *put_s(char *o, const char *s) { while (*s) *o++ = *s++; *o = 0; return o; }
static char *put_u(char *o, uint32_t u)
{
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) *o++ = t[--n];
    *o = 0;
    return o;
}
static char *put_kb_mb(char *o, uint32_t bytes)       /* "12.3MB 12620K" */
{
    uint32_t tenths = (uint32_t)(((uint64_t)bytes * 10u + 524288u) / 1048576u);
    o = put_u(o, tenths / 10); *o++ = '.'; o = put_u(o, tenths % 10);
    o = put_s(o, "MB ");
    o = put_u(o, bytes / 1024);
    return put_s(o, "K");
}

/* ---- measurements ---- */

static uint32_t g_free, g_largest, g_alloc_ok, g_alloc_size;
static uint32_t g_mhz, g_copy_mbs;

static void heap_read(void)
{
    g_free = hb_os_heap_free();
    g_largest = hb_os_heap_largest();
}

/* Grab (largest - 64 KB) for real, touch the first and last word, free it. */
static void heap_try(void)
{
    g_alloc_size = g_largest > 65536u ? g_largest - 65536u : 0;
    g_alloc_ok = 0;
    if (!g_alloc_size) return;
    volatile uint32_t *p = (volatile uint32_t *)hb_os_alloc(g_alloc_size);
    if (!p) return;
    p[0] = 0x12345678u;
    p[g_alloc_size/4 - 1] = 0x9abcdef0u;
    g_alloc_ok = (p[0] == 0x12345678u && p[g_alloc_size/4 - 1] == 0x9abcdef0u);
    hb_os_free((void *)p);
}

/* 32 dependent adds + subs + bne per iteration. Cortex-A5 is single-issue in order,
 * so a dependent ALU chain runs at ~1 op per cycle: cycles ~= 34 * iterations. */
static uint32_t alu_loop(uint32_t n)
{
    uint32_t a = 0;
    __asm__ volatile(
        "1:\n"
        ".rept 32\n"
        "add %0, %0, #1\n"
        ".endr\n"
        "subs %1, %1, #1\n"
        "bne 1b\n"
        : "+r"(a), "+r"(n) : : "cc");
    return a;
}

static void cpu_measure(void)
{
    const uint32_t iters = 200000;                 /* ~6.8 M cycles */
    alu_loop(1000);                                /* warm the i-cache */
    uint32_t t0 = hb_time_uptime_us();
    alu_loop(iters);
    uint32_t us = hb_time_uptime_us() - t0;
    g_mhz = us ? (uint32_t)((uint64_t)iters * 34u / us) : 0;
}

static uint8_t g_src[128*1024], g_dst[128*1024];

static void copy_measure(void)
{
    for (uint32_t i = 0; i < sizeof g_src; i++) g_src[i] = (uint8_t)i;
    memcpy(g_dst, g_src, sizeof g_src);            /* warm */
    const int reps = 32;                           /* 4 MB copied */
    uint32_t t0 = hb_time_uptime_us();
    for (int r = 0; r < reps; r++) memcpy(g_dst, g_src, sizeof g_src);
    uint32_t us = hb_time_uptime_us() - t0;
    g_copy_mbs = us ? (uint32_t)((uint64_t)reps * sizeof g_src / us) : 0;   /* bytes/us = MB/s */
}

/* ---- report ---- */

static char g_lines[8][48];

static void build_lines(void)
{
    char *o;
    o = put_s(g_lines[0], "HEAP FREE ");        put_kb_mb(o, g_free);
    o = put_s(g_lines[1], "LARGEST   ");        put_kb_mb(o, g_largest);
    o = put_s(g_lines[2], "ALLOC TEST ");
    if (!g_alloc_size) put_s(o, "SKIPPED");
    else { o = put_s(o, g_alloc_ok ? "OK " : "FAILED "); o = put_u(o, g_alloc_size / 1024); put_s(o, "K"); }
    o = put_s(g_lines[3], "CPU ~");             o = put_u(o, g_mhz); put_s(o, " MHZ (EST)");
    o = put_s(g_lines[4], "MEMCPY ");           o = put_u(o, g_copy_mbs); put_s(o, " MB/S");
    g_lines[5][0] = 0;
}

static void write_file(void)
{
    static char buf[512];
    char *o = buf;
    for (int i = 0; i < 5; i++) { o = put_s(o, g_lines[i]); *o++ = '\n'; }
    *o = 0;
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(OUT_DIR);
    hb_fs_write(OUT_PATH, buf, (uint32_t)(o - buf));
}

static void draw(void)
{
    hb_raw_fill(0x10141c);
    text(12, 14, "PROBE", 3, 0x7ee0a8);
    /* each report line is "LABEL value": draw label and value on separate rows so
       it fits 240 px at scale 1-2 */
    static const char *labels[5] = { "HEAP FREE", "LARGEST BLOCK", "ALLOC TEST", "CPU (ESTIMATE)", "MEMCPY" };
    static const int skip[5] = { 10, 10, 11, 4, 7 };
    for (int i = 0; i < 5; i++) {
        int y = 60 + i * 58;
        text(12, y, labels[i], 1, 0x8a93a8);
        text(12, y + 14, g_lines[i] + skip[i], 2, 0xffffff);
    }
    text(12, s_h - 40, "HEAP REFRESHES EVERY 1 S", 1, 0x8a93a8);
    text(12, s_h - 24, "TAP: RERUN + SAVE", 1, 0x8a93a8);
}

static void run_all(void)
{
    heap_read();
    heap_try();
    cpu_measure();
    copy_measure();
    heap_read();
    build_lines();
    write_file();
    hb_trace_log("PROBEMEM", g_free, g_largest);
    hb_trace_log("PROBECPU", g_mhz, g_copy_mbs);
    draw();
}

/* ---- entry ---- */

static uint32_t g_last_ms;
static int g_prev_down;

void hb_raw_init(int w, int h)
{
    s_fb = hb_raw_fb(); s_w = w; s_h = h;
    run_all();
    g_last_ms = hb_time_uptime_ms();
}

void hb_raw_frame(const hb_spoint_t *t)
{
    if (!t->down && g_prev_down) { run_all(); g_last_ms = hb_time_uptime_ms(); }
    g_prev_down = t->down;

    uint32_t now = hb_time_uptime_ms();
    if (now - g_last_ms >= 1000) {
        g_last_ms = now;
        heap_read();
        build_lines();
        draw();
    }
}
