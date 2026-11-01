/* game_host.c — the port on the Mac with no window: runs the game for a number of frames
 * with a scripted finger and writes screenshots. The drawing goes through the same pipeline
 * as on the iPod (port/src/glfe.c) at the nano's 240 by 432.
 *
 * usage: game_host <iPod data dir> <host texture dir> <out dir> <frames> [seed] [shots every n] */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <zlib.h>
#include <OpenGL/OpenGL.h>
#include "gl.h"
#include <OpenGL/glext.h>
#include "game.h"
#include "glfe.h"
#include "platform.h"
#include "rt.h"

#define W 240
#define H 432
static const char *sDirs[2];

uint64_t plat_time_us(void) { struct timeval tv; gettimeofday(&tv, NULL); return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec; }
void *plat_read_file(const char *name, uint32_t *size, int save) {
    if (save && !getenv("TR_SAVE_DIR")) return NULL;
    for (int i = 0; i < (save ? 1 : 2); i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", save ? getenv("TR_SAVE_DIR") : sDirs[i], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        void *data = malloc((size_t)n + 1);
        if (fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(data); return NULL; }
        fclose(f);
        *size = (uint32_t)n;
        return data;
    }
    return NULL;
}
int plat_write_file(const char *name, const void *data, uint32_t size) {
    if (getenv("TR_RUNS")) fprintf(stderr, "write %s: %u bytes\n", name, (unsigned)size);
    if (getenv("TR_SAVE_DIR")) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", getenv("TR_SAVE_DIR"), name);
        FILE *f = fopen(path, "wb");
        if (!f) return -1;
        fwrite(data, 1, size, f);
        fclose(f);
    }
    return 0;
}
void plat_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
void plat_poll(void) {}
void plat_log_flush(void) {}
void plat_fatal(const char *message) { fprintf(stderr, "fatal: %s\n", message); exit(2); }
void rt_host_sound(const char *name, int loop, float pitch, int stop) {
    if (getenv("TR_SOUNDS")) fprintf(stderr, "sound %s%s pitch %.2f%s\n", stop ? "stop " : "", name, (double)pitch, loop ? " (loop)" : "");
}

unsigned rt_host_load_texture(const char *name, const char *file, int repeat) {
    char base[64], path[80];
    if (getenv("TR_TEXLOG")) fprintf(stderr, "load texture %s from %s, repeat %d\n", name, file, repeat);
    uint32_t size = 0;
    snprintf(base, sizeof base, "%s", file);
    char *dot = strrchr(base, '.');
    int pvr = dot && !strcmp(dot, ".pvr");
    if (dot) *dot = 0;
    snprintf(path, sizeof path, "%s.rgba", base);
    uint8_t *d = plat_read_file(path, &size, 0);
    if (!d || size < 8) { plat_log("texture %s: %s is missing", name, path); return 0; }
    uint32_t w, h;
    memcpy(&w, d, 4); memcpy(&h, d + 4, 4);
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, pvr ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    if (pvr) glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d + 8);
    free(d);
    return t;
}

static void put32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len) {
    unsigned char b[4]; uint32_t crc = crc32(0, (const unsigned char *)type, 4);
    if (len) crc = crc32(crc, data, len);
    put32(b, len); fwrite(b, 1, 4, f); fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    put32(b, crc); fwrite(b, 1, 4, f);
}
static void screenshot(const char *path) {
    unsigned char *px = malloc((size_t)W * H * 4), *raw = malloc((size_t)(W * 3 + 1) * H), hdr[13];
    uLongf zlen = compressBound((uLong)(W * 3 + 1) * H); unsigned char *z = malloc(zlen);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < H; y++) {
        unsigned char *row = raw + (size_t)y * (W * 3 + 1), *src = px + (size_t)(H - 1 - y) * W * 4;
        row[0] = 0;
        for (int x = 0; x < W; x++) { row[1 + x * 3] = src[x * 4]; row[2 + x * 3] = src[x * 4 + 1]; row[3 + x * 3] = src[x * 4 + 2]; }
    }
    compress2(z, &zlen, raw, (uLong)(W * 3 + 1) * H, 6);
    FILE *f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    put32(hdr, (uint32_t)W); put32(hdr + 4, (uint32_t)H); hdr[8] = 8; hdr[9] = 2; hdr[10] = hdr[11] = hdr[12] = 0;
    chunk(f, "IHDR", hdr, 13); chunk(f, "IDAT", z, (uint32_t)zlen); chunk(f, "IEND", NULL, 0);
    fclose(f); free(px); free(raw); free(z);
}

