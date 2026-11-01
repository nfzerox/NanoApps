/* Arithmetic order follows the original routines. Motion tables preserve the
 * float results of their finite, double-precision sine phases on the nano. */
#include "objects.h"
#include "motion_tables.h"

/* -[PlatformObject tick] @ 0xa094 */
void platform_tick(DJObject *o) {
    if(o->fading)o->alpha=(float)((double)o->alpha-.1);
    if(o->vx) {
        if(!o->falling&&(o->x>292||o->x<28))o->vx=-o->vx;
        o->x+=o->vx;
    }
    if(o->falling) {
        if(++o->age==50)o->fading=1;
        if(o->texture<5)o->texture++;
        o->y-=3.5f;
    }
}

/* -[MonsterObject tick] @ 0xf830 (port types 7..9 = original types 0..2). */
void monster_tick(DJObject *o) {
    if(o->type!=9) {
        float x=monster_x[o->range_x],y=monster_y[o->range_y];
        o->x=(x-o->offset_x)+o->x;o->y=(y-o->offset_y)+o->y;
        o->offset_x=x;o->offset_y=y;
        if(++o->range_x>=157)o->range_x=0;
        if(++o->range_y>=126)o->range_y=0;
    } else {
        float y=monster_fly_y[o->range_y];
        if(o->x>292||o->x<28) {
            o->vx=-o->vx;if(++o->texture==4)o->texture=2;
        }
        o->x+=o->vx;o->y=(y-o->offset_y)+o->y;o->offset_y=y;
        if(++o->range_y>=42)o->range_y=0;
    }
}

/* -[UfoObject tick] @ 0xabec */
void ufo_tick(DJObject *o,unsigned random_value) {
    float x=ufo_x[o->range_x],y=ufo_y[o->range_y];
    o->texture=random_value%3!=0;
    o->x=(x-o->offset_x)+o->x;o->y=(y-o->offset_y)+o->y;
    o->offset_x=x;o->offset_y=y;
    if(++o->range_x>=315)o->range_x=0;
    if(++o->range_y>=158)o->range_y=0;
}
/* -[BonusObject tick] @ 0xa290 */
void bonus_tick(DJBonus *b) {
    if(!b->moving)return;
    float centre=b->x-b->offset;
    if(centre>292||centre<28)b->speed=-b->speed;
    b->x+=b->speed;
}
/* -[ProjectileObject tick] @ 0xa9a4 (init sets objSpeed = 18) */
void projectile_tick(DJShot *s){s->y+=18;}
