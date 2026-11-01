/* glfe.c — the engine's OpenGL ES 1.1 calls, drawn through a pipeline the nano's driver accepts.
 *
 * The original draws about two dozen indexed meshes a frame from buffer objects with the
 * fixed-function matrices, two texture units (base and light map), linear fog, and blended
 * sprites for effects and the score display. The nano's driver reboots the iPod on its own
 * clipping, on zero-area triangles, on small triangles when two texture units are on, and
 * on large buffer objects (analysis/nano-gl-notes.md), and it has no fog. So this file keeps
 * the GL state itself and does the transform, clipping, culling and fog on the CPU; GL is
 * given clip-space triangles from client arrays, in draws of at most 384 vertices.
 *
 * Fog without glFog: the fog colour is also the clear colour, so a fogged triangle is drawn
 * blended, with its colour and alpha scaled by the fog factor — what shows through is the
 * background, or geometry farther away that is nearer still to the fog colour. Triangles
 * closer than the fog start are drawn opaque first, the fogged ones after them, then
 * everything the engine draws without fog (effects, the runner, the display) in its order. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl.h"
#include "glfe.h"
#include "platform.h"
#include "rt.h"

#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#define FE_ARRAY_BUFFER 0x8892u
#define FE_ELEMENT_BUFFER 0x8893u
#define FE_MODELVIEW 0x1700u
#define FE_PROJECTION 0x1701u
#define FE_TEXTURE 0x1702u

#ifdef AB_NANO
extern void port_crumb(const char *tag, uint32_t a, uint32_t b);   /* RAM trail that survives a reboot */
#define CRUMB(tag, a) port_crumb(tag, (uint32_t)(a), 0)
#else
#define CRUMB(tag, a) ((void)0)
#endif

unsigned rt_texture_host(uint32_t id);

/* ---- state ------------------------------------------------------------------------------- */
typedef struct { int enabled, size, stride; uint32_t type, pointer, buffer; } Array;
/* A buffer object. The models' vertex buffers (position and one or two texture coordinates,
 * all floats) are kept packed as 16-bit integers with a scale each: half the memory, and the
 * steps are far below a pixel and a texel. Everything else is kept as the engine gave it. */
typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint8_t packed, channels, stride, packed_stride;    /* stride: of the engine's layout */
    float pos_scale, uv_scale[2];
    float lo[3], hi[3];                                 /* packed: a box around every position */
} Buffer;
#define BUFFERS 640
static Buffer sBuffers[BUFFERS];
static uint32_t sNames = 100, sArrayBuffer, sElementBuffer;

static float sModelView[8][16], sProjection[2][16], sTexture[2][2][16];
static int sModelViewTop, sProjectionTop, sTextureTop[2];
static uint32_t sMatrixMode = FE_MODELVIEW;
static int sUnit, sClientUnit;
static int sTexEnabled[2] = { 1, 1 };
static uint32_t sTexBound[2];
static Array sVertexArray, sColorArray, sTexCoordArray[2];
static float sColor[4] = { 1, 1, 1, 1 };
static float sClear[4] = { 0.196078f, 0.321569f, 0.337255f, 1.0f };
static int sFog, sBlend, sDepthTest = 1, sDepthMask = 1, sCull = 1;
static float sFogStart = 100.0f, sFogEnd = 400.0f;

static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

static float *current(void) {
    if (sMatrixMode == FE_PROJECTION) return sProjection[sProjectionTop];
    if (sMatrixMode == FE_TEXTURE) return sTexture[sUnit][sTextureTop[sUnit]];
    return sModelView[sModelViewTop];
}

