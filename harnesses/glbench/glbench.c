/*
 * glbench — how many small draw batches per frame can the nano's GPU
 * driver take? Measures hundreds of small textured batches per frame with a
 * texture or matrix change between most of them.
 *
 * Each frame: one fullscreen textured quad (fill cost), then N batches of 16
 * textured, vertex-coloured triangles, each with its own matrix and alternating
 * between two 32x32 textures. Three ways of feeding vertices are compared:
 *   CLI  client-side arrays (pointer into app RAM, buffer 0 bound)
 *   SUB  one VBO, glBufferSubData of each batch at its own offset, then draw
 *   DAT  one small VBO, glBufferData (re-specify) per batch, then draw
 * over the PHASES table (small batches, then the same triangles in a few big
 * batches). Each phase runs ~2.5 s (first 0.4 s discarded). The frame
 * interval shows GPU+CPU throughput; the time spent inside gl_app_frame shows
 * the CPU submission cost. Results are rewritten to
 * /Data/GLBench/results.txt after every phase, so a crash still leaves
 * the numbers up to the phase that killed it. The screen turns green when done.
 */
#include "hb_gl_surface.h"
#include "hb_sdk.h"
#include "gl.h"

#define OUT_DIR   "/Data/GLBench"
#define OUT_PATH  "/Data/GLBench/results.txt"

#define TEX_A     251          /* fixed GL names, clear of Blocks' 70-85 / 250 */
#define TEX_B     252
#define TEX_BG    253
#define VBO_SUB   90
#define VBO_DAT   91
#define VBO_STA   92
#define VBO_STR   93

#define TRIS      16
#define VPB       (TRIS * 3)                 /* vertices per batch */
#define MAXB      800

typedef struct { float x, y, z, u, v; uint8_t r, g, b, a; } vtx_t;   /* 24 bytes */

static vtx_t    g_verts[MAXB * VPB];
static uint32_t g_tex_a[32*32], g_tex_b[32*32], g_tex_bg[64*64];

enum { MODE_CLI, MODE_SUB, MODE_DAT, MODE_STA, MODE_STF, MODE_STR, NMODES };
static const char *const MODE_NAME[NMODES] = { "CLI", "SUB", "DAT", "STA", "STF", "STR" };

/* v1 on hardware: CLI 50 and 100 batches of 16 tris ran (38 fps, ~160 us CPU
 * per batch) and CLI 200 rebooted the nano. So: safe phases first, per-call vs
 * per-triangle cost next (same triangles in fewer, bigger batches), and the
 * phases that might crash last. "START" is saved before each phase so a crash
 * names the phase that caused it. */
/* v2 on hardware: CLI 5x320 tris ran at 4.2 ms CPU, CLI 10x320 rebooted — the
 * same 9,600 client vertices per frame as v1's CLI 200x16 crash, so the limit
 * is streamed vertices per frame, not draw calls. v3 tests STA (geometry
 * uploaded once to a GL_STATIC_DRAW VBO, as Blocks does, then drawn by range)
 * and narrows the streaming limit; possible crashes stay last. */
/* v3 on hardware: the first static-VBO phase (10x320 = 3,200 tris) rebooted
 * too. Every crash so far (CLI 200x16, CLI 10x320, STA 10x320) is 3,200 tris /
 * 9,600 vertices per frame whatever the source, and 1,600 tris always ran — so
 * this looks like a per-frame GPU limit (a tiler's parameter buffer), not a
 * streaming limit. v4 walks the static total up in steps (STA: pointer offset
 * per batch, like SUB), then tests 3,200 tris as triangle STRIPS (STR: ~1/3 the
 * vertices) to tell a triangle limit from a vertex limit, and finally STF (the
 * `first` argument instead of a pointer offset). */
