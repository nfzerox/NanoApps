/* host_main.c — runs the engine on the Mac for testing.
 *
 *   djhost [options]
 *     -d DIR      game data directory (converted assets)          [build/data]
 *     -S DIR      save directory                                  [build/save]
 *     -g WxH      logical game screen                             [320x480]
 *     -p WxH      panel (window / framebuffer) size in pixels     [240x432]
 *     -r          render rotated onto a portrait panel, as on the nano (panel 240x432)
 *     -w          interactive window (mouse = finger, right button = second finger at centre)
 *     -n N        headless: run N frames then exit                [600]
 *     -s N        headless: save a screenshot every N frames
 *     -i FILE     headless: input script, lines of "<frame> down X Y | move X Y | up | shot NAME"
 *     -o DIR      screenshot directory                            [build/shots]
 *     -f FPS      simulated frame rate (dt = 1/FPS)               [60]
 *     -j          alternate frame time by +/-30% to test uneven timing
 */
#define GL_SILENCE_DEPRECATION 1
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <zlib.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <GLUT/glut.h>
#include "../src/platform.h"
#include "../src/gfx.h"
#include "../src/game.h"

static const char *sDataDir = "build/data", *sSaveDir = "build/save", *sShotDir = "build/shots";
static float sLogW = 320.f, sLogH = 480.f;
static int sPanelW = 240, sPanelH = 432, sRot;
static int sFps = 60;
static int sJitter;
static uint32_t sReads;
static float sTilt;
static int sAuto;
float plat_tilt_x(void) { return sAuto ? game_test_autotilt() : sTilt; }

/* ---- platform ---- */
uint64_t plat_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

void *plat_read_file(const char *path, uint32_t *size, int save)
{
    char full[1024];
    FILE *f;
    long n;
    char *buf;
    snprintf(full, sizeof full, "%s/%s", save ? sSaveDir : sDataDir, path);
    f = fopen(full, "rb");
    if (!f)
        return NULL;
    sReads++;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = 0;
        if (size) *size = (uint32_t)n;
    }
    return buf;
}

int plat_write_file(const char *path, const void *data, uint32_t size)
{
    char full[1024];
    FILE *f;
    mkdir(sSaveDir, 0755);
    snprintf(full, sizeof full, "%s/%s", sSaveDir, path);
    f = fopen(full, "wb");
    if (!f)
        return -1;
    fwrite(data, 1, size, f);
    fclose(f);
    return 0;
}

void plat_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

extern void host_audio_init(void);

/* ---- PNG screenshot ---- */
static void put32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
    unsigned char b[4];
    uint32_t crc = crc32(0, (const unsigned char *)type, 4);
    if (len) crc = crc32(crc, data, len);
    put32(b, len); fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    put32(b, crc); fwrite(b, 1, 4, f);
}