static void multiply(float out[16], const float a[16], const float b[16]) {      /* out = a * b */
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            t[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    memcpy(out, t, sizeof t);
}

void fe_matrix_mode(uint32_t mode) { sMatrixMode = mode; }
void fe_load_identity(void) { memcpy(current(), kIdentity, sizeof kIdentity); }
void fe_mult_matrix(const float *m) { float t[16]; memcpy(t, m, sizeof t); multiply(current(), current(), t); }
void fe_translate(float x, float y, float z) {
    float *m = current();
    for (int r = 0; r < 4; r++) m[12 + r] += m[r] * x + m[4 + r] * y + m[8 + r] * z;
}
void fe_push_matrix(void) {
    if (sMatrixMode == FE_PROJECTION) { if (sProjectionTop < 1) { memcpy(sProjection[1], sProjection[0], 64); sProjectionTop = 1; } }
    else if (sMatrixMode == FE_TEXTURE) { if (sTextureTop[sUnit] < 1) { memcpy(sTexture[sUnit][1], sTexture[sUnit][0], 64); sTextureTop[sUnit] = 1; } }
    else if (sModelViewTop < 7) { memcpy(sModelView[sModelViewTop + 1], sModelView[sModelViewTop], 64); sModelViewTop++; }
}
void fe_pop_matrix(void) {
    if (sMatrixMode == FE_PROJECTION) { if (sProjectionTop) sProjectionTop--; }
    else if (sMatrixMode == FE_TEXTURE) { if (sTextureTop[sUnit]) sTextureTop[sUnit]--; }
    else if (sModelViewTop) sModelViewTop--;
}
void fe_frustum(float l, float r, float b, float t, float n, float f) {
    float m[16] = { 2 * n / (r - l), 0, 0, 0, 0, 2 * n / (t - b), 0, 0,
                    (r + l) / (r - l), (t + b) / (t - b), -(f + n) / (f - n), -1, 0, 0, -2 * f * n / (f - n), 0 };
    multiply(current(), current(), m);
}
void fe_ortho(float l, float r, float b, float t, float n, float f) {
    float m[16] = { 2 / (r - l), 0, 0, 0, 0, 2 / (t - b), 0, 0, 0, 0, -2 / (f - n), 0,
                    -(r + l) / (r - l), -(t + b) / (t - b), -(f + n) / (f - n), 1 };
    multiply(current(), current(), m);
}

void fe_active_texture(uint32_t unit) { sUnit = unit == GL_TEXTURE1; }
void fe_client_active_texture(uint32_t unit) { sClientUnit = unit == GL_TEXTURE1; }
void fe_bind_texture(uint32_t target, uint32_t id) { (void)target; sTexBound[sUnit] = id; }
void fe_enable(uint32_t cap, int on) {
    switch (cap) {
    case 0x0de1: sTexEnabled[sUnit] = on; break;
    case 0x0b60: sFog = on; break;
    case 0x0be2: sBlend = on; break;
    case 0x0b71: sDepthTest = on; break;
    case 0x0b44: sCull = on; break;
    default: break;
    }
}
void fe_client_state(uint32_t array, int on) {
    if (array == 0x8074) sVertexArray.enabled = on;
    else if (array == 0x8076) sColorArray.enabled = on;
    else if (array == 0x8078) sTexCoordArray[sClientUnit].enabled = on;
}
void fe_clear_color(float r, float g, float b, float a) { sClear[0] = r; sClear[1] = g; sClear[2] = b; sClear[3] = a; }
void fe_color(float r, float g, float b, float a) { sColor[0] = r; sColor[1] = g; sColor[2] = b; sColor[3] = a; }
void fe_depth_mask(int on) { sDepthMask = on; }
void fe_fogf(uint32_t name, float value) { if (name == 0x0b63) sFogStart = value; else if (name == 0x0b64) sFogEnd = value; }
void fe_fogfv(uint32_t name, const float *values) { (void)name; (void)values; }
void fe_hint(uint32_t target, uint32_t mode) { (void)target; (void)mode; }

static void set_array(Array *a, int size, uint32_t type, int stride, uint32_t pointer) {
    a->size = size; a->type = type; a->stride = stride; a->pointer = pointer; a->buffer = sArrayBuffer;
}
void fe_vertex_pointer(int size, uint32_t type, int stride, uint32_t pointer) { set_array(&sVertexArray, size, type, stride, pointer); }
void fe_color_pointer(int size, uint32_t type, int stride, uint32_t pointer) { set_array(&sColorArray, size, type, stride, pointer); }
void fe_texcoord_pointer(int size, uint32_t type, int stride, uint32_t pointer) { set_array(&sTexCoordArray[sClientUnit], size, type, stride, pointer); }
void fe_normal_pointer(uint32_t type, int stride, uint32_t pointer) { (void)type; (void)stride; (void)pointer; }

/* ---- buffer objects: kept here; the engine's own block is used when it can be held --------- */
void fe_gen_buffers(uint32_t count, uint32_t *names) { for (uint32_t i = 0; i < count; i++) names[i] = ++sNames; }
static Buffer *buffer(uint32_t name) { return name > 100 && name - 100 < BUFFERS ? &sBuffers[name - 100] : NULL; }
static uint32_t sMesh;
void fe_note_mesh(uint32_t mesh) { sMesh = mesh; }
unsigned fe_buffer_bytes;               /* memory held by buffer objects */
static void buffer_release(Buffer *b) {
    if (b->data) { fe_buffer_bytes -= b->packed ? b->size / b->stride * b->packed_stride : b->size; free((void *)b->data); }
    memset(b, 0, sizeof *b);
}

static int16_t quantize(float v, float inverse) {
    float q = v * inverse;
    return (int16_t)(q >= 0.f ? q + 0.5f : q - 0.5f);
}

/* Pack a model's vertices if its layout is the plain one: position first, then one or two
 * pairs of texture coordinates, nothing else. */
static int pack_vertices(Buffer *b, const uint8_t *data, uint32_t size) {
    uint32_t mesh = sMesh;
    if (!mesh || M8(mesh + 8) || M8(mesh + 9)) return 0;            /* normals or colours */
    uint32_t stride = M32(mesh + 0x5c), channels = M32(mesh + 0xc), offsets = M32(mesh + 0x6c);
    if (M32(mesh + 0x60) != 0 || channels < 1 || channels > 2 || stride != 12 + 8 * channels || !offsets || size % stride) return 0;
    for (uint32_t c = 0; c < channels; c++) if (M32(offsets + 4 * c) != 12 + 8 * c) return 0;
    uint32_t count = size / stride, packed_stride = 6 + 4 * channels;
    float pos_max = 0.f, uv_max[2] = { 0.f, 0.f };
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < count; i++) {
        float v[7];
        memcpy(v, data + (size_t)i * stride, stride);
        for (int k = 0; k < 3; k++) { if (v[k] < lo[k]) lo[k] = v[k]; if (v[k] > hi[k]) hi[k] = v[k]; }
        for (int k = 0; k < 3; k++) { float a = v[k] < 0.f ? -v[k] : v[k]; if (a > pos_max) pos_max = a; }
        for (uint32_t c = 0; c < channels; c++)
            for (int k = 0; k < 2; k++) { float a = v[3 + 2 * c + k]; if (a < 0.f) a = -a; if (a > uv_max[c]) uv_max[c] = a; }
    }
    if (!(pos_max < 1e6f) || !(uv_max[0] < 1e4f) || !(uv_max[1] < 1e4f)) return 0;     /* also rejects NaN */
    int16_t *out = malloc((size_t)count * packed_stride);
    if (!out) return 0;
    b->pos_scale = pos_max > 0.f ? pos_max / 32767.f : 1.f;
    for (int k = 0; k < 3; k++) { b->lo[k] = lo[k] - b->pos_scale; b->hi[k] = hi[k] + b->pos_scale; }   /* a rounding step wider */
    for (uint32_t c = 0; c < 2; c++) b->uv_scale[c] = uv_max[c] > 0.f ? uv_max[c] / 32767.f : 1.f;
    float ip = 1.f / b->pos_scale, iu[2] = { 1.f / b->uv_scale[0], 1.f / b->uv_scale[1] };
    int16_t *o = out;
    for (uint32_t i = 0; i < count; i++) {
        float v[7];
        memcpy(v, data + (size_t)i * stride, stride);
        *o++ = quantize(v[0], ip); *o++ = quantize(v[1], ip); *o++ = quantize(v[2], ip);
        for (uint32_t c = 0; c < channels; c++) { *o++ = quantize(v[3 + 2 * c], iu[c]); *o++ = quantize(v[4 + 2 * c], iu[c]); }
    }
    b->data = (const uint8_t *)out;
    b->packed = 1; b->channels = (uint8_t)channels; b->stride = (uint8_t)stride; b->packed_stride = (uint8_t)packed_stride;
    fe_buffer_bytes += count * packed_stride;
    return 1;
}
void fe_bind_buffer(uint32_t target, uint32_t name) {
    if (target == FE_ARRAY_BUFFER) sArrayBuffer = name; else if (target == FE_ELEMENT_BUFFER) sElementBuffer = name;
}
void fe_buffer_data(uint32_t target, uint32_t size, const void *data, uint32_t usage) {
    (void)usage;
#ifndef AB_NANO
    if (getenv("TR_GLLOG")) fprintf(stderr, "glBufferData target %x size %u\n", target, size);
#endif
    Buffer *b = buffer(target == FE_ARRAY_BUFFER ? sArrayBuffer : sElementBuffer);
    if (!b) return;
    buffer_release(b);
    b->size = size;
    if (target == FE_ARRAY_BUFFER && data && pack_vertices(b, data, size)) return;
    uint8_t *copy = malloc(size ? size : 1);
    if (!copy) { plat_fatal("no memory for a vertex buffer"); return; }
    if (data) memcpy(copy, data, size); else memset(copy, 0, size);
    b->data = copy;
    fe_buffer_bytes += size;
}
void fe_delete_buffers(uint32_t count, const uint32_t *names) {
#ifndef AB_NANO
    if (getenv("TR_GLLOG")) fprintf(stderr, "glDeleteBuffers %u\n", count);
#endif
    for (uint32_t i = 0; i < count; i++) { Buffer *b = buffer(names[i]); if (b) buffer_release(b); }
}
void fe_delete_textures(uint32_t count, const uint32_t *names) { (void)count; (void)names; }

/* ---- the frame's output ------------------------------------------------------------------ */
typedef struct { float x, y, z, w, u0, v0, u1, v1; uint32_t color; } OutVtx;   /* what GL is given */
/* The frame is kept as indexed triangles: a vertex shared by several triangles is stored and
 * given to GL once (the engine's models share each vertex between about two of the triangles
 * that are kept, and the driver's time goes by the vertex). A chunk is one draw call: up to
 * 384 vertices and the corners of up to 256 triangles, as positions in the pool. */
