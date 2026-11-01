#ifndef DJ_SPRITES_H
#define DJ_SPRITES_H
int sprites_load(void);
void sprites_forget(void);
void sprite(const char *name, float x, float y, float scale, float alpha);
void sprite_rect(const char *name, float x, float y, float width, float height, float alpha);
void sprite_size(const char *name, float *w, float *h);
void draw_number(int value, float x, float y, int centered);
void draw_score(int value, float base_x, float y);
#endif
