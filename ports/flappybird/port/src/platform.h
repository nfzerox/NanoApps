#ifndef FB_PLATFORM_H
#define FB_PLATFORM_H
#include <stdint.h>
#define port_checkpoint(stage,detail,value) ((void)0)
uint64_t plat_time_us(void);
void *plat_read_file(const char *path, uint32_t *size, int save);
int plat_write_file(const char *path, const void *data, uint32_t size);
void plat_log(const char *fmt, ...);
/* The original's five effects (events 9..13 of its engine). */
enum { FB_SND_POINT, FB_SND_SWOOSH, FB_SND_DIE, FB_SND_HIT, FB_SND_WING, FB_SND_COUNT };
void plat_sound(int sound, float gain);
/* Seed for the pipe-height generator (the original uses the wall clock). */
uint32_t plat_seed(void);

/* Touches in the game's 288x512 canvas. */
typedef struct { int count; struct { float x, y; } pt[2]; } PlatTouches;
int game_init(float w, float h);
void game_frame(float dt, const PlatTouches *touches);
void game_redraw(void);          /* draw the last step again (call instead of game_frame) */
void game_gl_lost(void);
uint32_t game_bg_color(void);
int game_wants_exit(void);
void game_debug_dump(const char *names);
#endif