#define CHUNK_VERTS 384
#define CHUNK_INDICES 768
#define CHUNKS 32
#define BATCHES 48
#define BATCH_CHUNKS 32
enum { OPAQUE, FOGGED, ORDERED };
typedef struct {
    unsigned tex0, tex1;
    uint8_t kind, blend, depth_test, depth_mask, chunks, chunk[BATCH_CHUNKS];
} Batch;
static OutVtx sPool[CHUNKS * CHUNK_VERTS];
static uint16_t sIndex[CHUNKS][CHUNK_INDICES];
static uint16_t sChunkVerts[CHUNKS], sChunkIndices[CHUNKS];
static Batch sBatches[BATCHES];
static int sBatchCount, sChunksUsed, sOrderedOpen = -1;
static float sHalfW = 120.0f, sHalfH = 216.0f;
float fe_ext_min_w = 1e30f, fe_ext_max_w, fe_ext_min_depth = 1e30f, fe_ext_max_depth = -1e30f, fe_ext_max_uv;   /* host: extremes given to GL */
int fe_stat_near;           /* triangles this frame that crossed the near plane */
int fe_stat_slivers;        /* host builds: triangles given to GL that fail the guard (must stay 0) */
int fe_stat_draws, fe_stat_vertices_in, fe_stat_triangles_in, fe_stat_triangles_out, fe_stat_tiny, fe_stat_clipped,
    fe_stat_dropped, fe_stat_calls, fe_stat_fogged, fe_stat_outside;

unsigned fe_time_engine_us;
/* Switches for finding what the nano's driver accepts: draw fogged triangles blended (1) or
 * plain (0); the smallest single-texture triangle kept, as twice its area in pixels. */
int fe_option_fog_blend = 1;
float fe_option_min_area2 = 4.0f;
/* The most triangles a frame may give the driver. It needs memory for every triangle of a
 * frame and the iPod reboots when it has none: the benchmark app found about 3,200 a frame
 * with twice the memory free that this game leaves, and here a frame of 1,507 was drawn and
 * the next, of about 1,550, rebooted it. One frame in seven of the game is over 1,500,
 * and the most seen is 2,300. */
int fe_option_budget = 1300;
int fe_option_box_cull = 1;
int fe_option_indexed = 1;              /* give GL indexed triangles (1) or, as earlier builds did, each triangle's own three vertices (0) */
int fe_stat_box_culled;                 /* this frame: vertices in models left out by their box */
unsigned fe_time_vertex_us;             /* this frame: the vertex half of the transform time */
int fe_stat_trimmed, fe_stat_cut;       /* this frame: triangles left out for the budget, and the level (see showing) of the last of them */
unsigned fe_time_clear_us, fe_time_first_draw_us;       /* this frame: the clear; the first draw call given to GL */
unsigned fe_time_transform_us, fe_time_submit_us;      /* this frame: in draw calls here; handing the frame to GL */
int fe_peak_vertices, fe_peak_chunks, fe_peak_batches;   /* the most one draw call, one frame have needed */
static Batch *batch_for(int kind, unsigned tex0, unsigned tex1, int blend, int depth_test, int depth_mask) {
    if (kind == ORDERED) {
        if (sOrderedOpen >= 0) {
            Batch *b = &sBatches[sOrderedOpen];
            if (b->tex0 == tex0 && b->tex1 == tex1 && b->blend == blend && b->depth_test == depth_test && b->depth_mask == depth_mask)
                return b;
        }
    } else {
        for (int i = 0; i < sBatchCount; i++) {
            Batch *b = &sBatches[i];
            if (b->kind == kind && b->tex0 == tex0 && b->tex1 == tex1) return b;
        }
    }
    if (sBatchCount == BATCHES) return NULL;
    Batch *b = &sBatches[sBatchCount];
    b->kind = (uint8_t)kind; b->tex0 = tex0; b->tex1 = tex1;
    b->blend = (uint8_t)blend; b->depth_test = (uint8_t)depth_test; b->depth_mask = (uint8_t)depth_mask;
    b->chunks = 0;
    if (kind == ORDERED) sOrderedOpen = sBatchCount;
    sBatchCount++;
    if (sBatchCount > fe_peak_batches) fe_peak_batches = sBatchCount;
    return b;
}

int fe_stat_idle_calls, fe_stat_idle_vertices, fe_stat_idle_offscreen;    /* draw calls that gave no triangle, their vertices, and those wholly off one side */
static int sFrameCount;                 /* triangles kept this frame */
/* The batch's newest chunk if it has room for this many more vertices and corners, else a
 * new one; -1 when the frame is full. */
static int chunk_for(Batch *b, int verts, int indices) {
    if (b->chunks) {
        int c = b->chunk[b->chunks - 1];
        if (sChunkVerts[c] + verts <= CHUNK_VERTS && sChunkIndices[c] + indices <= CHUNK_INDICES) return c;
    }
    if (sChunksUsed == CHUNKS || b->chunks == BATCH_CHUNKS) { fe_stat_dropped++; return -1; }
    int c = sChunksUsed++;
    b->chunk[b->chunks++] = (uint8_t)c;
    sChunkVerts[c] = sChunkIndices[c] = 0;
    if (sChunksUsed > fe_peak_chunks) fe_peak_chunks = sChunksUsed;
    return c;
}

/* ---- transform, clip, cull ----------------------------------------------------------------- */
/* Each vertex a draw call uses is transformed once into the form GL is given (OutVtx), with
 * its fog factor already folded into the colour; triangles then copy three of them. Only a
 * triangle that crosses the edge of the view is unpacked to floats (Vtx) for clipping. */
typedef struct { float x, y, z, w, u0, v0, u1, v1, r, g, b, a; } Vtx;
#define MAX_VERTS 2560
static OutVtx sOut[MAX_VERTS];
static float sSX[MAX_VERTS], sSY[MAX_VERTS];
static uint8_t sCode[MAX_VERTS], sFogged[MAX_VERTS];
/* Everything is clipped a little inside the view volume GL would clip to, so the driver
 * never clips: 1/1024 of the half-screen at the sides (a ninth of a pixel), and the same
 * fraction of the depth range. */
#define INSET 0.9990234375f

static float plane(const Vtx *v, int bit) {
    switch (bit) {
    case 0: return v->x + v->w * INSET;
    case 1: return v->w * INSET - v->x;
    case 2: return v->y + v->w * INSET;
    case 3: return v->w * INSET - v->y;
    case 4: return v->z + v->w * INSET;
    default: return v->w * INSET - v->z;
    }
}
static int clip_plane(const Vtx *in, int n, Vtx *out, int bit) {
    int count = 0;
    for (int k = 0; k < n; k++) {
        const Vtx *p = &in[k], *q = &in[(k + 1) % n];
        float dp = plane(p, bit), dq = plane(q, bit);
        if (dp >= 0.f) out[count++] = *p;
        if ((dp >= 0.f) != (dq >= 0.f)) {
            float t = dp / (dp - dq);
            const float *a = &p->x, *b = &q->x;
            float *o = &out[count++].x;
            for (int i = 0; i < (int)(sizeof(Vtx) / sizeof(float)); i++) o[i] = a[i] + (b[i] - a[i]) * t;
        }
    }
    return count;
}

