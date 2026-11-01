/* engine_run.c — run the translated engine with no display and dump its memory.
 *
 * tests/compare_engine.py runs the original machine code the same way in the reference
 * harness and compares the two dumps byte for byte.
 *
 * usage: engine_run <engine data dir> <app dir> <frames> <seed> <dump file> */
#include "rt.h"
#include "platform.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char *sDirs[2];
uint64_t plat_time_us(void) { struct timeval tv; gettimeofday(&tv, NULL); return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec; }
void *plat_read_file(const char *name, uint32_t *size, int save) {
    if (save) return NULL;
    for (int i = 0; i < 2; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", sDirs[i], name);
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
int plat_write_file(const char *name, const void *data, uint32_t size) { (void)name; (void)data; (void)size; return 0; }
void plat_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
void plat_poll(void) {}
void plat_log_flush(void) {}
void plat_fatal(const char *message) { fprintf(stderr, "fatal: %s\n", message); exit(2); }
unsigned rt_host_load_texture(const char *name, const char *file, int repeat) { (void)name; (void)file; (void)repeat; return 1; }
void rt_host_sound(const char *name, int loop, float pitch, int stop) { (void)name; (void)loop; (void)pitch; (void)stop; }

#ifdef RT_TRACE
static FILE *sTrace;
void rt_trace(uint32_t addr, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    if (sTrace) fprintf(sTrace, "%x %x %x %x %x\n", addr, r0, r1, r2, r3);
}
#endif

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: engine_run <engine dir> <app dir> <frames> <seed> <dump>\n"); return 1; }
    sDirs[0] = argv[1]; sDirs[1] = argv[2];
    int frames = atoi(argv[3]);
#ifdef RT_TRACE
    if (argc > 6) sTrace = fopen(argv[6], "w");
#endif
    if (!rt_init(6u << 20)) return 1;
    rt_srandom((uint32_t)atoi(argv[4]));
    uint32_t game = rt_alloc(596);
    rt_invoke(rt_lookup("__ZN15cGameControllerC1Efffb"), 5, game, rt_fbits(320.0f), rt_fbits(480.0f), rt_fbits(1.0f), 0u);
    uint32_t touch = rt_alloc(32);
    rt_invoke(rt_lookup("__ZN15cGameController9initalizeEv"), 1, game);
    rt_invoke(rt_lookup("__ZN15cGameController20loadLevelInformationEv"), 1, game);
    uint32_t simulate = rt_lookup("__ZN15cGameController8simulateEf");
    rt_invoke(simulate, 2, game, rt_fbits(0.01f));
    rt_invoke(rt_lookup("__ZN15cGameController5startEv"), 1, game);
    uint32_t began = rt_lookup("__ZN15cGameController16handleTouchBeganE4vec2ii"),
             moved = rt_lookup("__ZN15cGameController16handleTouchMovedE4vec2ii"),
             ended = rt_lookup("__ZN15cGameController16handleTouchEndedE4vec2ii"),
             tilt = rt_lookup("__ZN15cGameController24handleAccelerometerForceERK4vec3"),
             draw = rt_lookup("__ZN15cGameController4drawEv");
    for (int f = 0; f < frames; f++) {
        /* the same scripted input as tests/compare_engine.py: a swipe every 40 frames, steady tilt changes */
        int phase = f % 40, kind = (f / 40) % 4;
        float dx = kind == 0 ? -120.0f : kind == 1 ? 120.0f : 0.0f, dy = kind == 2 ? -120.0f : kind == 3 ? 120.0f : 0.0f;
        if (phase == 10) { MF(touch) = 160.0f; MF(touch + 4) = 240.0f; rt_invoke(began, 4, game, touch, 1u, 1u); }
        if (phase == 11) { MF(touch) = 160.0f + dx * 0.5f; MF(touch + 4) = 240.0f + dy * 0.5f; rt_invoke(moved, 4, game, touch, 1u, 1u); }
        if (phase == 12) { MF(touch) = 160.0f + dx; MF(touch + 4) = 240.0f + dy; rt_invoke(moved, 4, game, touch, 1u, 1u); }
        if (phase == 13) { rt_invoke(ended, 4, game, touch, 1u, 1u); }
        MF(touch + 8) = (float)((f / 25) % 5 - 2) * 0.125f; MF(touch + 12) = -0.75f;
        MF(touch + 16) = 0.0f;
        rt_invoke(tilt, 2, game, touch + 8);
        rt_invoke(simulate, 2, game, rt_fbits(1.0f / 30.0f));
        rt_invoke(draw, 1, game);
    }
    FILE *out = fopen(argv[5], "wb");
    uint32_t brk = rt_heap_break();
    fwrite(&brk, 4, 1, out);
    fwrite(g_mem + 0xd9000, 1, RT_IMAGE_HI - 0xd9000, out);
    fwrite(g_mem + RT_EXTERN, 1, RT_LITERALS - RT_EXTERN, out);
    fwrite(g_mem + RT_HEAP, 1, brk - RT_HEAP, out);
    fclose(out);
    fprintf(stderr, "%d frames; heap %u bytes, %u in use; stack low %x; random() calls %u\n", frames,
            (unsigned)rt_heap_peak(), (unsigned)rt_heap_used(), (unsigned)rt_stack_low(), (unsigned)rt_random_calls());
    return 0;
}
