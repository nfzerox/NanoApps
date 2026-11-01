/* Narrow C ABI for comparisons with original ARM methods. Not linked into apps. */
#include <string.h>
#include "game.h"
void probe_start(unsigned seed){memset(&G,0,sizeof G);dj_start(seed);}
int probe_count(void){return G.object_count;}
void probe_object(int i,float *out){DJObject *o=G.objects+i;out[0]=o->x;out[1]=o->y;out[2]=o->type;}
void probe_collision(int type,float x,float y,float vy,int projectile) {
    memset(&G,0,sizeof G);G.state=DJ_PLAY;G.x=x;G.y=y;G.vy=vy;G.player_w=46;G.player_h=59;
    G.target=-1;G.object_count=1;
    DJObject *o=G.objects;o->active=1;o->type=type;o->x=150;o->y=200;o->alpha=1;
    if(projectile){G.shots[0].active=1;G.shots[0].x=x;G.shots[0].y=y;G.x=20;G.y=400;}
    dj_collisions();
}
void probe_result(float *out) {
    out[0]=G.vy;out[1]=G.death;out[2]=G.capture_frames;out[3]=G.objects[0].falling;
    out[4]=G.objects[0].fading;out[5]=G.objects[0].active;out[6]=G.shots[0].active;
    out[7]=G.capture_dx;out[8]=G.capture_dy;
}

void probe_generate(float height,int count) {G.scroll=height;for(int i=0;i<count;i++)game_test_command("generate",0,0);}
void probe_generated(int i,float *out) {
    DJObject *o=G.objects+i;out[0]=o->x;out[1]=o->y;out[2]=o->type;out[3]=o->vx;out[4]=o->spring;
}
int probe_bonus_count(void){return G.bonus_count;}
void probe_bonus(int i,float *out){DJBonus *b=G.bonuses+i;out[0]=b->x;out[1]=b->y;out[2]=b->speed;out[3]=b->type;}
void probe_tick(float accel){G.accel=accel;dj_tick(accel);}
void probe_frame_state(float *out) {
    out[0]=G.x;out[1]=G.y;out[2]=G.vy;out[3]=G.scroll;out[4]=G.down;out[5]=G.score;
    out[6]=G.death;out[7]=G.state==DJ_OVER;
}

void probe_capture(int type) {
    probe_collision(type,150,200,2,0);
    G.shot_age=21;
    DJObject *p=G.objects+1;memset(p,0,sizeof *p);p->active=1;p->x=20;p->y=1000;p->alpha=1;
    G.object_count=2;G.last_platform=1;
}
void probe_capture_state(float *out) {
    out[0]=G.x;out[1]=G.y;out[2]=G.player_w;out[3]=G.player_h;out[4]=G.capture_frames;out[5]=G.down;
}
int probe_load_scenes(void){return dj_load_scenes();}
void probe_scene(float height){G.scroll=height;game_test_command("scene",0,0);}
void probe_acceleration(double tilt){dj_accelerometer(tilt);}
double probe_acceleration_value(void){return G.accel;}
void probe_acceleration_reset(void){G.accel=0;}
void probe_tick_current(void){dj_tick(0);}
int probe_jump_texture(void){return G.jump_texture;}
int probe_player_texture(void){return G.shot_age<21?1:G.direction>0?2:0;}
void probe_compound(int type1,int type2,float shot_x,float shot_y,int capturing) {
    memset(&G,0,sizeof G);G.state=DJ_PLAY;G.x=150;G.y=200;G.vy=2;G.player_w=46;G.player_h=59;
    G.target=-1;G.object_count=2;
    for(int i=0;i<2;i++) {
        DJObject *o=G.objects+i;o->active=1;o->type=i?type2:type1;o->x=150;o->y=200;o->alpha=1;
    }
    if(capturing){G.death=DJ_UFO;G.target=G.ufo_target=0;G.hit_ufo=1;G.player_w=20;G.player_h=30;}
    if(shot_x>=0){G.shots[0].active=1;G.shots[0].x=shot_x;G.shots[0].y=shot_y;}
    dj_collisions();
}
void probe_compound_result(float *out) {
    out[0]=G.objects[0].active;out[1]=G.objects[1].active;out[2]=G.shots[0].active;
    out[3]=G.vy;out[4]=G.death;out[5]=G.capture_frames;out[6]=G.player_w;out[7]=G.player_h;
    out[8]=G.was_hit_monster;out[9]=G.target;
}
