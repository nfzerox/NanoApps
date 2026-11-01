/*
 * blocks — a small Minecraft-style voxel sandbox. A GL_SURFACE .hbapp: the resident
 * injects a fullscreen GL view and calls gl_app_frame() between its begin/end scene.
 *
 * World: 32 x 32 columns, 24 blocks tall, generated from value noise (grass/dirt/stone,
 * sand in the lows, trees). Saved to /Data/Blocks/world.bin a moment after every
 * edit and periodically while you move, so there is no exit hook to rely on.
 *
 * Controls (single touch):
 *   drag in the world       look around
 *   tap a block             place the selected block against the face you tapped
 *   hold on a block         break it (keep holding to keep digging)
 *   hotbar                  pick the block to place
 *   arrows / jump           walk, strafe, jump (walking into a 1-block step auto-jumps)
 *   rotate (top-right)      cycle portrait / landscape (Home right) / landscape (Home left);
 *                           landscape overlays the controls on a full-screen view
 *
 * Rendering: the world is split into 4x4 chunks of 8x8 columns. Each chunk's visible
 * faces are meshed into one VBO (pos3 + uv2 floats, 6 verts per face) and drawn with a
 * single glDrawArrays. Directional shading is baked into the texture atlas (four shade
 * rows) so there is no colour array and one draw per chunk. Only the touched chunk (and
 * a neighbour when the edit is on a border) is re-meshed after an edit.
 */
#include "hb_gl_surface.h"
#include "hb_surface_input.h"
#include "hb_sdk.h"
#include "gl.h"

/* ============================ world =================================== */

#define WX 32
#define WZ 32
#define WY 24
#define CH 8                         /* chunk width in columns          */
#define NCX (WX/CH)
#define NCZ (WZ/CH)
#define NCHUNK (NCX*NCZ)

enum { AIR, GRASS, DIRT, STONE, COBBLE, PLANKS, LOG, LEAVES, SAND, BRICK, BEDROCK, NBLOCK };

#define SAVE_DIR   "/Data/Blocks"
#define SAVE_PATH  "/Data/Blocks/world.bin"
#define SAVE_MAGIC 0x314B4C42u       /* 'BLK1' */

typedef struct {
    uint32_t magic, version;
    float    px, py, pz, yaw, pitch;
    uint32_t slot;
    uint8_t  blk[WY][WZ][WX];
} save_t;

static save_t g_save;
#define B(x,y,z) g_save.blk[y][z][x]

static int in_world(int x, int y, int z)
{ return x >= 0 && x < WX && y >= 0 && y < WY && z >= 0 && z < WZ; }

/* Solid for collision: outside the world is a wall on the sides and floor, open above. */
static int solid_at(int x, int y, int z)
{
    if (y >= WY) return 0;
    if (!in_world(x, y, z)) return 1;
    return B(x, y, z) != AIR;
}

/* ============================ math ==================================== */

#define PI_  3.14159265f
#define TAU_ 6.28318531f
#define DEG  0.017453293f

static float fsinf(float x)
{
    while (x >  PI_) x -= TAU_;
    while (x < -PI_) x += TAU_;
    float x2 = x*x;
    return x*(1.f - x2*(0.16666667f - x2*(0.00833333f - x2*0.0001984127f)));
}
static float fcosf(float x) { return fsinf(x + 1.57079633f); }
static int   ifloor(float v) { int i = (int)v; return (v < (float)i) ? i - 1 : i; }
static float fabsf_(float v) { return v < 0 ? -v : v; }
static float fsqrtf(float v)
{
    if (v <= 0.f) return 0.f;
    float r = v > 1.f ? v : 1.f;
    for (int i = 0; i < 12; i++) r = 0.5f * (r + v / r);
    return r;
}

static uint32_t hash3(int x, int y, int s)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float hashf(int x, int y, int s) { return (float)(hash3(x, y, s) & 0xffff) / 65535.f; }

/* smooth value noise on a `cell`-sized lattice, 0..1 */
static float vnoise(float x, float y, int cell, int s)
{
    float fx = x / (float)cell, fy = y / (float)cell;
    int ix = ifloor(fx), iy = ifloor(fy);
    float tx = fx - ix, ty = fy - iy;
    tx = tx*tx*(3.f - 2.f*tx); ty = ty*ty*(3.f - 2.f*ty);
    float a = hashf(ix, iy, s),   b = hashf(ix+1, iy, s);
    float c = hashf(ix, iy+1, s), d = hashf(ix+1, iy+1, s);
    return (a + (b-a)*tx) + ((c + (d-c)*tx) - (a + (b-a)*tx))*ty;
}

/* ============================ terrain ================================= */

static void put_tree(int x, int y, int z)
{
    for (int ly = y+2; ly <= y+5; ly++)
        for (int dz = -2; dz <= 2; dz++)
            for (int dx = -2; dx <= 2; dx++) {
                int r = (ly >= y+4) ? 1 : 2;
                if (dx < -r || dx > r || dz < -r || dz > r) continue;
                if ((dx == -r || dx == r) && (dz == -r || dz == r) && hash3(x+dx, z+dz, ly) & 1) continue;
                if (in_world(x+dx, ly, z+dz) && B(x+dx, ly, z+dz) == AIR) B(x+dx, ly, z+dz) = LEAVES;
            }
    for (int ly = y; ly < y+5 && ly < WY; ly++) B(x, ly, z) = LOG;
}

