/* gfx.c — batched 2D quads over OpenGL ES 1.1 (see gfx.h). */
#include <string.h>
#include <stddef.h>
#include "gl.h"
#include "gfx.h"
#include "platform.h"

#ifdef AB_NANO
extern void port_crumb(const char *, uint32_t, uint32_t);
#define GFX_TRACE(tag, a, b) port_crumb(tag, a, b)
#else
#define GFX_TRACE(tag, a, b) ((void)0)
#endif

#define MAX_TEX   64          /* the nano's GL driver reboots somewhere past ~80 live textures */
#define MAX_VERTS 1536        /* 256 quads per batch */
#define FRAME_VERTS 4096
#define FRAME_BATCHES 256
#define MERGE_LOOKBACK 8

typedef struct { float x, y, u, v; uint32_t c; } Vert;

static struct { unsigned gl; int w, h, pending_delete; uint32_t bytes; } sTex[MAX_TEX + 1];
static Vert sVerts[MAX_VERTS];
/* On the nano, interleaving texture uploads with draws can reboot the device.
 * Record a frame first; issue every draw only after its texture uploads finish. */
typedef struct {
    GLuint texture;
    int first, count, blend, clip_on;
    float clip[4];
    float bounds[4]; /* min x/y, max x/y in logical pixels */
} Batch;
static Vert sFrameVerts[FRAME_VERTS];
static Batch sBatches[FRAME_BATCHES];
static int sFrameCount, sBatchCount, sOverflow, sUploads;
static GLuint sVbo;
static uint32_t sTextureBudget = 3u << 20;
static int sCount, sCurTex = -1, sBlend = -1, sWantBlend;
static int sClipOn;
static float sClip[4];
static int sPanelW, sPanelH, sRot;
static float sLogW, sLogH;
static int sDraws, sQuads;
static float sPixelArea, sPixelX, sPixelY;
static int sMovedVerts, sDrawTextured, sDrawClipOn;
static GLuint sDrawTexture;
static float sDrawClip[4];

int gfx_stat_draws, gfx_stat_quads, gfx_stat_textures;
uint32_t gfx_stat_tex_bytes;
uint32_t gfx_stat_upload_us;
uint32_t gfx_stat_buffer_us;
int gfx_stat_merges, gfx_stat_state_calls;

int gfx_tex_create(int w, int h, int fmt, const void *pixels, int linear, int repeat)
{
    int id;
    GLuint name = 0;
    GLenum glfmt, type;
    uint32_t bytes = (uint32_t)w * (uint32_t)h * (fmt == GFX_FMT_RGBA8888 ? 4u : 2u);
#ifdef AB_NANO
    if (w < 1 || h < 1 || w > 256 || h > 256 || w * h > 32768 || w > h * 4 || h > w * 4) {
        plat_log("texture %dx%d exceeds device page limits", w, h);
        return 0;
    }
#endif
    if (bytes > sTextureBudget || gfx_stat_tex_bytes > sTextureBudget - bytes) {
        plat_log("texture budget exceeded: %u + %u > %u", gfx_stat_tex_bytes, bytes, sTextureBudget);
        return 0;
    }
    for (id = 1; id <= MAX_TEX && sTex[id].gl; id++) {}
    if (id > MAX_TEX)
        return 0;
    gfx_flush();
    glGenTextures(1, &name);
    if (!name)
        return 0;
    glBindTexture(GL_TEXTURE_2D, name);
    sCurTex = -1;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (fmt == GFX_FMT_RGB565) {
        glfmt = GL_RGB; type = GL_UNSIGNED_SHORT_5_6_5;
    } else if (fmt == GFX_FMT_RGBA4444) {
        glfmt = GL_RGBA; type = GL_UNSIGNED_SHORT_4_4_4_4;
    } else {
        glfmt = GL_RGBA; type = GL_UNSIGNED_BYTE;
    }
    GFX_TRACE("tex-upload", ((uint32_t)w << 16) | (uint32_t)h, gfx_stat_tex_bytes);
    port_checkpoint("texture-upload", fmt == GFX_FMT_RGB565 ? "RGB565" : "RGBA", bytes);
    GLenum prior_error = glGetError();
    if (prior_error) port_checkpoint("texture-state-error", "", prior_error);
    uint64_t upload_start = plat_time_us();
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)glfmt, w, h, 0, glfmt, type, pixels);
    gfx_stat_upload_us += (uint32_t)(plat_time_us() - upload_start);
    GLenum error = glGetError();
    if (error) {
        plat_log("texture %dx%d rejected by GL: 0x%x", w, h, (unsigned)error);
        port_checkpoint("texture-error", "upload rejected", error);
        glDeleteTextures(1, &name);
        return 0;
    }
    port_checkpoint("texture-ready", "", bytes);
    sUploads++;
    GFX_TRACE("tex-ready", name, gfx_stat_tex_bytes + bytes);
    sTex[id].gl = name;
    sTex[id].w = w;
    sTex[id].h = h;
    sTex[id].bytes = bytes;
    gfx_stat_textures++;
    gfx_stat_tex_bytes += sTex[id].bytes;
    return id;
}

