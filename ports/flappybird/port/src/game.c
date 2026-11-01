/* Flappy Bird 1.2: the original game logic, translated from the decompiled engine.
 *
 * The original is one C++ "Engine" object with a "Game" subclass; addresses below are the
 * functions in the inspected armv7 executable (see analysis/port-notes.md). Everything
 * here runs once per displayed frame, as on the phone: positions are whole canvas pixels
 * (288x512), the bird's speed and rotation are floats, and time is counted in frames.
 * Drawing happens inside the update, in the original's order. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "gfx.h"
#include "sprites.h"
#include "game.h"
#include "tables.h"

FBGame G;
static int ready_flag;
static int best_saved;

/* ---- random numbers ------------------------------------------------------------------ */

/* Pipe heights, screen shake and medal sparkles: two 16-bit multiply-with-carry
 * generators, combined (FUN_00010b18, FUN_0000dba0, FUN_0000fad0). */
static uint32_t step_z(uint32_t z) { return (z & 0xffffu) * 0x9069u + (uint32_t)((int32_t)z >> 16); }
static uint32_t step_w(uint32_t w) { return (w & 0xffffu) * 18000u + (uint32_t)((int32_t)w >> 16); }
static int absolute(uint32_t v) { return (int32_t)v < 0 ? (int)(0u - v) : (int)v; }
static int mwc_abs(void) {
    G.z = step_z(G.z);
    G.w = step_w(G.w);
    return absolute(G.w + (G.z << 16));
}

/* The C library's rand(), never seeded by the game: the background and bird colours follow
 * the same sequence after every launch (Libc stdlib/FreeBSD/rand.c, do_rand). */
static int libc_rand(void) {
    int32_t hi, lo, x;
    if (G.rand_next == 0) G.rand_next = 123459876u;
    hi = (int32_t)(G.rand_next / 127773u);
    lo = (int32_t)(G.rand_next % 127773u);
    x = 16807 * lo - 2836 * hi;
    if (x < 0) x += 0x7fffffff;
    G.rand_next = (uint32_t)x;
    return (int)((uint32_t)x % 0x80000000u);
}

/* ---- tweens (FUN_0000e148, FUN_0000e790) ---------------------------------------------- */

static void tween_step(FBTween *t) {
    if (t->done) return;
    int cur = ++t->cur;
    float v = t->inv * (float)cur;
    t->t = v;
    if (t->type == 5 || t->type == 11) {
        int i = (int)(v * 100.0f);
        if (i > 99) i = 99;     /* only on the last step, whose value is replaced below */
        v = (t->type == 5 ? kEaseCubicOut : kEaseQuintOut)[i];
        t->t = v;
    }
    t->value = v * t->delta + t->start;
    if (cur == t->total) {
        t->done = 1;
        t->value = t->end;
        t->t = 1.0f;
    }
}

static void tween_start(FBTween *t, float start, float end, int type, float seconds) {
    t->start = start;
    t->end = end;
    t->total = (int)(seconds * 60.0f);
    t->inv = 1.0f / (float)t->total;
    t->delta = end - start;
    t->cur = 0;
    t->type = type;
    t->done = 0;
    t->value = start;
}

/* ---- engine services ------------------------------------------------------------------ */

static void on_timer(int id, int arg);

/* FUN_0000e754: fire `id` after about `ms`; the engine counts 30 per frame. */
static void timer_add(int id, int arg, int ms) {
    G.timer_id[G.timer_next] = id;
    G.timer_arg[G.timer_next] = arg;
    G.timer_ms[G.timer_next] = ms;
    if (++G.timer_next > 49) G.timer_next = 0;
}

/* FUN_0000e874 queues a numbered event for the Objective-C side; these are the ones that
 * matter away from the App Store, Game Center and the ad banner. */
void (*fb_event_hook)(int id, int value);       /* tests only: every engine event, in order */

static void event(int id, int value) {
    if (fb_event_hook) fb_event_hook(id, value);
    switch (id) {
    case 0:                                   /* score reported: the best is saved */
        if (value > best_saved) {
            unsigned char file[8];
            uint32_t words[2] = { (uint32_t)value, (uint32_t)G.plays };
            best_saved = value;
            for (int i = 0; i < 8; i++) file[i] = (unsigned char)(words[i / 4] >> (8 * (i % 4)));
            plat_write_file("score.dat", file, 8);
        }
        break;
    case 9:  plat_sound(FB_SND_POINT, 0.2f); break;
    case 10: plat_sound(FB_SND_SWOOSH, 1.0f); break;
    case 11: plat_sound(FB_SND_DIE, 1.0f); break;
    case 12: plat_sound(FB_SND_HIT, 1.0f); break;
    case 13: plat_sound(FB_SND_WING, 1.0f); break;
    default: break;     /* 1 leaderboard, 2 rate, 4 share, 3/6/7/8 adverts and Game Center */
    }
}

