/* mesh.c — reads the original model files unchanged. Layout, from cMesh3D::loadFileBin
 * (0x56e2c): 'v', int version (3 or 4), int frame count, 16 bytes per frame (vertex count,
 * index count, first vertex, first index), int primitive type, int vertex count, then if
 * non-zero: vertex size, position descriptor (type, components, stride, offset), a flag
 * byte + descriptor for normals, the same for colours, the number of texture-coordinate
 * channels (an int in version 4, a flag byte in version 3) with a descriptor each, and the
 * interleaved vertex bytes; then int index count and, if non-zero, three ints and the
 * 16-bit indices. */
#include <stdlib.h>
#include <string.h>
#include "mesh.h"

typedef struct { const uint8_t *p, *end; int bad; } Reader;
static int32_t i32(Reader *r) {
    if (r->end - r->p < 4) { r->bad = 1; return 0; }
    int32_t v; memcpy(&v, r->p, 4); r->p += 4; return v;
}
static int u8(Reader *r) {
    if (r->end - r->p < 1) { r->bad = 1; return 0; }
    return *r->p++;
}
static int descriptor(Reader *r) {      /* type, components, stride, offset -> offset */
    i32(r); i32(r); i32(r);
    return i32(r);
}

void mesh_free(TRMesh *m) {
    if (!m) return;
    free(m->frames); free(m->vertices); free(m->indices); free(m);
}

TRMesh *mesh_load(const uint8_t *data, uint32_t size) {
    Reader r = { data, data + size, 0 };
    if (size < 9 || u8(&r) != 'v') return NULL;
    int version = i32(&r);
    if (version != 3 && version != 4) return NULL;
    TRMesh *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->normal_offset = m->color_offset = -1;
    m->frame_count = i32(&r);
    if (m->frame_count < 1 || m->frame_count > 256) goto bad;
    m->frames = malloc(sizeof(TRFrame) * (size_t)m->frame_count);
    if (!m->frames) goto bad;
    for (int i = 0; i < m->frame_count; i++) {
        m->frames[i].vertex_count = i32(&r);
        m->frames[i].index_count = i32(&r);
        m->frames[i].vertex_start = i32(&r);
        m->frames[i].index_start = i32(&r);
    }
    i32(&r);                                    /* primitive type: 0 = triangles */
    m->vertex_count = i32(&r);
    if (m->vertex_count < 0 || m->vertex_count > 200000) goto bad;
    if (m->vertex_count > 0) {
        m->vertex_size = i32(&r);
        if (m->vertex_size < 12 || m->vertex_size > 64) goto bad;
        m->pos_offset = descriptor(&r);
        if (u8(&r)) m->normal_offset = descriptor(&r);
        if (u8(&r)) m->color_offset = descriptor(&r);
        m->uv_count = version == 4 ? i32(&r) : u8(&r);
        if (m->uv_count < 0 || m->uv_count > 4) goto bad;
        for (int i = 0; i < m->uv_count; i++) m->uv_offset[i] = descriptor(&r);
        size_t bytes = (size_t)m->vertex_count * (size_t)m->vertex_size;
        if (r.bad || (size_t)(r.end - r.p) < bytes) goto bad;
        m->vertices = malloc(bytes);
        if (!m->vertices) goto bad;
        memcpy(m->vertices, r.p, bytes);
        r.p += bytes;
    }
    m->index_count = i32(&r);
    if (m->index_count < 0 || m->index_count > 400000) goto bad;
    if (m->index_count > 0) {
        i32(&r); i32(&r); i32(&r);
        size_t bytes = (size_t)m->index_count * 2u;
        if (r.bad || (size_t)(r.end - r.p) < bytes) goto bad;
        m->indices = malloc(bytes);
        if (!m->indices) goto bad;
        memcpy(m->indices, r.p, bytes);
        r.p += bytes;
    }
    if (r.bad) goto bad;
    for (int i = 0; i < m->frame_count; i++) {
        const TRFrame *f = &m->frames[i];
        if (f->vertex_count < 0 || f->index_count < 0 || f->vertex_start < 0 || f->index_start < 0 ||
            f->vertex_start + f->vertex_count > m->vertex_count || f->index_start + f->index_count > m->index_count)
            goto bad;
    }
    for (int k = 0; k < 3; k++) { m->min[k] = 1e30f; m->max[k] = -1e30f; }
    for (int i = 0; i < m->frames[0].vertex_count; i++) {
        float p[3];
        memcpy(p, m->vertices + (size_t)(m->frames[0].vertex_start + i) * m->vertex_size + m->pos_offset, 12);
        for (int k = 0; k < 3; k++) {
            if (p[k] < m->min[k]) m->min[k] = p[k];
            if (p[k] > m->max[k]) m->max[k] = p[k];
        }
    }
    return m;
bad:
    mesh_free(m);
    return NULL;
}