static void generate(int seed)
{
    for (int z = 0; z < WZ; z++)
        for (int x = 0; x < WX; x++) {
            float n = vnoise((float)x, (float)z, 12, seed) * 0.7f
                    + vnoise((float)x, (float)z, 5, seed + 1) * 0.3f;
            int top = 5 + (int)(n * 10.f);             /* 5..14 */
            for (int y = 0; y < WY; y++) {
                uint8_t b = AIR;
                if (y == 0)            b = BEDROCK;
                else if (y < top - 3)  b = STONE;
                else if (y < top)      b = (top <= 7) ? SAND : DIRT;
                else if (y == top)     b = (top <= 7) ? SAND : GRASS;
                B(x, y, z) = b;
            }
        }
    for (int z = 3; z < WZ-3; z++)
        for (int x = 3; x < WX-3; x++) {
            if (hash3(x, z, seed + 7) % 53 != 0) continue;
            int y = WY - 1;
            while (y > 0 && B(x, y, z) == AIR) y--;
            if (B(x, y, z) == GRASS && y + 7 < WY) put_tree(x, y + 1, z);
        }
}

/* ============================ atlas =================================== */

/* 12 tiles x 4 shade rows (top, x-sides, z-sides, bottom) in an 8x8 grid of 16px tiles. */
enum { T_GRASS_TOP, T_GRASS_SIDE, T_DIRT, T_STONE, T_COBBLE, T_PLANKS, T_LOG_SIDE,
       T_LOG_TOP, T_LEAVES, T_SAND, T_BRICK, T_BEDROCK, NTILE };
#define ATLAS 128
#define TPX   16
static uint8_t g_atlas[ATLAS*ATLAS*4];
static const uint8_t SHADE[4] = { 255, 204, 170, 135 };