typedef struct { uint8_t mode; uint16_t batches, tris; } phase_t;
static const phase_t PHASES[] = {
    { MODE_CLI,   0,  16 },          /* background only: frame-rate ceiling    */
    { MODE_STA,   5, 320 },          /* 1,600 tris (the size that always ran)  */
    { MODE_STA,   6, 320 },          /* 1,920                                  */
    { MODE_STA,   7, 320 },          /* 2,240                                  */
    { MODE_STA,   8, 320 },          /* 2,560                                  */
    { MODE_STA,   9, 320 },          /* 2,880                                  */
    { MODE_STR,  10, 320 },          /* 3,200 tris as strips: 3,220 vertices   */
    { MODE_STF,   5, 320 },          /* 1,600 tris via glDrawArrays `first`    */
};
#define SUB_VERTS  (10 * 320 * 3)
#define STRIP_VERTS (320 + 2)
static vtx_t g_strip[10 * STRIP_VERTS];
#define NPHASES  (int)(sizeof PHASES / sizeof PHASES[0])

#define PHASE_US   2500000u
#define WARMUP_US   400000u

static int      g_inited, g_phase, g_done;
static uint32_t g_phase_start, g_last_frame_start;
static uint32_t g_frames, g_interval_sum, g_cpu_sum, g_interval_max;
static uint32_t g_res_fps10[NPHASES], g_res_cpu[NPHASES], g_res_max[NPHASES];
static int      g_res_valid[NPHASES];

/* ---- text output ---- */

static char *put_s(char *o, const char *s) { while (*s) *o++ = *s++; *o = 0; return o; }
static char *put_u(char *o, uint32_t u)
{
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) *o++ = t[--n];
    *o = 0;
    return o;
}

static void write_results(void)
{
    static char buf[2048];
    char *o = put_s(buf, "MODE BATCHES TRIS_PER_BATCH TOTAL_TRIS FPS FRAME_CPU_US WORST_MS\n");
    for (int p = 0; p < NPHASES; p++) {
        if (!g_res_valid[p]) continue;
        const phase_t *ph = &PHASES[p];
        o = put_s(o, MODE_NAME[ph->mode]); *o++ = ' ';
        o = put_u(o, ph->batches); *o++ = ' ';
        o = put_u(o, ph->tris); *o++ = ' ';
        o = put_u(o, (uint32_t)ph->batches * ph->tris); *o++ = ' ';
        o = put_u(o, g_res_fps10[p] / 10); *o++ = '.'; o = put_u(o, g_res_fps10[p] % 10); *o++ = ' ';
        o = put_u(o, g_res_cpu[p]); *o++ = ' ';
        o = put_u(o, g_res_max[p] / 1000); *o++ = '\n';
    }
    if (g_done) o = put_s(o, "DONE\n");
    else { o = put_s(o, "START "); o = put_u(o, (uint32_t)g_phase); *o++ = '\n'; }
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(OUT_DIR);
    hb_fs_write(OUT_PATH, buf, (uint32_t)(o - buf));
}

/* ---- setup ---- */

static void make_tex(uint32_t *px, int size, uint32_t c0, uint32_t c1, int cell)
{
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            px[y * size + x] = (((x / cell) ^ (y / cell)) & 1) ? c1 : c0;
}

