#ifndef FB_GAME_H
#define FB_GAME_H
#include <stdint.h>
/* The original's objects, flattened into one fixed structure: nothing is allocated while
 * the game runs. Field comments give the offset in the original object where it helps. */
#define FB_BLINKS 10
typedef struct { int total, cur; float inv, value, t, start, end, delta; int type, done; } FBTween;
typedef struct { int index, timer, duration, count, current, finished, loop; const int *frames; } FBAnim;
typedef struct {
    int x, y, w, h, ox, oy, visible;             /* +0x30.. : a 20x20 box inside the 48x48 picture */
    FBAnim flap, glide, *playing;
    float rotation, turn, turn_accel;            /* +0x60 degrees, +0x64, +0x68 */
    float vy, gravity, hover;                    /* +0x70, +0x74, +0x80 */
    int dead, hovering, phase, colour;           /* +0x78, +0x79, +0x7c, +0x84 */
} FBBird;
typedef struct { int image, x, y, w, h, active, visible, pressed, just_pressed, released; } FBButton;
typedef struct { int active, visible, x, y, w, h, index, timer; } FBMedal;
typedef struct { int active, visible, x, y; FBAnim anim; } FBBlink;
typedef struct { int active, visible, x, y, w, h, shown, best, state, score, is_new, target_y; FBTween tween; } FBPanel;
typedef struct { int active, visible, state; FBTween tween; } FBReady;
typedef struct { int active, visible, state, offset; float vy, accel; FBTween tween; } FBOver;
typedef struct {
    /* engine */
    int timer_ms[50], timer_id[50], timer_arg[50], timer_next;
    FBTween fade, flash;
    int fade_id, shake_x, shake_y, shake_amp, shake_frames;
    uint32_t z, w, rand_next, frames;
    float screen_scale;
    int touch_count, touch_x[2], touch_y[2];
    /* game */
    int score, best, plays, title, game_over;    /* +0x538, +0x53c, +0x540, +0x550, +0x551 */
    int background, land_x, speed, start_delay;  /* +0x57c, +0x5a0, +0x5d0, +0x5c0 */
    int pipe_x[3], pipe_y[3], pipe_spacing;      /* +0x5b0.., +0x5a4.., +0x5bc */
    FBBird bird;
    FBButton play, scores, rate, pause;
    FBPanel panel;
    FBMedal medal;
    FBBlink blinks[FB_BLINKS];
    int blink_next;
    FBReady ready;
    FBOver over;
    int img_bg_day, img_bg_night, img_land, img_pipe_up, img_pipe_down, img_title, img_copyright, img_bird,
        img_font, img_number, img_medal, img_blink, img_panel, img_new, img_ready, img_tutorial, img_game_over;
} FBGame;
extern FBGame G;
void fb_start(uint32_t seed);
/* Headless test driver only; not bound to anything on the device. */
int game_test_autopilot(void);
extern void (*fb_event_hook)(int id, int value);
#endif