static void tex_px(uint8_t *p, int r, int g, int b)
{
    p[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    p[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
    p[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    p[3] = 255;
}

static void tile_pixel(int t, int x, int y, uint8_t *p)
{
    int n = (int)(hash3(x, y, t * 31 + 5) & 31) - 16;       /* per-pixel grain */
    switch (t) {
    case T_GRASS_TOP: tex_px(p, 92 + n, 160 + n, 52 + n/2); break;
    case T_DIRT:      tex_px(p, 134 + n, 96 + n, 67 + n); break;
    case T_GRASS_SIDE: {
        int edge = 3 + (int)(hash3(x, 0, 99) % 3);
        if (y < edge) tex_px(p, 92 + n, 160 + n, 52 + n/2);
        else          tex_px(p, 134 + n, 96 + n, 67 + n);
        break;
    }
    case T_STONE: { int v = 126 + n + ((hash3(x/3, y/2, 17) & 3) == 0 ? -18 : 0); tex_px(p, v, v, v); break; }
    case T_COBBLE: {
        int row = y / 5, off = (row & 1) * 3, cx = (x + off) / 6;
        int ex = (x + off) % 6 == 0, ey = y % 5 == 0;
        int v = (ex || ey) ? 70 + n/2 : 120 + (int)(hash3(cx, row, 3) % 40) + n/2;
        tex_px(p, v, v, v);
        break;
    }
    case T_PLANKS: {
        int line = (y % 4) == 3, seam = ((x + (y/4)*5) % 16) == 0;
        int d = (line || seam) ? -45 : 0;
        tex_px(p, 170 + n/2 + d, 136 + n/2 + d, 82 + n/2 + d);
        break;
    }
    case T_LOG_SIDE: { int s = ((x + (int)(hash3(x, y/4, 8) & 1)) % 4 == 0) ? -30 : 0;
                       tex_px(p, 104 + n/2 + s, 80 + n/2 + s, 50 + n/2 + s); break; }
    case T_LOG_TOP: {
        int dx = x*2 - 15, dy = y*2 - 15, r = (dx*dx + dy*dy);
        if (r > 196) tex_px(p, 104 + n/2, 80 + n/2, 50 + n/2);
        else { int ring = (r / 40) & 1; tex_px(p, 182 + n/2 - ring*30, 150 + n/2 - ring*30, 96 + n/2 - ring*20); }
        break;
    }
    case T_LEAVES: {
        uint32_t h = hash3(x, y, 77) & 7;
        if (h == 0) tex_px(p, 24, 52, 20);
        else tex_px(p, 52 + n + (int)h*4, 118 + n + (int)h*5, 38 + n);
        break;
    }
    case T_SAND:    tex_px(p, 219 + n/2, 206 + n/2, 160 + n/2); break;
    case T_BRICK: {
        int row = y / 4, off = (row & 1) * 4;
        int mortar = (y % 4 == 3) || ((x + off) % 8 == 7);
        if (mortar) tex_px(p, 170 + n/3, 164 + n/3, 156 + n/3);
        else        tex_px(p, 150 + n, 68 + n/2, 52 + n/2);
        break;
    }
    default: { int v = 60 + n*2; tex_px(p, v, v, v); break; }   /* bedrock */
    }
}

static void build_atlas(void)
{
    for (int s = 0; s < 4; s++)
        for (int t = 0; t < NTILE; t++) {
            int idx = s*NTILE + t, ox = (idx % 8) * TPX, oy = (idx / 8) * TPX;
            for (int y = 0; y < TPX; y++)
                for (int x = 0; x < TPX; x++) {
                    uint8_t *p = &g_atlas[((oy + y)*ATLAS + ox + x)*4];
                    tile_pixel(t, x, y, p);
                    for (int c = 0; c < 3; c++) p[c] = (uint8_t)(p[c] * SHADE[s] / 255);
                }
        }
}

/* face: 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z */
static int tile_for(int b, int face)
{
    switch (b) {
    case GRASS:  return face == 2 ? T_GRASS_TOP : face == 3 ? T_DIRT : T_GRASS_SIDE;
    case DIRT:   return T_DIRT;
    case STONE:  return T_STONE;
    case COBBLE: return T_COBBLE;
    case PLANKS: return T_PLANKS;
    case LOG:    return (face == 2 || face == 3) ? T_LOG_TOP : T_LOG_SIDE;
    case LEAVES: return T_LEAVES;
    case SAND:   return T_SAND;
    case BRICK:  return T_BRICK;
    default:     return T_BEDROCK;
    }
}
static int shade_for(int face) { return face == 2 ? 0 : face == 3 ? 3 : face < 2 ? 1 : 2; }

static void tile_rect(int idx, float *u0, float *v0, float *u1, float *v1)
{
    const float e = 0.25f / ATLAS;                          /* inset: no neighbour bleed */
    *u0 = (float)((idx % 8) * TPX) / ATLAS + e;
    *v0 = (float)((idx / 8) * TPX) / ATLAS + e;
    *u1 = *u0 + (float)TPX / ATLAS - 2*e;
    *v1 = *v0 + (float)TPX / ATLAS - 2*e;
}

static void tile_uv(int b, int face, float *u0, float *v0, float *u1, float *v1)
{ tile_rect(shade_for(face)*NTILE + tile_for(b, face), u0, v0, u1, v1); }

/* ============================ meshing ================================= */

#define MAXF_CHUNK 1400
#define VSTRIDE    5                         /* floats per vertex: x y z u v */
static float    g_mesh[MAXF_CHUNK * 6 * VSTRIDE];
static int      g_chunk_faces[NCHUNK];
static uint8_t  g_chunk_dirty[NCHUNK];

#define VBO_BASE 70                          /* fixed ids, reused every launch */
#define TEX_ID   250

/* corners of each face, CCW seen from outside; (u,v) per corner from the tile rect */
static const int8_t FACE_V[6][4][3] = {
    {{1,0,1},{1,0,0},{1,1,0},{1,1,1}},       /* +x */
    {{0,0,0},{0,0,1},{0,1,1},{0,1,0}},       /* -x */
    {{0,1,1},{1,1,1},{1,1,0},{0,1,0}},       /* +y */
    {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},       /* -y */
    {{0,0,1},{1,0,1},{1,1,1},{0,1,1}},       /* +z */
    {{1,0,0},{0,0,0},{0,1,0},{1,1,0}},       /* -z */
};
static const int8_t FACE_N[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};

static int mesh_chunk(int ci)
{
    int cx0 = (ci % NCX) * CH, cz0 = (ci / NCX) * CH, nf = 0;
    float *o = g_mesh;
    for (int y = 0; y < WY; y++)
        for (int z = cz0; z < cz0 + CH; z++)
            for (int x = cx0; x < cx0 + CH; x++) {
                int b = B(x, y, z);
                if (b == AIR) continue;
                for (int f = 0; f < 6; f++) {
                    int nx = x + FACE_N[f][0], ny = y + FACE_N[f][1], nz = z + FACE_N[f][2];
                    if (ny >= WY) { /* open sky: draw */ }
                    else if (!in_world(nx, ny, nz) || B(nx, ny, nz) != AIR) continue;
                    if (nf >= MAXF_CHUNK) return nf;
                    float u0, v0, u1, v1;
                    tile_uv(b, f, &u0, &v0, &u1, &v1);
                    /* corner uv: 0=(u0,v1) 1=(u1,v1) 2=(u1,v0) 3=(u0,v0) — v0 is the tile top */
                    const float cu[4] = { u0, u1, u1, u0 }, cv[4] = { v1, v1, v0, v0 };
                    static const int TRI[6] = { 0, 1, 2, 0, 2, 3 };
                    for (int k = 0; k < 6; k++) {
                        int c = TRI[k];
                        *o++ = (float)(x + FACE_V[f][c][0]);
                        *o++ = (float)(y + FACE_V[f][c][1]);
                        *o++ = (float)(z + FACE_V[f][c][2]);
                        *o++ = cu[c]; *o++ = cv[c];
                    }
                    nf++;
                }
            }
    return nf;
}

static void upload_chunk(int ci)
{
    int nf = mesh_chunk(ci);
    g_chunk_faces[ci] = nf;
    g_chunk_dirty[ci] = 0;
    if (nf == 0) return;
    glBindBuffer(GL_ARRAY_BUFFER, VBO_BASE + ci);
    glBufferData(GL_ARRAY_BUFFER, nf * 6 * VSTRIDE * (int)sizeof(float), g_mesh, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void mark_dirty(int x, int z)
{
    int cx = x / CH, cz = z / CH;
    g_chunk_dirty[cz*NCX + cx] = 1;
    if (x % CH == 0      && cx > 0)       g_chunk_dirty[cz*NCX + cx - 1] = 1;
    if (x % CH == CH - 1 && cx < NCX - 1) g_chunk_dirty[cz*NCX + cx + 1] = 1;
    if (z % CH == 0      && cz > 0)       g_chunk_dirty[(cz-1)*NCX + cx] = 1;
    if (z % CH == CH - 1 && cz < NCZ - 1) g_chunk_dirty[(cz+1)*NCX + cx] = 1;
}

/* ============================ player ================================== */

#define PW     0.3f      /* half width  */
#define PH     1.8f      /* height      */
#define EYE    1.62f
#define GRAV   22.f
#define JUMPV  7.6f
#define SPEED  4.3f

static float g_vy;
static int   g_ground;
static float g_dt;

static int box_hits(float px, float py, float pz)
{
    int x0 = ifloor(px - PW), x1 = ifloor(px + PW - 0.001f);
    int y0 = ifloor(py),      y1 = ifloor(py + PH - 0.001f);
    int z0 = ifloor(pz - PW), z1 = ifloor(pz + PW - 0.001f);
    for (int y = y0; y <= y1; y++)
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++)
                if (solid_at(x, y, z)) return 1;
    return 0;
}

static void spawn(void)
{
    int x = WX/2, z = WZ/2, y = WY - 1;
    while (y > 0 && B(x, y, z) == AIR) y--;
    g_save.px = x + 0.5f; g_save.py = (float)(y + 1); g_save.pz = z + 0.5f;
    g_save.yaw = 0.f; g_save.pitch = -10.f;
}

static void unstick(void)
{
    while (g_save.py < WY && box_hits(g_save.px, g_save.py, g_save.pz)) g_save.py += 1.f;
}

/* Move along one axis, snapping to the blocking face. Returns 1 if blocked. */
static int move_axis(int axis, float d)
{
    float *p = axis == 0 ? &g_save.px : axis == 1 ? &g_save.py : &g_save.pz;
    float old = *p;
    *p += d;
    if (!box_hits(g_save.px, g_save.py, g_save.pz)) return 0;
    if (axis == 1) {
        if (d < 0) *p = (float)ifloor(*p) + 1.f;
        else       *p = (float)ifloor(*p + PH) - PH - 0.001f;
    } else {
        if (d > 0) *p = (float)ifloor(*p + PW) - PW - 0.001f;
        else       *p = (float)ifloor(*p - PW) + 1.f + PW + 0.001f;
    }
    if (box_hits(g_save.px, g_save.py, g_save.pz)) *p = old;
    return 1;
}

static void physics(float mx, float mz, int jump)
{
    if (jump && g_ground) g_vy = JUMPV;
    g_vy -= GRAV * g_dt;
    if (g_vy < -30.f) g_vy = -30.f;

    /* substeps keep each move well under a block */
    int steps = 1 + (int)((fabsf_(g_vy) + SPEED) * g_dt / 0.3f);
    float sdt = g_dt / steps;
    for (int i = 0; i < steps; i++) {
        float fy = g_save.py;
        int bx = mx != 0.f && move_axis(0, mx * sdt);
        int bz = mz != 0.f && move_axis(2, mz * sdt);
        /* auto-jump a single block step when walking into it */
        if ((bx || bz) && g_ground) {
            float tx = g_save.px + (mx > 0 ? 0.5f : mx < 0 ? -0.5f : 0.f);
            float tz = g_save.pz + (mz > 0 ? 0.5f : mz < 0 ? -0.5f : 0.f);
            if (!box_hits(tx, fy + 1.05f, tz) && !box_hits(g_save.px, fy + 1.05f, g_save.pz))
                g_vy = JUMPV;
        }
        g_ground = 0;
        if (move_axis(1, g_vy * sdt)) {
            if (g_vy < 0) g_ground = 1;
            g_vy = 0.f;
        }
    }
    if (g_save.py < -10.f) { spawn(); unstick(); g_vy = 0.f; }
}

/* ============================ orientation ============================= */

/* 0 portrait, 1 landscape with the Home button on the right, 2 landscape with it on
 * the left. Kept in its own file so the world save format is untouched. Everything
 * below works in logical (as-held) coordinates; only touch input, the viewport and
 * the projection know about the physical portrait framebuffer. */
#define ORIENT_PATH "/Data/Blocks/orient.cfg"
static int g_orient = 1;
static int g_lw, g_lh;                       /* logical screen size */

static void load_orient(void)
{
    uint8_t o;
    if (hb_fs_read(ORIENT_PATH, &o, 1) == 1 && o < 3) g_orient = o;
}

static void save_orient(void)
{
    uint8_t o = (uint8_t)g_orient;
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(SAVE_DIR);
    hb_fs_write(ORIENT_PATH, &o, 1);
}

static void set_logical_size(int w, int h)
{
    if (g_orient) { g_lw = h; g_lh = w; }
    else          { g_lw = w; g_lh = h; }
}

static void to_logical(int w, int h, int px, int py, int *lx, int *ly)
{
    if (g_orient == 1)      { *lx = py;         *ly = w - 1 - px; }
    else if (g_orient == 2) { *lx = h - 1 - py; *ly = px; }
    else                    { *lx = px;         *ly = py; }
}

/* ============================ camera / picking ======================== */

#define HUD_H   104
#define BAR_H   36                           /* hotbar row, top of the HUD */
#define ZNEAR   0.08f
#define ZFAR    60.f
#define HFOV_K  0.62f                        /* near-plane half width / near (portrait) */
#define HFOV_KL 0.75f                        /* same, landscape */

static int g_vw, g_vh;                       /* 3D view size, logical */

static float fov_k(void) { return g_orient ? HFOV_KL : HFOV_K; }

static void screen_ray(int tx, int ty, float *d)
{
    float nx = ((tx + 0.5f) / g_vw) * 2.f - 1.f;
    float ny = 1.f - ((ty + 0.5f) / g_vh) * 2.f;
    float hw = fov_k(), hh = hw * g_vh / g_vw;
    float x = nx * hw, y = ny * hh, z = -1.f;
    float yaw = g_save.yaw * DEG, pit = g_save.pitch * DEG;
    float cp = fcosf(pit), sp = fsinf(pit), cy = fcosf(yaw), sy = fsinf(yaw);
    float y1 = y*cp - z*sp, z1 = y*sp + z*cp;
    d[0] = x*cy + z1*sy;
    d[1] = y1;
    d[2] = -x*sy + z1*cy;
    float inv = 1.f / fsqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    d[0] *= inv; d[1] *= inv; d[2] *= inv;
}

/* Voxel DDA from the eye along unit d. hit = first solid cell, prev = the empty cell before it. */
static int raycast(const float *d, int *hit, int *prev)
{
    float ox = g_save.px, oy = g_save.py + EYE, oz = g_save.pz;
    int ix = ifloor(ox), iy = ifloor(oy), iz = ifloor(oz);
    int sx = d[0] > 0 ? 1 : -1, sy = d[1] > 0 ? 1 : -1, sz = d[2] > 0 ? 1 : -1;
    float big = 1e9f;
    float tdx = d[0] != 0 ? fabsf_(1.f / d[0]) : big;
    float tdy = d[1] != 0 ? fabsf_(1.f / d[1]) : big;
    float tdz = d[2] != 0 ? fabsf_(1.f / d[2]) : big;
    float tmx = d[0] != 0 ? ((sx > 0 ? ix + 1 - ox : ox - ix) * tdx) : big;
    float tmy = d[1] != 0 ? ((sy > 0 ? iy + 1 - oy : oy - iy) * tdy) : big;
    float tmz = d[2] != 0 ? ((sz > 0 ? iz + 1 - oz : oz - iz) * tdz) : big;
    const float tmax = 6.f;                  /* reach in blocks */
    prev[0] = ix; prev[1] = iy; prev[2] = iz;
    for (int i = 0; i < 64; i++) {
        float t;
        if (tmx < tmy && tmx < tmz) { t = tmx; ix += sx; tmx += tdx; }
        else if (tmy < tmz)         { t = tmy; iy += sy; tmy += tdy; }
        else                        { t = tmz; iz += sz; tmz += tdz; }
        if (t > tmax) return 0;
        if (in_world(ix, iy, iz) && B(ix, iy, iz) != AIR) {
            hit[0] = ix; hit[1] = iy; hit[2] = iz;
            return 1;
        }
        prev[0] = ix; prev[1] = iy; prev[2] = iz;
    }
    return 0;
}

/* ============================ saving ================================== */

static int      g_save_dirty;
static uint32_t g_edit_ms, g_last_save_ms;
static float    g_saved_px, g_saved_pz;

static void save_world(uint32_t now)
{
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(SAVE_DIR);
    g_save.magic = SAVE_MAGIC; g_save.version = 1;
    if (hb_fs_write(SAVE_PATH, &g_save, sizeof g_save)) {
        g_save_dirty = 0;
        g_saved_px = g_save.px; g_saved_pz = g_save.pz;
    }
    g_last_save_ms = now;
    hb_trace_log("BLKSAVE ", g_save_dirty, sizeof g_save);
}

static void load_or_generate(void)
{
    uint32_t n = hb_fs_read(SAVE_PATH, &g_save, sizeof g_save);
    if (n == sizeof g_save && g_save.magic == SAVE_MAGIC && g_save.version == 1) {
        if (g_save.slot > 7) g_save.slot = 0;
        hb_trace_log("BLKLOAD ", n, 0);
    } else {
        generate((int)(hb_time_uptime_ms() & 0xffff));
        g_save.slot = 0;
        spawn();
        g_save_dirty = 1;
        hb_trace_log("BLKGEN  ", n, 0);
    }
    unstick();
    g_saved_px = g_save.px; g_saved_pz = g_save.pz;
}

static void edit_block(int x, int y, int z, int b, uint32_t now)
{
    B(x, y, z) = (uint8_t)b;
    mark_dirty(x, z);
    g_save_dirty = 1;
    g_edit_ms = now;
}

/* ============================ input =================================== */

static const uint8_t HOTBAR[8] = { DIRT, STONE, COBBLE, PLANKS, LOG, BRICK, SAND, LEAVES };
enum { BTN_NONE = -1, BTN_LEFT, BTN_FWD, BTN_BACK, BTN_RIGHT, BTN_JUMP, NBTN };

enum { T_IDLE, T_PENDING, T_LOOK, T_HUD, T_ROT };
static int      g_tmode, g_prev_down, g_lx, g_ly, g_sx, g_sy, g_moved;
static uint32_t g_down_ms;
static int      g_held_btn = BTN_NONE, g_jump;

/* Landscape overlay: d-pad bottom-left, hotbar bottom-centre, jump bottom-right. */
#define L_DP_C   34                          /* d-pad cell */
#define L_DP_X   56                          /* d-pad centre */
#define L_DP_Y   (g_lh - 60)
#define L_SLOT   27
#define L_BAR_X  ((g_lw - 8*L_SLOT) / 2)
#define L_BAR_Y  (g_lh - L_SLOT - 6)
#define L_JUMP   68
#define L_JUMP_X (g_lw - L_JUMP - 8)
#define L_JUMP_Y (g_lh - L_JUMP - 8)
#define ROT_S    30                          /* rotate button, top-right corner */
#define ROT_X    (g_lw - ROT_S - 6)
#define ROT_Y    6

static int in_box(int x, int y, int bx, int by, int bw, int bh)
{ return x >= bx && x < bx + bw && y >= by && y < by + bh; }

static int on_rotate(int x, int y) { return in_box(x, y, ROT_X - 6, ROT_Y - 6, ROT_S + 12, ROT_S + 12); }

static int slot_at(int x, int y)
{
    if (g_orient == 0) {
        if (y < g_lh - HUD_H || y >= g_lh - HUD_H + BAR_H) return -1;
        int s = x / (g_lw / 8);
        return s > 7 ? 7 : s;
    }
    if (!in_box(x, y, L_BAR_X, L_BAR_Y - 4, 8*L_SLOT, L_SLOT + 10)) return -1;
    return (x - L_BAR_X) / L_SLOT;
}

static int hud_button_at(int x, int y)
{
    if (g_orient == 0) {
        if (y < g_lh - HUD_H + BAR_H) return BTN_NONE;
        int bw = g_lw / NBTN;
        int i = x / bw;
        return i >= NBTN ? NBTN - 1 : i;
    }
    if (in_box(x, y, L_JUMP_X - 6, L_JUMP_Y - 6, L_JUMP + 12, L_JUMP + 12)) return BTN_JUMP;
    int dx = x - L_DP_X, dy = y - L_DP_Y;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax > L_DP_C*3/2 + 8 || ay > L_DP_C*3/2 + 8) return BTN_NONE;
    if (ax < L_DP_C/4 && ay < L_DP_C/4) return BTN_NONE;      /* dead centre */
    if (ax > ay) return dx < 0 ? BTN_LEFT : BTN_RIGHT;
    return dy < 0 ? BTN_FWD : BTN_BACK;
}

/* A touch that lands on any HUD control (not the world). */
static int on_hud(int x, int y)
{
    if (g_orient == 0) return y >= g_lh - HUD_H;
    return slot_at(x, y) >= 0 || hud_button_at(x, y) != BTN_NONE;
}

static void do_break(int tx, int ty, uint32_t now)
{
    float d[3]; int hit[3], prev[3];
    screen_ray(tx, ty, d);
    if (raycast(d, hit, prev) && hit[1] > 0)
        edit_block(hit[0], hit[1], hit[2], AIR, now);
}

static void do_place(int tx, int ty, uint32_t now)
{
    float d[3]; int hit[3], prev[3];
    screen_ray(tx, ty, d);
    if (!raycast(d, hit, prev) || !in_world(prev[0], prev[1], prev[2])) return;
    if (B(prev[0], prev[1], prev[2]) != AIR) return;
    B(prev[0], prev[1], prev[2]) = HOTBAR[g_save.slot];
    if (box_hits(g_save.px, g_save.py, g_save.pz)) {      /* would trap the player */
        B(prev[0], prev[1], prev[2]) = AIR;
        return;
    }
    edit_block(prev[0], prev[1], prev[2], HOTBAR[g_save.slot], now);
}

#define HOLD_MS   320
#define REPEAT_MS 380

static void input(int w, int h, uint32_t now)
{
    hb_spoint_t t;
    hb_surface_touch_read(&t);
    int x = t.x, y = t.y, down = t.down;
    if (x < 0) x = 0;
    if (x >= w) x = w - 1;
    if (y < 0) y = 0;
    if (y >= h) y = h - 1;
    to_logical(w, h, x, y, &x, &y);

    g_held_btn = BTN_NONE; g_jump = 0;

    if (down && !g_prev_down) {
        g_sx = g_lx = x; g_sy = g_ly = y; g_moved = 0; g_down_ms = now;
        if (on_rotate(x, y)) {
            g_tmode = T_ROT;
        } else if (on_hud(x, y)) {
            g_tmode = T_HUD;
            int s = slot_at(x, y);
            if (s >= 0 && g_save.slot != (uint32_t)s) { g_save.slot = (uint32_t)s; g_save_dirty = 1; g_edit_ms = now; }
        } else {
            g_tmode = T_PENDING;
        }
    } else if (down && g_prev_down) {
        int dx = x - g_lx, dy = y - g_ly;
        g_lx = x; g_ly = y;
        if (g_tmode == T_PENDING || g_tmode == T_LOOK) {
            g_moved += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (g_tmode == T_PENDING && g_moved > 12) g_tmode = T_LOOK;
            if (g_tmode == T_LOOK) {
                g_save.yaw   -= dx * 0.42f;
                g_save.pitch -= dy * 0.36f;
                if (g_save.pitch >  89.f) g_save.pitch =  89.f;
                if (g_save.pitch < -89.f) g_save.pitch = -89.f;
                if (g_save.yaw > 360.f) g_save.yaw -= 360.f;
                if (g_save.yaw < 0.f)   g_save.yaw += 360.f;
            }
            if (g_tmode == T_PENDING && now - g_down_ms >= HOLD_MS) {
                do_break(g_sx, g_sy, now);
                g_down_ms = now - HOLD_MS + REPEAT_MS;   /* keep digging while held */
                g_moved = -1000;                          /* holding: never becomes a tap */
            }
        }
    } else if (!down && g_prev_down) {
        if (g_tmode == T_PENDING && g_moved >= 0 && g_moved <= 12 && now - g_down_ms < HOLD_MS)
            do_place(g_sx, g_sy, now);
        if (g_tmode == T_ROT && on_rotate(x, y)) {
            g_orient = (g_orient + 1) % 3;
            save_orient();
            set_logical_size(w, h);
        }
        g_tmode = T_IDLE;
    }
    if (down && g_tmode == T_HUD) {
        int b = hud_button_at(x, y);
        if (b == BTN_JUMP) g_jump = 1;       /* held: hops again on each landing */
        else               g_held_btn = b;
    }
    g_prev_down = down;
}

/* ============================ drawing ================================= */

static void draw_world(int w, int h)
{
    if (g_orient == 0) {
        g_vw = w; g_vh = h - HUD_H;
        glViewport(0, HUD_H, g_vw, g_vh);
        glScissor(0, HUD_H, g_vw, g_vh);
    } else {                                 /* whole screen; the HUD overlays it */
        g_vw = g_lw; g_vh = g_lh;
        glViewport(0, 0, w, h);
        glScissor(0, 0, w, h);
    }
    glEnable(GL_SCISSOR_TEST);
    glClearColor(0.53f, 0.75f, 0.98f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float hw = ZNEAR * fov_k(), hh = hw * g_vh / g_vw;
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    /* turn clip space so logical right lands on physical down (1) or up (2) */
    if (g_orient == 1) glRotatef(-90.f, 0.f, 0.f, 1.f);
    if (g_orient == 2) glRotatef( 90.f, 0.f, 0.f, 1.f);
    glFrustumf(-hw, hw, -hh, hh, ZNEAR, ZFAR);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glRotatef(-g_save.pitch, 1.f, 0.f, 0.f);
    glRotatef(-g_save.yaw,   0.f, 1.f, 0.f);
    glTranslatef(-g_save.px, -(g_save.py + EYE), -g_save.pz);

    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
    glDisable(GL_LIGHTING); glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, TEX_ID);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1.f, 1.f, 1.f, 1.f);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);

    /* skip chunks entirely behind the camera */
    float yaw = g_save.yaw * DEG, pit = g_save.pitch * DEG;
    float fx = -fsinf(yaw) * fcosf(pit), fy = fsinf(pit), fz = -fcosf(yaw) * fcosf(pit);
    for (int ci = 0; ci < NCHUNK; ci++) {
        if (g_chunk_faces[ci] == 0) continue;
        float cx = (ci % NCX) * CH + CH*0.5f - g_save.px;
        float cy = WY*0.5f - (g_save.py + EYE);
        float cz = (ci / NCX) * CH + CH*0.5f - g_save.pz;
        if (cx*fx + cy*fy + cz*fz < -14.f) continue;
        glBindBuffer(GL_ARRAY_BUFFER, VBO_BASE + ci);
        glVertexPointer(3, GL_FLOAT, VSTRIDE*4, (const GLvoid *)0);
        glTexCoordPointer(2, GL_FLOAT, VSTRIDE*4, (const GLvoid *)12);
        glDrawArrays(GL_TRIANGLES, 0, g_chunk_faces[ci] * 6);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_SCISSOR_TEST);
}

/* 2D in logical, top-left-origin coordinates for the current orientation. */
static void begin2d(int w, int h)
{
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);   glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrthof(0.f, (float)w, (float)h, 0.f, -1.f, 1.f);    /* physical, top-left origin */
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    if (g_orient == 1)      { glTranslatef((float)w, 0.f, 0.f); glRotatef( 90.f, 0.f, 0.f, 1.f); }
    else if (g_orient == 2) { glTranslatef(0.f, (float)h, 0.f); glRotatef(-90.f, 0.f, 0.f, 1.f); }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableClientState(GL_VERTEX_ARRAY);
}