static void upload_tex(int id, const uint32_t *px, int size)
{
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* Each batch: a 4x2 strip of quads (16 triangles) about 1 unit across, in its
 * own local space; the per-batch matrix places it. */
static void build_geometry(void)
{
    uint32_t seed = 12345;
    for (int b = 0; b < MAXB; b++) {
        vtx_t *v = &g_verts[b * VPB];
        for (int q = 0; q < 8; q++) {
            float x0 = (float)(q % 4) * 0.25f - 0.5f, y0 = (float)(q / 4) * 0.25f - 0.25f;
            float x1 = x0 + 0.25f, y1 = y0 + 0.25f;
            float c[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
            static const int TRI[6] = { 0, 1, 2, 0, 2, 3 };
            for (int k = 0; k < 6; k++) {
                int i = TRI[k];
                seed = seed * 1103515245u + 12345u;
                v->x = c[i][0]; v->y = c[i][1]; v->z = (float)((seed >> 16) & 15) * 0.01f;
                v->u = (c[i][0] + 0.5f); v->v = (c[i][1] + 0.25f) * 2.f;
                v->r = (uint8_t)(128 + ((seed >> 8) & 127));
                v->g = (uint8_t)(128 + ((seed >> 15) & 127));
                v->b = (uint8_t)(128 + ((seed >> 22) & 127));
                v->a = 255;
                v++;
            }
        }
    }
    /* strips: a 1 x 0.5 zig-zag ribbon, 320 triangles from 322 vertices each */
    for (int s = 0; s < 10; s++)
        for (int i = 0; i < STRIP_VERTS; i++) {
            vtx_t *v = &g_strip[s * STRIP_VERTS + i];
            v->x = (float)(i / 2) / (float)(STRIP_VERTS / 2) - 0.5f;
            v->y = (i & 1) ? 0.25f : -0.25f;
            v->z = 0.f;
            v->u = v->x + 0.5f; v->v = (i & 1) ? 1.f : 0.f;
            v->r = v->g = v->b = v->a = 255;
        }
}

static void lazy_init(void)
{
    make_tex(g_tex_a, 32, 0xff3050e0u, 0xffe0e0e0u, 4);
    make_tex(g_tex_b, 32, 0xff20c040u, 0xff104010u, 8);
    make_tex(g_tex_bg, 64, 0xff402010u, 0xff201008u, 16);
    upload_tex(TEX_A, g_tex_a, 32);
    upload_tex(TEX_B, g_tex_b, 32);
    upload_tex(TEX_BG, g_tex_bg, 64);
    build_geometry();
    glBindBuffer(GL_ARRAY_BUFFER, VBO_STA);
    glBufferData(GL_ARRAY_BUFFER, (int)sizeof g_verts, g_verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, VBO_STR);
    glBufferData(GL_ARRAY_BUFFER, (int)sizeof g_strip, g_strip, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, VBO_SUB);
    glBufferData(GL_ARRAY_BUFFER, SUB_VERTS * (int)sizeof(vtx_t), g_verts, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, VBO_DAT);
    glBufferData(GL_ARRAY_BUFFER, VPB * (int)sizeof(vtx_t), g_verts, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    g_inited = 1;
}

/* ---- drawing ---- */

static void set_pointers(const vtx_t *base)
{
    const uint8_t *p = (const uint8_t *)base;
    glVertexPointer(3, GL_FLOAT, (int)sizeof(vtx_t), p);
    glTexCoordPointer(2, GL_FLOAT, (int)sizeof(vtx_t), p + 12);
    glColorPointer(4, GL_UNSIGNED_BYTE, (int)sizeof(vtx_t), p + 20);
}

static void draw_background(int w, int h)
{
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrthof(0.f, 1.f, 0.f, 1.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    static const float v[8] = { 0,0, 1,0, 0,1, 1,1 };
    static const float t[8] = { 0,0, 4,0, 0,4, 4,4 };
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, TEX_BG);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glTexCoordPointer(2, GL_FLOAT, 0, t);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void draw_batches(int w, int h, int mode, int n, int tris, uint32_t frame)
{
    const int vpb = tris * 3;
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    float aspect = (float)w / (float)h;
    glFrustumf(-0.1f * aspect, 0.1f * aspect, -0.1f, 0.1f, 0.2f, 100.f);
    glMatrixMode(GL_MODELVIEW);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    if (mode == MODE_CLI) glBindBuffer(GL_ARRAY_BUFFER, 0);
    else glBindBuffer(GL_ARRAY_BUFFER, mode == MODE_SUB ? VBO_SUB :
                                       mode == MODE_STR ? VBO_STR :
                                       (mode == MODE_STA || mode == MODE_STF) ? VBO_STA : VBO_DAT);
    if (mode == MODE_STF) set_pointers((const vtx_t *)0);

    int cols = 40;
    for (int b = 0; b < n; b++) {
        glBindTexture(GL_TEXTURE_2D, (b & 1) ? TEX_B : TEX_A);
        glLoadIdentity();
        float x = (float)(b % cols) - (float)cols * 0.5f + 0.5f;
        float y = (float)((b / cols) % 20) - 10.f + 0.5f;
        glTranslatef(x * 1.1f, y * 1.1f, -30.f - (float)(b / 800));
        glRotatef((float)((frame * 3 + (uint32_t)b * 7) % 360), 0.f, 0.f, 1.f);

        const vtx_t *src = &g_verts[b * vpb];
        if (mode == MODE_CLI) {
            set_pointers(src);
            glDrawArrays(GL_TRIANGLES, 0, vpb);
        } else if (mode == MODE_STA) {
            set_pointers((const vtx_t *)(uintptr_t)(b * vpb * (int)sizeof(vtx_t)));
            glDrawArrays(GL_TRIANGLES, 0, vpb);
        } else if (mode == MODE_STF) {
            glDrawArrays(GL_TRIANGLES, b * vpb, vpb);
        } else if (mode == MODE_STR) {
            set_pointers((const vtx_t *)(uintptr_t)(b * STRIP_VERTS * (int)sizeof(vtx_t)));
            glDrawArrays(GL_TRIANGLE_STRIP, 0, STRIP_VERTS);
        } else if (mode == MODE_SUB) {
            int off = b * vpb * (int)sizeof(vtx_t);
            glBufferSubData(GL_ARRAY_BUFFER, off, vpb * (int)sizeof(vtx_t), src);
            set_pointers((const vtx_t *)(uintptr_t)off);
            glDrawArrays(GL_TRIANGLES, 0, vpb);
        } else {
            glBufferData(GL_ARRAY_BUFFER, vpb * (int)sizeof(vtx_t), src, GL_DYNAMIC_DRAW);
            set_pointers((const vtx_t *)0);
            glDrawArrays(GL_TRIANGLES, 0, vpb);
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
}

static void draw_progress(int w, int h)
{
    glViewport(0, 0, w, h);
    glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrthof(0.f, (float)w, (float)h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableClientState(GL_VERTEX_ARRAY);
    float pw = (float)(w - 40) * (float)(g_phase + 1) / (float)NPHASES;
    float v[8] = { 20.f, 20.f, 20.f + pw, 20.f, 20.f, 36.f, 20.f + pw, 36.f };
    glColor4f(1.f, 0.85f, 0.2f, 1.f);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glColor4f(1.f, 1.f, 1.f, 1.f);
}

/* ---- phase bookkeeping ---- */

static void finish_phase(void)
{
    if (g_frames && g_interval_sum) {
        g_res_fps10[g_phase] = (uint32_t)((uint64_t)g_frames * 10000000u / g_interval_sum);
        g_res_cpu[g_phase] = g_cpu_sum / g_frames;
        g_res_max[g_phase] = g_interval_max;
        g_res_valid[g_phase] = 1;
    }
    hb_trace_log("GLBENCH ", (uint32_t)g_phase, g_res_fps10[g_phase]);
    g_phase++;
    if (g_phase >= NPHASES) g_done = 1;
    write_results();
    g_frames = g_interval_sum = g_cpu_sum = g_interval_max = 0;
    g_phase_start = 0;
}

void gl_app_init(void)
{
    /* No GL context current at load — all GL happens in the first frame. */
}

void gl_app_frame(int w, int h, uint32_t frame)
{
    uint32_t t0 = hb_time_uptime_us();
    if (!g_inited) { lazy_init(); write_results(); }

    if (g_done) {
        glViewport(0, 0, w, h);
        glClearColor(0.05f, 0.45f, 0.2f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    if (!g_phase_start) { g_phase_start = t0; g_last_frame_start = 0; }

    const phase_t *ph = &PHASES[g_phase];
    draw_background(w, h);
    if (ph->batches) draw_batches(w, h, ph->mode, ph->batches, ph->tris, frame);
    draw_progress(w, h);

    uint32_t t1 = hb_time_uptime_us();
    if (t0 - g_phase_start >= WARMUP_US && g_last_frame_start) {
        uint32_t interval = t0 - g_last_frame_start;
        g_interval_sum += interval;
        if (interval > g_interval_max) g_interval_max = interval;
        g_cpu_sum += t1 - t0;
        g_frames++;
    }
    g_last_frame_start = t0;
    if (t0 - g_phase_start >= PHASE_US) finish_phase();
}