static void screenshot(const char *name)
{
    char path[1024];
    int w = sPanelW, h = sPanelH, y;
    unsigned char *px = malloc((size_t)w * h * 4), *raw = malloc((size_t)(w * 3 + 1) * h), hdr[13];
    uLongf zlen = compressBound((uLong)(w * 3 + 1) * h);
    unsigned char *z = malloc(zlen);
    FILE *f;
    mkdir(sShotDir, 0755);
    snprintf(path, sizeof path, "%s/%s.png", sShotDir, name);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (y = 0; y < h; y++) {
        unsigned char *row = raw + (size_t)y * (w * 3 + 1), *src = px + (size_t)(h - 1 - y) * w * 4;
        int x;
        row[0] = 0;
        for (x = 0; x < w; x++) { row[1 + x * 3] = src[x * 4]; row[2 + x * 3] = src[x * 4 + 1]; row[3 + x * 3] = src[x * 4 + 2]; }
    }
    compress2(z, &zlen, raw, (uLong)(w * 3 + 1) * h, 6);
    f = fopen(path, "wb");
    if (f) {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        put32(hdr, (uint32_t)w); put32(hdr + 4, (uint32_t)h);
        hdr[8] = 8; hdr[9] = 2; hdr[10] = 0; hdr[11] = 0; hdr[12] = 0;
        chunk(f, "IHDR", hdr, 13);
        chunk(f, "IDAT", z, (uint32_t)zlen);
        chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(px); free(raw); free(z);
}

/* ---- one frame ---- */
static PlatTouches sTouch;
static uint32_t sFrame;

static uint64_t sFrameUs;
static int sPeakDraws, sPeakQuads;
static uint32_t sPeakTex;
static uint64_t sTotalDraws, sTotalMerges, sTotalStateCalls;
static const char *sDumpNames;
static int sDumpEvery = 1, sDumpFrom;

static void run_frame(float dt)
{
    if (sDumpNames && (int)sFrame >= sDumpFrom && (sFrame % (uint32_t)sDumpEvery) == 0) {
        fprintf(stderr, "[%u] ", sFrame);
        game_debug_dump(sDumpNames);
    }
    uint32_t bg = game_bg_color();
    gfx_begin_frame(sPanelW, sPanelH, sLogW, sLogH, sRot,
                    ((bg >> 16) & 255) / 255.f, ((bg >> 8) & 255) / 255.f, (bg & 255) / 255.f);
    {
        uint64_t t0 = plat_time_us();
        game_frame(dt, &sTouch);
        gfx_end_frame();
        sTotalDraws += gfx_stat_draws;
        sTotalMerges += gfx_stat_merges;
        sTotalStateCalls += gfx_stat_state_calls;
        sFrameUs += plat_time_us() - t0;
    }
    if (gfx_stat_draws > sPeakDraws) sPeakDraws = gfx_stat_draws;
    if (gfx_stat_quads > sPeakQuads) sPeakQuads = gfx_stat_quads;
    if (gfx_stat_tex_bytes > sPeakTex) sPeakTex = gfx_stat_tex_bytes;
    sFrame++;
}

/* panel pixel -> logical game pixel */
static void to_logical(int px, int py, float *lx, float *ly)
{
    if (sRot) {   /* logical x runs down the panel, logical y right to left */
        *lx = (float)py * sLogW / (float)sPanelH;
        *ly = (float)(sPanelW - 1 - px) * sLogH / (float)sPanelW;
    } else {
        *lx = (float)px * sLogW / (float)sPanelW;
        *ly = (float)py * sLogH / (float)sPanelH;
    }
}

/* ---- interactive (GLUT) ---- */
static int sWinScale = 2, sSecond;

static void on_display(void)
{
    static uint64_t last;
    uint64_t now = plat_time_us();
    float dt = last ? (float)(now - last) / 1e6f : 1.f / 60.f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    glViewport(0, 0, sPanelW, sPanelH);
    run_frame(dt);
    glutSwapBuffers();
    if (game_wants_exit())
        exit(0);
}

static void on_timer(int v)
{
    (void)v;
    glutPostRedisplay();
    glutTimerFunc(16, on_timer, 0);
}

static void set_touches(int x, int y, int down)
{
    sTouch.count = 0;
    if (down) {
        to_logical(x / sWinScale, y / sWinScale, &sTouch.pt[0].x, &sTouch.pt[0].y);
        sTouch.count = 1;
        if (sSecond) {
            sTouch.pt[1].x = sLogW * 0.5f;
            sTouch.pt[1].y = sLogH * 0.5f;
            sTouch.count = 2;
        }
    }
}

static int sDown;
static void on_mouse(int button, int state, int x, int y)
{
    if (button == GLUT_RIGHT_BUTTON) sSecond = state == GLUT_DOWN;
    sDown = state == GLUT_DOWN;
    set_touches(x, y, sDown);
}
static void on_motion(int x, int y) { set_touches(x, y, sDown); }
static void on_key_up(unsigned char k,int x,int y) {(void)x;(void)y;if(k=='a'||k=='d')sTilt=0;}
static void on_special(int k,int x,int y) {(void)x;(void)y;if(k==GLUT_KEY_LEFT)sTilt=-.5f;if(k==GLUT_KEY_RIGHT)sTilt=.5f;}
static void on_special_up(int k,int x,int y) {(void)k;(void)x;(void)y;sTilt=0;}
static void on_key(unsigned char k, int x, int y)
{
    (void)x; (void)y;
    if (k == 'a') sTilt=-.5f;
    if (k == 'd') sTilt=.5f;
    if (k == 's') sTilt=0;
    if (k == ' ') dj_shoot();
    if (k == 27 || k == 'q') exit(0);
    if (k == 'p') { char n[32]; snprintf(n, sizeof n, "manual%05u", sFrame); screenshot(n); }
}

/* ---- headless (CGL + FBO) ---- */
typedef struct { uint32_t frame; char cmd[16]; float x, y; char name[64]; } Event;
static Event *sEvents;
static int sEventCount;

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    if (!f) { fprintf(stderr, "can't open %s\n", path); exit(1); }
    while (fgets(line, sizeof line, f)) {
        Event e;
        memset(&e, 0, sizeof e);
        if (line[0] == '#' || sscanf(line, "%u %15s", &e.frame, e.cmd) < 2)
            continue;
        if (!strcmp(e.cmd, "shot"))
            sscanf(line, "%*u %*s %63s", e.name);
        else
            sscanf(line, "%*u %*s %f %f", &e.x, &e.y);
        sEvents = realloc(sEvents, sizeof(Event) * (size_t)(sEventCount + 1));
        sEvents[sEventCount++] = e;
    }
    fclose(f);
}