/* FUN_0000d938: fade to black (mode 1) or from black; `id` is delivered when it ends. */
static void fade_start(int mode, int id, float seconds) {
    FBTween *t = &G.fade;
    if (!t->done) return;
    t->total = (int)(seconds * 60.0f);
    t->inv = 1.0f / (float)t->total;
    t->cur = 0;
    t->type = 5;
    t->done = 0;
    if (mode == 1) { t->start = 0.0f; t->end = 1.0f; t->delta = 1.0f; t->value = 0.0f; }
    else           { t->start = 1.0f; t->end = 0.0f; t->delta = -1.0f; t->value = 1.0f; }
    tween_step(t);
    G.fade_id = id;
}

/* FUN_0000e7e0: white flash. */
static void flash_start(float seconds) {
    FBTween *t = &G.flash;
    if (!t->done) return;
    t->total = (int)(seconds * 60.0f);
    t->inv = 1.0f / (float)t->total;
    t->cur = 0;
    t->type = 11;
    t->done = 0;
    t->start = 1.0f; t->end = 0.0f; t->delta = -1.0f; t->value = 1.0f;
    tween_step(t);
}

/* FUN_0000e838: shake the picture; the amount is in screen pixels. */
static void shake_start(int amount, float seconds) {
    G.shake_amp = (int)((float)amount / G.screen_scale);
    G.shake_frames = (int)(seconds * 60.0f);
}

/* ---- drawing helpers ------------------------------------------------------------------ */

static void draw_image(int id, int x, int y, float alpha) {       /* FUN_0000ceec */
    spr_rect(id, x, y, x + spr_w(id), y + spr_h(id), alpha);
}

/* FUN_00010014: turn a point about (cx, cy) by whole degrees, with the engine's tables. */
static void rotate(int *px, int *py, float cx, float cy, int angle) {
    int a = angle % 360;
    if (a < 0) a += 360;
    float dy = (float)*py - cy, dx = (float)*px - cx, s = kSin[a], c = kCos[a];
    float x = (dx * c - dy * s) + cx, y = (dx * s + dy * c) + cy;
    *px = (int)x;
    *py = (int)y;
}

/* FUN_0000e5a0: an image turned about its centre; each corner lands on a whole pixel. */
static void draw_rotated(int id, int x, int y, float alpha, int angle) {
    int w = spr_w(id), h = spr_h(id);
    float cx = (float)(x + (w >> 1)), cy = (float)(y + (h >> 1));
    int bl[2] = { x, y + h }, br[2] = { x + w, y + h }, tr[2] = { x + w, y }, tl[2] = { x, y };
    rotate(&bl[0], &bl[1], cx, cy, angle);
    rotate(&br[0], &br[1], cx, cy, angle);
    rotate(&tr[0], &tr[1], cx, cy, angle);
    rotate(&tl[0], &tl[1], cx, cy, angle);
    float q[8] = { (float)tl[0], (float)tl[1], (float)tr[0], (float)tr[1],
                   (float)br[0], (float)br[1], (float)bl[0], (float)bl[1] };
    spr_quad(id, q, alpha);
}