void gfx_tex_destroy(int tex)
{
    GLuint name;
    if (tex <= 0 || tex > MAX_TEX || !sTex[tex].gl)
        return;
    gfx_flush();
    name = sTex[tex].gl;
    for (int i = 0; i < sBatchCount; i++) {
        if (sBatches[i].texture == name) {
            sTex[tex].pending_delete = 1;
            return; /* Keep its pixels and budget charged until the recorded draws finish. */
        }
    }
    glDeleteTextures(1, &name);
    gfx_stat_textures--;
    gfx_stat_tex_bytes -= sTex[tex].bytes;
    sTex[tex].gl = 0;
    sTex[tex].pending_delete = 0;
    sCurTex = -1;
}

void gfx_forget_all(void)
{
    sVbo = 0;
    memset(sTex, 0, sizeof(sTex));
    gfx_stat_textures = 0;
    gfx_stat_tex_bytes = 0;
    sCount = 0;
    sFrameCount = sBatchCount = 0;
    sCurTex = -1;
    sBlend = -1;
}

static void apply_clip(void)
{
    if (sDrawClipOn == sClipOn && (!sClipOn || !memcmp(sDrawClip, sClip, sizeof(sClip))))
        return;
    int changed = sDrawClipOn != sClipOn;
    sDrawClipOn = sClipOn;
    memcpy(sDrawClip, sClip, sizeof(sClip));
    if (!sClipOn) {
        glDisable(GL_SCISSOR_TEST);
        gfx_stat_state_calls++;
        return;
    }
    {
        /* logical -> panel pixels (GL window coordinates have y up) */
        float sx = (float)(sRot ? sPanelH : sPanelW) / sLogW;
        float sy = (float)(sRot ? sPanelW : sPanelH) / sLogH;
        int x0 = (int)(sClip[0] * sx), y0 = (int)(sClip[1] * sy);
        int x1 = (int)((sClip[0] + sClip[2]) * sx + 0.999f), y1 = (int)((sClip[1] + sClip[3]) * sy + 0.999f);
        if (changed) { glEnable(GL_SCISSOR_TEST); gfx_stat_state_calls++; }
        if (sRot)   /* logical x runs down the panel, logical y right to left */
            glScissor(sPanelW - y1, sPanelH - x1, y1 - y0, x1 - x0);
        else
            glScissor(x0, sPanelH - y1, x1 - x0, y1 - y0);
        gfx_stat_state_calls++;
    }
}

