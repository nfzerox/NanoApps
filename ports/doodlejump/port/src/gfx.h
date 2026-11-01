/* gfx.h — the small 2D renderer the engine draws through (OpenGL ES 1.1).
 *
 * Coordinates are logical game pixels (origin top-left, y down). The platform maps the
 * logical screen onto the panel in gfx_begin_frame. Quads are batched per texture. */
#ifndef AB_GFX_H
#define AB_GFX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void gfx_set_texture_budget(uint32_t bytes);

enum { GFX_FMT_RGB565 = 0, GFX_FMT_RGBA4444 = 1, GFX_FMT_RGBA8888 = 2 };
enum { GFX_BLEND_ALPHA = 0, GFX_BLEND_ADD = 1, GFX_BLEND_NONE = 2 };

/* Textures. Returns 0 on failure (0 is also "no texture" for solid drawing). */
int  gfx_tex_create(int w, int h, int fmt, const void *pixels, int linear, int repeat);
void gfx_tex_destroy(int tex);
/* GL objects did not survive (new GL view): forget every texture without deleting. */
void gfx_forget_all(void);

/* Frame. logical_w/h is the game's screen size; rot90 draws it rotated onto a portrait
 * panel (logical x runs down the panel, as the other NanoApps landscape games do). */
void gfx_begin_frame(int panel_w, int panel_h, float logical_w, float logical_h, int rot90,
                     float r, float g, float b);
void gfx_end_frame(void);

/* Clip rectangle in logical pixels; w <= 0 disables clipping. */
void gfx_set_clip(float x, float y, float w, float h);
void gfx_set_blend(int mode);

/* A quad: four corners in order top-left, top-right, bottom-right, bottom-left, with
 * texture coordinates in texels of `tex` (tex_w/tex_h are the texture's size). tex 0
 * draws a solid colour. argb is 0xAARRGGBB with premultiplied alpha (blending is
 * GL_ONE, GL_ONE_MINUS_SRC_ALPHA, as in the original). */
void gfx_quad(int tex, int tex_w, int tex_h, const float xy[8], const float uv[8], uint32_t argb);
void gfx_tri(const float xy[6], uint32_t argb);
void gfx_flush(void);

/* Statistics for the last finished frame. */
extern int gfx_stat_draws, gfx_stat_quads, gfx_stat_textures;
extern uint32_t gfx_stat_tex_bytes;
extern uint32_t gfx_stat_upload_us;
extern uint32_t gfx_stat_buffer_us;
extern int gfx_stat_merges, gfx_stat_state_calls; /* per-frame draw merges and batch-state GL calls */

#ifdef __cplusplus
}
#endif

#endif
