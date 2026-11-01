/* Doodle Jump 1.0 gameplay, rebuilt from the named Objective-C methods.
 * See analysis/port-notes.md for evidence and deliberate platform differences. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "platform.h"
#include "sprites.h"
#include "gfx.h"
#include "objects.h"

DJGame G;
typedef struct { int16_t type,x,y; } SceneObject;
static struct { unsigned count; SceneObject objects[64]; } scenes[19];
static int ready,exit_requested,tap_pending;
static float tap_x,tap_y;
static int fade_phase,fade_ticks,pressed_button,tracking_button;
static unsigned clock_ticks;
static double clock_remaining;
static float minimum(float a,float b){return a<b?a:b;}
static float maximum(float a,float b){return a>b?a:b;}
static float clamp(float x,float lo,float hi){return maximum(lo,minimum(x,hi));}
static uint32_t random_u32(void) {
    return dj_random(&G.random);
}
static unsigned choose(unsigned n){return n?random_u32()%n:0;}
static void effect(const char *name){plat_audio_play(name,1,0,0);}
static int ufo_count(void) {
    int count=0;for(int i=0;i<G.object_count;i++)count+=G.objects[i].active&&G.objects[i].type==5;
    return count;
}
static int intersects(float x,float y,float w,float h,float a,float b,float c,float d) {
    return x<a+c && x+w>a && y<b+d && y+h>b;
}
static int contains(float x,float y,float w,float h,float a,float b) {
    return a>=x && a<x+w && b>=y && b<y+h;
}
static uint32_t read32(const unsigned char *p) {
    return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
int dj_load_scenes(void) {
    uint32_t size=0,off=8;unsigned i,j;
    unsigned char *d=plat_read_file("scenes.bin",&size,0);
    if(!d||size<8||memcmp(d,"DJS1",4)||read32(d+4)!=19){free(d);return -1;}
    for(i=0;i<19;i++) {
        if(off+4>size){free(d);return -1;}
        unsigned n=read32(d+off);off+=4;
        if(!n||n>64||off+6*n>size){free(d);return -1;}
        scenes[i].count=n;
        for(j=0;j<n;j++,off+=6) {
            SceneObject *s=scenes[i].objects+j;
            s->type=d[off]|d[off+1]<<8;s->x=d[off+2]|d[off+3]<<8;s->y=d[off+4]|d[off+5]<<8;
            if(s->type>9||s->type<0||s->x<0||s->x>320||s->y<0||s->y>2000){free(d);return -1;}
        }
    }
    free(d);return off==size?0:-1;
}
static DJObject *object(int type,float x,float y) {
    int i;
    if(G.object_count==DJ_MAX_OBJECTS) {
        int n=0;
        for(i=0;i<G.object_count;i++)if(G.objects[i].active) {
            if(G.last_platform==i)G.last_platform=n;
            if(G.target==i)G.target=n;
            if(G.hole_target==i)G.hole_target=n;
            if(G.ufo_target==i)G.ufo_target=n;
            G.objects[n++]=G.objects[i];
        }
        memset(G.objects+n,0,(DJ_MAX_OBJECTS-n)*sizeof(DJObject));G.object_count=n;
    }
    if(G.object_count<DJ_MAX_OBJECTS) {
        i=G.object_count++;
        DJObject *o=G.objects+i;memset(o,0,sizeof *o);
        o->active=1;o->type=type;o->x=x;o->y=y;o->alpha=1;
        if(type<=3)G.last_platform=i;
        if(type==1)o->vx=1;
        if(type>=7){o->vx=2;o->texture=type-7;}
        else if(type<=3)o->texture=type==3?6:type;
        return o;
    }
    return NULL;
}
static void spring(DJObject *o) {
    if(!o)return;
    o->spring=1;o->spring_offset=(float)choose(39)-19.5f;
    if(G.bonus_count==64) {
        int n=0;for(int i=0;i<64;i++)if(G.bonuses[i].active)G.bonuses[n++]=G.bonuses[i];
        G.bonus_count=n;
    }
    if(G.bonus_count<64) {
        DJBonus *b=G.bonuses+G.bonus_count++;memset(b,0,sizeof *b);
        b->active=1;b->x=o->x+o->spring_offset;b->y=o->y+17.5f;
        b->offset=o->spring_offset;b->speed=o->vx;b->moving=o->vx!=0;
    }
}
static void generate_platform(void) {
    DJObject previous=G.objects[G.last_platform];
    int moving=G.scroll>5500 && choose(100)<G.scroll/400.f;
    /* The original consumes the speed random number even for static platforms. */
    float speed=minimum((float)(.5+.05*choose(10)+(double)(G.scroll/30000.f)),2.5f);
    int type=choose(5)==0 && previous.type!=2 ? 2 : moving?1:0;
    float x=28.5f+choose(263);
    float gap=minimum(25.f+choose((unsigned)((150.f+G.scroll)/150.f)),90.f);
    if(previous.type==2)gap=minimum(gap,50);
    if(previous.spring) {
        gap+=15;
        if(G.bonus_count && G.bonuses[G.bonus_count-1].type==0)gap+=minimum(30.f+choose((unsigned)((50.f+G.scroll)/50.f)),70.f);
    }
    DJObject *o=object(type,x,previous.y+gap);
    if(o) {o->vx=moving?speed:0;if(choose(100)>90 && type!=2)spring(o);}
}
static void generate_scene(void) {
    unsigned i,s=G.scroll<15000?14+choose(5):choose(14);
    float base=G.objects[G.last_platform].y;
    for(i=0;i<scenes[s].count;i++) {
        SceneObject *r=scenes[s].objects+i;
        int first_ufo=r->type==5 && !ufo_count();
        DJObject *o=object(r->type==6?0:r->type,r->x,(r->y+base)+10);
        if(o && first_ufo)plat_audio_play("ufo.wav",1,1,0);
        if(o && r->type==1)o->vx=minimum((float)(.5+.05*choose(10)+(double)(G.scroll/30000.f)),2.5f);
        if(r->type==6)spring(o);
    }
    G.scene_count++;
}
static void move_screen(float offset,int score) {
    int i;
    if(score){G.scroll+=offset;G.score=(int)((float)G.score+offset);}
    else {G.down+=offset;G.background+=offset;}
    for(i=0;i<DJ_MAX_OBJECTS;i++)if(G.objects[i].active) {
        DJObject *o=G.objects+i;o->y-=offset;
        /* Original removal is performed below, one oldest object per list. */
    }
    for(i=0;i<G.bonus_count;i++)if(G.bonuses[i].active)G.bonuses[i].y-=offset;
    for(i=0;i<DJ_MAX_SHOTS;i++)if(G.shots[i].active)G.shots[i].y-=offset;
    for(int category=0;category<4;category++) {
        for(i=0;i<G.object_count;i++) {
            DJObject *o=G.objects+i;
            int c=o->type<4?0:o->type==4?1:o->type==5?2:3;
            if(!o->active||c!=category)continue;
            float cutoff=category==2?-65:category==3?-30:-40;
            if(o->y<cutoff && i!=G.target) {
                o->active=0;
                if(category==2 && !ufo_count())plat_audio_stop("ufo.wav");
                if(category==0)for(int j=0;j<G.bonus_count;j++)if(G.bonuses[j].active) {
                    if(G.bonuses[j].y< -12)G.bonuses[j].active=0;
                    break;
                }
            }
            break;
        }
    }
    if(G.objects[G.last_platform].y<500) {
        if(G.scroll>5000 && choose(20)==10)generate_scene();else generate_platform();
    }
}
static void save_best(void) {
    if(G.score>G.best) {
        uint32_t record[3]={0x31534a44u,(uint32_t)G.score,(uint32_t)G.score^0x5a1743c9u};
        G.best=G.score;
        if(plat_write_file("best.bin",record,sizeof record))plat_log("Could not save high score");
    }
}
static void finish(void){save_best();G.state=DJ_OVER;G.elapsed=0;}
static void start_game(DJRandom rng) {
    int best=G.best,i,direction=G.direction;float last_x=G.last_x;double accel=G.accel;
    memset(&G,0,sizeof G);G.best=best;G.random=rng;G.accel=accel;
    G.state=DJ_PLAY;G.x=150;G.y=25;G.vy=9;G.player_w=46;G.player_h=59;
    G.target=G.hole_target=G.ufo_target=-1;G.direction=direction;G.shot_age=21;G.last_x=last_x;
    for(i=0;i<21;i++){object(0,28.5f+choose(263),25.f*i);random_u32();}
}
void dj_start(uint32_t seed) {
    DJRandom rng;dj_srandom(&rng,seed);start_game(rng);
    fade_phase=0;fade_ticks=0;
}
void dj_shoot(void) {
    int i;
    if(G.state!=DJ_PLAY || G.death!=DJ_ALIVE || G.player_frozen)return;
    for(i=0;i<DJ_MAX_SHOTS;i++)if(!G.shots[i].active) {
        effect(choose(2)?"pucanje2.wav":"pucanje.wav");
        G.shots[i].active=1;G.shots[i].x=G.x;G.shots[i].y=G.y+10;
        G.shot_age=0;G.shots_fired++;return;
    }
}
static void capture(int i,int death) {
    effect(death==DJ_HOLE?"crnarupa.wav":"usaugateufo.wav");
    DJObject *o=G.objects+i;
    if(death==DJ_HOLE){G.hit_hole=1;G.hole_target=i;}
    else {G.hit_ufo=1;G.ufo_target=i;}
    G.target=G.hit_hole?G.hole_target:G.ufo_target;
    G.death=G.hit_hole?DJ_HOLE:DJ_UFO;
    G.capture_frames=death==DJ_HOLE?12:40;
    float frames=(float)G.capture_frames;
    G.capture_dx=(o->x-G.x)/frames;
    G.capture_dy=(o->y+(death==DJ_UFO?35.f:0)-G.y)/frames;
}
/* The original removes hit objects after each entire category's scan. This lets
 * a projectile strike overlapping enemies in the same category in one frame. */
