/* r3d.c — see r3d.h. */
#include <stddef.h>
#include <string.h>
#include "gl.h"
#include "platform.h"
#include "r3d.h"

#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif

#ifdef AB_NANO
extern void port_crumb(const char *tag, uint32_t a, uint32_t b);   /* RAM trail that survives a reboot */
#define CRUMB(tag, a) port_crumb(tag, (uint32_t)(a), 0)
#else
#define CRUMB(tag, a) ((void)0)
#endif

#define MAX_MESH_VERTS 2048
#define BATCHES        6
/* One pool of vertices per frame, handed out in chunks to the texture pairs; nothing in it
 * is overwritten until the next frame, and it is uploaded to a buffer object before the
 * draws (a pattern verified on the nano). */
#define CHUNK_VERTS    1536             /* a multiple of 3 */
#define CHUNKS         9
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
/* Everything is clipped here, a little inside the view volume GL would clip to, so the
 * driver never clips (its own clipping of a corner that sits on a boundary can leave the
 * zero-area sliver that reboots the iPod). The side planes are inset by 1/1024 of the
 * half-screen (a ninth of a pixel on the nano's 240-pixel width). */
#define INSET          0.9990234375f    /* 1 - 1/1024 */
static float sNearW = 1.015625f, sFarW = 393.75f;       /* set from the projection in r3d_begin */

/* Working vertex: clip-space position, texture coordinates, and the world position (sent
 * instead of the clip position in R3D_WORLD mode). */
typedef struct { float x, y, z, w, u0, v0, u1, v1, px, py, pz; } Vtx;
typedef struct { float x, y, z, w, u0, v0, u1, v1; uint32_t color; } OutVtx;   /* what GL is given */
typedef struct { unsigned tex0, tex1; int used, chunks, chunk[CHUNKS], last; } Batch;   /* last: vertices in the newest chunk */

int r3d_stat_draws, r3d_stat_vertices_in, r3d_stat_triangles_in, r3d_stat_triangles_out, r3d_stat_tiny,
    r3d_stat_clipped, r3d_stat_outside;
static Batch sBatch[BATCHES];
static OutVtx sPool[CHUNKS * CHUNK_VERTS];
/* What the nano's driver was found to accept (see analysis/nano-gl-notes.md): clip-space
 * positions in client arrays, with a colour array, in draws of at most 384 vertices. */
static int sMode = R3D_SMALL | R3D_COLOR;
static float sProjection[16], sView[16];
void r3d_set_mode(int mode) { sMode = mode; }
/* With two texture units the driver reboots on small triangles: an area guard of 1/8 pixel
 * crashed in the first frame every time, 2 square pixels ran. (Twice the area is compared.) */
static float sMinArea2 = 4.0f, sInset = 0.9990234375f;
void r3d_set_guard(float min_area2, float inset) { sMinArea2 = min_area2; sInset = inset; }
static int sChunksUsed;
static GLuint sVbo;
int r3d_stat_dropped;
static float sViewProj[16], sHalfW, sHalfH;
static Vtx sClip[MAX_MESH_VERTS];
static float sSX[MAX_MESH_VERTS], sSY[MAX_MESH_VERTS];
static unsigned char sCode[MAX_MESH_VERTS];

static void multiply(float out[16], const float a[16], const float b[16]) {      /* out = a * b */
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
}

void r3d_init(void) {
    for (int i = 0; i < BATCHES; i++) sBatch[i].used = 0;
    sChunksUsed = 0;
    sVbo = 0;                           /* a new GL view has none of our objects */
}

void r3d_begin(int panel_w, int panel_h, const float projection[16], const float view[16]) {
    multiply(sViewProj, projection, view);
    memcpy(sProjection, projection, sizeof sProjection);
    memcpy(sView, view, sizeof sView);
    /* a perspective matrix: z_clip = A*z_eye + B, w = -z_eye; near = B/(A-1), far = B/(A+1) */
    float A = projection[10], B = projection[14];
    sNearW = B / (A - 1.f) * 1.015625f;
    sFarW = B / (A + 1.f) * 0.984375f;
    sHalfW = (float)panel_w * 0.5f;
    sHalfH = (float)panel_h * 0.5f;
    r3d_stat_draws = r3d_stat_vertices_in = r3d_stat_triangles_in = r3d_stat_triangles_out = 0;
    r3d_stat_tiny = r3d_stat_clipped = r3d_stat_dropped = 0;
    for (int i = 0; i < BATCHES; i++) sBatch[i].used = 0;
    sChunksUsed = 0;
}

