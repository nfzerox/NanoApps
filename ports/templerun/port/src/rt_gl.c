/* rt_gl.c — the engine's OpenGL calls, unpacked from the ARM calling convention (floats
 * arrive as their bits in the integer registers, the fifth argument onward on the stack). */
#include "rt.h"
#include "glfe.h"

#define F(x) rt_float(x)
#define STACK_ARG(i) M32(SP + 4 * (i))
#define DONE() (R0 = 0)
void imp_glActiveTexture(void) { fe_active_texture(R0); DONE(); }
void imp_glClientActiveTexture(void) { fe_client_active_texture(R0); DONE(); }
void imp_glBindTexture(void) { fe_bind_texture(R0, R1); DONE(); }
void imp_glBindBuffer(void) { fe_bind_buffer(R0, R1); DONE(); }
void imp_glBufferData(void) { fe_buffer_data(R0, R1, R2 ? g_mem + R2 : 0, R3); DONE(); }
void imp_glGenBuffers(void) { fe_gen_buffers(R0, (uint32_t *)(g_mem + R1)); DONE(); }
void imp_glDeleteBuffers(void) { fe_delete_buffers(R0, (const uint32_t *)(g_mem + R1)); DONE(); }
void imp_glDeleteTextures(void) { fe_delete_textures(R0, (const uint32_t *)(g_mem + R1)); DONE(); }
void imp_glEnable(void) { fe_enable(R0, 1); DONE(); }
void imp_glDisable(void) { fe_enable(R0, 0); DONE(); }
void imp_glEnableClientState(void) { fe_client_state(R0, 1); DONE(); }
void imp_glDisableClientState(void) { fe_client_state(R0, 0); DONE(); }
void imp_glClearColor(void) { fe_clear_color(F(R0), F(R1), F(R2), F(R3)); DONE(); }
void imp_glColor4f(void) { fe_color(F(R0), F(R1), F(R2), F(R3)); DONE(); }
void imp_glDepthMask(void) { fe_depth_mask((int)(R0 & 1)); DONE(); }
void imp_glFogf(void) { fe_fogf(R0, F(R1)); DONE(); }
void imp_glFogfv(void) { fe_fogfv(R0, (const float *)(g_mem + R1)); DONE(); }
void imp_glHint(void) { fe_hint(R0, R1); DONE(); }
void imp_glMatrixMode(void) { fe_matrix_mode(R0); DONE(); }
void imp_glLoadIdentity(void) { fe_load_identity(); DONE(); }
void imp_glPushMatrix(void) { fe_push_matrix(); DONE(); }
void imp_glPopMatrix(void) { fe_pop_matrix(); DONE(); }
void imp_glMultMatrixf(void) { fe_mult_matrix((const float *)(g_mem + R0)); DONE(); }
void imp_glTranslatef(void) { fe_translate(F(R0), F(R1), F(R2)); DONE(); }
void imp_glFrustumf(void) { fe_frustum(F(R0), F(R1), F(R2), F(R3), F(STACK_ARG(0)), F(STACK_ARG(1))); DONE(); }
void imp_glOrthof(void) { fe_ortho(F(R0), F(R1), F(R2), F(R3), F(STACK_ARG(0)), F(STACK_ARG(1))); DONE(); }
void imp_glVertexPointer(void) { fe_vertex_pointer((int)R0, R1, (int)R2, R3); DONE(); }
void imp_glTexCoordPointer(void) { fe_texcoord_pointer((int)R0, R1, (int)R2, R3); DONE(); }
void imp_glColorPointer(void) { fe_color_pointer((int)R0, R1, (int)R2, R3); DONE(); }
void imp_glNormalPointer(void) { fe_normal_pointer(R0, (int)R1, R2); DONE(); }
void imp_glDrawArrays(void) { fe_draw_arrays(R0, (int)R1, (int)R2); DONE(); }
void imp_glDrawElements(void) { fe_draw_elements(R0, (int)R1, R2, R3); DONE(); }
void rt_note_mesh(uint32_t mesh) { fe_note_mesh(mesh); }
