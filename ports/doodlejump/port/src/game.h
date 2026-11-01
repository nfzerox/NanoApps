#ifndef DJ_GAME_H
#define DJ_GAME_H
#include <stdint.h>
#include "random.h"
/* Fixed-capacity objects avoid allocation, fragmentation and unbounded growth
 * during play. Coordinates and per-tick speeds use the original y-up units. */
#define DJ_MAX_OBJECTS 192
#define DJ_MAX_SHOTS 24
enum { DJ_MENU, DJ_PLAY, DJ_OVER, DJ_SCORES };
enum { DJ_ALIVE, DJ_FALL, DJ_MONSTER, DJ_HOLE, DJ_UFO, DJ_SCROLL_END };
typedef struct {
    float x,y,vx,alpha,offset_x,offset_y,spring_offset;
    int type,active,age,falling,fading,spring,spring_used,range_x,range_y,texture;
} DJObject;
typedef struct { float x,y; int active; } DJShot;
typedef struct { float x,y,speed,offset; int active,type,moving; } DJBonus;
typedef struct {
    DJObject objects[DJ_MAX_OBJECTS]; DJShot shots[DJ_MAX_SHOTS];
    DJBonus bonuses[64];int bonus_count;
    double accel;
    DJRandom random;
    float x,y,vy,scroll,down,fall_speed,fall_offset,last_x,player_w,player_h;
    float elapsed,capture_dx,capture_dy,background;
    uint32_t ticks;
    int object_count,capture_frames,was_hit_monster,stars_ticks,player_frozen;
    int hit_hole,hit_ufo,hole_target,ufo_target;
    int state,death,score,best,last_platform,target,shot_age,direction,touch_down,jump_texture;
    int landings,spring_hits,broken,shots_fired,kills,scene_count;
} DJGame;
extern DJGame G;
void dj_start(uint32_t seed);
void dj_tick(float tilt);
void dj_accelerometer(double tilt);
void dj_shoot(void);
void dj_collisions(void);
int dj_load_scenes(void);
/* Headless test driver only; these are not bound to controls on the device. */
void game_test_command(const char *name,float a,float b);
float game_test_autotilt(void);
#endif