/* FUN_0000cda4 + FUN_0000cdc0 with the "font" glyphs: the big score, centred on (x, y). */
static void draw_score(int value, int x, int y) {
    char text[12];
    int n = 0, width = 0, max_h = 0;
    unsigned v = (unsigned)value;
    do { text[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);      /* the original prints "%u" */
    for (int i = 0; i < n / 2; i++) { char c = text[i]; text[i] = text[n - 1 - i]; text[n - 1 - i] = c; }
    for (int i = 0; i < 10; i++) {
        int h = spr_h(G.img_font + i);
        if (h > max_h) max_h = h;
    }
    for (int i = 0; i < n; i++) width += spr_w(G.img_font + (text[i] - '0')) - 4;
    width += 2;
    x -= width / 2;
    y -= max_h / 2;
    for (int i = 0; i < n; i++) {
        int id = G.img_font + (text[i] - '0');
        spr_rect(id, x, y, x + spr_w(id), y + spr_h(id), 1.0f);
        x += spr_w(id) - 4;
    }
}

/* FUN_0000f930: a number in the small digits, right-aligned at x. */
static void draw_number(int value, int x, int y) {
    int w = spr_w(G.img_number), first = 1;
    x -= w;
    for (int i = 0; i < 10; i++) {
        if (value < 1 && !first) break;
        draw_image(G.img_number + value % 10, x, y, 1.0f);
        x -= w;
        first = 0;
        value /= 10;
    }
}

/* ---- animations (FUN_0000f62c, FUN_0000f684, FUN_0000f4f0) ----------------------------- */

static const int kBirdFrames[12] = { 0, 1, 2, 1, 0, 1, 2, 1, 0, 1, 2, 1 };
static const int kBlinkFrames[5] = { 0, 1, 2, 1, 0 };

static void anim_define(FBAnim *a, const int *frames, int count, int fps, int loop) {
    memset(a, 0, sizeof *a);
    a->frames = frames;
    a->count = count;
    a->duration = 1000 / fps;
    a->loop = loop;
}

static void anim_play(FBAnim *a, int restart) {
    if (restart == 1) { a->timer = 0; a->index = 0; a->current = a->frames[0]; }
    if (!a->loop && a->finished) { a->timer = 0; a->index = 0; a->current = a->frames[0]; }
    a->finished = 0;
}

static void anim_step(FBAnim *a) {
    if (a->finished) return;
    a->timer += 15;
    if (a->timer < a->duration) return;
    a->timer = 0;
    int i = ++a->index;
    if (i >= a->count) {
        if (!a->loop) {
            a->finished = 1;
            return;             /* the original reads one frame past the list here; unused */
        }
        i = a->index = 0;
    }
    a->current = a->frames[i];
}

/* ---- the bird (FUN_000108a8, FUN_00011194, FUN_0001134c, FUN_00011434) ----------------- */

static void bird_reset(void) {
    FBBird *b = &G.bird;
    b->x = 80;
    b->y = 246;
    b->visible = 1;
    b->rotation = 0.0f;
    b->vy = 0.0f;
    b->gravity = 1.0f;
    b->turn_accel = 0.4f;
    b->dead = 0;
    b->hovering = 1;
    b->phase = 0;
    b->playing = &b->glide;
    anim_play(&b->glide, 1);
    int r = libc_rand() % 10;
    b->colour = r < 6 ? 0 : r < 8 ? 1 : 2;
}

static void bird_flap(void) {
    FBBird *b = &G.bird;
    if (b->hovering) b->hovering = 0;
    if (b->y >= 0 && !b->dead) {
        b->playing = &b->flap;
        anim_play(&b->flap, 1);
        b->vy = -5.0f;
        b->gravity = 0.3f;
        b->turn = -10.0f;
        b->turn_accel = 0.4f;
        timer_add(4, 0, 5);                 /* wing sound */
    }
}

static void bird_update(void) {
    FBBird *b = &G.bird;
    if (b->playing) anim_step(b->playing);
    if (b->hovering) {
        int phase = b->phase + 8;
        if (phase == 360) phase = 0;
        b->phase = phase;
        b->hover = kSin[phase] * 4.0f;
        return;
    }
    b->hover = 0.0f;
    float vy = b->gravity + b->vy;
    if (vy > 8.0f) vy = 8.0f;
    b->vy = vy;
    b->y = (int)((float)b->y + vy);
    if (b->y > 400 - b->h) {
        b->y = 400 - b->h;
        b->vy = 0.0f;
        b->gravity = 0.0f;
    }
    float turn = b->turn, r = turn + b->rotation;
    b->rotation = r;
    b->turn = b->turn_accel + turn;
    if (r < -20.0f) b->rotation = -20.0f;
    else if (r > 90.0f) b->rotation = 90.0f;
}

static void bird_draw(void) {
    FBBird *b = &G.bird;
    if (!b->visible) return;
    int id = G.img_bird + b->colour * 3 + 1;
    if (b->playing && !b->playing->finished) id = G.img_bird + b->playing->current + b->colour * 3;
    int y = (int)(b->hover + (float)(b->y - b->oy));
    draw_rotated(id, b->x - b->ox, y, 1.0f, (int)b->rotation);
}

/* ---- buttons (FUN_0000f748, FUN_0000f75c, FUN_0000f7dc) -------------------------------- */

static void button_show(FBButton *b, int x, int y) {
    b->x = x;
    b->y = y;
    b->active = b->visible = 1;
    b->pressed = b->just_pressed = b->released = 0;
}

static void button_update(FBButton *b) {
    int hit = 0;
    for (int i = 0; i < G.touch_count; i++) {
        int tx = G.touch_x[i], ty = G.touch_y[i];
        if (b->x < tx && tx < b->w + b->x && b->y < ty && ty < b->y + b->h) { hit = 1; break; }
    }
    b->just_pressed = b->released = 0;
    if (hit == b->pressed) return;
    if (!b->pressed) { b->pressed = 1; b->just_pressed = 1; }
    else { b->released = 1; b->pressed = 0; }
}

static void button_draw(const FBButton *b) {
    draw_image(b->image, b->x, b->pressed ? b->y + 2 : b->y, 1.0f);
}

/* ---- medal and its sparkles (FUN_0000fad0, FUN_0000fbbc, blink class at 0xfa74) -------- */

static void medal_update(void) {
    FBMedal *m = &G.medal;
    if (m->active && m->timer > 0 && --m->timer < 1) {
        m->timer = 30;
        uint32_t z1 = step_z(G.z), w1 = step_w(G.w);
        G.z = step_z(z1);
        G.w = step_w(w1);
        int dy = absolute(G.w + (G.z << 16)) % (m->h + 6);
        int dx = absolute(w1 + (z1 << 16)) % (m->w + 6);
        FBBlink *k = &G.blinks[G.blink_next];
        if (++G.blink_next == FB_BLINKS) G.blink_next = 0;
        k->x = dx + m->x - 3;
        k->y = dy + m->y - 3;
        k->active = k->visible = 1;
        anim_play(&k->anim, 1);
    }
}

static void blinks_update(void) {
    for (int i = 0; i < FB_BLINKS; i++) {
        FBBlink *k = &G.blinks[i];
        if (!k->active) continue;
        anim_step(&k->anim);
        if (k->anim.finished) k->active = k->visible = 0;
    }
}

static void blinks_draw(void) {
    for (int i = 0; i < FB_BLINKS; i++) {
        const FBBlink *k = &G.blinks[i];
        if (k->visible) draw_image(G.img_blink + k->anim.current, k->x - 4, k->y - 4, 1.0f);
    }
}

/* ---- score panel (FUN_0000fcc0, FUN_0000fd54, FUN_0000fea4) ---------------------------- */

static void panel_show(int score, int best) {
    FBPanel *p = &G.panel;
    p->score = score;
    p->best = best;
    p->shown = 0;
    p->active = p->visible = 1;
    p->is_new = 0;
    p->x = (288 - p->w) >> 1;
    p->y = 504;
    FBTween *t = &p->tween;
    t->start = 504.0f;
    t->end = (float)p->target_y;
    t->delta = (float)p->target_y + -504.0f;
    t->total = 30;
    t->inv = 1.0f / 30.0f;
    t->cur = 0;
    t->type = 11;
    t->done = 0;
    t->value = 504.0f;
    p->state = 0;
    G.medal.active = G.medal.visible = 0;
}

/* FUN_0000e8a4: the count-up finished. */
static void score_shown(int value) {
    event(0, value);
    if (G.best < value) G.best = value;
}

static void panel_update(void) {
    FBPanel *p = &G.panel;
    if (!p->active) return;
    if (!p->tween.done) tween_step(&p->tween);
    if (p->state == 2) {
        medal_update();
    } else if (p->state == 1) {
        p->shown = (int)p->tween.value;
        if (!p->tween.done) return;
        p->state = 2;
        score_shown(p->shown);
        if (p->best < p->shown) { p->best = p->shown; p->is_new = 1; }
        /* 10 bronze, 20 silver, 30 gold, 40 platinum (images medals_3 .. medals_0) */
        int medal = p->shown >= 40 ? 0 : p->shown >= 30 ? 1 : p->shown >= 20 ? 2 : p->shown >= 10 ? 3 : -1;
        if (medal >= 0) {
            G.medal.w = G.medal.h = 44;
            G.medal.index = medal;
            G.medal.timer = 30;
            G.medal.active = G.medal.visible = 1;
        }
        G.medal.x = p->x + 32;
        G.medal.y = p->y + 44;
    } else if (p->state == 0) {
        p->y = (int)p->tween.value;
        if (!p->tween.done) return;
        if (p->score > 0) {
            float end = (float)p->score;
            p->state = 1;
            FBTween *t = &p->tween;
            t->start = 0.0f; t->end = end; t->delta = end;
            t->total = 30; t->inv = 1.0f / 30.0f; t->cur = 0; t->type = 0; t->done = 0; t->value = 0.0f;
        } else {
            p->state = 2;
        }
    }
}

static void panel_draw(void) {
    const FBPanel *p = &G.panel;
    if (!p->visible) return;
    draw_image(G.img_panel, p->x, p->y, 1.0f);
    draw_number(p->shown, p->x + 210, p->y + 36);
    draw_number(p->best, p->x + 210, p->y + 78);
    if (p->is_new) draw_image(G.img_new, p->x + 142, p->y + 60, 1.0f);
    if (G.medal.visible) draw_image(G.img_medal + G.medal.index, G.medal.x, G.medal.y, 1.0f);
}

/* ---- "Get Ready" and "Game Over" (FUN_00010350.., FUN_00010430..) ---------------------- */

static void ready_update(void) {
    FBReady *r = &G.ready;
    tween_step(&r->tween);
    if (r->state == 2) {
        if (r->tween.done) { r->active = 1; r->visible = 0; }
    } else if (r->state == 0 && r->tween.done) {
        r->state = 1;
    }
}

static void ready_draw(void) {
    float alpha = G.ready.tween.value;
    draw_image(G.img_ready, (288 - spr_w(G.img_ready)) >> 1, 146, alpha);
    draw_image(G.img_tutorial, (288 - spr_w(G.img_tutorial)) >> 1, 220, alpha);
}

static void over_show(void) {
    FBOver *o = &G.over;
    o->active = o->visible = 1;
    tween_start(&o->tween, 0.0f, 1.0f, 11, 1.0f);
    o->offset = -1;
    o->vy = -2.0f;
    o->accel = 0.25f;
    o->state = 0;
    event(10, 0);
}

static void over_update(void) {
    FBOver *o = &G.over;
    tween_step(&o->tween);
    if (o->offset < 0) {
        float vy = o->vy;
        o->offset = (int)((float)o->offset + vy);
        o->vy = o->accel + vy;
    } else {
        o->offset = 0;
    }
    if (o->state == 1) {
        if (G.panel.state == 2) o->state = 2;
    } else if (o->state == 0 && o->tween.done) {
        o->state = 1;
        panel_show(G.score, G.best);
        event(10, 0);
    }
}

static void over_draw(void) {
    draw_image(G.img_game_over, (288 - spr_w(G.img_game_over)) >> 1, G.over.offset + 130, G.over.tween.value);
}

/* ---- the game (FUN_00010918, FUN_00010a18, FUN_00011104, FUN_00010b18) ----------------- */

/* Back to the title state; a restart then changes a few things again. The pipes keep
 * their positions: the start delay hides them until they have scrolled away. */
static void reset_to_title(void) {
    G.background = libc_rand() % 10 < 6 ? G.img_bg_day : G.img_bg_night;
    for (int i = 0; i < FB_BLINKS; i++) G.blinks[i].active = G.blinks[i].visible = 0;
    button_show(&G.play, (0x110 - G.play.w - G.scores.w) >> 1, 340);
    button_show(&G.scores, G.play.w + G.play.x + 16, 340);
    button_show(&G.rate, (288 - G.rate.w) >> 1, 270);
    G.ready.active = 0;
    G.over.active = 0;
    G.panel.active = G.panel.visible = 0;
    G.title = 1;
    G.land_x = 0;
    bird_reset();
    G.speed = 2;
    G.start_delay = 1;
    G.score = 0;
}

static void on_timer(int id, int arg) {
    (void)arg;
    switch (id) {
    case 2:                                         /* a second after the crash */
        G.game_over = 1;
        event(0, G.score);
        over_show();
        event(8, 0);
        break;
    case 3: event(11, 0); break;                    /* falling sound, after a pipe hit */
    case 4: event(13, 0); break;                    /* wing */
    case 5:                                         /* Play pressed and the screen is black */
        reset_to_title();
        G.play.active = 0;
        G.scores.active = 0;
        G.rate.active = 0;
        G.title = 0;
        fade_start(0, 0, 0.5f);
        G.land_x = 0;
        bird_reset();
        G.speed = 2;
        G.start_delay = 1;
        G.score = 0;
        G.ready.active = G.ready.visible = 1;
        tween_start(&G.ready.tween, 0.0f, 1.0f, 0, 0.5f);
        G.ready.state = 0;
        event(7, 0);
        G.plays++;
        break;
    case 6:                                         /* first frame after launch */
        reset_to_title();
        G.bird.colour = 0;
        fade_start(0, 0, 0.5f);
        event(6, 0);
        break;
    case 7: event(3, 0); break;
    default: break;
    }
}

/* A finger went down. The never-shown pause button sits at the canvas origin and still
 * swallows touches within 20 pixels of it. */
static void touch_began(int x, int y) {
    if (G.title) return;
    const FBButton *p = &G.pause;
    if (!(x < p->x - 20 || p->x + p->w + 20 < x || y < p->y - 20 || p->h + p->y + 20 < y)) return;
    if (!G.bird.hovering) {
        if (G.speed < 1) return;
    } else {
        if (!G.ready.active || G.ready.state != 1) return;
        G.ready.state = 2;
        tween_start(&G.ready.tween, 1.0f, 0.0f, 0, 0.5f);
        G.bird.hovering = 0;
    }
    bird_flap();
}

static void crash(int die_sound) {
    G.speed = 0;
    event(12, 0);
    if (die_sound) timer_add(3, 0, 500);
    timer_add(2, 0, 1000);
}

/* One pipe pair against the bird's 20x20 box; the gap is the 96 pixels above pipe_y. */
static void pipe_collide(int i) {
    FBBird *b = &G.bird;
    int px = G.pipe_x[i], py = G.pipe_y[i], right = b->w + b->x;
    int down_w = spr_w(G.img_pipe_down), down_h = spr_h(G.img_pipe_down);
    int up_w = spr_w(G.img_pipe_up), up_h = spr_h(G.img_pipe_up);
    if (px > right) return;
    if (!(down_w + px < b->x) && !(b->h + b->y < (py - 96) - down_h || py - 96 < b->y)) {
        flash_start(1.0f);                          /* the upper pipe */
        shake_start(4, 0.5f);
        if (i == 1) b->dead = 1;                    /* the first pair leaves the flag clear */
    } else {
        if (up_w + px < b->x || b->h + b->y < py || up_h + py < b->y) return;
        flash_start(1.0f);                          /* the lower pipe */
        shake_start(4, 0.5f);
        b->dead = 1;
    }
    crash(1);
}

static void game_update(void) {
    FBBird *b = &G.bird;
    int pipe_w = spr_w(G.img_pipe_up);
    draw_image(G.background, 0, 0, 1.0f);

    int speed = G.speed, land = G.land_x - speed;
    if (land < -23) land = 0;
    G.land_x = land;
    if (!b->hovering) {
        int x0 = G.pipe_x[0] - speed;
        G.pipe_x[0] = x0;
        G.pipe_x[1] -= speed;
        G.pipe_x[2] -= speed;
        if (speed > 0 && G.start_delay < 1 && (x0 == b->x || x0 == b->x - 1)) {
            G.score++;
            event(9, 0);
        }
        if (x0 < -pipe_w) {
            G.pipe_x[0] = G.pipe_x[1];
            G.pipe_y[0] = G.pipe_y[1];
            G.pipe_x[1] = G.pipe_x[2];
            G.pipe_y[1] = G.pipe_y[2];
            G.pipe_x[2] = G.pipe_x[2] + G.pipe_spacing + pipe_w;
            G.pipe_y[2] = mwc_abs() % 180 + 180;
            if (G.start_delay > 0 && --G.start_delay == 0) {
                G.pipe_x[1] = -pipe_w;
                G.pipe_x[0] = -pipe_w;
            }
        }
    }
    bird_update();

    if (G.title) {
        draw_image(G.img_title, (288 - spr_w(G.img_title)) >> 1, 150, 1.0f);
        b->x = (288 - b->w) >> 1;
        b->y = spr_h(G.img_title) + 170;
        bird_draw();
    } else {
        if (b->y >= 400 - b->h && G.speed >= 1) {   /* the ground */
            flash_start(1.0f);
            shake_start(4, 0.5f);
            crash(0);
        }
        if (!b->dead && G.start_delay < 1 && G.speed > 0) {
            pipe_collide(0);
            pipe_collide(1);
        }
        if (G.start_delay < 1) {
            int down_h = spr_h(G.img_pipe_down);
            for (int i = 0; i < 3; i++) {
                if (i == 2 && G.pipe_x[2] >= 288) break;
                draw_image(G.img_pipe_up, G.pipe_x[i], G.pipe_y[i], 1.0f);
                draw_image(G.img_pipe_down, G.pipe_x[i], (G.pipe_y[i] - 96) - down_h, 1.0f);
            }
        }
        if (G.panel.active && G.panel.state == 2 && !G.play.active) {
            button_show(&G.play, (0x110 - G.play.w - G.scores.w) >> 1, 340);
            button_show(&G.scores, G.play.w + G.play.x + 16, 340);
        }
        if (!G.over.active) {
            draw_score(G.score, 144, 100);
        } else {
            over_update();
            over_draw();
        }
        bird_draw();
    }
    draw_image(G.img_land, G.land_x, 512 - spr_h(G.img_land), 1.0f);
    if (G.ready.active) {
        ready_update();
        ready_draw();
    }
    if (G.title)
        draw_image(G.img_copyright, (288 - spr_w(G.img_copyright)) >> 1, 432 - spr_h(G.img_copyright), 1.0f);
    if (G.play.active) {
        button_update(&G.play);
        button_draw(&G.play);
        button_update(&G.scores);
        button_draw(&G.scores);
        if (G.play.released) {
            fade_start(1, 5, 0.5f);
            event(10, 0);
        }
        if (G.scores.released) {
            event(1, 0);
            event(10, 0);
        }
        if (G.rate.active) {
            button_update(&G.rate);
            button_draw(&G.rate);
            if (G.rate.released) event(2, 0);
        }
    }
}

/* FUN_0000dba0: one displayed frame. */
static void engine_frame(void) {
    for (int i = 0; i < 50; i++) {
        if (G.timer_ms[i] > 0) {
            G.timer_ms[i] -= 30;
            if (G.timer_ms[i] < 1) on_timer(G.timer_id[i], G.timer_arg[i]);
        }
    }
    game_update();
    panel_update();
    blinks_update();
    if (!G.fade.done || G.fade.value != 0.0f) {
        tween_step(&G.fade);
        if (G.fade.done) on_timer(G.fade_id, 0);
    }
    tween_step(&G.flash);
    if (G.shake_frames < 1) {
        G.shake_x = G.shake_y = 0;
    } else {
        G.shake_frames--;
        int amp = G.shake_amp;
        G.shake_x = amp ? mwc_abs() % (amp << 1) - amp : (mwc_abs(), 0);
        G.shake_y = amp ? mwc_abs() % (amp << 1) - amp : (mwc_abs(), 0);
    }
    panel_draw();
    blinks_draw();
    /* the engine draws both overlays far past the canvas edges, so a shaken picture stays covered */
    if (!G.fade.done || G.fade.value != 0.0f) spr_fill(-144, -256, 720, 1280, 0, G.fade.value);
    if (!G.flash.done || G.flash.value != 0.0f) spr_fill(-144, -256, 720, 1280, 1, G.flash.value);
    gfx_set_frame_offset((float)G.shake_x / G.screen_scale, (float)G.shake_y / G.screen_scale);
}

/* ---- set-up (FUN_0000d268, FUN_000105dc, FUN_00011234) --------------------------------- */

static void button_init(FBButton *b, const char *image) {
    memset(b, 0, sizeof *b);
    b->image = spr_find(image);
    b->w = spr_w(b->image);
    b->h = spr_h(b->image);
}

static uint32_t read32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void fb_start(uint32_t seed) {
    int best = G.best, plays = G.plays;
    float scale = G.screen_scale;
    memset(&G, 0, sizeof G);
    G.best = best;
    G.plays = plays;
    G.screen_scale = scale;
    /* dot_EAGLView setup: both generators are seeded from the clock */
    G.w = (uint32_t)((int32_t)seed % 32000);
    G.z = (uint32_t)((int32_t)seed % 65535);
    G.rand_next = 1;

    G.fade.done = 1;
    G.flash.done = 1;
    G.img_bg_day = spr_find("bg_day");
    G.img_bg_night = spr_find("bg_night");
    G.img_land = spr_find("land");
    G.img_pipe_up = spr_find("pipe_up");
    G.img_pipe_down = spr_find("pipe_down");
    G.img_title = spr_find("title");
    G.img_copyright = spr_find("brand_copyright");
    G.img_bird = spr_find("bird");
    G.img_font = spr_find("font");
    G.img_number = spr_find("number_score");
    G.img_medal = spr_find("medals");
    G.img_blink = spr_find("blink");
    G.img_panel = spr_find("score_panel");
    G.img_new = spr_find("new");
    G.img_ready = spr_find("text_ready");
    G.img_tutorial = spr_find("tutorial");
    G.img_game_over = spr_find("text_game_over");

    G.panel.w = spr_w(G.img_panel);
    G.panel.h = spr_h(G.img_panel);
    G.panel.target_y = (0x200 - G.panel.h) >> 1;
    G.panel.tween.done = 1;
    for (int i = 0; i < FB_BLINKS; i++) anim_define(&G.blinks[i].anim, kBlinkFrames, 5, 10, 0);

    button_init(&G.play, "button_play");
    button_init(&G.scores, "button_score");
    button_init(&G.rate, "button_rate");
    button_init(&G.pause, "button_pause");
    G.background = G.img_bg_day;

    int pipe_w = spr_w(G.img_pipe_up);
    G.pipe_spacing = (0x120 - (pipe_w * 3) / 2) / 2;
    G.pipe_x[0] = G.pipe_spacing - (pipe_w >> 1);
    G.pipe_x[1] = G.pipe_x[0] + G.pipe_spacing + pipe_w;
    G.pipe_x[2] = pipe_w + G.pipe_x[1] + G.pipe_spacing;
    G.pipe_y[0] = G.pipe_y[1] = G.pipe_y[2] = 0x112;

    FBBird *b = &G.bird;
    b->w = b->h = 20;
    b->ox = b->oy = 14;
    anim_define(&b->flap, kBirdFrames, 12, 30, 0);
    anim_define(&b->glide, kBirdFrames, 12, 10, 1);
    b->hovering = 1;
    b->colour = libc_rand() % 3;
    bird_reset();

    G.ready.tween.done = 1;
    G.over.tween.done = 1;
    G.title = 1;
    timer_add(6, 0, 1);
    fade_start(0, 0, 0.5f);     /* the engine starts fading in before the first frame */
}

int game_init(float panel_w, float panel_h) {
    uint32_t size = 0;
    (void)panel_h;
    ready_flag = 0;
    if (sprites_load()) return -1;
    memset(&G, 0, sizeof G);
    /* the engine sizes the shake by how much the canvas is scaled to the screen width */
    G.screen_scale = panel_w / 288.0f;
    unsigned char *d = plat_read_file("score.dat", &size, 1);
    if (d && size == 8 && read32(d) < 100000000u) {
        G.best = (int)read32(d);
        G.plays = (int)read32(d + 4);
    }
    free(d);
    best_saved = G.best;
    fb_start(plat_seed());
    ready_flag = 1;
    return 0;
}

void game_frame(float dt, const PlatTouches *touches) {
    static int was_down;
    (void)dt;                   /* the original advances one fixed step per displayed frame */
    if (!ready_flag) return;
    int n = touches ? touches->count : 0;
    if (n > 2) n = 2;
    G.touch_count = n;
    for (int i = 0; i < n; i++) {
        G.touch_x[i] = (int)touches->pt[i].x;
        G.touch_y[i] = (int)touches->pt[i].y;
    }
    /* iOS delivers the touch before the frame is drawn */
    if (n > 0 && !was_down) touch_began(G.touch_x[0], G.touch_y[0]);
    was_down = n > 0;
    spr_frame_begin();
    engine_frame();
    G.frames++;
}

/* The same picture again, without advancing the game. */
void game_redraw(void) {
    if (!ready_flag) return;
    spr_frame_replay();
    gfx_set_frame_offset((float)G.shake_x / G.screen_scale, (float)G.shake_y / G.screen_scale);
}

void game_gl_lost(void) { gfx_forget_all(); sprites_forget(); ready_flag = 0; }
uint32_t game_bg_color(void) { return 0; }
int game_wants_exit(void) { return 0; }
void game_debug_dump(const char *names) {
    (void)names;
    plat_log("frame=%u title=%d hovering=%d y=%d vy=%.3f rot=%.1f speed=%d score=%d best=%d pipes=%d,%d %d,%d %d,%d "
             "delay=%d over=%d panel=%d/%d medal=%d colour=%d bg=%s",
             (unsigned)G.frames, G.title, G.bird.hovering, G.bird.y, (double)G.bird.vy, (double)G.bird.rotation,
             G.speed, G.score, G.best, G.pipe_x[0], G.pipe_y[0], G.pipe_x[1], G.pipe_y[1], G.pipe_x[2], G.pipe_y[2],
             G.start_delay, G.over.active, G.panel.active, G.panel.state, G.medal.active ? G.medal.index : -1,
             G.bird.colour, G.background == G.img_bg_night ? "night" : "day");
}

#ifndef AB_NANO
/* Test-only pilot: flaps when the bird would otherwise drop below the middle of the next
 * gap. It only supplies taps; it never changes the simulation. */
int game_test_autopilot(void) {
    const FBBird *b = &G.bird;
    if (G.title || G.speed < 1) return 0;
    if (b->hovering) return G.ready.active && G.ready.state == 1;
    int limit = 256;            /* stay near the starting height until pipes arrive */
    if (G.start_delay < 1) {
        int i = G.pipe_x[0] + spr_w(G.img_pipe_up) < b->x ? 1 : 0;
        limit = G.pipe_y[i] - 30;       /* the bird's top must stay above pipe_y - 20 */
    }
    return (float)b->y + b->vy + b->gravity > (float)limit;
}
#endif
