/* gl.h (host) — the engine includes "gl.h" for OpenGL ES 1.1; on the Mac that is the
 * legacy desktop OpenGL with the few ES-only names mapped. */
#ifndef AB_HOST_GL_H
#define AB_HOST_GL_H
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#define glOrthof(l, r, b, t, n, f)   glOrtho((l), (r), (b), (t), (n), (f))
#define glFrustumf(l, r, b, t, n, f) glFrustum((l), (r), (b), (t), (n), (f))
#define glClearDepthf(d)             glClearDepth(d)
#endif
