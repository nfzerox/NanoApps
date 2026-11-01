/* r3d.h — the port's 3D submission layer.
 *
 * The nano's GL driver reboots the iPod on zero-area triangles, which a perspective
 * scene produces now and then: far-off detail shrinking to a point, slivers left
 * by near-plane clipping. Vertices are
 * transformed here, triangles are clipped to the near plane and culled (back-facing,
 * off-screen, under about 1/8 pixel), and only the survivors go to GL, as clip-space
 * positions with identity matrices, batched per texture pair. */
#ifndef TR_R3D_H
#define TR_R3D_H
#include "mesh.h"

/* How the surviving triangles are handed to GL (for finding what the nano's driver accepts). */
enum {
    R3D_WORLD = 1,      /* world-space xyz with GL's own matrices, instead of clip-space xyzw */
    R3D_VBO = 2,        /* upload the frame to a buffer object, instead of client arrays */
    R3D_SMALL = 4,      /* at most 384 vertices per draw call */
    R3D_COLOR = 8,      /* send a (white) colour array */
    R3D_REVERSE = 16,   /* draw the texture batches last to first */
    R3D_EACH = 32       /* draw the second batch one triangle per call */
};
void r3d_set_mode(int mode);
/* Smallest screen area kept (twice the area, in pixels) and how far inside the view volume
 * the side planes clip (1 = on the boundary). */
void r3d_set_guard(float min_area2, float inset);
void r3d_init(void);
/* Column-major 4x4 matrices, as OpenGL. */
void r3d_begin(int panel_w, int panel_h, const float projection[16], const float view[16]);
/* tex1 = 0 for a single texture; cull = 1 drops faces turned away (the models' front faces
 * are clockwise). */
void r3d_mesh(const TRMesh *m, int frame, const float model[16], unsigned tex0, unsigned tex1, int cull);
void r3d_end(void);

extern int r3d_stat_draws, r3d_stat_vertices_in, r3d_stat_triangles_in, r3d_stat_triangles_out,
           r3d_stat_tiny, r3d_stat_clipped;
extern int r3d_stat_dropped;    /* triangles lost because the frame's vertex pool was full */
extern int r3d_stat_outside;    /* host builds: vertices that reached GL on or outside a clip boundary (must stay 0) */
#endif
