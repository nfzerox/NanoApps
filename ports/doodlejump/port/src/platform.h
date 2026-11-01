#ifndef DJ_PLATFORM_H
#define DJ_PLATFORM_H
#include <stdint.h>
#define port_checkpoint(stage,detail,value) ((void)0)
uint64_t plat_time_us(void);
void *plat_read_file(const char *path, uint32_t *size, int save);
int plat_write_file(const char *path, const void *data, uint32_t size);
void plat_log(const char *fmt, ...);
float plat_tilt_x(void); /* gravity units, positive = move right */
float plat_audio_play(const char *, float, int, int);
void plat_audio_stop(const char *);
void plat_audio_track_volume(int, float);
void plat_audio_clip_volume(const char *, float);
typedef struct { int count; struct { float x,y; } pt[2]; } PlatTouches;
int game_init(float w, float h);
void game_frame(float dt, const PlatTouches *touches);
void game_gl_lost(void);
uint32_t game_bg_color(void);
int game_wants_exit(void);
void game_debug_dump(const char *names);
#endif