static void draw_batch(const Batch *b, int index) {
    CRUMB("batch", (uint32_t)(index << 16) | (uint32_t)(b->tex1 ? 2 : 1));
    glActiveTexture(GL_TEXTURE1);
    glClientActiveTexture(GL_TEXTURE1);
    if (b->tex1) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, b->tex1);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    } else {
        glDisable(GL_TEXTURE_2D);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    CRUMB("unit1", b->tex1);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, b->tex0);
    CRUMB("unit0", b->tex0);
    for (int k = 0; k < b->chunks; k++) {
        int n = k + 1 == b->chunks ? b->last : CHUNK_VERTS;
        if (!n) continue;
        int step = (sMode & R3D_SMALL) ? 384 : CHUNK_VERTS;
        if ((sMode & R3D_EACH) && index == 1) step = 3;      /* one triangle a call, to find a bad one */
        for (int first = 0; first < n; first += step) {
            int count = n - first < step ? n - first : step;
            CRUMB("arrays", (uint32_t)(b->chunk[k] << 16) | (uint32_t)(step == 3 ? first : count));
            glDrawArrays(GL_TRIANGLES, b->chunk[k] * CHUNK_VERTS + first, count);
            r3d_stat_draws++;
        }
        CRUMB("drew", (uint32_t)(b->chunk[k] << 16) | (uint32_t)n);
        r3d_stat_triangles_out += n / 3;
    }
}

static Batch *batch_for(unsigned tex0, unsigned tex1) {
    Batch *free_one = NULL;
    for (int i = 0; i < BATCHES; i++) {
        Batch *b = &sBatch[i];
        if (b->used && b->tex0 == tex0 && b->tex1 == tex1) return b;
        if (!b->used && !free_one) free_one = b;
    }
    if (!free_one) return NULL;         /* more texture pairs in a frame than BATCHES */
    free_one->used = 1;
    free_one->tex0 = tex0;
    free_one->tex1 = tex1;
    free_one->chunks = 0;
    free_one->last = CHUNK_VERTS;       /* forces a first chunk */
    return free_one;
}

/* Room for one more triangle in the batch, or NULL when the frame's pool is full. */
static OutVtx *reserve(Batch *b) {
    if (b->last + 3 > CHUNK_VERTS) {
        if (sChunksUsed == CHUNKS) { r3d_stat_dropped++; return NULL; }
        b->chunk[b->chunks++] = sChunksUsed++;
        b->last = 0;
    }
    OutVtx *v = &sPool[b->chunk[b->chunks - 1] * CHUNK_VERTS + b->last];
    b->last += 3;
    return v;
}

/* One triangle wholly inside the view volume: cull, guard, emit. */
static void emit(Batch *bt, const Vtx *a, const Vtx *b, const Vtx *c,
                 float ax, float ay, float bx, float by, float cx, float cy, int cull) {
    float area2 = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);       /* pixels, y up */
    if (cull && area2 >= 0.f) return;                                   /* front faces are clockwise */
    if (area2 < 0.f) area2 = -area2;
    if (!(area2 >= sMinArea2)) { r3d_stat_tiny++; return; }             /* also rejects NaN */
    OutVtx *v = reserve(bt);
    if (!v) return;
    const Vtx *in[3] = { a, b, c };
    for (int k = 0; k < 3; k++) {
        const Vtx *p = in[k];
        if (sMode & R3D_WORLD) { v[k].x = p->px; v[k].y = p->py; v[k].z = p->pz; v[k].w = 1.f; }
        else { v[k].x = p->x; v[k].y = p->y; v[k].z = p->z; v[k].w = p->w; }
        v[k].u0 = p->u0; v[k].v0 = p->v0; v[k].u1 = p->u1; v[k].v1 = p->v1;
        v[k].color = 0xffffffffu;
    }
}

/* Signed distance to clip plane `bit` (>= 0 is inside). */
static float plane(const Vtx *v, int bit) {
    switch (bit) {
    case 0: return v->x + v->w * sInset;
    case 1: return v->w * sInset - v->x;
    case 2: return v->y + v->w * sInset;
    case 3: return v->w * sInset - v->y;
    case 4: return v->w - sNearW;
    default: return sFarW - v->w;
    }
}