/* Whether the driver can be given this triangle (corners in pixels, twice its area). It
 * reboots the iPod on triangles with no area, and it places corners on a grid finer than a
 * pixel, so one thinner than that grid has none: clipping the score display's sprites at the
 * screen edge left slivers a tenth of a pixel wide and a hundred long, and the first frame
 * that drew them rebooted it. Kept: at least 2 square pixels, and at least half a pixel
 * across at its thinnest. */
static inline int drawable(float ax, float ay, float bx, float by, float cx, float cy, float area2, float min_area2) {
    if (!(area2 >= min_area2)) return 0;                /* also rejects NaN */
    float e0 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay), e1 = (cx - bx) * (cx - bx) + (cy - by) * (cy - by),
          e2 = (ax - cx) * (ax - cx) + (ay - cy) * (ay - cy);
    float longest = e0 > e1 ? (e0 > e2 ? e0 : e2) : (e1 > e2 ? e1 : e2);
    return area2 * area2 >= 0.25f * longest;            /* thinnest width = area2 / longest edge >= 0.5 */
}

static inline uint32_t byte_of(float v) { return v >= 255.f ? 255u : v <= 0.f ? 0u : (uint32_t)(v + 0.5f); }
static void unpack(Vtx *v, const OutVtx *o) {
    v->x = o->x; v->y = o->y; v->z = o->z; v->w = o->w; v->u0 = o->u0; v->v0 = o->v0; v->u1 = o->u1; v->v1 = o->v1;
    v->r = (float)(o->color & 255u); v->g = (float)((o->color >> 8) & 255u);
    v->b = (float)((o->color >> 16) & 255u); v->a = (float)(o->color >> 24);
}

typedef struct { unsigned tex0, tex1; int fogged_scene, cull; float min_area2; Batch *plain, *fogged; } DrawState;

/* The batch a triangle of this draw call goes to; looked up once per call and kind. */
static Batch *target(DrawState *s, int fogged) {
    Batch **slot = fogged ? &s->fogged : &s->plain;
    if (!*slot) {
        if (s->fogged_scene) *slot = batch_for(fogged ? FOGGED : OPAQUE, s->tex0, s->tex1, fogged, 1, 1);
        else *slot = batch_for(ORDERED, s->tex0, s->tex1, sBlend, sDepthTest, sDepthMask);
    }
    return *slot;
}

static const uint8_t *array_base(const Array *a) {
    if (a->buffer) {
        Buffer *b = buffer(a->buffer);
        return b && b->data && !b->packed ? b->data + a->pointer : NULL;
    }
    return a->pointer ? g_mem + a->pointer : NULL;
}
static Buffer *packed_buffer(const Array *a) {
    Buffer *b = a->buffer ? buffer(a->buffer) : NULL;
    return b && b->data && b->packed ? b : NULL;
}

static uint64_t sDrawStart;
/* What a vertex needs beyond its place on the screen: texture coordinates, colour, fog. Most
 * of the vertices the engine sends are only in triangles that face away, are off the screen
 * or cover under two pixels, so this half is done when a triangle is kept, for its corners
 * (about one vertex in four). */
typedef struct {
    const int16_t *pk;
    const uint8_t *uv0, *uv1, *col;
    int pks, pk0, pk1, t0s, t1s, cs, fog_blend;
    float us0, us1, tu0, tv0, tu1, tv1;
    uint32_t flat;
} Finish;
static uint8_t sDone[MAX_VERTS], sLost[MAX_VERTS];
static float sFogFactor[MAX_VERTS];                   /* how much shows through the fog: 1 clear, 0 none */
static inline void finish(const Finish *f, unsigned i) {
    if (sDone[i]) return;
    sDone[i] = 1;
    OutVtx *o = &sOut[i];
    if (f->pk) {
        const int16_t *q = f->pk + (size_t)i * (size_t)f->pks;
        if (f->pk0 >= 0) { o->u0 = (float)q[3 + 2 * f->pk0] * f->us0 + f->tu0; o->v0 = (float)q[4 + 2 * f->pk0] * f->us0 + f->tv0; }
        if (f->pk1 >= 0) { o->u1 = (float)q[3 + 2 * f->pk1] * f->us1 + f->tu1; o->v1 = (float)q[4 + 2 * f->pk1] * f->us1 + f->tv1; }
    }
    if (f->uv0) { float t[2]; __builtin_memcpy(t, f->uv0 + (size_t)i * (size_t)f->t0s, 8); o->u0 = t[0] + f->tu0; o->v0 = t[1] + f->tv0; }
    else if (f->pk0 < 0) o->u0 = o->v0 = 0.f;
    if (f->uv1) { float t[2]; __builtin_memcpy(t, f->uv1 + (size_t)i * (size_t)f->t1s, 8); o->u1 = t[0] + f->tu1; o->v1 = t[1] + f->tv1; }
    else if (f->pk1 < 0) o->u1 = o->v1 = 0.f;
    uint32_t c = f->flat;
    if (f->col) __builtin_memcpy(&c, f->col + (size_t)i * (size_t)f->cs, 4);
    uint8_t fogged = 0;
    if (f->fog_blend) {
        float v = sFogFactor[i];
        if (v < 0.998f) {                               /* colour and alpha scaled by the fog factor */
            uint32_t k = v <= 0.f ? 0u : (uint32_t)(v * 256.f);
            c = ((c & 0x00ff00ffu) * k >> 8 & 0x00ff00ffu) | ((c >> 8 & 0x00ff00ffu) * k & 0xff00ff00u);
            fogged = 1;
        }
    }
    o->color = c;
    sFogged[i] = fogged;
}

/* Where each vertex of the draw call in hand already is in the pool, for the triangles going
 * to its plain batch and to its fogged one; a place in a chunk that has since been closed
 * does not count. */
#define NO_SLOT 0xffffu
static uint16_t sSlot[2][MAX_VERTS];
static inline void emit(Batch *bt, uint16_t *slot, unsigned ia, unsigned ib, unsigned ic) {
    int c = chunk_for(bt, 3, 3);
    if (c < 0) return;
    const unsigned corner[3] = { ia, ib, ic };
    uint16_t *index = &sIndex[c][sChunkIndices[c]];
    const unsigned base = (unsigned)c * CHUNK_VERTS;
    for (int k = 0; k < 3; k++) {
        unsigned p = slot[corner[k]];
        if (p - base >= CHUNK_VERTS) {                  /* not in this chunk (NO_SLOT is in none) */
            p = base + sChunkVerts[c]++;
            sPool[p] = sOut[corner[k]];
            slot[corner[k]] = (uint16_t)p;
        }
        index[k] = (uint16_t)p;
    }
    sChunkIndices[c] += 3;
    sFrameCount++;
}

