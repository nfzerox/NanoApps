#ifndef DJ_OBJECTS_H
#define DJ_OBJECTS_H
#include "game.h"
void platform_tick(DJObject *o);
void monster_tick(DJObject *o);
void ufo_tick(DJObject *o,unsigned random_value);
void bonus_tick(DJBonus *b);
void projectile_tick(DJShot *s);
#endif