static unsigned char outcode(const Vtx *v) {
    float e = v->w * sInset;
    unsigned char code = 0;
    if (v->x < -e) code |= 1;
    if (v->x > e) code |= 2;
    if (v->y < -e) code |= 4;
    if (v->y > e) code |= 8;
    if (v->w < sNearW) code |= 16;
    if (v->w > sFarW) code |= 32;
    return code;
}

/* Sutherland-Hodgman against one plane; returns the new vertex count. */
static int clip_plane(const Vtx *in, int n, Vtx *out, int bit) {
    int count = 0;
    for (int k = 0; k < n; k++) {
        const Vtx *p = &in[k], *q = &in[(k + 1) % n];
        float dp = plane(p, bit), dq = plane(q, bit);
        if (dp >= 0.f) out[count++] = *p;
        if ((dp >= 0.f) != (dq >= 0.f)) {
            float t = dp / (dp - dq);
            Vtx *o = &out[count++];
            o->x = p->x + (q->x - p->x) * t;  o->y = p->y + (q->y - p->y) * t;
            o->z = p->z + (q->z - p->z) * t;  o->w = p->w + (q->w - p->w) * t;
            o->u0 = p->u0 + (q->u0 - p->u0) * t;  o->v0 = p->v0 + (q->v0 - p->v0) * t;
            o->u1 = p->u1 + (q->u1 - p->u1) * t;  o->v1 = p->v1 + (q->v1 - p->v1) * t;
            o->px = p->px + (q->px - p->px) * t;  o->py = p->py + (q->py - p->py) * t;
            o->pz = p->pz + (q->pz - p->pz) * t;
        }
    }
    return count;
}

void r3d_mesh(const TRMesh *m, int frame, const float model[16], unsigned tex0, unsigned tex1, int cull) {
    const TRFrame *f = &m->frames[frame];
    int n = f->vertex_count;
    if (n > MAX_MESH_VERTS || !tex0) return;
    float mvp[16];
    multiply(mvp, sViewProj, model);
    const unsigned char *src = m->vertices + (size_t)f->vertex_start * (size_t)m->vertex_size;
    int two = tex1 && m->uv_count > 1;
    if (!two) tex1 = 0;
    unsigned char all = 0xff;
    for (int i = 0; i < n; i++, src += m->vertex_size) {
        const float *p = (const float *)(const void *)(src + m->pos_offset);
        const float *t0 = (const float *)(const void *)(src + m->uv_offset[0]);
        Vtx *o = &sClip[i];
        float x = p[0], y = p[1], z = p[2];
        o->x = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
        o->y = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
        o->z = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14];
        o->w = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
        o->px = model[0] * x + model[4] * y + model[8] * z + model[12];
        o->py = model[1] * x + model[5] * y + model[9] * z + model[13];
        o->pz = model[2] * x + model[6] * y + model[10] * z + model[14];
        o->u0 = t0[0]; o->v0 = t0[1];
        if (two) {
            const float *t1 = (const float *)(const void *)(src + m->uv_offset[1]);
            o->u1 = t1[0]; o->v1 = t1[1];
        } else {
            o->u1 = o->v1 = 0.f;
        }
        unsigned char code = outcode(o);
        sCode[i] = code;
        all &= code;
        if (!code) {
            float inv = 1.f / o->w;
            sSX[i] = o->x * inv * sHalfW;
            sSY[i] = o->y * inv * sHalfH;
        }
    }
    r3d_stat_vertices_in += n;
    r3d_stat_triangles_in += f->index_count / 3;
    if (all) return;                                    /* the whole mesh is off one side */
    Batch *b = batch_for(tex0, tex1);
    if (!b) return;
    const uint16_t *idx = m->indices + f->index_start;
    for (int i = 0; i + 2 < f->index_count; i += 3) {
        unsigned ia = idx[i], ib = idx[i + 1], ic = idx[i + 2];
        if (ia >= (unsigned)n || ib >= (unsigned)n || ic >= (unsigned)n) continue;
        unsigned char ca = sCode[ia], cb = sCode[ib], cc = sCode[ic];
        if (ca & cb & cc) continue;                     /* off one side of the view */
        unsigned char any = ca | cb | cc;
        if (!any) {
            emit(b, &sClip[ia], &sClip[ib], &sClip[ic], sSX[ia], sSY[ia], sSX[ib], sSY[ib], sSX[ic], sSY[ic], cull);
            continue;
        }
        /* crosses the edge of the view: clip to every plane it crosses */
        Vtx bufa[10], bufb[10], *in = bufa, *out = bufb;
        int count = 3;
        in[0] = sClip[ia]; in[1] = sClip[ib]; in[2] = sClip[ic];
        for (int bit = 0; bit < 6 && count >= 3; bit++) {
            if (!(any & (1 << bit))) continue;
            count = clip_plane(in, count, out, bit);
            Vtx *t = in; in = out; out = t;
        }
        r3d_stat_clipped++;
        if (count < 3) continue;
        float sx[10], sy[10];
        for (int k = 0; k < count; k++) {
            float inv = 1.f / in[k].w;
            sx[k] = in[k].x * inv * sHalfW;
            sy[k] = in[k].y * inv * sHalfH;
        }
        for (int k = 1; k + 1 < count; k++)
            emit(b, &in[0], &in[k], &in[k + 1], sx[0], sy[0], sx[k], sy[k], sx[k + 1], sy[k + 1], cull);
    }
}