static int headless(int frames, int shot_every)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pf;
    CGLContextObj ctx;
    GLint npf;
    GLuint fbo, rb;
    int i, e;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf) {
        CGLPixelFormatAttribute soft[] = { kCGLPFAColorSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0 };
        if (CGLChoosePixelFormat(soft, &pf, &npf) || !pf) { fprintf(stderr, "no GL pixel format\n"); return 1; }
    }
    if (CGLCreateContext(pf, NULL, &ctx)) { fprintf(stderr, "no GL context\n"); return 1; }
    CGLSetCurrentContext(ctx);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, sPanelW, sPanelH);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) {
        fprintf(stderr, "framebuffer incomplete\n");
        return 1;
    }
    if (game_init(sLogW, sLogH)) { fprintf(stderr, "game_init failed\n"); return 1; }
    for (i = 0; i < frames; i++) {
        char name[80];
        name[0] = 0;
        for (e = 0; e < sEventCount; e++) {
            Event *ev = &sEvents[e];
            if (ev->frame != (uint32_t)i) continue;
            if (!strcmp(ev->cmd, "down") || !strcmp(ev->cmd, "move")) {
                sTouch.count = 1; sTouch.pt[0].x = ev->x; sTouch.pt[0].y = ev->y;
            } else if (!strcmp(ev->cmd, "up")) {
                sTouch.count = 0;
            } else if (!strcmp(ev->cmd, "tilt")) { sTilt=ev->x;
            } else if (!strcmp(ev->cmd, "auto")) { sAuto=(int)ev->x;
            } else if (!strcmp(ev->cmd, "shot")) {
                snprintf(name, sizeof name, "%s", ev->name);
            } else game_test_command(ev->cmd,ev->x,ev->y);
        }
        run_frame((sJitter ? (i & 1 ? 1.3f : 0.7f) : 1.f) / (float)sFps);
        glFinish();
        if (name[0]) screenshot(name);
        if (shot_every && (i % shot_every) == 0) { snprintf(name, sizeof name, "f%05d", i); screenshot(name); }
        if (game_wants_exit()) break;
    }
    fprintf(stderr, "ran %d frames; last frame: %d draws, %d quads, %d textures, %u KB of texture\n",
            i, gfx_stat_draws, gfx_stat_quads, gfx_stat_textures, gfx_stat_tex_bytes / 1024u);
    fprintf(stderr, "peaks: %d draws, %d quads, %u KiB textures; %.0f us/frame host CPU submission\n",
            sPeakDraws,sPeakQuads,sPeakTex/1024u,(double)sFrameUs/(i?i:1));
    fprintf(stderr, "file reads: %u; draws: %llu; merges: %llu\n",sReads,
            (unsigned long long)sTotalDraws,(unsigned long long)sTotalMerges);
    game_debug_dump("all");
    return 0;
}

int main(int argc, char **argv)
{
    int window = 0, frames = 600, shot_every = 0, i, panel_set = 0;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "-d")) { sDataDir = v; i++; }
        else if (!strcmp(a, "-S")) { sSaveDir = v; i++; }
        else if (!strcmp(a, "-o")) { sShotDir = v; i++; }
        else if (!strcmp(a, "-g")) { sscanf(v, "%fx%f", &sLogW, &sLogH); i++; }
        else if (!strcmp(a, "-p")) { sscanf(v, "%dx%d", &sPanelW, &sPanelH); panel_set = 1; i++; }
        else if (!strcmp(a, "-r")) sRot = 1;
        else if (!strcmp(a, "-w")) window = 1;
        else if (!strcmp(a, "-n")) { frames = atoi(v); i++; }
        else if (!strcmp(a, "-s")) { shot_every = atoi(v); i++; }
        else if (!strcmp(a, "-i")) { load_script(v); i++; }
        else if (!strcmp(a, "-f")) { sFps = atoi(v); i++; }
        else if (!strcmp(a, "-j")) { sJitter = 1; }
        else if (!strcmp(a, "-x")) { sWinScale = atoi(v); i++; }
        else if (!strcmp(a, "-D")) { sDumpNames = v; i++; }          /* log these script globals ... */
        else if (!strcmp(a, "-De")) { sDumpEvery = atoi(v); i++; }   /* ... every N frames ... */
        else if (!strcmp(a, "-Df")) { sDumpFrom = atoi(v); i++; }    /* ... from frame N */
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    if(sFps<1 || sPanelW<1 || sPanelH<1)return 2;
    if (sRot && !panel_set) { sPanelW = 240; sPanelH = 432; }
    if (!window)
        return headless(frames, shot_every);

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(sPanelW * sWinScale, sPanelH * sWinScale);
    glutCreateWindow("Doodle Jump (nano port, host build)");
    host_audio_init();
    /* draw at panel resolution into the corner, like the device; the window is scaled 2x
     * by rendering into an FBO-less viewport of panel size and letting GLUT stretch */
    sPanelW *= sWinScale;
    sPanelH *= sWinScale;
    if (game_init(sLogW, sLogH)) { fprintf(stderr, "game_init failed\n"); return 1; }
    {
        int ws = sWinScale;
        sWinScale = 1;           /* mouse coordinates are already in (scaled) panel pixels */
        (void)ws;
    }
    glutDisplayFunc(on_display);
    glutMouseFunc(on_mouse);
    glutMotionFunc(on_motion);
    glutKeyboardFunc(on_key);
    glutKeyboardUpFunc(on_key_up);
    glutSpecialFunc(on_special);
    glutSpecialUpFunc(on_special_up);
    glutTimerFunc(16, on_timer, 0);
    glutMainLoop();
    return 0;
}
