#ifndef FB_SPRITES_H
#define FB_SPRITES_H
#include <stdint.h>
/* Images are numbered in atlas.txt order, as in the original engine: animation frames,
 * digits and medals are found as "first image with this prefix" plus an offset. */
int  sprites_load(void);
void sprites_forget(void);
int  spr_find(const char *prefix);                 /* first image whose name starts so; -1 */
int  spr_w(int id);
int  spr_h(int id);
/* The image stretched over the rectangle (x0,y0)-(x1,y1), canvas pixels, y down. */
void spr_rect(int id, int x0, int y0, int x1, int y1, float alpha);
/* The image over any quad: corners top-left, top-right, bottom-right, bottom-left. */
void spr_quad(int id, const float xy[8], float alpha);
/* A solid rectangle: grey level 0 or 1, premultiplied by alpha. */
void spr_fill(int x0, int y0, int x1, int y1, int white, float alpha);
/* Start remembering a game step's drawing / draw the remembered step again. */
void spr_frame_begin(void);
void spr_frame_replay(void);
#endif
