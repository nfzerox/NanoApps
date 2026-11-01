/* The same digest on the development host and on the final ARM/VFP compiler target:
 * the whole game (menus, flight, crash, score panel, restart) is run with the test pilot
 * and every frame's state and drawn quads are hashed. Linux/QEMU's write and exit
 * syscalls are the only ARM host adapter. build/atlas_table.h is generated from the
 * local atlas.txt (tests/cross_check.sh); no game data is kept in the repository. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "game.h"
#include "atlas_table.h"

typedef struct { int id; float q[8]; float alpha; } Quad;
void probe_reset(void);
void probe_add_image(const char *name, int w, int h);
int probe_start(uint32_t seed, float panel_w);
void probe_frame(int down, int x, int y);
int probe_pilot(void);
int probe_quads(Quad *out);
void probe_state(float *f, int *i);

#ifdef FB_ARM_CHECK
static void output(const char *s, unsigned length) {
    register unsigned r0 __asm__("r0") = 1;
    register const char *r1 __asm__("r1") = s;
    register unsigned r2 __asm__("r2") = length;
    register unsigned r7 __asm__("r7") = 4;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
}
void *memcpy(void *d, const void *s, size_t n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { char *a = d; while (n--) *a++ = (char)c; return d; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
    return 0;
}
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
void free(void *p) { (void)p; }
#else
#include <unistd.h>
static void output(const char *s, unsigned length) { (void)write(1, s, length); }
#endif

static uint32_t hash = 2166136261u;
static void word(uint32_t v) { for (int i = 0; i < 4; i++) { hash ^= v & 255; hash *= 16777619u; v >>= 8; } }
static void number(float f) { uint32_t v; memcpy(&v, &f, 4); word(v); }
static Quad quads[512];

static void run(uint32_t seed, int frames) {
    float f[7];
    int s[24], tap = 0, hold = 0, idle = 0, press_at = 40;
    probe_reset();
    for (unsigned i = 0; i < sizeof kAtlas / sizeof kAtlas[0]; i++) probe_add_image(kAtlas[i].name, kAtlas[i].w, kAtlas[i].h);
    probe_start(seed, 240.0f);
    for (int frame = 0; frame < frames; frame++) {
        int down = 0, x = 144, y = 300;
        probe_state(f, s);
        int speed = s[9], title = s[20], hovering = s[3];
        if (frame == press_at) hold = 4;
        if (hold) { down = 1; x = 78; y = 375; hold--; }               /* the Play button */
        else if (!title && speed > 0) { tap = probe_pilot() && !tap; down = tap; }
        idle = (!title && speed == 0 && !hovering) ? idle + 1 : 0;
        if (idle == 300) press_at = frame + 1;                          /* play again */
        probe_frame(down, x, y);
        probe_state(f, s);
        for (int i = 0; i < 7; i++) number(f[i]);
        for (int i = 0; i < 24; i++) word((uint32_t)s[i]);
        int n = probe_quads(quads);
        word((uint32_t)n);
        for (int i = 0; i < n; i++) {
            word((uint32_t)quads[i].id);
            for (int k = 0; k < 8; k++) number(quads[i].q[k]);
            number(quads[i].alpha);
        }
    }
}

int main(void) {
    run(1, 6000);
    run(777, 6000);
    run(20140209, 6000);
    char text[9];
    for (int i = 0; i < 8; i++) text[i] = "0123456789abcdef"[(hash >> (28 - 4 * i)) & 15];
    text[8] = '\n';
    output(text, 9);
    return 0;
}
#ifdef FB_ARM_CHECK
__attribute__((noreturn)) void _start(void) {
    int result = main();
    register unsigned r0 __asm__("r0") = (unsigned)result, r7 __asm__("r7") = 1;
    __asm__ volatile("svc 0" : : "r"(r0), "r"(r7) : "memory");
    __builtin_unreachable();
}
#endif