static int sHist[40], sHistMax, sHistMaxFrame, capped_frames, cut_hist[9], cut_max;
static long capped, detail_sum;
static int sFound = -1000, sFinds;
static long calls, idle_calls, idle_vertices, idle_off, vin, tin, boxed;
int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: game_host <iPod data dir> <host texture dir> <out dir> <frames> [seed] [shot every n]\n"); return 2; }
    sDirs[0] = argv[1]; sDirs[1] = argv[2];
    int frames = atoi(argv[4]), every = argc > 6 ? atoi(argv[6]) : 60;
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
                                        kCGLPFADepthSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pf; GLint npf; CGLContextObj ctx;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf || CGLCreateContext(pf, NULL, &ctx)) return 1;
    CGLSetCurrentContext(ctx);
    GLuint fbo, rb[2];
    glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(2, rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[0]);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, W, H);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb[0]);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[1]);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24, W, H);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, rb[1]);
    glViewport(0, 0, W, H);
    glEnable(GL_DEPTH_TEST);
    for (int i = 0; i < 3; i++) { game_splash(W, H, i > 0); glFinish(); if (i == 2 && getenv("TR_SHOT_SPLASH")) screenshot(getenv("TR_SHOT_SPLASH")); }
    if (!game_init(W, H, getenv("TR_HEAP") ? (uint32_t)strtoul(getenv("TR_HEAP"), 0, 0) : 8u << 20, argc > 5 ? (uint32_t)atoi(argv[5]) : 1u)) return 1;
    fprintf(stderr, "loaded: guest heap %u bytes high water, %u in use; buffer objects %u bytes\n", (unsigned)rt_heap_peak(),
            (unsigned)rt_heap_used(), fe_buffer_bytes);
    long tris = 0, tiny = 0, clipped = 0, dropped = 0, fogged = 0, draws = 0;
    int runs = 0, last_state = GAME_TITLE, best = 0, last_over = 0;
    game_autopilot(getenv("TR_NOAUTO") ? 0 : 1);

    uint64_t t0 = plat_time_us();
    if (getenv("TR_NOCULL")) fe_option_box_cull = 0;
    if (getenv("TR_ARRAYS")) fe_option_indexed = 0;
    if (getenv("TR_CAP")) fe_option_budget = atoi(getenv("TR_CAP"));
    for (int f = 0; f < frames; f++) {

        if (getenv("TR_TAP") && f == atoi(getenv("TR_TAP"))) { setenv("TR_GLLOG", "1", 1); setenv("TR_TEXLOG", "1", 1); fprintf(stderr, "--- tap\n"); game_touch(0, 120, 300); }
        if (getenv("TR_TAP") && f == atoi(getenv("TR_TAP")) + 3) game_touch(2, 120, 300);
        if (getenv("TR_DUMP") && f == atoi(getenv("TR_DUMP"))) setenv("TR_DUMP_BATCHES", "1", 1); else unsetenv("TR_DUMP_BATCHES");
        game_frame(getenv("TR_DT") ? (float)atof(getenv("TR_DT")) : 1.0f / 30.0f);
        if (getenv("TR_TAP") && f >= atoi(getenv("TR_TAP")) - 1 && f <= atoi(getenv("TR_TAP")) + 4)
            fprintf(stderr, "frame %d: state %d, %d triangles out, %d draws, score %d\n", f, game_state(), fe_stat_triangles_out, fe_stat_draws, game_score());
        glFinish();
        { extern int fe_stat_near; if (getenv("TR_NEAR") && (f < 12 || f % 20 == 0)) fprintf(stderr, "frame %d: %d triangles crossed the near plane, %d clipped, %d out\n", f, fe_stat_near, fe_stat_clipped, fe_stat_triangles_out); }
        if (f == 0 && getenv("TR_RUNS")) fprintf(stderr, "best on record at start: %d\n", game_best());
        if (game_state() == GAME_OVER && last_state == GAME_RUNNING) {
            runs++;
            if (game_distance() > best) best = game_distance();
            if (getenv("TR_RUNS")) fprintf(stderr, "run %d ended at frame %d: distance %d, score %d, coins %d\n", runs, f, game_distance(), game_score(), game_coins());
        }
        last_state = game_state();
        capped += fe_stat_trimmed; if (fe_stat_trimmed) capped_frames++; if (fe_stat_cut > cut_max) cut_max = fe_stat_cut; cut_hist[fe_stat_trimmed ? 1 + fe_stat_cut / 32 : 0]++;
        { int bin = fe_stat_triangles_out / 100; if (bin > 39) bin = 39; sHist[bin]++; if (fe_stat_triangles_out > sHistMax) { sHistMax = fe_stat_triangles_out; sHistMaxFrame = f; } }
        { extern int fe_stat_idle_calls, fe_stat_idle_vertices, fe_stat_idle_offscreen, fe_stat_calls, fe_stat_vertices_in, fe_stat_triangles_in;
          calls += fe_stat_calls; idle_calls += fe_stat_idle_calls; idle_vertices += fe_stat_idle_vertices; idle_off += fe_stat_idle_offscreen; boxed += fe_stat_box_culled; vin += fe_stat_vertices_in; tin += fe_stat_triangles_in; }
        tris += fe_stat_triangles_out; tiny += fe_stat_tiny; clipped += fe_stat_clipped; dropped += fe_stat_dropped;
        fogged += fe_stat_fogged; draws += fe_stat_draws;
        if (getenv("TR_SHOT_AT")) {         /* a list of frames, e.g. 617,1134 */
            char list[256]; snprintf(list, sizeof list, ",%s,", getenv("TR_SHOT_AT"));
            char key[24]; snprintf(key, sizeof key, ",%d,", f);
            if (strstr(list, key)) { char path[1024]; snprintf(path, sizeof path, "%s/at%05d.png", argv[3], f); screenshot(path); fprintf(stderr, "frame %d: %d triangles, %d left out, cut %d\n", f, fe_stat_triangles_out, fe_stat_trimmed, fe_stat_cut); }
        }
        if (getenv("TR_FIND") && game_state() == GAME_RUNNING && fe_stat_trimmed > 350 && f > sFound + 300 && sFinds < 6) { sFound = f; sFinds++; fprintf(stderr, "find %d cut %d trimmed %d\n", f, fe_stat_cut, fe_stat_trimmed); }
        if ((every && f % every == every - 1) || (getenv("TR_SHOT_STATES") && (f == 30 || (game_state() == GAME_OVER && last_over != runs && (last_over = runs))))) {
            char path[1024];
            snprintf(path, sizeof path, "%s/game%04d.png", argv[3], f + 1);
            screenshot(path);
        }
    }
    fprintf(stderr, "%d frames in %.2f s: per frame %ld triangles (%ld fogged), %ld draw calls, %ld clipped, %ld tiny, %ld dropped; "
            "%d outside clip bounds, %d slivers; %d runs, best distance %d; state %d score %d coins %d distance %d; heap %u high, %u in use\n",
            frames, (double)(plat_time_us() - t0) / 1e6, tris / frames, fogged / frames, draws / frames, clipped / frames,
            tiny / frames, dropped, fe_stat_outside, fe_stat_slivers, runs, best, game_state(), game_score(), game_coins(), game_distance(),
            (unsigned)rt_heap_peak(), (unsigned)rt_heap_used());
    { extern float fe_ext_min_w, fe_ext_max_w, fe_ext_min_depth, fe_ext_max_depth, fe_ext_max_uv; extern int fe_stat_near;
      fprintf(stderr, "extremes: w %.3f..%.1f, depth %.5f..%.5f, |uv| up to %.2f; near crossings in the last frame %d\n", (double)fe_ext_min_w,
              (double)fe_ext_max_w, (double)fe_ext_min_depth, (double)fe_ext_max_depth, (double)fe_ext_max_uv, fe_stat_near); }
    fprintf(stderr, "per frame: %ld draw calls from the engine, %ld vertices and %ld triangles in; %ld calls gave nothing (%ld vertices, %ld of them in meshes off one side)\n",
            calls / frames, vin / frames, tin / frames, idle_calls / frames, idle_vertices / frames, idle_off / frames);
    fprintf(stderr, "left out by their box: %ld vertices a frame\n", boxed / frames);
    { extern long fe_host_vertices; fprintf(stderr, "given to GL: %ld vertices a frame\n", fe_host_vertices / frames); }
    if (getenv("TR_HIST")) {
        fprintf(stderr, "budget %d: %ld triangles left out in %d frames; highest cut level %d of 255; frames by cut (none, then 32 wide):", fe_option_budget, capped, capped_frames, cut_max);
        for (int i = 0; i < 9; i++) fprintf(stderr, " %d", cut_hist[i]);
        fprintf(stderr, "\ntriangles per frame: most %d (frame %d);", sHistMax, sHistMaxFrame);
        int over[5] = { 1200, 1300, 1400, 1500, 2000 };
        for (int k = 0; k < 5; k++) { int n = 0; for (int i = over[k] / 100; i < 40; i++) n += sHist[i]; fprintf(stderr, " over %d: %d;", over[k], n); }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "peaks: %d vertices in one draw, %d chunks of 384 vertices and %d batches in one frame\n", fe_peak_vertices,
            fe_peak_chunks, fe_peak_batches);
    return 0;
}
