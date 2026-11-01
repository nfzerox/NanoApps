/* Same digest on the development host and the final ARM/VFP compiler target.
 * Linux/QEMU's write and exit syscalls are the only ARM host adapter. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "game.h"
#include "objects.h"
#ifdef DJ_ARM_CHECK
static void output(const char *s,unsigned length) {
    register unsigned r0 __asm__("r0")=1;
    register const char *r1 __asm__("r1")=s;
    register unsigned r2 __asm__("r2")=length;
    register unsigned r7 __asm__("r7")=4;
    __asm__ volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r7):"memory");
}
#else
#include <unistd.h>
static void output(const char *s,unsigned length){(void)write(1,s,length);}
#endif
float plat_audio_play(const char *n,float v,int l,int t){(void)n;(void)v;(void)l;(void)t;return 0;}
void plat_audio_stop(const char *n){(void)n;}
void plat_log(const char *n,...){(void)n;}
int plat_write_file(const char *n,const void *d,uint32_t s){(void)n;(void)d;(void)s;return 0;}
static uint32_t hash=2166136261u;
static void word(uint32_t v){for(int i=0;i<4;i++){hash^=v&255;hash*=16777619u;v>>=8;}}
static void number(float f){uint32_t v;memcpy(&v,&f,4);word(v);}
static void object_state(DJObject *o){number(o->x);number(o->y);number(o->alpha);word(o->texture);}
int main(void) {
    for(int kind=0;kind<4;kind++) {
        DJObject o={0};o.type=kind;o.x=293;o.y=300;o.vx=1.25;o.alpha=1;o.falling=kind==2;o.fading=kind==3;
        for(int i=0;i<600;i++){platform_tick(&o);object_state(&o);}
    }
    for(int kind=7;kind<=9;kind++) {
        DJObject o={0};o.type=kind;o.x=293;o.y=300;o.vx=2;o.alpha=1;o.texture=kind-7;
        for(int i=0;i<1800;i++){monster_tick(&o);object_state(&o);}
    }
    DJObject u={0};u.x=200;u.y=300;
    for(int i=0;i<1800;i++){ufo_tick(&u,i);object_state(&u);}
    dj_start(42);G.accel=0;
    for(int clock=1;clock<=30000;clock++) {
        if(clock%3==0)dj_accelerometer(((clock/30)%21-10)*.05);
        if(clock%5)continue;
        if(clock%55==0)dj_shoot();
        dj_tick(0);number(G.x);number(G.y);number(G.vy);number(G.scroll);number(G.down);
        word(G.score);word(G.death);word(G.random.state[0]);
        for(int i=0;i<G.object_count;i++)if(G.objects[i].active)object_state(G.objects+i);
        if(G.state==DJ_OVER)dj_start(clock);
    }
    char text[9];for(int i=0;i<8;i++)text[i]="0123456789abcdef"[(hash>>(28-4*i))&15];text[8]='\n';output(text,9);
    return 0;
}
#ifdef DJ_ARM_CHECK
__attribute__((noreturn)) void _start(void) {
    int result=main();register unsigned r0 __asm__("r0")=result,r7 __asm__("r7")=1;
    __asm__ volatile("svc 0"::"r"(r0),"r"(r7):"memory");__builtin_unreachable();
}
#endif