static void draw_triangles(int count, const uint16_t *idx, int first) {
    fe_stat_calls++;
    Buffer *packed = sVertexArray.enabled ? packed_buffer(&sVertexArray) : NULL;
    const uint8_t *pos = sVertexArray.enabled && !packed ? array_base(&sVertexArray) : NULL;
    if ((!pos && !packed) || count < 3) return;
    int n = 0;
    if (idx) { for (int i = 0; i < count; i++) if (idx[i] >= n) n = idx[i] + 1; }
    else n = first + count;
    if (n > fe_peak_vertices) fe_peak_vertices = n;
    if (n > MAX_VERTS) { fe_stat_dropped++; return; }
    if (packed && sVertexArray.pointer / packed->stride + (uint32_t)n > packed->size / packed->stride) return;

    DrawState s;
    Finish fin;
    s.plain = s.fogged = NULL;
    s.tex0 = sTexEnabled[0] ? rt_texture_host(sTexBound[0]) : 0;
    fin.uv0 = s.tex0 && sTexCoordArray[0].enabled ? array_base(&sTexCoordArray[0]) : NULL;
    fin.uv1 = sTexEnabled[1] && sTexCoordArray[1].enabled ? array_base(&sTexCoordArray[1]) : NULL;
    /* a packed buffer holds the texture coordinates too: which of its pairs each unit reads */
    fin.pk0 = fin.pk1 = -1;
    fin.pk = NULL;
    if (packed) {
        uint32_t base = sVertexArray.pointer / packed->stride;
        if (sVertexArray.pointer % packed->stride || (int)packed->stride != sVertexArray.stride) return;
        fin.pk = (const int16_t *)(const void *)packed->data + (size_t)base * (packed->packed_stride / 2);
        for (int u = 0; u < 2; u++) {
            const Array *t = &sTexCoordArray[u];
            if (!t->enabled || t->buffer != sVertexArray.buffer || t->pointer < sVertexArray.pointer) continue;
            uint32_t rel = t->pointer - sVertexArray.pointer;
            if (rel == 12 || (rel == 20 && packed->channels > 1)) { if (u == 0) fin.pk0 = rel == 12 ? 0 : 1; else fin.pk1 = rel == 12 ? 0 : 1; }
        }
        if (!s.tex0) fin.pk0 = -1;
        if (!sTexEnabled[1]) fin.pk1 = -1;
    }
    s.tex1 = (fin.uv1 || fin.pk1 >= 0) ? rt_texture_host(sTexBound[1]) : 0;
    if (!s.tex1) { fin.uv1 = NULL; fin.pk1 = -1; }
    fin.col = sColorArray.enabled ? array_base(&sColorArray) : NULL;
    s.fogged_scene = sFog && !sBlend && sDepthTest && sDepthMask;
    s.cull = sCull;
    /* The driver reboots on small triangles: with two texture units (found with the test
     * scene) and also with one (the first frame drawn with a 1/8-pixel guard rebooted the iPod,
     * after 180 frames at 2 square pixels). Twice the area is compared. */
    s.min_area2 = s.tex1 ? 4.0f : fe_option_min_area2;

    float m[16];
    const float *mv = sModelView[sModelViewTop];
    multiply(m, sProjection[sProjectionTop], mv);
    fin.tu0 = sTexture[0][sTextureTop[0]][12]; fin.tv0 = sTexture[0][sTextureTop[0]][13];
    fin.tu1 = sTexture[1][sTextureTop[1]][12]; fin.tv1 = sTexture[1][sTextureTop[1]][13];
    const int fog = sFog && sFogEnd > sFogStart, fog_blend = fog && s.fogged_scene && fe_option_fog_blend;
    const float fog_scale = fog ? 1.0f / (sFogEnd - sFogStart) : 0.0f;
    const float f2 = mv[2], f6 = mv[6], f10 = mv[10], f14 = mv[14], fog_end = sFogEnd;
    /* with the usual perspective the depth the fog goes by is the w just computed */
    const int depth_is_w = m[3] == -f2 && m[7] == -f6 && m[11] == -f10 && m[15] == -f14;
    const int vs = sVertexArray.stride ? sVertexArray.stride : 12;
    fin.cs = sColorArray.stride ? sColorArray.stride : 4;
    fin.t0s = sTexCoordArray[0].stride ? sTexCoordArray[0].stride : 8; fin.t1s = sTexCoordArray[1].stride ? sTexCoordArray[1].stride : 8;
    const int pks = packed ? packed->packed_stride / 2 : 0;
    fin.pks = pks;
    const float ps = packed ? packed->pos_scale : 0.f;
    fin.us0 = packed && fin.pk0 >= 0 ? packed->uv_scale[fin.pk0] : 0.f; fin.us1 = packed && fin.pk1 >= 0 ? packed->uv_scale[fin.pk1] : 0.f;
    fin.flat = byte_of(sColor[0] * 255.f) | byte_of(sColor[1] * 255.f) << 8 | byte_of(sColor[2] * 255.f) << 16
               | byte_of(sColor[3] * 255.f) << 24;
    fin.fog_blend = fog_blend;
    const float hw = sHalfW, hh = sHalfH;
    if (packed && fe_option_box_cull) {
        /* The model's box first: if its eight corners are all off the same side of the view,
         * or all past the end of the fog, so is every vertex, and none need be transformed.
         * (A quarter of the vertices the engine sends are in such models.) */
        uint8_t off = 0x3f;
        float nearest = 1e30f;
        int sign = 0;
        for (int c = 0; c < 8; c++) {
            float x = c & 1 ? packed->hi[0] : packed->lo[0], y = c & 2 ? packed->hi[1] : packed->lo[1], z = c & 4 ? packed->hi[2] : packed->lo[2];
            float cx = m[0] * x + m[4] * y + m[8] * z + m[12], cy = m[1] * x + m[5] * y + m[9] * z + m[13];
            float cz = m[2] * x + m[6] * y + m[10] * z + m[14], cw = m[3] * x + m[7] * y + m[11] * z + m[15];
            float e = cw + (cw < 0.f ? -cw : cw) * 0.001f;          /* a little outside what the vertices are held to */
            off &= (uint8_t)((cx < -e) | (cx > e) << 1 | (cy < -e) << 2 | (cy > e) << 3 | (cz < -e) << 4 | (cz > e) << 5);
            float ez = f2 * x + f6 * y + f10 * z + f14;
            sign |= ez < 0.f ? 1 : 2;
            if (ez < 0.f) ez = -ez;
            if (ez < nearest) nearest = ez;
        }
        if (off || (fog_blend && sign != 3 && nearest > fog_end * 1.001f)) { fe_stat_box_culled += n; return; }
    }
    const int start = idx ? 0 : first;
    const int16_t *pk = fin.pk;
    uint8_t all = 0xff;
    for (int i = start; i < n; i++) {
        float x, y, z;
        OutVtx *o = &sOut[i];
        if (packed) {
            const int16_t *q = pk + (size_t)i * (size_t)pks;
            x = (float)q[0] * ps; y = (float)q[1] * ps; z = (float)q[2] * ps;
        } else {
            float p[3];
            __builtin_memcpy(p, pos + (size_t)i * (size_t)vs, 12);
            x = p[0]; y = p[1]; z = p[2];
        }
        float cx = m[0] * x + m[4] * y + m[8] * z + m[12], cy = m[1] * x + m[5] * y + m[9] * z + m[13];
        float cz = m[2] * x + m[6] * y + m[10] * z + m[14], cw = m[3] * x + m[7] * y + m[11] * z + m[15];
        o->x = cx; o->y = cy; o->z = cz; o->w = cw;
        uint8_t lost = 0;
        if (fog_blend) {
            float ez = depth_is_w ? cw : f2 * x + f6 * y + f10 * z + f14;
            float f = (fog_end - (ez < 0.f ? -ez : ez)) * fog_scale;
            sFogFactor[i] = f;
            if (f < 0.0078125f) lost = 2;               /* under 2/256: lost in the fog */
        }
        sLost[i] = lost;
        float e = cw * INSET;
        uint8_t code = (uint8_t)((cx < -e) | (cx > e) << 1 | (cy < -e) << 2 | (cy > e) << 3 | (cz < -e) << 4 | (cz > e) << 5);
        sCode[i] = code;
        all &= code;
        if (!code) {
            float inv = 1.f / cw;
            sSX[i] = cx * inv * hw;
            sSY[i] = cy * inv * hh;
        }
    }
    fe_stat_vertices_in += n;
    fe_stat_triangles_in += count / 3;
    fe_time_vertex_us += (unsigned)(plat_time_us() - sDrawStart);
    if (all) { fe_stat_idle_offscreen += n; return; }   /* the whole mesh is off one side */
    memset(sDone + start, 0, (size_t)(n - start));
    memset(sSlot[0] + start, 0xff, (size_t)(n - start) * 2);
    memset(sSlot[1] + start, 0xff, (size_t)(n - start) * 2);
    const int cull = s.cull;
    const float min_area2 = s.min_area2;
    for (int i = 0; i + 2 < count; i += 3) {
        unsigned ia = idx ? idx[i] : (unsigned)(first + i), ib = idx ? idx[i + 1] : (unsigned)(first + i + 1),
                 ic = idx ? idx[i + 2] : (unsigned)(first + i + 2);
        uint8_t ca = sCode[ia], cb = sCode[ib], cc = sCode[ic];
        if (ca & cb & cc) continue;
        if (sLost[ia] & sLost[ib] & sLost[ic]) continue;        /* lost in the fog */
        if (!(ca | cb | cc)) {
            float ax = sSX[ia], ay = sSY[ia];
            float area2 = (sSX[ib] - ax) * (sSY[ic] - ay) - (sSX[ic] - ax) * (sSY[ib] - ay);    /* pixels, y up */
            if (cull && area2 >= 0.f) continue;         /* front faces are clockwise */
            if (area2 < 0.f) area2 = -area2;
            if (!drawable(ax, ay, sSX[ib], sSY[ib], sSX[ic], sSY[ic], area2, min_area2)) { fe_stat_tiny++; continue; }
            finish(&fin, ia); finish(&fin, ib); finish(&fin, ic);
            int fogged = (sFogged[ia] | sFogged[ib] | sFogged[ic]) != 0;
            Batch *bt = target(&s, fogged);
            if (!bt) continue;
            emit(bt, sSlot[fogged], ia, ib, ic);
            fe_stat_fogged += fogged;
            continue;
        }
        /* crosses the edge of the view: clip to every plane it crosses */
        finish(&fin, ia); finish(&fin, ib); finish(&fin, ic);
        int fogged = (sFogged[ia] | sFogged[ib] | sFogged[ic]) != 0;
        Vtx bufa[10], bufb[10], *in = bufa, *out = bufb;
        uint8_t any = ca | cb | cc;
        int k = 3;
        unpack(&in[0], &sOut[ia]); unpack(&in[1], &sOut[ib]); unpack(&in[2], &sOut[ic]);
        for (int bit = 0; bit < 6 && k >= 3; bit++) {
            if (!(any & (1 << bit))) continue;
            k = clip_plane(in, k, out, bit);
            Vtx *t = in; in = out; out = t;
        }
        fe_stat_clipped++;
        if (any & 16) fe_stat_near++;
        if (k < 3) continue;
        float sx[10], sy[10];
        for (int j = 0; j < k; j++) {
            float inv = 1.f / in[j].w;
            sx[j] = in[j].x * inv * hw;
            sy[j] = in[j].y * inv * hh;
        }
        int chunk = -1;                                 /* the polygon's corners go in once, as its triangles need them */
        uint16_t at[10];
        for (int j = 1; j + 1 < k; j++) {
            float area2 = (sx[j] - sx[0]) * (sy[j + 1] - sy[0]) - (sx[j + 1] - sx[0]) * (sy[j] - sy[0]);
            if (cull && area2 >= 0.f) continue;
            if (area2 < 0.f) area2 = -area2;
            if (!drawable(sx[0], sy[0], sx[j], sy[j], sx[j + 1], sy[j + 1], area2, min_area2)) { fe_stat_tiny++; continue; }
            if (chunk < 0) {
                Batch *bt = target(&s, fogged);
                chunk = bt ? chunk_for(bt, k, (k - 2) * 3) : -1;
                if (chunk < 0) break;
                for (int t = 0; t < k; t++) at[t] = NO_SLOT;
            }
            const int corner[3] = { 0, j, j + 1 };
            uint16_t *index = &sIndex[chunk][sChunkIndices[chunk]];
            for (int t = 0; t < 3; t++) {
                int c = corner[t];
                if (at[c] == NO_SLOT) {
                    const Vtx *p = &in[c];
                    at[c] = (uint16_t)(chunk * CHUNK_VERTS + sChunkVerts[chunk]++);
                    OutVtx *v = &sPool[at[c]];
                    v->x = p->x; v->y = p->y; v->z = p->z; v->w = p->w;
                    v->u0 = p->u0; v->v0 = p->v0; v->u1 = p->u1; v->v1 = p->v1;
                    v->color = byte_of(p->r) | byte_of(p->g) << 8 | byte_of(p->b) << 16 | byte_of(p->a) << 24;
                }
                index[t] = at[c];
            }
            sChunkIndices[chunk] += 3;
            sFrameCount++;
            fe_stat_fogged += fogged;
        }
    }
}