void gfx_begin_frame(int panel_w, int panel_h, float logical_w, float logical_h, int rot90,
                     float r, float g, float b)
{
    sPanelW = panel_w; sPanelH = panel_h; sRot = rot90;
    sLogW = logical_w; sLogH = logical_h;
    sPixelArea = (float)panel_w * panel_h / (logical_w * logical_h);
    sPixelX = logical_w / (float)(rot90 ? panel_h : panel_w);
    sPixelY = logical_h / (float)(rot90 ? panel_w : panel_h);
    sDraws = sQuads = 0;
    sCount = 0;
    sFrameCount = sBatchCount = sOverflow = 0;
    sUploads = 0;
    gfx_stat_upload_us = 0;
    gfx_stat_buffer_us = 0;
    gfx_stat_merges = gfx_stat_state_calls = sMovedVerts = 0;
    sDrawTextured = -1;
    sDrawTexture = 0;
    sDrawClipOn = 0; /* glDisable below establishes the actual state */
    sCurTex = -1;
    sBlend = -1;
    sWantBlend = GFX_BLEND_ALPHA;
    sClipOn = 0;

    glViewport(0, 0, panel_w, panel_h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    if (rot90)
        glRotatef(-90.f, 0.f, 0.f, 1.f);    /* logical right -> physical down */
    glOrthof(0.f, logical_w, logical_h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
#ifdef GL_ALPHA_TEST
    glDisable(GL_ALPHA_TEST);
#endif
    glShadeModel(GL_SMOOTH);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
}

void gfx_flush(void)
{
    if (!sCount)
        return;
    if (sFrameCount + sCount > FRAME_VERTS) {
        if (!sOverflow) plat_log("renderer: frame capacity exceeded");
        sOverflow = 1;
        sCount = 0;
        return;
    }
    Batch current;
    current.texture = sCurTex > 0 ? sTex[sCurTex].gl : 0;
    current.first = sFrameCount;
    current.count = sCount;
    current.blend = sWantBlend;
    current.clip_on = sClipOn;
    memcpy(current.clip, sClip, sizeof(sClip));
    current.bounds[0] = current.bounds[1] = 1e30f;
    current.bounds[2] = current.bounds[3] = -1e30f;
    for (int v = 0; v < sCount; v++) {
        float x = sVerts[v].x, y = sVerts[v].y;
        if (!(x >= -1e20f && x <= 1e20f && y >= -1e20f && y <= 1e20f)) {
            current.bounds[0] = current.bounds[1] = -1e30f;
            current.bounds[2] = current.bounds[3] = 1e30f;
            break; /* invalid coordinates must never permit reordering */
        }
        if (x < current.bounds[0]) current.bounds[0] = x;
        if (y < current.bounds[1]) current.bounds[1] = y;
        if (x > current.bounds[2]) current.bounds[2] = x;
        if (y > current.bounds[3]) current.bounds[3] = y;
    }
    /* Move this draw next to an earlier compatible one only if it cannot touch
     * any intervening draw. Include a physical-pixel margin for rasterization.
     * Limit both lookback and copying; no scratch allocation or texture growth. */
    for (int i = sBatchCount - 1; i >= 0 && i >= sBatchCount - MERGE_LOOKBACK; i--) {
        Batch *prior = &sBatches[i];
        if (prior->texture == current.texture && prior->blend == current.blend &&
            prior->clip_on == current.clip_on && (!current.clip_on || !memcmp(prior->clip, current.clip, sizeof(sClip)))) {
            if (prior->count + sCount > MAX_VERTS) break;
            int end = prior->first + prior->count, move = sFrameCount - end;
            if (sMovedVerts + move > FRAME_VERTS) break;
            memmove(sFrameVerts + end + sCount, sFrameVerts + end, (size_t)move * sizeof(Vert));
            memcpy(sFrameVerts + end, sVerts, (size_t)sCount * sizeof(Vert));
            for (int j = i + 1; j < sBatchCount; j++) sBatches[j].first += sCount;
            prior->count += sCount;
            for (int j = 0; j < 2; j++) {
                if (current.bounds[j] < prior->bounds[j]) prior->bounds[j] = current.bounds[j];
                if (current.bounds[j + 2] > prior->bounds[j + 2]) prior->bounds[j + 2] = current.bounds[j + 2];
            }
            sFrameCount += sCount;
            sMovedVerts += move;
            sCount = 0;
            gfx_stat_merges++;
            return;
        }
        if (!(current.bounds[2] + sPixelX < prior->bounds[0] || prior->bounds[2] + sPixelX < current.bounds[0] ||
              current.bounds[3] + sPixelY < prior->bounds[1] || prior->bounds[3] + sPixelY < current.bounds[1]))
            break;
    }
    if (sBatchCount == FRAME_BATCHES) {
        if (!sOverflow) plat_log("renderer: frame capacity exceeded");
        sOverflow = 1; sCount = 0; return;
    }
    sBatches[sBatchCount++] = current;
    memcpy(sFrameVerts + sFrameCount, sVerts, sCount * sizeof(Vert));
    sFrameCount += sCount;
    sCount = 0;
}

static void draw_batch(const Batch *batch)
{
    if (sBlend != batch->blend) {
        sBlend = batch->blend;
        if (sBlend == GFX_BLEND_NONE) {
            glDisable(GL_BLEND);
            gfx_stat_state_calls++;
        } else {
            glEnable(GL_BLEND);
            /* the game's images (and the colours passed in) are premultiplied */
            glBlendFunc(GL_ONE, sBlend == GFX_BLEND_ADD ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
            gfx_stat_state_calls += 2;
        }
    }
    sClipOn = batch->clip_on;
    memcpy(sClip, batch->clip, sizeof(sClip));
    apply_clip();
    int textured = batch->texture != 0;
    if (textured && sDrawTexture != batch->texture) {
        glBindTexture(GL_TEXTURE_2D, batch->texture);
        sDrawTexture = batch->texture;
        gfx_stat_state_calls++;
    }
    if (textured != sDrawTextured) {
        sDrawTextured = textured;
        if (textured) {
            glEnable(GL_TEXTURE_2D);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        } else {
            glDisable(GL_TEXTURE_2D);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
        gfx_stat_state_calls += 2;
    }
    glDrawArrays(GL_TRIANGLES, batch->first, batch->count);
    sDraws++;
}

void gfx_set_texture_budget(uint32_t bytes) { sTextureBudget = bytes; }

void gfx_end_frame(void)
{
    gfx_flush();
    if (sUploads) port_checkpoint("draw-begin", "new textures", sFrameCount);
    if (sFrameCount) {
        if (!sVbo) glGenBuffers(1, &sVbo);
        if (sVbo) {
            glBindBuffer(GL_ARRAY_BUFFER, sVbo);
            uint64_t buffer_start = plat_time_us();
            glBufferData(GL_ARRAY_BUFFER, sFrameCount * (GLsizeiptr)sizeof(Vert), sFrameVerts, GL_DYNAMIC_DRAW);
            gfx_stat_buffer_us = (uint32_t)(plat_time_us() - buffer_start);
            glVertexPointer(2, GL_FLOAT, sizeof(Vert), (const void *)offsetof(Vert, x));
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vert), (const void *)offsetof(Vert, c));
            glTexCoordPointer(2, GL_FLOAT, sizeof(Vert), (const void *)offsetof(Vert, u));
            gfx_stat_state_calls++; /* texture coordinates share one VBO throughout the frame */
            for (int i = 0; i < sBatchCount; i++) draw_batch(&sBatches[i]);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        } else {
            plat_log("renderer: vertex buffer allocation failed");
        }
    }
    sBatchCount = sFrameCount = 0;
    for (int i = 1; i <= MAX_TEX; i++)
        if (sTex[i].pending_delete) gfx_tex_destroy(i);
    glDisable(GL_SCISSOR_TEST);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    gfx_stat_draws = sDraws;
    gfx_stat_quads = sQuads;
    if (sUploads) port_checkpoint("draw-end", "new textures", sDraws);
}

void gfx_set_clip(float x, float y, float w, float h)
{
    int on = w > 0.f && h > 0.f;
    if (on == sClipOn && (!on || (x == sClip[0] && y == sClip[1] && w == sClip[2] && h == sClip[3])))
        return;
    gfx_flush();
    sClipOn = on;
    sClip[0] = x; sClip[1] = y; sClip[2] = w; sClip[3] = h;
}

void gfx_set_blend(int mode)
{
    if (mode == sWantBlend)
        return;
    gfx_flush();
    sWantBlend = mode;
}

static void bind(int tex)
{
    if (tex == sCurTex)
        return;
    gfx_flush();
    sCurTex = tex;
}

/* GL wants RGBA byte order in memory. */
static uint32_t pack(uint32_t argb)
{
    return (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFFu) | ((argb & 0xFFu) << 16);
}

/* The nano's GL driver reboots the iPod on zero-area triangles. */
static int degenerate(const float *a, const float *b, const float *c)
{
    float area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    area *= sPixelArea;
    return !(area < -0.25f || area > 0.25f); /* also reject NaNs; 1/8-pixel guard */
}

static void emit(const float *p, float u, float v, uint32_t c)
{
    Vert *o = &sVerts[sCount++];
    o->x = p[0]; o->y = p[1]; o->u = u; o->v = v; o->c = c;
}

void gfx_quad(int tex, int tex_w, int tex_h, const float xy[8], const float uv[8], uint32_t argb)
{
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    uint32_t c = pack(argb);
    float iu = 0.f, iv = 0.f;
    int t, k;
    if (tex < 0 || tex > MAX_TEX || (tex && !sTex[tex].gl))
        return;
    bind(tex);
    if (sCount + 6 > MAX_VERTS)
        gfx_flush();
    if (tex) {
        iu = 1.f / (float)tex_w;
        iv = 1.f / (float)tex_h;
    }
    for (t = 0; t < 2; t++) {
        const int *o = &order[t * 3];
        if (degenerate(&xy[o[0] * 2], &xy[o[1] * 2], &xy[o[2] * 2]))
            continue;
        for (k = 0; k < 3; k++) {
            int i = o[k];
            emit(&xy[i * 2], tex ? uv[i * 2] * iu : 0.f, tex ? uv[i * 2 + 1] * iv : 0.f, c);
        }
    }
    sQuads++;
}

void gfx_tri(const float xy[6], uint32_t argb)
{
    uint32_t c = pack(argb);
    int k;
    if (degenerate(&xy[0], &xy[2], &xy[4]))
        return;
    bind(0);
    if (sCount + 3 > MAX_VERTS)
        gfx_flush();
    for (k = 0; k < 3; k++)
        emit(&xy[k * 2], 0.f, 0.f, c);
}
