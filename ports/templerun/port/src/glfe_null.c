/* glfe_null.c — an OpenGL that draws nothing, for running the engine's logic alone. */
#include "glfe.h"

static uint32_t sNames = 100;
void fe_active_texture(uint32_t unit) { (void)unit; }
void fe_client_active_texture(uint32_t unit) { (void)unit; }
void fe_bind_texture(uint32_t target, uint32_t id) { (void)target; (void)id; }
void fe_bind_buffer(uint32_t target, uint32_t name) { (void)target; (void)name; }
void fe_buffer_data(uint32_t target, uint32_t size, const void *data, uint32_t usage) { (void)target; (void)size; (void)data; (void)usage; }
void fe_gen_buffers(uint32_t count, uint32_t *names) { for (uint32_t i = 0; i < count; i++) names[i] = ++sNames; }
void fe_delete_buffers(uint32_t count, const uint32_t *names) { (void)count; (void)names; }
void fe_delete_textures(uint32_t count, const uint32_t *names) { (void)count; (void)names; }
void fe_enable(uint32_t cap, int on) { (void)cap; (void)on; }
void fe_client_state(uint32_t array, int on) { (void)array; (void)on; }
void fe_clear_color(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; }
void fe_color(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; }
void fe_depth_mask(int on) { (void)on; }
void fe_fogf(uint32_t name, float value) { (void)name; (void)value; }
void fe_fogfv(uint32_t name, const float *values) { (void)name; (void)values; }
void fe_hint(uint32_t target, uint32_t mode) { (void)target; (void)mode; }
void fe_matrix_mode(uint32_t mode) { (void)mode; }
void fe_load_identity(void) {}
void fe_push_matrix(void) {}
void fe_pop_matrix(void) {}
void fe_mult_matrix(const float *m) { (void)m; }
void fe_translate(float x, float y, float z) { (void)x; (void)y; (void)z; }
void fe_frustum(float l, float r, float b, float t, float n, float f) { (void)l; (void)r; (void)b; (void)t; (void)n; (void)f; }
void fe_ortho(float l, float r, float b, float t, float n, float f) { (void)l; (void)r; (void)b; (void)t; (void)n; (void)f; }
void fe_vertex_pointer(int size, uint32_t type, int stride, uint32_t pointer) { (void)size; (void)type; (void)stride; (void)pointer; }
void fe_texcoord_pointer(int size, uint32_t type, int stride, uint32_t pointer) { (void)size; (void)type; (void)stride; (void)pointer; }
void fe_color_pointer(int size, uint32_t type, int stride, uint32_t pointer) { (void)size; (void)type; (void)stride; (void)pointer; }
void fe_normal_pointer(uint32_t type, int stride, uint32_t pointer) { (void)type; (void)stride; (void)pointer; }
void fe_draw_arrays(uint32_t mode, int first, int count) { (void)mode; (void)first; (void)count; }
void fe_draw_elements(uint32_t mode, int count, uint32_t type, uint32_t indices) { (void)mode; (void)count; (void)type; (void)indices; }
void fe_note_mesh(uint32_t mesh) { (void)mesh; }