static void draw(int count, const uint16_t *idx, int first) {
    plat_poll();
    uint64_t t0 = plat_time_us();
    sDrawStart = t0;
    int before = sFrameCount, vin = fe_stat_vertices_in;
    draw_triangles(count, idx, first);
    if (sFrameCount == before) { fe_stat_idle_calls++; fe_stat_idle_vertices += fe_stat_vertices_in - vin; }
    fe_time_transform_us += (unsigned)(plat_time_us() - t0);
}

void fe_draw_elements(uint32_t mode, int count, uint32_t type, uint32_t indices) {
    if (mode != 4 || type != 0x1403) return;            /* the engine only draws indexed triangle lists */
    const uint8_t *base;
    if (sElementBuffer) {
        Buffer *b = buffer(sElementBuffer);
        if (!b || !b->data) return;
        base = b->data + indices;
    } else base = g_mem + indices;
    draw(count, (const uint16_t *)(const void *)base, 0);
}
void fe_draw_arrays(uint32_t mode, int first, int count) { if (mode == 4) draw(count, NULL, first); }

/* A flat picture over the finished scene, for the port's own screens: a rectangle in panel
 * pixels (y down from the top), a rectangle of the texture in texels over its size, and a
 * premultiplied colour. Drawn in call order after everything the engine drew. */