static void projectile_hits(int ufos) {
    unsigned char killed[DJ_MAX_OBJECTS]={0},spent[DJ_MAX_SHOTS]={0};
    int original_ufos=ufo_count();
    for(int i=0;i<G.object_count;i++) {
        DJObject *o=G.objects+i;
        if(!o->active || (ufos?o->type!=5:o->type<7))continue;
        float w=ufos?76:o->type==9?26:28;
        float h=ufos?28:o->type==7?24:o->type==8?18:30;
        float y=ufos?o->y+33:o->y-h*.5f;
        for(int j=0;j<DJ_MAX_SHOTS;j++)if(G.shots[j].active &&
            contains(o->x-w*.5f,y,w,h,G.shots[j].x,G.shots[j].y)) {
            effect(ufos?"ufopogodak.wav":"monsterpogodak.wav");
            killed[i]=1;spent[j]=1;
            if(ufos && original_ufos==1)plat_audio_stop("ufo.wav");
            if(ufos && i==G.ufo_target && G.hit_ufo) {
                G.hit_ufo=0;G.ufo_target=-1;
                G.death=G.hit_hole?DJ_HOLE:DJ_ALIVE;G.target=G.hit_hole?G.hole_target:-1;
                G.player_w=46;G.player_h=59;
            }
        }
    }
    for(int i=0;i<G.object_count;i++)if(killed[i]){G.objects[i].active=0;G.kills++;}
    for(int i=0;i<DJ_MAX_SHOTS;i++)if(spent[i])G.shots[i].active=0;
}
/* -[JumpAppDelegate checkForCollisions], 0x4f6c. The two-pixel feet strip
 * and narrow collision boxes are intentional; artwork bounds are larger. */
