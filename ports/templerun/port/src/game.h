/* game.h — the port's shell around the translated engine. */
#ifndef TR_GAME_H
#define TR_GAME_H
#include <stdint.h>

/* Loads the engine and its data for a display of the given size in pixels; 0 on failure. */
/* Before game_init: draw (or, with draw 0, only clear) a frame of the loading screen. */
void game_splash(int panel_w, int panel_h, int draw);
int game_init(int panel_w, int panel_h, uint32_t heap_bytes, uint32_t seed);
/* One displayed frame: advance by dt seconds and draw. */
void game_frame(float dt);
/* A finger at (x, y) in pixels from the top left: phase 0 down, 1 moved, 2 up. */
void game_touch(int phase, float x, float y);
/* Tilt: the sideways part of gravity, -1 (left edge down) to 1. */
void game_tilt(float x);

/* With drawing off a frame only clears the screen (the engine still advances). */
void game_set_drawing(int on);
/* Until the scene is ready a frame shows a loading screen (the logo) instead of the game. */
void game_set_scene_ready(int ready);
/* Let the shell play by itself (for soak tests and demonstrations). */
void game_autopilot(int on);

enum { GAME_TITLE, GAME_RUNNING, GAME_OVER };
int game_state(void);
int game_best(void);                    /* the best score on record */
int game_score(void);
int game_coins(void);
int game_distance(void);
#endif