void fe_overlay(unsigned texture, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t rgba) {
    Batch *b = batch_for(ORDERED, texture, 0, 1, 0, 0);
    if (!b) return;
    float l = x / sHalfW - 1.f, r = (x + w) / sHalfW - 1.f, t = 1.f - y / sHalfH, bt = 1.f - (y + h) / sHalfH;
    /* nothing given to GL may reach the edge of the view: trim the rectangle to just inside it */
    if (!(r > l) || !(t > bt)) return;
    float du = (u1 - u0) / (r - l), dv = (v1 - v0) / (bt - t);
    if (l < -INSET) { u0 += (-INSET - l) * du; l = -INSET; }
    if (r > INSET) { u1 -= (r - INSET) * du; r = INSET; }
    if (t > INSET) { v0 += (INSET - t) * dv; t = INSET; }
    if (bt < -INSET) { v1 -= (-INSET - bt) * dv; bt = -INSET; }
    if (!(r > l) || !(t > bt)) return;
    const float corner[4][4] = { { l, t, u0, v0 }, { r, t, u1, v0 }, { r, bt, u1, v1 }, { l, bt, u0, v1 } };
    int c = chunk_for(b, 4, 6);
    if (c < 0) return;
    unsigned first = (unsigned)c * CHUNK_VERTS + sChunkVerts[c];
    for (int k = 0; k < 4; k++) {
        OutVtx *v = &sPool[first + (unsigned)k];
        v->x = corner[k][0]; v->y = corner[k][1]; v->z = 0.f; v->w = 1.f;
        v->u0 = corner[k][2]; v->v0 = corner[k][3]; v->u1 = v->v1 = 0.f;
        v->color = rgba;
    }
    static const uint8_t order[6] = { 0, 1, 2, 0, 2, 3 };
    uint16_t *index = &sIndex[c][sChunkIndices[c]];
    for (int k = 0; k < 6; k++) index[k] = (uint16_t)(first + order[k]);
    sChunkVerts[c] += 4;
    sChunkIndices[c] += 6;
    sFrameCount += 2;
}