void r3d_end(void) {
    if (!sChunksUsed) return;
    glMatrixMode(GL_PROJECTION);
    if (sMode & R3D_WORLD) glLoadMatrixf(sProjection); else glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    if (sMode & R3D_WORLD) glLoadMatrixf(sView); else glLoadIdentity();
    glDisable(GL_CULL_FACE);                            /* culled above */
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glColor4f(1.f, 1.f, 1.f, 1.f);
#ifndef AB_NANO
    /* host check: nothing handed to GL may touch a clip boundary */
    if (!(sMode & R3D_WORLD))
        for (int i = 0; i < BATCHES; i++) {
            const Batch *bt = &sBatch[i];
            for (int k = 0; bt->used && k < bt->chunks; k++) {
                int n = k + 1 == bt->chunks ? bt->last : CHUNK_VERTS;
                for (int j = 0; j < n; j++) {
                    const OutVtx *v = &sPool[bt->chunk[k] * CHUNK_VERTS + j];
                    if (!(v->w > 0.f && v->x > -v->w && v->x < v->w && v->y > -v->w && v->y < v->w && v->z > -v->w && v->z < v->w))
                        r3d_stat_outside++;
                }
            }
        }
#endif
    const unsigned char *base = (const unsigned char *)sPool;
    if (sMode & R3D_VBO) {
        if (!sVbo) glGenBuffers(1, &sVbo);
        if (!sVbo) { plat_log("r3d: no vertex buffer"); return; }
        CRUMB("buffer", sChunksUsed);
        glBindBuffer(GL_ARRAY_BUFFER, sVbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)sChunksUsed * CHUNK_VERTS * sizeof(OutVtx)), sPool, GL_DYNAMIC_DRAW);
        base = NULL;
    }
    int components = (sMode & R3D_WORLD) ? 3 : 4;
    glVertexPointer(components, GL_FLOAT, sizeof(OutVtx), base + offsetof(OutVtx, x));
    if (sMode & R3D_COLOR) {
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(OutVtx), base + offsetof(OutVtx, color));
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
    }
    glActiveTexture(GL_TEXTURE1);
    glClientActiveTexture(GL_TEXTURE1);
    glTexCoordPointer(2, GL_FLOAT, sizeof(OutVtx), base + offsetof(OutVtx, u1));
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, sizeof(OutVtx), base + offsetof(OutVtx, u0));
    for (int i = 0; i < BATCHES; i++) {
        int k = (sMode & R3D_REVERSE) ? BATCHES - 1 - i : i;
        if (sBatch[k].used) draw_batch(&sBatch[k], k);
    }
    CRUMB("drawn-all", 0);
    glActiveTexture(GL_TEXTURE1);
    glClientActiveTexture(GL_TEXTURE1);
    glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glDisableClientState(GL_COLOR_ARRAY);
    if (sMode & R3D_VBO) glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (sMode & R3D_WORLD) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
    }
    CRUMB("ended", r3d_stat_triangles_out);
}