void dj_collisions(void) {
    int i;
    float px=G.x-15,py=G.y-27;
    if(G.vy<0) {
        for(i=0;i<DJ_MAX_OBJECTS;i++) {
            DJObject *o=G.objects+i;
            if(!o->active||o->type>3)continue;
            if(intersects(px,py,28,2,o->x-25.5f,o->y,52,7.5f)) {
                if(o->type==2) {if(!o->falling){effect("lomise.wav");o->falling=1;o->age=0;G.broken++;}}
                else if(o->type!=3 || o->alpha==1) {
                    effect("jump.wav");G.vy=9;G.landings++;
                    if(o->type==3){effect("bijeli.wav");o->fading=1;}
                }
            }
        }
    }
    if(G.vy<0) {
        for(i=0;i<G.bonus_count;i++) {
            DJBonus *b=G.bonuses+i;
            if(b->active && intersects(px,py,28,2,b->x-9,b->y-(b->type?14:10),18,b->type?28:20)) {
                if(b->type==0){effect("feder.wav");G.vy=15.5f;b->type=1;G.spring_hits++;}
                break;
            }
        }
    }
    projectile_hits(1);
    if(!G.hit_ufo)for(i=0;i<G.object_count;i++) {
        DJObject *o=G.objects+i;
        if(o->active && o->type==5 && intersects(px,py,28,35,o->x-35,o->y-26,70,37))capture(i,DJ_UFO);
    }
    projectile_hits(0);
    int already_hit=G.was_hit_monster;
    for(i=0;i<DJ_MAX_OBJECTS;i++) {
        DJObject *o=G.objects+i;if(!o->active)continue;
        if(o->type>=7 && !already_hit) {
            float w=o->type==9?26:28,h=o->type==7?24:o->type==8?18:30;
            if(intersects(px,py,28,35,o->x-w*.5f,o->y-h*.5f,w,h)) {
                effect("monstersudar.wav");
                if(G.death==DJ_ALIVE)G.death=DJ_MONSTER;
                G.was_hit_monster=1;G.vy=0;
            }
        }
    }
    if(!G.hit_hole)for(i=0;i<G.object_count;i++) {
        DJObject *o=G.objects+i;
        if(o->active && o->type==4 && intersects(px,py,28,35,o->x-10,o->y-10,20,20))capture(i,DJ_HOLE);
    }
}
static void tick_objects(void) {
    int i;
    for(i=0;i<DJ_MAX_OBJECTS;i++) {
        DJObject *o=G.objects+i;if(!o->active)continue;
        if(o->type<=3) platform_tick(o);
        else if(o->type==5) ufo_tick(o,random_u32());
        else if(o->type>=7) monster_tick(o);
        if(o->alpha<.05f || (o->falling&&o->y< -40))o->active=0;
    }
    for(i=0;i<G.bonus_count;i++) {
        DJBonus *b=G.bonuses+i;if(b->active)bonus_tick(b);
    }
    for(i=0;i<DJ_MAX_SHOTS;i++)if(G.shots[i].active) {
        projectile_tick(G.shots+i);if(G.shots[i].y>500)G.shots[i].active=0;
    }
}
/* -[JumpAppDelegate mainGameLoop], 0x630c, runs at 60 simulation ticks/s. */
void dj_accelerometer(double tilt) {G.accel=.06*tilt+.94*G.accel;}
void dj_tick(float tilt) {
    (void)tilt; /* Acceleration is filtered by the independent 100 Hz callback. */
    G.ticks++;
    if(G.state!=DJ_PLAY)return;
    if(G.death==DJ_SCROLL_END) {
        if(G.down<=-975)finish();else move_screen(-12,0);
    } else if(G.death!=DJ_HOLE && G.death!=DJ_UFO && !G.player_frozen) {
        G.x=(float)((double)G.x+20.0*G.accel);
        if(G.x>320)G.x=0;
        if(G.x<0)G.x=320;
        G.y+=G.vy;G.vy=maximum((float)((double)G.vy-.24),-9);
        if(G.death==DJ_ALIVE&&G.vy>=0&&G.y>230) {
            float delta=G.y-230;G.y=230;move_screen(delta,1);
        }
        if(G.y<10 && G.death!=DJ_FALL) {
            effect("pada.wav");plat_audio_stop("ufo.wav");
            float offset=10+G.vy-G.y-18;
            G.death=DJ_FALL;G.vy=-2;G.fall_speed=-18;G.y=28;G.fall_offset=0;
            move_screen(offset,0);
        } else if(G.death==DJ_FALL) {
            G.fall_speed=minimum((float)((double)G.fall_speed+.3),0);G.y-=G.fall_speed;
            if(G.fall_speed!=0)move_screen(G.vy+G.fall_speed,0);
            else if(G.y<=-30)finish();
            else {G.fall_offset-=2;G.y+=G.fall_offset;}
        }
        if(__builtin_fabsf(G.last_x-G.x)>=4) {G.direction=G.last_x<G.x?1:-1;G.last_x=G.x;}
        if(G.shot_age<21)G.shot_age++;
        G.jump_texture=G.vy>=8.5f;
    }
    tick_objects();
    if((G.death==DJ_ALIVE || G.death==DJ_UFO || G.death==DJ_HOLE) && !G.was_hit_monster)dj_collisions();
    if(G.death==DJ_HOLE || G.death==DJ_UFO) {
        if(G.capture_frames==0) {
            if(G.death==DJ_UFO)plat_audio_stop("ufo.wav");
            G.death=DJ_SCROLL_END;G.down=-455;
        }
        else {
            G.player_w=(float)((double)G.player_w-(G.death==DJ_HOLE?2.5:.8));
            G.player_h=(float)((double)G.player_h-(G.death==DJ_HOLE?2.5:.8));
            DJObject *target=G.objects+G.target;
            G.x=target->x-G.capture_frames*G.capture_dx;
            G.y=target->y+(G.death==DJ_UFO?35:0)-G.capture_frames*G.capture_dy;
            G.capture_frames--;
        }
    }
    if(G.was_hit_monster)G.stars_ticks++;
    if(G.background< -480)G.background+=480;
}
static void menu(void) {
    int best=G.best;DJRandom rng=G.random;double accel=G.accel;
    memset(&G,0,sizeof G);G.best=best;G.random=rng;G.accel=accel;
    G.state=DJ_MENU;G.x=60;G.y=25;G.vy=8;G.direction=1;G.shot_age=21;
    plat_audio_stop("ufo.wav");
    object(0,60,98);object(5,250,400);object(4,255,220);
}
static void draw_objects(void) {
    int i;
    static const char *normal[]={"platform1","platform2","slomljeni0","platform4"};
    for(int pass=0;pass<5;pass++) {
    if(pass==2)for(i=0;i<G.bonus_count;i++) {
        DJBonus *b=G.bonuses+i;if(b->active)sprite(b->type?"feder-gore":"feder-dolje",b->x,b->y,1,1);
    }
    for(i=0;i<DJ_MAX_OBJECTS;i++) {
        DJObject *o=G.objects+i;const char *s=NULL;
        if(!o->active||o->y< -80||o->y>570)continue;
        int p=o->type==4?0:o->type<4?1:o->type==5?3:4;if(p!=pass)continue;
        if(o->type<=3) {
            s=normal[o->type];
            if(o->falling)s=o->texture==3?"slomljeni1":o->texture==4?"slomljeni2":"slomljeni3";
        } else if(o->type==4)s="rupa";
        else if(o->type==5)s=o->texture?"ufo1":"ufo0";
        else if(o->type==7)s="monster1";
        else if(o->type==8)s="monster2";
        /* textureMonster[2] is monster3-r.png and [3] monster3-l.png in the original. */
        else if(o->type==9)s=o->texture==2?"monster3-r":"monster3-l";
        if(s)sprite(s,o->x,o->y,1,o->alpha);
    }
    }
    for(i=0;i<DJ_MAX_SHOTS;i++)if(G.shots[i].active)sprite("metak",G.shots[i].x,G.shots[i].y,1,1);
}
static void draw_player(void) {
    int jump=G.jump_texture;
    const char *s=G.shot_age<21?(jump?"lik-puca-odskok":"lik-puca"):
        G.direction>0?(jump?"lik-right-odskok":"lik-right"):(jump?"lik-left-odskok":"lik-left");
    if(G.death==DJ_SCROLL_END)return;
    if(G.death==DJ_HOLE||G.death==DJ_UFO)
        sprite_rect(s,G.x,G.y,G.player_w,G.player_h,1);
    else sprite(s,G.x,G.y,1,1);
    if(G.was_hit_monster) {
        const char *stars[]={"stars1","stars2","stars3"};
        sprite(stars[(G.stars_ticks/3)%3],G.x,G.y+14,1,1);
    }
}
static void draw_gameover(void) {
    if(G.down>=-560)return;
    sprite("game-over",150,-600-G.down,1,1);
    sprite("your-score",108,-700-G.down,1,1);
    draw_score(G.score,169,-677-G.down);
    draw_score(G.best,192,-716-G.down);
    sprite(pressed_button==3?"play-again-on":"play-again",160,-800-G.down,1,1);
    sprite("game-over-podrapano",160,-953-G.down,1,1);
}
static void render(void) {
    gfx_set_clip(0,0,320,480);
    if(G.death==DJ_FALL||G.death==DJ_SCROLL_END) {
        float offset=G.background;
        sprite("bck",160,240-offset,1,1);sprite("bck",160,-240-offset,1,1);
    } else sprite("bck",160,240,1,1);
    if(G.state==DJ_MENU) {
        sprite("doodle-jump",114,414,1,1);
        sprite("game-over-podrapano",160,22,1,1);
        sprite("platform1",G.objects[0].x,G.objects[0].y,1,1);
        sprite(G.objects[1].texture?"ufo1":"ufo0",G.objects[1].x,G.objects[1].y,1,1);
        sprite("rupa",G.objects[2].x,G.objects[2].y,1,1);
        sprite("buba1",195,315,1,1);sprite("buba2",40,275,1,1);
        sprite("buba3",60,450,1,1);sprite("buba4",215,147,1,1);sprite("buba5",135,195,1,1);
        draw_player();
        sprite(pressed_button==1?"play-on":"play",105.5f,333,1,1);
        sprite(pressed_button==2?"scores-on":"scores",138,272,1,1);
    } else if(G.state==DJ_PLAY || G.state==DJ_OVER) {
        draw_gameover();
        if(G.state==DJ_PLAY){draw_objects();draw_player();}
        sprite("top-score",160,457,1,1);draw_score(G.score,10,463);
    } else {
        /* UIKit's original score-list frame, with an explicit offline service
         * adaptation: the nano has no interface to the iOS online leaderboard. */
        sprite("high-scores-top",160,420.5f,1,1);
        sprite("high-scores-left",29,261.5f,1,1);
        sprite("high-scores-bottom",160,81,1,1);
        sprite("local-score-label",178,310,1,1);draw_score(G.best,140,270);
        sprite("offline-label",178,205,1,1);
        sprite(pressed_button==4?"menu-on":"menu",230,87,1,1);
    }
    if(fade_phase) {
        /* UIKit's default ease-in/ease-out timing, cubic-bezier(.42,0,.58,1). */
        float t=fade_ticks/(fade_phase==1?90.f:60.f),lo=0,hi=1;
        for(int i=0;i<12;i++) {
            float u=(lo+hi)*.5f,v=1-u,x=3*.42f*v*v*u+3*.58f*v*u*u+u*u*u;
            if(x<t)lo=u;else hi=u;
        }
        float u=(lo+hi)*.5f,alpha=3*(1-u)*u*u+u*u*u;
        if(fade_phase==2)alpha=1-alpha;
        float xy[8]={0,0,320,0,320,480,0,480},uv[8]={0};
        gfx_quad(0,1,1,xy,uv,(uint32_t)(alpha*255.f)<<24);
    }
    gfx_set_clip(0,0,0,0);
}
int game_init(float w,float h) {
    uint32_t size=0;unsigned char *d;
    (void)w;(void)h;ready=0;exit_requested=0;tap_pending=0;
    if(sprites_load()||dj_load_scenes())return -1;
    G.best=0;d=plat_read_file("best.bin",&size,1);
    if(d&&size==12&&read32(d)==0x31534a44u&&read32(d+4)<100000000u&&
       (read32(d+4)^0x5a1743c9u)==read32(d+8))G.best=(int)read32(d+4);
    free(d);dj_srandom(&G.random,(uint32_t)plat_time_us());
    fade_phase=fade_ticks=pressed_button=tracking_button=0;clock_ticks=0;clock_remaining=0;
    menu();ready=1;return 0;
}
static int button_at(float x,float y) {
    if(fade_phase)return 0;
    if(G.state==DJ_MENU) {
        if(contains(50,127,111,40,x,y))return 1;
        if(contains(82,188,112,40,x,y))return 2;
    } else if(G.state==DJ_OVER) {
        if(contains(104,1280+G.down-20.5f,112,41,x,y))return 3;
    } else if(G.state==DJ_SCORES) {
        if(contains(165,363,130,60,x,y))return 4;
    }
    return 0;
}
void game_frame(float dt,const PlatTouches *touches) {
    int touched=touches&&touches->count>0;
    if(!ready)return;
    if(touched) {tap_x=touches->pt[0].x;tap_y=touches->pt[0].y;}
    if(touched&&!G.touch_down)tracking_button=button_at(tap_x,tap_y);
    if(touched)pressed_button=tracking_button==button_at(tap_x,tap_y)?tracking_button:0;
    if(!touched&&G.touch_down)tap_pending=1;
    G.touch_down=touched;
    if(tap_pending) {
        tap_pending=0;
        if(pressed_button==1||pressed_button==3){fade_phase=1;fade_ticks=0;}
        else if(pressed_button==2)G.state=DJ_SCORES;
        else if(pressed_button==4)menu();
        else if(G.state==DJ_PLAY && !fade_phase && tap_y>=0&&tap_y<=480)dj_shoot();
        pressed_button=tracking_button=0;
        G.touch_down=touched;
    }
    clock_remaining+=clamp(dt,0,.1f);
    while(clock_remaining>=1.0/300.0) {
        clock_remaining-=1.0/300.0;clock_ticks++;
        if(clock_ticks%3==0)dj_accelerometer(plat_tilt_x());
        if(fade_phase) {
            fade_ticks++;
            if(fade_phase==1 && fade_ticks==90) {
                start_game(G.random);dj_tick(0);G.player_frozen=1;
                fade_phase=2;fade_ticks=0;
            } else if(fade_phase==2 && fade_ticks==60) {
                fade_phase=0;G.player_frozen=0;
            }
        }
        if(clock_ticks%5 || fade_phase==1)continue;
        if(G.state==DJ_MENU) {
            G.y+=G.vy;G.vy=(float)((double)G.vy-.24);
            G.jump_texture=G.vy>6.5f;
            if(G.vy<0 && intersects(G.x-15,G.y-27,28,2,31.5f,98,57,7.5f)){G.vy=7;effect("jump.wav");}
            tick_objects();G.ticks++;
        } else dj_tick(0);
    }
    render();
}
void game_gl_lost(void){gfx_forget_all();sprites_forget();ready=0;}
uint32_t game_bg_color(void){return 0xe9e7d1;}
int game_wants_exit(void){return exit_requested;}
void game_debug_dump(const char *names) {
    int i,n=0;(void)names;for(i=0;i<DJ_MAX_OBJECTS;i++)n+=!!G.objects[i].active;
    plat_log("state=%d death=%d x=%.2f y=%.2f vy=%.2f score=%d objects=%d landings=%d springs=%d broken=%d shots=%d kills=%d scenes=%d",
        G.state,G.death,(double)G.x,(double)G.y,(double)G.vy,G.score,n,G.landings,G.spring_hits,G.broken,G.shots_fired,G.kills,G.scene_count);
}
#ifndef AB_NANO
void game_test_command(const char *name,float a,float b) {
    if(!strcmp(name,"start"))dj_start((uint32_t)a);
    else if(!strcmp(name,"shoot"))dj_shoot();
    else if(!strcmp(name,"difficulty")){G.scroll=a;G.score=(int)a;}
    else if(!strcmp(name,"scene"))generate_scene();
    else if(!strcmp(name,"generate"))generate_platform();
    else if(!strcmp(name,"player")){G.x=a;G.y=b;}
}
/* Test-only autopilot chooses a reachable platform and supplies tilt input.
 * It never changes the simulation state or forces landings. */
float game_test_autotilt(void) {
    int i;DJObject *best=NULL;float cost=1e9f;
    for(i=0;i<DJ_MAX_OBJECTS;i++) {
        DJObject *o=G.objects+i;if(!o->active||o->type>3||o->type==2||o->fading)continue;
        float feet=G.y-27;
        if(o->y>feet+maximum(0,G.vy)*maximum(0,G.vy)/(2*.24f)-8 || o->y<0)continue;
        if(G.vy<0&&o->y>feet-3)continue;
        float c=fabsf(o->x-G.x)*.35f-o->y;
        if(c<cost){cost=c;best=o;}
    }
    if(!best)return 0;
    return clamp((best->x-G.x)*.022f-G.accel*2.f,-.7f,.7f);
}
#endif