void fe_frame_begin(int panel_w, int panel_h) {
    sHalfW = (float)panel_w * 0.5f;
    sHalfH = (float)panel_h * 0.5f;
    sBatchCount = sChunksUsed = 0;
    sOrderedOpen = -1;
    fe_stat_draws = fe_stat_vertices_in = fe_stat_triangles_in = fe_stat_triangles_out = fe_stat_tiny = 0;
    fe_stat_clipped = fe_stat_dropped = fe_stat_calls = fe_stat_fogged = 0;
    fe_stat_idle_calls = fe_stat_idle_vertices = fe_stat_idle_offscreen = 0;
    fe_stat_box_culled = 0;
    fe_time_vertex_us = 0;
    fe_time_transform_us = fe_time_submit_us = 0;
    fe_stat_near = 0;
    uint64_t t0 = plat_time_us();
    glViewport(0, 0, panel_w, panel_h);
    glClearColor(sClear[0], sClear[1], sClear[2], sClear[3]);
    glClearDepthf(1.f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    fe_time_clear_us = (unsigned)(plat_time_us() - t0);
    fe_time_first_draw_us = 0;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glShadeModel(GL_SMOOTH);
}

static int sSpare;                      /* the next unused place in the pool, for draws without indices */
int fe_stat_vertices_out;               /* this frame: vertices GL was given */
long fe_host_vertices;                  /* host: vertices given to GL, all frames */
static void draw_batch(const Batch *b, int index) {
    plat_poll();
    CRUMB("batch", (uint32_t)(index << 16) | (uint32_t)(b->tex1 ? 2 : 1) | (uint32_t)b->kind << 8);
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
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    if (b->tex0) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, b->tex0);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    } else {
        glDisable(GL_TEXTURE_2D);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    if (b->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (b->depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(b->depth_mask ? GL_TRUE : GL_FALSE);
    for (int k = 0; k < b->chunks; k++) {
        int c = b->chunk[k], n = sChunkIndices[c];
        if (!n) continue;
        uint64_t t0 = fe_stat_draws ? 0 : plat_time_us();
        if (fe_option_indexed) {
            CRUMB("elements", (uint32_t)(c << 16) | (uint32_t)n);
            glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_SHORT, sIndex[c]);
            fe_stat_draws++;
            fe_stat_vertices_out += sChunkVerts[c];
        } else {
            /* each triangle's own three vertices, written out in the part of the pool the
             * frame has not used, 384 to a call */
            for (int from = 0; from < n; from += CHUNK_VERTS) {
                int part = n - from < CHUNK_VERTS ? n - from : CHUNK_VERTS;
                if (sSpare + part > CHUNKS * CHUNK_VERTS) { fe_stat_dropped++; break; }
                OutVtx *out = &sPool[sSpare];
                for (int j = 0; j < part; j++) out[j] = sPool[sIndex[c][from + j]];
                CRUMB("arrays", (uint32_t)(sSpare / CHUNK_VERTS << 16) | (uint32_t)part);
                glDrawArrays(GL_TRIANGLES, sSpare, part);
                sSpare += part;
                fe_stat_draws++;
                fe_stat_vertices_out += part;
            }
        }
        if (t0) fe_time_first_draw_us = (unsigned)(plat_time_us() - t0);
        fe_stat_triangles_out += n / 3;
    }
}

static int batch_triangles(const Batch *b) {
    int n = 0;
    for (int k = 0; k < b->chunks; k++) n += sChunkIndices[b->chunk[k]] / 3;
    return n;
}
/* How much of a triangle is seen: its area in pixels times how far it shows through the fog
 * (its colours already carry that), as a level on a scale of four steps per doubling, which
 * reaches from a triangle of a pixel barely showing to one many times the size of the panel. */
#define LEVELS 128
static inline unsigned showing(const OutVtx *p, const OutVtx *q, const OutVtx *r) {
    float ia = 1.f / p->w, ib = 1.f / q->w, ic = 1.f / r->w;
    float ax = p->x * ia, ay = p->y * ia;
    float area2 = ((q->x * ib - ax) * (r->y * ic - ay) - (r->x * ic - ax) * (q->y * ib - ay)) * sHalfW * sHalfH;
    unsigned a = p->color >> 24, b = q->color >> 24, c = r->color >> 24;
    union { float f; uint32_t u; } score;
    score.f = (area2 < 0.f ? -area2 : area2) * (float)(a + b + c + 3u);
    uint32_t level = (score.u >> 21) & 0x3ffu;          /* exponent and two bits: 4 per doubling */
    return level <= 508u ? 0u : level >= 508u + LEVELS ? LEVELS - 1u : level - 508u;
}
/* Keep the frame within the budget: the scenery's triangles that show least give way, the
 * small and those far into the fog (an unfogged one counts by its area alone). As the run
 * begins the camera swings round the temple with nearly everything close and clear; taking
 * only fogged triangles there left the rest to be cut off the end of the unfogged batches,
 * and that was whole stretches of floor. Only corners are taken out of the lists; a vertex
 * no triangle uses any more stays in its chunk. */
static void trim(void) {
    int total = 0;
    fe_stat_trimmed = fe_stat_cut = 0;
    for (int i = 0; i < sBatchCount; i++) total += batch_triangles(&sBatches[i]);
    int excess = total - fe_option_budget;
    if (excess <= 0) return;
    static uint16_t count[2 * LEVELS];
    static uint8_t level[CHUNKS * CHUNK_INDICES / 3];
    for (int i = 0; i < 2 * LEVELS; i++) count[i] = 0;
    int t = 0;
    for (int i = 0; i < sBatchCount; i++) {
        const Batch *b = &sBatches[i];
        if (b->kind == ORDERED) continue;
        /* the scenery has a light map; what has none is the monkeys and the water, which stay
         * unless the scenery alone cannot make the room */
        const unsigned keep = b->tex1 ? 0u : LEVELS;
        for (int k = 0; k < b->chunks; k++) {
            const uint16_t *index = sIndex[b->chunk[k]];
            for (int j = 0, n = sChunkIndices[b->chunk[k]]; j < n; j += 3) {
                unsigned l = showing(&sPool[index[j]], &sPool[index[j + 1]], &sPool[index[j + 2]]) + keep;
                count[level[t++] = (uint8_t)l]++;
            }
        }
    }
    int cut = 0, going = 0;
    while (cut < 2 * LEVELS - 1 && going + count[cut] < excess) going += count[cut++];
    int partial = excess - going;                       /* this many of the last level go too */
    fe_stat_cut = cut;
    t = 0;
    for (int i = 0; i < sBatchCount; i++) {
        Batch *b = &sBatches[i];
        if (b->kind == ORDERED) continue;
        for (int k = 0; k < b->chunks; k++) {
            uint16_t *index = sIndex[b->chunk[k]];
            int kept = 0;
            for (int j = 0, n = sChunkIndices[b->chunk[k]]; j < n; j += 3) {
                int l = level[t++];
                if (l < cut || (l == cut && partial > 0 && partial--)) { fe_stat_trimmed++; continue; }
                if (kept != j) { index[kept] = index[j]; index[kept + 1] = index[j + 1]; index[kept + 2] = index[j + 2]; }
                kept += 3;
            }
            sChunkIndices[b->chunk[k]] = (uint16_t)kept;
        }
    }
    excess -= fe_stat_trimmed;
    for (int i = sBatchCount - 1; i >= 0 && excess > 0; i--) {
        Batch *b = &sBatches[i];
        if (b->kind != OPAQUE) continue;
        for (int k = b->chunks - 1; k >= 0 && excess > 0; k--) {
            int n = sChunkIndices[b->chunk[k]] / 3, drop = n < excess ? n : excess;
            sChunkIndices[b->chunk[k]] = (uint16_t)((n - drop) * 3);
            excess -= drop; fe_stat_trimmed += drop;
        }
    }
}

static void submit(void);
void fe_frame_end(void) {
    uint64_t t0 = plat_time_us();
    submit();
    fe_time_submit_us = (unsigned)(plat_time_us() - t0);
}
static void submit(void) {
    if (!sChunksUsed) return;
    trim();
    sSpare = sChunksUsed * CHUNK_VERTS;
    fe_stat_vertices_out = 0;
#ifndef AB_NANO
    if (getenv("TR_DUMP_BATCHES"))
        for (int i = 0; i < sBatchCount; i++) {
            const Batch *bt = &sBatches[i];
            int vertices = 0;
            for (int k = 0; k < bt->chunks; k++) vertices += sChunkVerts[bt->chunk[k]];
            fprintf(stderr, "batch %d kind %d tex %u/%u blend %d depth %d mask %d: %d triangles, %d vertices in %d chunks\n", i, bt->kind, bt->tex0, bt->tex1,
                    bt->blend, bt->depth_test, bt->depth_mask, batch_triangles(bt), vertices, bt->chunks);
        }
    for (int i = 0; i < sBatchCount; i++) {             /* host check: nothing given to GL may touch a clip boundary */
        const Batch *bt = &sBatches[i];
        for (int k = 0; k < bt->chunks; k++) {
            int c = bt->chunk[k];
            const uint16_t *index = sIndex[c];
            for (int j = 0, n = sChunkIndices[c]; j < n; j++) {
                if (index[j] < c * CHUNK_VERTS || index[j] >= c * CHUNK_VERTS + sChunkVerts[c]) { fe_stat_outside++; continue; }   /* a corner outside its chunk */
                const OutVtx *v = &sPool[index[j]];
                if (!(v->w > 0.f && v->x > -v->w && v->x < v->w && v->y > -v->w && v->y < v->w && v->z > -v->w && v->z < v->w))
                    fe_stat_outside++;
                float d = v->z / v->w, au = v->u0 < 0 ? -v->u0 : v->u0, av = v->v0 < 0 ? -v->v0 : v->v0;
                if (v->w < fe_ext_min_w) fe_ext_min_w = v->w;
                if (v->w > fe_ext_max_w) fe_ext_max_w = v->w;
                if (d < fe_ext_min_depth) fe_ext_min_depth = d;
                if (d > fe_ext_max_depth) fe_ext_max_depth = d;
                if (au > fe_ext_max_uv) fe_ext_max_uv = au;
                if (av > fe_ext_max_uv) fe_ext_max_uv = av;
            }
            for (int j = 0, n = sChunkIndices[c]; j + 2 < n; j += 3) {      /* and none may be a sliver */
                float x[3], y[3];
                for (int e = 0; e < 3; e++) { const OutVtx *v = &sPool[index[j + e]]; x[e] = v->x / v->w * sHalfW; y[e] = v->y / v->w * sHalfH; }
                float area2 = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
                if (area2 < 0.f) area2 = -area2;
                if (!drawable(x[0], y[0], x[1], y[1], x[2], y[2], area2 * 1.02f, 3.9f)) fe_stat_slivers++;
            }
            fe_host_vertices += sChunkVerts[c];
        }
    }
#endif
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_CULL_FACE);                            /* culled above */
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);        /* the engine's colours are premultiplied */
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    const unsigned char *base = (const unsigned char *)sPool;
    glVertexPointer(4, GL_FLOAT, sizeof(OutVtx), base + offsetof(OutVtx, x));
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(OutVtx), base + offsetof(OutVtx, color));
    glActiveTexture(GL_TEXTURE1);
    glClientActiveTexture(GL_TEXTURE1);
    glTexCoordPointer(2, GL_FLOAT, sizeof(OutVtx), base + offsetof(OutVtx, u1));
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glTexCoordPointer(2, GL_FLOAT, sizeof(OutVtx), base + offsetof(OutVtx, u0));
    for (int kind = OPAQUE; kind <= ORDERED; kind++)
        for (int i = 0; i < sBatchCount; i++)
            if (sBatches[i].kind == kind) draw_batch(&sBatches[i], i);
    CRUMB("drawn-all", fe_stat_triangles_out);
    glActiveTexture(GL_TEXTURE1);
    glClientActiveTexture(GL_TEXTURE1);
    glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

/* A new GL view starts with none of this file's GL state; the engine's state is kept. */
void fe_reset(void) {
    for (int i = 0; i < 8; i++) memcpy(sModelView[i], kIdentity, sizeof kIdentity);
    for (int i = 0; i < 2; i++) {
        memcpy(sProjection[i], kIdentity, sizeof kIdentity);
        memcpy(sTexture[0][i], kIdentity, sizeof kIdentity);
        memcpy(sTexture[1][i], kIdentity, sizeof kIdentity);
    }
}