static void rect2d(float x, float y, float w, float h)
{
    float v[8] = { x,y, x+w,y, x,y+h, x+w,y+h };
    glVertexPointer(2, GL_FLOAT, 0, v);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void tri2d(float x0, float y0, float x1, float y1, float x2, float y2)
{
    float v[6] = { x0,y0, x1,y1, x2,y2 };
    glVertexPointer(2, GL_FLOAT, 0, v);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void block_icon(int b, float x, float y, float s)
{
    float u0, v0, u1, v1;
    tile_rect(tile_for(b, 0), &u0, &v0, &u1, &v1);    /* side tile, unshaded row */
    float v[8] = { x,y, x+s,y, x,y+s, x+s,y+s };
    float t[8] = { u0,v0, u1,v0, u0,v1, u1,v1 };
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, TEX_ID);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glTexCoordPointer(2, GL_FLOAT, 0, t);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

static void arrow2d(int dir, float cx, float cy, float r)
{
    switch (dir) {
    case BTN_LEFT:  tri2d(cx - r, cy, cx + r*0.7f, cy - r, cx + r*0.7f, cy + r); break;
    case BTN_RIGHT: tri2d(cx + r, cy, cx - r*0.7f, cy - r, cx - r*0.7f, cy + r); break;
    case BTN_FWD:   tri2d(cx, cy - r, cx - r, cy + r*0.7f, cx + r, cy + r*0.7f); break;
    case BTN_BACK:  tri2d(cx, cy + r, cx - r, cy - r*0.7f, cx + r, cy - r*0.7f); break;
    default:                                 /* jump */
        tri2d(cx, cy - r, cx - r, cy, cx + r, cy);
        rect2d(cx - r*0.35f, cy, r*0.7f, r*0.9f);
        break;
    }
}

/* A small phone outline with a turn arrow: tap to cycle the orientation. */
static void draw_rotate_button(void)
{
    float x = (float)ROT_X, y = (float)ROT_Y, s = (float)ROT_S;
    glColor4f(0.f, 0.f, 0.f, 0.35f);
    rect2d(x, y, s, s);
    glColor4f(1.f, 1.f, 1.f, 0.9f);
    float px = x + 7.f, py = y + 6.f, pw = 11.f, ph = 18.f;
    rect2d(px, py, pw, 2.f); rect2d(px, py + ph - 2.f, pw, 2.f);
    rect2d(px, py, 2.f, ph); rect2d(px + pw - 2.f, py, 2.f, ph);
    tri2d(x + s - 5.f, y + s - 9.f, x + s - 12.f, y + s - 4.f, x + s - 12.f, y + s - 14.f);
}

static void draw_hud_landscape(void)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glColor4f(1.f, 1.f, 1.f, 0.9f);
    rect2d(g_lw*0.5f - 1.f, g_lh*0.5f - 1.f, 2.f, 2.f);

    /* d-pad */
    static const int DIRS[4][3] = { { BTN_FWD, 0, -1 }, { BTN_BACK, 0, 1 }, { BTN_LEFT, -1, 0 }, { BTN_RIGHT, 1, 0 } };
    float c = (float)L_DP_C;
    for (int i = 0; i < 4; i++) {
        float cx = L_DP_X + DIRS[i][1]*c, cy = L_DP_Y + DIRS[i][2]*c;
        if (DIRS[i][0] == g_held_btn) glColor4f(0.95f, 0.95f, 1.f, 0.55f);
        else                          glColor4f(0.1f, 0.1f, 0.12f, 0.4f);
        rect2d(cx - c*0.5f + 1.f, cy - c*0.5f + 1.f, c - 2.f, c - 2.f);
        glColor4f(1.f, 1.f, 1.f, 0.85f);
        arrow2d(DIRS[i][0], cx, cy, c*0.24f);
    }

    /* jump */
    if (g_jump) glColor4f(0.95f, 0.95f, 1.f, 0.55f);
    else        glColor4f(0.1f, 0.1f, 0.12f, 0.4f);
    rect2d((float)L_JUMP_X, (float)L_JUMP_Y, (float)L_JUMP, (float)L_JUMP);
    glColor4f(1.f, 1.f, 1.f, 0.85f);
    arrow2d(BTN_JUMP, L_JUMP_X + L_JUMP*0.5f, L_JUMP_Y + L_JUMP*0.5f, L_JUMP*0.22f);

    /* hotbar */
    float bx = (float)L_BAR_X, by = (float)L_BAR_Y, s = (float)L_SLOT;
    glColor4f(0.1f, 0.1f, 0.12f, 0.55f);
    rect2d(bx - 2.f, by - 2.f, 8*s + 4.f, s + 4.f);
    for (int i = 0; i < 8; i++) {
        float x = bx + i*s;
        if ((uint32_t)i == g_save.slot) { glColor4f(1.f, 1.f, 1.f, 1.f); rect2d(x, by - 1.f, s, s + 2.f); }
        glDisable(GL_BLEND);
        block_icon(HOTBAR[i], x + 4.f, by + 4.f, s - 8.f);
        glEnable(GL_BLEND);
    }
}

static void draw_hud(int w, int h)
{
    begin2d(w, h);
    if (g_orient) {
        draw_hud_landscape();
        draw_rotate_button();
        glDisable(GL_BLEND);
        return;
    }
    w = g_lw; h = g_lh;
    float top = (float)(h - HUD_H);

    /* no crosshair (you tap the block itself); a centre dot shows where you're facing */
    glColor4f(1.f, 1.f, 1.f, 1.f);
    rect2d(w*0.5f - 1.f, g_vh*0.5f - 1.f, 2.f, 2.f);

    glColor4f(0.12f, 0.12f, 0.14f, 1.f);
    rect2d(0.f, top, (float)w, (float)HUD_H);

    /* hotbar */
    float sw = (float)(w / 8);
    for (int i = 0; i < 8; i++) {
        float x = i * sw;
        if ((uint32_t)i == g_save.slot) { glColor4f(1.f, 1.f, 1.f, 1.f); rect2d(x + 1.f, top + 2.f, sw - 2.f, BAR_H - 4.f); }
        glColor4f(0.25f, 0.25f, 0.28f, 1.f);
        rect2d(x + 3.f, top + 4.f, sw - 6.f, BAR_H - 8.f);
        float s = (sw < BAR_H ? sw : BAR_H) - 12.f;
        block_icon(HOTBAR[i], x + (sw - s)*0.5f, top + (BAR_H - s)*0.5f, s);
    }

    /* movement buttons */
    float by = top + BAR_H, bh = (float)(HUD_H - BAR_H), bw = (float)(w / NBTN);
    for (int i = 0; i < NBTN; i++) {
        float x = i * bw, cx = x + bw*0.5f, cy = by + bh*0.5f, r = bw*0.26f;
        int held = (i == g_held_btn) || (i == BTN_JUMP && g_jump);
        if (held) glColor4f(0.42f, 0.42f, 0.48f, 1.f);
        else      glColor4f(0.22f, 0.22f, 0.25f, 1.f);
        rect2d(x + 3.f, by + 5.f, bw - 6.f, bh - 10.f);
        glColor4f(0.92f, 0.92f, 0.95f, 1.f);
        arrow2d(i, cx, cy, r);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    draw_rotate_button();
    glDisable(GL_BLEND);
}

/* ============================ entry =================================== */

static int g_inited;

static void lazy_init(void)
{
    load_orient();
    load_or_generate();
    build_atlas();
    glBindTexture(GL_TEXTURE_2D, TEX_ID);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS, ATLAS, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    for (int ci = 0; ci < NCHUNK; ci++) upload_chunk(ci);
    g_inited = 1;
}

void gl_app_init(void)
{
    /* No GL context current at load — defer all GL to the first gl_app_frame. */
}

void gl_app_frame(int w, int h, uint32_t frame)
{
    (void)frame;
    uint32_t now = hb_time_uptime_ms();
    if (!g_inited) { lazy_init(); g_last_save_ms = now; }

    static uint32_t s_last;
    g_dt = s_last ? (float)(now - s_last) * 0.001f : 0.016f;
    s_last = now;
    if (g_dt > 0.1f) g_dt = 0.1f;

    set_logical_size(w, h);
    input(w, h, now);

    float yaw = g_save.yaw * DEG, cy = fcosf(yaw), sy = fsinf(yaw);
    float f = 0.f, r = 0.f;
    if (g_held_btn == BTN_FWD)   f =  1.f;
    if (g_held_btn == BTN_BACK)  f = -1.f;
    if (g_held_btn == BTN_LEFT)  r = -1.f;
    if (g_held_btn == BTN_RIGHT) r =  1.f;
    float mx = (-sy * f + cy * r) * SPEED, mz = (-cy * f - sy * r) * SPEED;
    physics(mx, mz, g_jump);

    for (int ci = 0; ci < NCHUNK; ci++)
        if (g_chunk_dirty[ci]) upload_chunk(ci);

    glDisable(GL_BLEND); glColorMask(1, 1, 1, 1);
    draw_world(w, h);
    draw_hud(w, h);
    (void)glGetError();

    /* save 1.5 s after the last edit (not mid-gesture), or every 20 s after walking */
    if (!g_prev_down) {
        float mv = fabsf_(g_save.px - g_saved_px) + fabsf_(g_save.pz - g_saved_pz);
        if ((g_save_dirty && now - g_edit_ms > 1500) ||
            (mv > 0.5f && now - g_last_save_ms > 20000))
            save_world(now);
    }
}
