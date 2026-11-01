/* tex.c — texture loading (cTextureManager::loadPVRTexture, 0x52374, on the phone). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl.h"
#include "platform.h"
#include "tex.h"

#ifndef GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG
#define GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG 0x8C00
#endif
#ifndef GL_LINEAR_MIPMAP_NEAREST
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

int tex_last_kind;
unsigned tex_bytes;

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

unsigned tex_load(const char *name) {
    char file[64];
    uint32_t size = 0;
    GLuint t = 0;
    glGenTextures(1, &t);
    if (!t) return 0;
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    tex_last_kind = 0;
#ifdef AB_NANO
    /* The original file: a 52-byte PVR v2 header, then PVRTC 4bpp levels, largest first. */
    snprintf(file, sizeof file, "%s.pvr", name);
    uint8_t *d = plat_read_file(file, &size, 0);
    if (d && size > 52 && u32(d) == 52 && !memcmp(d + 44, "PVR!", 4)) {
        uint32_t h = u32(d + 4), w = u32(d + 8), mips = u32(d + 12), total = u32(d + 20);
        const uint8_t *p = d + 52, *end = d + 52 + total;
        if (end <= d + size) {
            while (glGetError()) {}
            uint32_t lw = w, lh = h, level = 0, used = 0;
            for (; level <= mips && p < end; level++) {
                uint32_t bw = lw < 8 ? 8 : lw, bh = lh < 8 ? 8 : lh, bytes = bw * bh / 2;
                if (p + bytes > end) break;
                glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG,
                                       (GLsizei)lw, (GLsizei)lh, 0, (GLsizei)bytes, p);
                if (glGetError()) break;
                used += bytes;
                p += bytes;
                if (lw == 1 && lh == 1) { level++; break; }
                lw = lw > 1 ? lw / 2 : 1;
                lh = lh > 1 ? lh / 2 : 1;
            }
            if (level > mips) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
                tex_last_kind = 1;
            } else if (level >= 1) {
                tex_last_kind = 2;
            }
            if (tex_last_kind) tex_bytes += used;
            plat_log("texture %s: PVRTC %ux%u, %u of %u levels accepted", name, (unsigned)w, (unsigned)h,
                     (unsigned)level, (unsigned)mips + 1u);
        }
    }
    free(d);
    if (!tex_last_kind) {
        /* 16-bit copy made by the converter, small enough for the sizes proven on the nano */
        snprintf(file, sizeof file, "%s.565", name);
        d = plat_read_file(file, &size, 0);
        if (d && size > 8) {
            uint32_t w = u32(d), h = u32(d + 4);
            if (size == 8 + w * h * 2) {
                while (glGetError()) {}
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei)w, (GLsizei)h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, d + 8);
                if (!glGetError()) { tex_last_kind = 3; tex_bytes += w * h * 2; }
                plat_log("texture %s: RGB565 %ux%u %s", name, (unsigned)w, (unsigned)h, tex_last_kind ? "ok" : "rejected");
            }
        }
        free(d);
    }
#else
    snprintf(file, sizeof file, "%s.rgba", name);
    uint8_t *d = plat_read_file(file, &size, 0);
    if (d && size > 8 && size == 8 + u32(d) * u32(d + 4) * 4) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)u32(d), (GLsizei)u32(d + 4), 0, GL_RGBA, GL_UNSIGNED_BYTE, d + 8);
        tex_last_kind = 4;
        tex_bytes += u32(d) * u32(d + 4) * 4;
    }
    free(d);
#endif
    if (!tex_last_kind) {
        glDeleteTextures(1, &t);
        plat_log("texture %s: could not be loaded", name);
        return 0;
    }
    return t;
}
