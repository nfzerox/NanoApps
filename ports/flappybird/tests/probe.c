/* Test build of the game logic with the drawing calls recorded instead of rendered, so
 * every frame can be compared with the original executable (tests/compare_original.py). */
#include <string.h>
#include <stdio.h>
#include "platform.h"
#include "sprites.h"
#include "gfx.h"
#include "game.h"

typedef struct { int id; float q[8]; float alpha; } Quad;
static Quad quads[512];
static int quad_count, image_count, events[64][2], event_count;
static char names[96][32];
static int widths[96], heights[96];
static uint32_t seed;
static float offset[2];

void probe_reset(void) { image_count = 0; }
void probe_add_image(const char *name, int w, int h) {
    if (image_count == 96) return;
    strncpy(names[image_count], name, 31);
    names[image_count][31] = 0;
    widths[image_count] = w;
    heights[image_count] = h;
    image_count++;
}
int sprites_load(void) { return 0; }
void sprites_forget(void) {}
int spr_find(const char *prefix) {
    size_t n = strlen(prefix);
    for (int i = 0; i < image_count; i++) if (!strncmp(names[i], prefix, n)) return i;
    return -1;
}
int spr_w(int id) { return id >= 0 && id < image_count ? widths[id] : 0; }
int spr_h(int id) { return id >= 0 && id < image_count ? heights[id] : 0; }
void spr_quad(int id, const float xy[8], float alpha) {
    if (quad_count == 512) return;
    Quad *q = &quads[quad_count++];
    q->id = id;
    memcpy(q->q, xy, sizeof q->q);
    q->alpha = alpha;
}
void spr_rect(int id, int x0, int y0, int x1, int y1, float alpha) {
    float q[8] = { (float)x0, (float)y0, (float)x1, (float)y0, (float)x1, (float)y1, (float)x0, (float)y1 };
    spr_quad(id, q, alpha);
}
void spr_fill(int x0, int y0, int x1, int y1, int white, float alpha) {
    spr_rect(white ? -2 : -1, x0, y0, x1, y1, alpha);
}
void spr_frame_begin(void) {}
void spr_frame_replay(void) {}
void gfx_set_frame_offset(float dx, float dy) { offset[0] = dx; offset[1] = dy; }
void gfx_forget_all(void) {}

uint64_t plat_time_us(void) { return 0; }
void *plat_read_file(const char *path, uint32_t *size, int save) { (void)path; (void)size; (void)save; return NULL; }
int plat_write_file(const char *path, const void *data, uint32_t size) { (void)path; (void)data; (void)size; return 0; }
void plat_log(const char *fmt, ...) { (void)fmt; }
void plat_sound(int sound, float gain) { (void)sound; (void)gain; }
uint32_t plat_seed(void) { return seed; }

static void on_event(int id, int value) {
    if (event_count < 64) { events[event_count][0] = id; events[event_count][1] = value; event_count++; }
}
int probe_start(uint32_t s, float panel_w) {
    seed = s;
    fb_event_hook = on_event;
    return game_init(panel_w, 0);
}
void probe_frame(int down, int x, int y) {
    PlatTouches t;
    memset(&t, 0, sizeof t);
    if (down) { t.count = 1; t.pt[0].x = (float)x; t.pt[0].y = (float)y; }
    quad_count = event_count = 0;
    game_frame(0, &t);
}
int probe_pilot(void) { return game_test_autopilot(); }
int probe_quads(Quad *out) { memcpy(out, quads, sizeof(Quad) * (size_t)quad_count); return quad_count; }
int probe_events(int *out) { memcpy(out, events, sizeof(int) * 2 * (size_t)event_count); return event_count; }
/* The same fields, in the same order, that the comparison reads from the original. */
void probe_state(float *f, int *i) {
    const FBBird *b = &G.bird;
    f[0] = b->rotation; f[1] = b->turn; f[2] = b->vy; f[3] = b->gravity; f[4] = b->hover;
    f[5] = G.fade.value; f[6] = G.flash.value;
    int n = 0;
    i[n++] = b->x; i[n++] = b->y; i[n++] = b->dead; i[n++] = b->hovering; i[n++] = b->phase; i[n++] = b->colour;
    i[n++] = G.score; i[n++] = G.best; i[n++] = G.plays; i[n++] = G.speed; i[n++] = G.start_delay; i[n++] = G.land_x;
    for (int k = 0; k < 3; k++) i[n++] = G.pipe_x[k];
    for (int k = 0; k < 3; k++) i[n++] = G.pipe_y[k];
    i[n++] = G.shake_x; i[n++] = G.shake_y; i[n++] = G.title; i[n++] = G.background == G.img_bg_night;
    i[n++] = (int)G.z; i[n++] = (int)G.w;
}
