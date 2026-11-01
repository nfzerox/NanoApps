/* viewer.c — first milestone on the Mac: draw the original models with the original
 * textures (base + lightmap) and the runner's keyframes, headless, to PNG.
 *   viewer <app dir> <decoded texture dir> <out.png> [player frame] */
#define GL_SILENCE_DEPRECATION 1
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include "../src/mesh.h"

static const char *sApp, *sTex;
static int W = 240 * 3, H = 432 * 3;

static uint8_t *read_file(const char *dir, const char *name, uint32_t *size) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    fread(d, 1, (size_t)n, f); fclose(f);
    *size = (uint32_t)n;
    return d;
}
static TRMesh *model(const char *name) {
    uint32_t size; uint8_t *d = read_file(sApp, name, &size);
    TRMesh *m = mesh_load(d, size);
    if (!m) { fprintf(stderr, "bad model %s\n", name); exit(1); }
    free(d);
    return m;
}
static GLuint texture(const char *name) {
    uint32_t size; uint8_t *d = read_file(sTex, name, &size);
    uint32_t w, h; memcpy(&w, d, 4); memcpy(&h, d + 4, 4);
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d + 8);
    free(d);
    return t;
}
static void draw(const TRMesh *m, int frame, GLuint base, GLuint light, float x, float y, float z, float scale) {
    const TRFrame *f = &m->frames[frame];
    const uint8_t *v = m->vertices + (size_t)f->vertex_start * m->vertex_size;
    glPushMatrix();
    glTranslatef(x, y, z);
    glScalef(scale, scale, scale);
    glVertexPointer(3, GL_FLOAT, m->vertex_size, v + m->pos_offset);
    glActiveTexture(GL_TEXTURE0); glClientActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, base);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, m->vertex_size, v + m->uv_offset[0]);
    glActiveTexture(GL_TEXTURE1); glClientActiveTexture(GL_TEXTURE1);
    if (light && m->uv_count > 1) {
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, light);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, m->vertex_size, v + m->uv_offset[1]);
    } else {
        glDisable(GL_TEXTURE_2D); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glDrawElements(GL_TRIANGLES, f->index_count, GL_UNSIGNED_SHORT, m->indices + f->index_start);
    glPopMatrix();
}

static void put32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len) {
    unsigned char b[4]; uint32_t crc = crc32(0, (const unsigned char *)type, 4);
    if (len) crc = crc32(crc, data, len);
    put32(b, len); fwrite(b, 1, 4, f); fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    put32(b, crc); fwrite(b, 1, 4, f);
}
static void screenshot(const char *path) {
    unsigned char *px = malloc((size_t)W * H * 4), *raw = malloc((size_t)(W * 3 + 1) * H), hdr[13];
    uLongf zlen = compressBound((uLong)(W * 3 + 1) * H); unsigned char *z = malloc(zlen);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < H; y++) {
        unsigned char *row = raw + (size_t)y * (W * 3 + 1), *src = px + (size_t)(H - 1 - y) * W * 4;
        row[0] = 0;
        for (int x = 0; x < W; x++) { row[1 + x * 3] = src[x * 4]; row[2 + x * 3] = src[x * 4 + 1]; row[3 + x * 3] = src[x * 4 + 2]; }
    }
    compress2(z, &zlen, raw, (uLong)(W * 3 + 1) * H, 6);
    FILE *f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    put32(hdr, (uint32_t)W); put32(hdr + 4, (uint32_t)H); hdr[8] = 8; hdr[9] = 2; hdr[10] = hdr[11] = hdr[12] = 0;
    chunk(f, "IHDR", hdr, 13); chunk(f, "IDAT", z, (uint32_t)zlen); chunk(f, "IEND", NULL, 0);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    sApp = argv[1]; sTex = argv[2];
    int frame = argc > 4 ? atoi(argv[4]) : 0;
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
                                        kCGLPFADepthSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pf; GLint npf; CGLContextObj ctx;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf || CGLCreateContext(pf, NULL, &ctx)) return 1;
    CGLSetCurrentContext(ctx);
    GLuint fbo, rb[2];
    glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(2, rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[0]);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, W, H);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb[0]);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[1]);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24, W, H);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, rb[1]);

    TRMesh *straight = model("templeStraightVert.bksb"), *tower = model("templeStraightTowerVert.bksb"),
           *turn = model("templeTDown.bksb"), *player = model("player.bksb"), *enemy = model("enemy.bksb"),
           *tree = model("treeAlone2.bksb");
    GLuint wall = texture("wallTexture.rgba"), light = texture("lightMapTexture.rgba"),
           treeTex = texture("treeTexture.rgba"), playerTex = texture("playerTexture.rgba"),
           enemyTex = texture("enemyTexture.rgba");

    /* the game's own defaults (ImangiConfigDefaults.plist) */
    float fog[4] = { 50 / 255.f, 82 / 255.f, 86 / 255.f, 1 };
    glViewport(0, 0, W, H);
    glClearColor(fog[0], fog[1], fog[2], 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); if (getenv("TR_CULL")) { glEnable(GL_CULL_FACE); glFrontFace(atoi(getenv("TR_CULL")) ? GL_CW : GL_CCW); }
    glEnable(GL_FOG); glFogi(GL_FOG_MODE, GL_LINEAR); glFogf(GL_FOG_START, 100); glFogf(GL_FOG_END, 400); glFogfv(GL_FOG_COLOR, fog);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    float fov = 50.f, near = 1.f, far = 400.f, top = near * tanf(fov * 3.14159265f / 360.f), right = top * (float)W / H;
    glFrustum(-right, right, -top, top, near, far);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    /* camera 50 behind and 35 above the runner, looking 20 above them, running towards -z */
    float eye[3] = { 0, 35, 50 }, at[3] = { 0, 20, 0 };
    float fwd[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
    float len = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    for (int i = 0; i < 3; i++) fwd[i] /= len;
    float side[3] = { -fwd[2], 0, fwd[0] };                    /* fwd x up(0,1,0) */
    len = sqrtf(side[0] * side[0] + side[2] * side[2]); side[0] /= len; side[2] /= len;
    float up[3] = { side[1] * fwd[2] - side[2] * fwd[1], side[2] * fwd[0] - side[0] * fwd[2], side[0] * fwd[1] - side[1] * fwd[0] };
    float view[16] = { side[0], up[0], -fwd[0], 0, side[1], up[1], -fwd[1], 0, side[2], up[2], -fwd[2], 0, 0, 0, 0, 1 };
    glMultMatrixf(view);
    glTranslatef(-eye[0], -eye[1], -eye[2]);

    glEnableClientState(GL_VERTEX_ARRAY);
    glColor4f(1, 1, 1, 1);
    for (int i = -1; i < 6; i++)
        draw(i == 2 ? tower : i == 5 ? turn : straight, 0, wall, light, 0, 0, -60.f * i, 1);
    draw(tree, 0, treeTex, light, -40, 0, -150, 1);
    draw(player, frame % player->frame_count, playerTex, 0, 0, 0, 0, 1.5f);
    for (int i = 0; i < 3; i++)
        draw(enemy, (frame + i * 5) % enemy->frame_count, enemyTex, 0, -8.f + 8.f * i, 0, 22.f + 4.f * (i & 1), 1);
    glFinish();
    screenshot(argv[3]);
    printf("player %d frames x %d verts; straight piece %d tris\n", player->frame_count, player->frames[0].vertex_count,
           straight->frames[0].index_count / 3);
    return 0;
}
