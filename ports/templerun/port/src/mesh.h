/* mesh.h — Temple Run's .bksb models (cMesh3D::loadFileBin, versions 3 and 4). */
#ifndef TR_MESH_H
#define TR_MESH_H
#include <stdint.h>

typedef struct { int vertex_count, index_count, vertex_start, index_start; } TRFrame;
typedef struct {
    int frame_count, vertex_count, index_count, vertex_size;
    int pos_offset, normal_offset, color_offset;        /* -1 when absent */
    int uv_count, uv_offset[4];
    TRFrame *frames;
    uint8_t *vertices;          /* interleaved, vertex_size bytes each */
    uint16_t *indices;
    float min[3], max[3];       /* bounds of frame 0 (cMesh3D::updateBoundingVolumes) */
} TRMesh;

/* Parses the file's bytes; the mesh keeps pointers into its own copies. NULL on a bad file. */
TRMesh *mesh_load(const uint8_t *data, uint32_t size);
void mesh_free(TRMesh *m);
#endif
