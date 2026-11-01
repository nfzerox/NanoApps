/* host_scene.c — runs the hardware test scene on the Mac, headless, to PNG files.
 *   scenehost <app dir> <decoded texture dir> <out dir> */
#define GL_SILENCE_DEPRECATION 1
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <zlib.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include "../src/platform.h"
#include "../src/scene_test.h"
#include "../src/r3d.h"

static const char *sDirs[2];
static int W = 240, H = 432;
uint64_t plat_time_us(void) { struct timeval tv; gettimeofday(&tv, NULL); return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec; }
void *plat_read_file(const char *name, uint32_t *size, int save) {
    (void)save;
    for (int i = 0; i < 2; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", sDirs[i], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *d = malloc((size_t)n + 1);
        fread(d, 1, (size_t)n, f); fclose(f);
        *size = (uint32_t)n;
        return d;
    }
    return NULL;
}
int plat_write_file(const char *name, const void *data, uint32_t size) { (void)name; (void)data; (void)size; return -1; }
void plat_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }

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

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    sDirs[0] = argv[1]; sDirs[1] = argv[2];
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
    if (scene_init()) { fprintf(stderr, "scene_init failed\n"); return 1; }
    int frames = argc > 4 ? atoi(argv[4]) : 90;
    if (argc > 5) { int m = atoi(argv[5]); if (m < 0) scene_indexed = 1; else r3d_set_mode(m); }
    if (argc > 6) { scene_overscan_x = 3; scene_overscan_y = 5; r3d_set_guard(0.25f, (float)atof(argv[6])); }
    if (getenv("TR_AREA2")) { scene_overscan_x = scene_overscan_y = 0; r3d_set_guard((float)atof(getenv("TR_AREA2")), 0.9990234375f); }
    long tiny = 0, clipped = 0, out = 0;
    for (int i = 0; i < frames; i++) {
        /* uneven steps, so the camera lands on many different positions */
        scene_frame(W, H, (i % 7 == 3 ? 0.0411f : 1.f / 30.f) + (float)(i % 13) * 0.0007f, i < 60 || i >= 90 ? 0 : 1);
        tiny += r3d_stat_tiny; clipped += r3d_stat_clipped; out += r3d_stat_triangles_out;
        glFinish();
        if (i == 20 || i == 50 || i == 80) {
            char path[1024];
            snprintf(path, sizeof path, "%s/scene%02d.png", argv[3], i);
            screenshot(path);
        }
    }
    printf("%d draws, %d vertices, %d triangles per frame\n", scene_stat_draws, scene_stat_vertices, scene_stat_triangles);
    printf("%d frames: %ld triangles sent, %ld clipped, %ld dropped as tiny, %d vertices on or outside a clip boundary\n",
           frames, out, clipped, tiny, r3d_stat_outside);
    return 0;
}
