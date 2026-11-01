#ifndef TR_TEX_H
#define TR_TEX_H
/* Loads a texture by the game's own name ("wallTexture"). Returns the GL name, 0 on failure.
 * On the nano the original PVRTC file is uploaded as it is; the Mac build reads the decoded
 * copy the converter wrote (desktop GL has no PVRTC). */
unsigned tex_load(const char *name);
extern int tex_last_kind;       /* 1 PVRTC with mipmaps, 2 PVRTC top level only, 3 RGB565 fallback, 4 decoded RGBA */
extern unsigned tex_bytes;      /* texture memory in use */
#endif
