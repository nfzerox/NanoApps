/*
 * opengl — a comprehensive, touch-interactive tour of the hardware GLES 1.1 surface
 * exposed by sdk/gl.h on the iPod GPU. A GL_SURFACE .hbapp: the resident
 * injects a fullscreen GL view and calls gl_app_frame() between its begin/end scene;
 * we issue GLES 1.1 into the current context and the OS composites the result.
 *
 * Twelve screens, each exercising a slice of the API (and several combining many at
 * once). A bottom bar has prev/next buttons + a page indicator; dragging in the
 * content area drives each screen (spin a cube faster, move a light, change a blend
 * mode, slide a sub-texture, orbit a scene...). Touch comes from the resident's
 * event-path mailbox in its shared block ([magic 'TNPT'], x, y, pressed).
 *
 * Pure fixed-function GLES 1.1: matrices (glFrustumf/glOrthof/glRotatef/...), vertex/
 * color/normal/texcoord arrays, VBOs (+glBufferSubData), glDrawArrays + glDrawElements,
 * textures (glTexImage2D/glTexSubImage2D/glTexParameteri/glTexEnvi), lighting
 * (GL_LIGHT0 + glLightfv/glMaterialfv + normals), blending, alpha test, depth, cull,
 * scissor, line width, color mask, glReadPixels, glGetString/glGetIntegerv.
 */
#include "hb_gl_surface.h"
#include "hb_surface_input.h"
#include "gl.h"

extern void hb_trace_log(const char *, unsigned, unsigned);   /* NANOAPPS diag */

/* ---- tokens not in gl.h ---- */
#define GL_NORMALIZE   0x0BA1
#define GL_ALPHA_TEST  0x0BC0
#define GL_LINE_STRIP  0x0003


#define NAV_H   46          /* bottom nav-bar height (screen px)        */
#define NSCREENS 12

/* ---- tiny trig (no libm) ---- */
#define PI_  3.14159265f
#define TAU_ 6.28318531f
static float fsinf(float x){
    while (x >  PI_) x -= TAU_;
    while (x < -PI_) x += TAU_;
    float x2 = x*x;
    return x*(1.f - x2*(0.16666667f - x2*(0.00833333f - x2*0.0001984127f)));
}
static float fcosf(float x){ return fsinf(x + 1.57079633f); }

/* =========================== geometry ================================= */

/* Cube: 24 verts (4/face), interleaved pos3 nrm3 uv2, + per-vertex color, + 36 idx. */
static float          g_cube_v[24*8];
static unsigned char  g_cube_c[24*4];
static unsigned short g_cube_i[36];

static void build_cube(void)
{
    static const signed char N[6][3] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    static const signed char U[6][3] = {{1,0,0},{-1,0,0},{0,0,-1},{0,0,1},{1,0,0},{1,0,0}};
    static const signed char V[6][3] = {{0,1,0},{0,1,0},{0,1,0},{0,1,0},{0,0,-1},{0,0,1}};
    static const unsigned char COL[6][3] = {
        {230,70,70},{70,200,90},{70,120,230},{235,200,60},{200,90,220},{60,210,210}};
    int ii = 0;
    for (int f = 0; f < 6; f++) {
        for (int c = 0; c < 4; c++) {
            int su = (c==1||c==2) ? 1 : -1;
            int sv = (c>=2) ? 1 : -1;
            float *p = &g_cube_v[(f*4+c)*8];
            for (int k = 0; k < 3; k++)
                p[k] = (N[f][k] + su*U[f][k] + sv*V[f][k]) * 0.5f;
            p[3]=N[f][0]; p[4]=N[f][1]; p[5]=N[f][2];
            p[6]=(c==1||c==2)?1.f:0.f; p[7]=(c>=2)?1.f:0.f;
            unsigned char *cc = &g_cube_c[(f*4+c)*4];
            cc[0]=COL[f][0]; cc[1]=COL[f][1]; cc[2]=COL[f][2]; cc[3]=255;
        }
        unsigned short b = (unsigned short)(f*4);
        g_cube_i[ii++]=b; g_cube_i[ii++]=(unsigned short)(b+1); g_cube_i[ii++]=(unsigned short)(b+2);
        g_cube_i[ii++]=b; g_cube_i[ii++]=(unsigned short)(b+2); g_cube_i[ii++]=(unsigned short)(b+3);
    }
}

/* Sphere: lat/long grid, interleaved pos3 nrm3 uv2 + indices. */
#define SLON 24
#define SLAT 16
#define SVERT ((SLON+1)*(SLAT+1))
#define SIDX  (SLON*SLAT*6)
static float          g_sph_v[SVERT*8];
static unsigned short g_sph_i[SIDX];
static unsigned char  g_sph_c[SVERT*4];   /* per-vertex color (from the normal) */

static void build_sphere(void)
{
    int k = 0, ci = 0;
    for (int j = 0; j <= SLAT; j++) {
        float v = (float)j/SLAT, theta = v*PI_;
        float st = fsinf(theta), ct = fcosf(theta);
        for (int i = 0; i <= SLON; i++) {
            float u = (float)i/SLON, phi = u*TAU_;
            float sp = fsinf(phi), cp = fcosf(phi);
            float x = st*cp, y = ct, z = st*sp;
            g_sph_v[k++]=x; g_sph_v[k++]=y; g_sph_v[k++]=z;   /* pos (unit) */
            g_sph_v[k++]=x; g_sph_v[k++]=y; g_sph_v[k++]=z;   /* normal      */
            g_sph_v[k++]=u*3.f; g_sph_v[k++]=v*2.f;           /* uv          */
            g_sph_c[ci++]=(unsigned char)((x*0.5f+0.5f)*255); /* color=normal */
            g_sph_c[ci++]=(unsigned char)((y*0.5f+0.5f)*255);
            g_sph_c[ci++]=(unsigned char)((z*0.5f+0.5f)*255);
            g_sph_c[ci++]=255;
        }
    }
    int n = 0;
    for (int j = 0; j < SLAT; j++)
        for (int i = 0; i < SLON; i++) {
            int a = j*(SLON+1)+i, b = a+(SLON+1);
            g_sph_i[n++]=(unsigned short)a; g_sph_i[n++]=(unsigned short)b;   g_sph_i[n++]=(unsigned short)(b+1);
            g_sph_i[n++]=(unsigned short)a; g_sph_i[n++]=(unsigned short)(b+1); g_sph_i[n++]=(unsigned short)(a+1);
        }
}

/* Torus: pos3 nrm3 uv2 interleaved + indices. */
#define TU 32
#define TV 16
#define TVERT ((TU+1)*(TV+1))
#define TIDX  (TU*TV*6)
#define TMAJOR 1.0f
#define TMINOR 0.42f
static float          g_tor_v[TVERT*8];
static unsigned short g_tor_i[TIDX];

static void build_torus(void)
{
    int k=0;
    for (int i=0;i<=TU;i++) {
        float u=(float)i/TU*TAU_, cu=fcosf(u), su=fsinf(u);
        for (int j=0;j<=TV;j++) {
            float v=(float)j/TV*TAU_, cv=fcosf(v), sv=fsinf(v);
            float rr=TMAJOR+TMINOR*cv;
            g_tor_v[k++]=rr*cu; g_tor_v[k++]=rr*su; g_tor_v[k++]=TMINOR*sv;
            g_tor_v[k++]=cv*cu; g_tor_v[k++]=cv*su; g_tor_v[k++]=sv;
            g_tor_v[k++]=(float)i/TU*4.f; g_tor_v[k++]=(float)j/TV;
        }
    }
    int n=0;
    for (int i=0;i<TU;i++) for (int j=0;j<TV;j++) {
        int a=i*(TV+1)+j, b=a+(TV+1);
        g_tor_i[n++]=(unsigned short)a; g_tor_i[n++]=(unsigned short)b;   g_tor_i[n++]=(unsigned short)(b+1);
        g_tor_i[n++]=(unsigned short)a; g_tor_i[n++]=(unsigned short)(b+1); g_tor_i[n++]=(unsigned short)(a+1);
    }
}

/* Dynamic wave grid in a VBO (animated via glBufferSubData). */
#define GN 28
#define GVERT (GN*GN)
#define GIDX  ((GN-1)*(GN-1)*6)
static float          g_grid_v[GVERT*3];
static unsigned char  g_grid_c[GVERT*4];   /* per-vertex color (from height) */
static unsigned short g_grid_i[GIDX];
static GLuint         g_grid_vbo;

static void build_grid_indices(void)
{
    int n = 0;
    for (int j = 0; j < GN-1; j++)
        for (int i = 0; i < GN-1; i++) {
            int a = j*GN+i, b = a+GN;
            g_grid_i[n++]=(unsigned short)a; g_grid_i[n++]=(unsigned short)b;     g_grid_i[n++]=(unsigned short)(a+1);
            g_grid_i[n++]=(unsigned short)(a+1); g_grid_i[n++]=(unsigned short)b; g_grid_i[n++]=(unsigned short)(b+1);
        }
}

/* ---- procedural textures ---- */
static unsigned char g_tex_checker[64*64*4];
static unsigned char g_tex_dyn[64*64*4];
static unsigned char g_tex_detail[32*32*4];   /* second unit (multitexture screen) */
static GLuint        g_chk_id, g_dyn_id, g_det_id, g_tor_tex;

static void build_checker(void)
{
    for (int y=0;y<64;y++) for (int x=0;x<64;x++) {
        unsigned char *p=&g_tex_checker[(y*64+x)*4];
        int g = ((x>>3)+(y>>3)) & 1;
        if (g) { p[0]=240; p[1]=240; p[2]=250; }
        else   { p[0]=(unsigned char)(40+x*3); p[1]=(unsigned char)(30+y*3); p[2]=160; }
        p[3]=255;
    }
}

/* =========================== touch ==================================== */

static int   g_screen;
static float g_t;            /* logical animation time (seconds-scaled)  */
static float g_dt;           /* real elapsed time this frame (seconds)   */
static int   g_touch, g_tx, g_ty, g_dx, g_dy;
static int   s_prev_down, s_lx, s_ly, s_dx0, s_dy0, s_moved;

static void touch_update(int w, int h)
{
    /* Shared surface touch source: event-path mailbox + OS-list poll fallback
     * (catches holds the mailbox didn't latch). Gesture deltas stay app-side. */
    hb_spoint_t t;
    hb_surface_touch_read(&t);
    int down = t.down;
    int x = t.x, y = t.y;
    if (x < 0) x = 0; if (x > w) x = w;
    if (y < 0) y = 0; if (y > h) y = h;

    g_dx = g_dy = 0; g_touch = 0;

    if (down && !s_prev_down) {
        s_dx0 = s_lx = x; s_dy0 = s_ly = y; s_moved = 0;
    } else if (down && s_prev_down) {
        g_dx = x - s_lx; g_dy = y - s_ly;
        s_moved += (g_dx<0?-g_dx:g_dx) + (g_dy<0?-g_dy:g_dy);
        s_lx = x; s_ly = y;
    } else if (!down && s_prev_down) {
        if (s_moved < 16 && s_dy0 > h - NAV_H) {
            if (s_dx0 < w/2) g_screen = (g_screen + NSCREENS - 1) % NSCREENS;
            else             g_screen = (g_screen + 1) % NSCREENS;
        }
    }
    if (down && y < h - NAV_H) { g_touch = 1; g_tx = x; g_ty = y; }
    s_prev_down = down;
}

/* =========================== 2D overlay =============================== */

static void begin2d(int w, int h)
{
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);   glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);      glDisable(GL_ALPHA_TEST);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrthof(0.f, (float)w, (float)h, 0.f, -1.f, 1.f);   /* top-left origin */
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void fill2d(const float *xy, int n, GLenum mode)
{
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, xy);
    glDrawArrays(mode, 0, n);
}

static void rect2d(float x, float y, float ww, float hh)
{
    float v[8] = { x,y, x+ww,y, x,y+hh, x+ww,y+hh };
    fill2d(v, 4, GL_TRIANGLE_STRIP);
}

static void draw_nav(int w, int h)
{
    begin2d(w, h);
    float top = (float)(h - NAV_H);
    glColor4f(0.10f, 0.11f, 0.14f, 1.f);
    rect2d(0.f, top, (float)w, (float)NAV_H);
    glColor4f(0.f, 0.f, 0.f, 1.f);
    rect2d(0.f, top, (float)w, 2.f);

    float cy = top + NAV_H*0.5f;
    glColor4f(0.85f, 0.85f, 0.9f, 1.f);
    { float a[6] = { 22.f, cy, 38.f, cy-11.f, 38.f, cy+11.f }; fill2d(a, 3, GL_TRIANGLES); }
    { float bx = (float)w-22.f, b2 = (float)w-38.f;
      float a[6] = { bx, cy, b2, cy-11.f, b2, cy+11.f }; fill2d(a, 3, GL_TRIANGLES); }

    float dx0 = (float)w*0.5f - (NSCREENS-1)*5.f;
    for (int i = 0; i < NSCREENS; i++) {
        if (i == g_screen) glColor4f(0.95f, 0.95f, 1.f, 1.f);
        else               glColor4f(0.35f, 0.36f, 0.42f, 1.f);
        float cx = dx0 + i*10.f, r = (i==g_screen)?3.f:2.f;
        rect2d(cx-r, cy-r, 2*r, 2*r);
    }
}

/* common perspective setup into the content viewport (above the nav bar) */
static void begin3d(int w, int h, float znear, float zfar)
{
    int ch = h - NAV_H;
    glViewport(0, NAV_H, w, ch);
    glScissor(0, NAV_H, w, ch); glEnable(GL_SCISSOR_TEST);
    /* FIT WIDTH: fix the horizontal half-extent, let the (portrait) viewport make the
     * vertical taller — so a unit object spans the width and isn't clipped. */
    float aspect = (float)w / (float)ch, hw = 0.55f;
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glFrustumf(-hw, hw, -hw/aspect, hw/aspect, znear, zfar);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
}

static float g_yaw, g_pitch = 18.f, g_spin;
/* xs/ys let each screen pick the natural drag sign for its particular view setup. */
static void orbit2(float xs, float ys)
{
    if (g_touch) { g_yaw += xs * g_dx * 0.6f; g_pitch += ys * g_dy * 0.4f;
                   if (g_pitch > 80.f) g_pitch = 80.f; if (g_pitch < -80.f) g_pitch = -80.f; }
}
static void orbit_from_drag(void) { orbit2(-1.f, 1.f); }   /* default */

/* =========================== screens ================================= */

/* 0 — clear / color-mask / scissor / readpixels */
static void screen_clear(int w, int h)
{
    static int mask = 7, cd;
    if (g_dx > 6 || g_dx < -6) { if (++cd > 4){ cd=0; mask = (mask % 7) + 1; } }
    int hw = w/2, hh = (h-NAV_H)/2;
    float t = g_t;
    struct { int x,y; float r,g,b; } q[4] = {
        {0,   NAV_H,    0.5f+0.5f*fsinf(t),      0.2f, 0.3f},
        {hw,  NAV_H,    0.2f, 0.5f+0.5f*fsinf(t+2.f), 0.3f},
        {0,   NAV_H+hh, 0.2f, 0.3f, 0.5f+0.5f*fsinf(t+4.f)},
        {hw,  NAV_H+hh, 0.5f+0.5f*fsinf(t+1.f), 0.5f+0.5f*fsinf(t+3.f), 0.4f},
    };
    glColorMask((mask&1)!=0, (mask&2)!=0, (mask&4)!=0, 1);
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < 4; i++) {
        glScissor(q[i].x, q[i].y, hw, hh);
        glClearColor(q[i].r, q[i].g, q[i].b, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1,1,1,1);

    unsigned char px[4] = {0,0,0,255};
    glReadPixels(w/2, h/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    begin2d(w, h);
    glColor4f(px[0]/255.f, px[1]/255.f, px[2]/255.f, 1.f);
    rect2d((float)w-30.f, (float)NAV_H+8.f, 22.f, 22.f);
}

/* 1 — spinning per-face-colored cube (transforms + vertex/color arrays + depth + cull) */
static void screen_cube(int w, int h)
{
    orbit_from_drag();
    if (g_touch) g_spin += g_dx * 18.f;          /* fling -> deg/sec */
    g_spin *= (1.f - 1.8f*g_dt); if (g_spin < -1e4f) g_spin=0;

    glClearColor(0.05f, 0.06f, 0.10f, 1.f);
    begin3d(w, h, 1.5f, 12.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);  glCullFace(GL_BACK);
    glShadeModel(GL_FLAT);

    static float a; a += (60.f + g_spin) * g_dt;
    glTranslatef(0.f, 0.f, -3.2f);
    glRotatef(g_pitch, 1.f, 0.f, 0.f);
    glRotatef(a + g_yaw, 0.f, 1.f, 0.f);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(3, GL_FLOAT, 32, g_cube_v);
    glColorPointer(4, GL_UNSIGNED_BYTE, 4, g_cube_c);
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, g_cube_i);
    glDisable(GL_SCISSOR_TEST);
}

/* 2 — primitive modes + glLineWidth (drag) */
static void screen_prims(int w, int h)
{
    static float lw = 3.f;
    if (g_touch) { lw += g_dx * 0.05f; if (lw < 1.f) lw = 1.f; if (lw > 12.f) lw = 12.f; }
    glClearColor(0.06f, 0.07f, 0.09f, 1.f);
    glViewport(0, NAV_H, w, h-NAV_H);
    glScissor(0, NAV_H, w, h-NAV_H); glEnable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING); glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrthof(0,1,0,1,-1,1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glLineWidth(lw);
    float ph = g_t;
    { float v[16]; for (int i=0;i<8;i++){ v[i*2]=0.1f+i*0.1f; v[i*2+1]=0.86f; }
      glColor4f(1,0.8f,0.3f,1); glVertexPointer(2,GL_FLOAT,0,v); glDrawArrays(GL_POINTS,0,8); }
    { float v[16]; for (int i=0;i<4;i++){ v[i*4]=0.1f+i*0.22f; v[i*4+1]=0.70f; v[i*4+2]=0.2f+i*0.22f; v[i*4+3]=0.76f; }
      glColor4f(0.4f,0.9f,1,1); glVertexPointer(2,GL_FLOAT,0,v); glDrawArrays(GL_LINES,0,8); }
    { float v[40]; for (int i=0;i<20;i++){ v[i*2]=0.08f+i*0.045f; v[i*2+1]=0.55f+0.06f*fsinf(ph+i*0.5f); }
      glColor4f(0.6f,1,0.5f,1); glVertexPointer(2,GL_FLOAT,0,v); glDrawArrays(GL_LINE_STRIP,0,20); }
    { float v[24]; for (int i=0;i<6;i++){ v[i*4]=0.12f+i*0.14f; v[i*4+1]=0.34f; v[i*4+2]=0.12f+i*0.14f; v[i*4+3]=0.44f; }
      glColor4f(1,0.5f,0.7f,1); glVertexPointer(2,GL_FLOAT,0,v); glDrawArrays(GL_TRIANGLE_STRIP,0,12); }
    { float v[20]; v[0]=0.5f; v[1]=0.16f;
      for (int i=0;i<9;i++){ float a=ph*0.5f+i*(TAU_/8.f); v[2+i*2]=0.5f+0.10f*fcosf(a); v[3+i*2]=0.16f+0.10f*fsinf(a); }
      glColor4f(0.9f,0.9f,0.4f,1); glVertexPointer(2,GL_FLOAT,0,v); glDrawArrays(GL_TRIANGLE_FAN,0,10); }
    glDisable(GL_SCISSOR_TEST);
}

/* 3 — indexed sphere (glDrawElements); drag rotates */
static void screen_indexed(int w, int h)
{
    orbit2(-1.f, -1.f);
    glClearColor(0.04f, 0.05f, 0.09f, 1.f);
    begin3d(w, h, 1.5f, 12.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
    static float a; a += 30.f * g_dt;
    glTranslatef(0,0,-3.0f);
    glRotatef(g_pitch,1,0,0); glRotatef(a+g_yaw,0,1,0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(3, GL_FLOAT, 32, g_sph_v);
    glColorPointer(4, GL_UNSIGNED_BYTE, 4, g_sph_c);   /* normal-as-color: shows the 3D form */
    glDrawElements(GL_TRIANGLES, SIDX, GL_UNSIGNED_SHORT, g_sph_i);
    glDisable(GL_SCISSOR_TEST);
}

/* 4 — dynamic VBO via glBufferSubData; drag warps amplitude */
static void screen_vbo(int w, int h)
{
    orbit_from_drag();
    static float amp = 0.18f;
    if (g_touch) { amp += g_dy * 0.002f; if (amp<0.02f) amp=0.02f; if (amp>0.5f) amp=0.5f; }
    for (int j=0;j<GN;j++) for (int i=0;i<GN;i++) {
        float x = (float)i/(GN-1)-0.5f, z = (float)j/(GN-1)-0.5f;
        float d = (x*x+z*z);
        float y = amp * fsinf(g_t*2.f - d*18.f);
        float *p=&g_grid_v[(j*GN+i)*3]; p[0]=x*2.f; p[1]=y; p[2]=z*2.f;
        float tt = y/amp*0.5f+0.5f;                       /* height -> color ramp */
        unsigned char *c=&g_grid_c[(j*GN+i)*4];
        c[0]=(unsigned char)(40+tt*200); c[1]=(unsigned char)(120+(1.f-tt)*100);
        c[2]=(unsigned char)(210-tt*120); c[3]=255;
    }
    glBindBuffer(GL_ARRAY_BUFFER, g_grid_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(g_grid_v), g_grid_v);

    glClearColor(0.03f,0.04f,0.07f,1.f);
    begin3d(w, h, 1.5f, 14.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glTranslatef(0,-0.1f,-3.2f);
    glRotatef(55.f + g_pitch*0.3f, 1,0,0);
    static float a; a += 24.f * g_dt; glRotatef(a + g_yaw, 0,1,0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid*)0);    /* position from the VBO */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glColorPointer(4, GL_UNSIGNED_BYTE, 4, g_grid_c);     /* color from a client array (mixed) */
    glDrawElements(GL_TRIANGLES, GIDX, GL_UNSIGNED_SHORT, g_grid_i);
    glDisable(GL_SCISSOR_TEST);
}

/* 5 — textured cube; horizontal drag cycles tex-env, vertical toggles filter */
static void screen_texture(int w, int h)
{
    static int env = 0, filt = 1, ce, cf;
    if (g_touch && (g_dx>8||g_dx<-8)) { if(++ce>5){ce=0; env=(env+1)&3;} }
    if (g_touch && (g_dy>8||g_dy<-8)) { if(++cf>5){cf=0; filt^=1;} }
    static const GLenum ENV[4] = { GL_MODULATE, GL_DECAL, GL_REPLACE, GL_ADD };

    glClearColor(0.06f,0.06f,0.08f,1.f);
    begin3d(w, h, 1.5f, 12.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, g_chk_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt?GL_LINEAR:GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt?GL_LINEAR:GL_NEAREST);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, ENV[env]);

    static float a; a += 54.f * g_dt;
    glTranslatef(0,0,-3.0f);
    glRotatef(20.f + g_pitch, 1,0,0); glRotatef(a + g_yaw, 0,1,0);
    glColor4f(0.85f,0.9f,1.f,1.f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(3, GL_FLOAT, 32, g_cube_v);
    glTexCoordPointer(2, GL_FLOAT, 32, &g_cube_v[6]);
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, g_cube_i);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_SCISSOR_TEST);
    orbit2(1.f, 1.f);
}

/* 6 — glTexSubImage2D: a moving block written into a sub-rect each frame */
static void screen_subtex(int w, int h)
{
    static int bx = 24, by = 24;
    if (g_touch) { bx = (g_tx * 56) / w; by = ((g_ty-NAV_H) * 56) / (h-NAV_H);
                   if (bx<0)bx=0; if(bx>56)bx=56; if(by<0)by=0; if(by>56)by=56; }
    else { bx = (int)(28 + 24*fsinf(g_t)); by = (int)(28 + 24*fcosf(g_t*0.7f)); }
    static unsigned char blk[8*8*4];
    unsigned char r=(unsigned char)(128+126*fsinf(g_t)), gg=(unsigned char)(128+126*fcosf(g_t*1.3f));
    for (int i=0;i<8*8;i++){ blk[i*4]=r; blk[i*4+1]=gg; blk[i*4+2]=60; blk[i*4+3]=255; }
    /* Per-frame partial update: glTexSubImage2D only patches the texture's LINEAR
     * store, which the GPU frees once the texture has been rendered (twiddled). So
     * re-spec a fresh base with glTexImage2D first (restores the linear buffer), then
     * patch the moving block onto it — the harness's Image-then-Sub pattern, repeated. */
    glBindTexture(GL_TEXTURE_2D, g_dyn_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_tex_dyn);
    glTexSubImage2D(GL_TEXTURE_2D, 0, bx, by, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, blk);

    glClearColor(0.05f,0.05f,0.06f,1.f);
    glViewport(0, NAV_H, w, h-NAV_H); glScissor(0,NAV_H,w,h-NAV_H); glEnable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, g_dyn_id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrthof(0,1,1,0,-1,1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glColor4f(1,1,1,1);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    { float v[8]={0.12f,0.12f, 0.88f,0.12f, 0.12f,0.88f, 0.88f,0.88f};
      float t[8]={0,0, 1,0, 0,1, 1,1};
      glVertexPointer(2,GL_FLOAT,0,v); glTexCoordPointer(2,GL_FLOAT,0,t);
      glDrawArrays(GL_TRIANGLE_STRIP,0,4); }
    glDisable(GL_TEXTURE_2D); glDisable(GL_SCISSOR_TEST);
}

/* 7 — blending. The 3 orbiting quads cycle src-alpha / additive blend modes (drag
 * to switch), over a textured strip.
 *   No GL_ALPHA_TEST here: it crashes on this draw path regardless of VBO- vs
 * stack-backed geometry. The GPU routes alpha-test geometry through a separate
 * punch-through pass, and the embedded GL view our surface apps render into never
 * sets that pass up — only a standalone fullscreen GL path does. See the sdk/gl.c
 * glAlphaFunc note. */
static void screen_blend(int w, int h)
{
    static int bm = 0, cb;
    if (g_touch && (g_dx>8||g_dx<-8)) { if(++cb>5){cb=0; bm=(bm+1)%3;} }
    static const GLenum SF[3]={GL_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE};
    static const GLenum DF[3]={GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE};

    glClearColor(0.04f,0.04f,0.06f,1.f);
    glViewport(0, NAV_H, w, h-NAV_H); glScissor(0,NAV_H,w,h-NAV_H); glEnable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrthof(0,1,0,1,-1,1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnable(GL_BLEND); glBlendFunc(SF[bm], DF[bm]); glDepthMask(GL_FALSE);
    const float C[3][3]={{1,0.3f,0.3f},{0.3f,1,0.4f},{0.4f,0.5f,1}};
    for (int i=0;i<3;i++) {
        float a = g_t*0.6f + i*(TAU_/3.f);
        float cx = 0.5f + 0.16f*fcosf(a), cy = 0.62f + 0.16f*fsinf(a), s = 0.22f;
        float v[8]={cx-s,cy-s, cx+s,cy-s, cx-s,cy+s, cx+s,cy+s};
        glColor4f(C[i][0],C[i][1],C[i][2], 0.5f);
        glVertexPointer(2,GL_FLOAT,0,v); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    }
    glDisable(GL_BLEND); glDepthMask(GL_TRUE);

    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, g_chk_id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4f(1,1,1,1);
    { float v[8]={0.1f,0.06f, 0.9f,0.06f, 0.1f,0.24f, 0.9f,0.24f};
      float t[8]={0,0, 3,0, 0,1, 3,1};
      glVertexPointer(2,GL_FLOAT,0,v); glTexCoordPointer(2,GL_FLOAT,0,t);
      glDrawArrays(GL_TRIANGLE_STRIP,0,4); }
    glDisable(GL_TEXTURE_2D); glDisable(GL_SCISSOR_TEST);
}

/* 8 — lighting: lit sphere; drag moves the light */
static void screen_light(int w, int h)
{
    static float lx = 1.2f, ly = 1.5f;
    if (g_touch) { lx += g_dx*0.01f; ly -= g_dy*0.01f;
                   if (lx>3)lx=3; if(lx<-3)lx=-3; if(ly>3)ly=3; if(ly<-3)ly=-3; }

    glClearColor(0.03f,0.03f,0.05f,1.f);
    begin3d(w, h, 1.5f, 12.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); glEnable(GL_NORMALIZE);
    glShadeModel(GL_SMOOTH);

    /* Directional light (w=0): the whole facing hemisphere lights evenly, vs a near
     * positional light that only grazes a thin band. Drag aims the direction. */
    GLfloat lpos[4]={lx, ly, 1.2f, 0.f};
    GLfloat ldif[4]={1.f,0.95f,0.85f,1.f}, lamb[4]={0.35f,0.35f,0.45f,1.f}, lspc[4]={1,1,1,1};
    glLightfv(GL_LIGHT0, GL_POSITION, lpos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, ldif);
    glLightfv(GL_LIGHT0, GL_AMBIENT, lamb);
    glLightfv(GL_LIGHT0, GL_SPECULAR, lspc);
    GLfloat mamb[4]={0.25f,0.3f,0.45f,1.f}, mdif[4]={0.3f,0.55f,0.9f,1.f}, mspc[4]={0.9f,0.9f,0.9f,1.f};
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, mamb);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, mdif);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspc);
    glMaterialf (GL_FRONT_AND_BACK, GL_SHININESS, 24.f);

    static float a; a += 30.f * g_dt;
    glTranslatef(0,0,-3.0f); glRotatef(a,0,1,0);
    glColor4f(1,1,1,1);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(3, GL_FLOAT, 32, g_sph_v);
    glNormalPointer(GL_FLOAT, 32, &g_sph_v[3]);
    glDrawElements(GL_TRIANGLES, SIDX, GL_UNSIGNED_SHORT, g_sph_i);
    glDisable(GL_LIGHTING); glDisable(GL_SCISSOR_TEST);
}

/* 9 — combo "everything": lit+textured cube, ground plane, translucent orbiting ring */
static void screen_combo(int w, int h)
{
    orbit_from_drag();
    glClearColor(0.02f,0.03f,0.06f,1.f);
    begin3d(w, h, 1.0f, 20.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE); glCullFace(GL_BACK);

    glTranslatef(0.f, -0.3f, -4.5f);
    glRotatef(g_pitch, 1,0,0);
    glRotatef(g_yaw,   0,1,0);

    glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); glEnable(GL_NORMALIZE);
    glShadeModel(GL_SMOOTH);
    GLfloat lp[4]={0.4f,1.f,0.6f,0.f}, ld[4]={1,1,0.95f,1}, la[4]={0.35f,0.35f,0.4f,1};
    glLightfv(GL_LIGHT0,GL_POSITION,lp); glLightfv(GL_LIGHT0,GL_DIFFUSE,ld); glLightfv(GL_LIGHT0,GL_AMBIENT,la);
    GLfloat ma[4]={0.3f,0.3f,0.35f,1}, md[4]={0.65f,0.65f,0.7f,1};
    glMaterialfv(GL_FRONT_AND_BACK,GL_AMBIENT,ma); glMaterialfv(GL_FRONT_AND_BACK,GL_DIFFUSE,md);

    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, g_chk_id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glColor4f(1,1,1,1);
    { float g = 3.f;
      float v[]={ -g,-1.f,-g, 0,1,0, 0,0,  g,-1.f,-g, 0,1,0, 4,0,
                  -g,-1.f, g, 0,1,0, 0,4,  g,-1.f, g, 0,1,0, 4,4 };
      glVertexPointer(3,GL_FLOAT,32,&v[0]); glNormalPointer(GL_FLOAT,32,&v[3]); glTexCoordPointer(2,GL_FLOAT,32,&v[6]);
      glDrawArrays(GL_TRIANGLE_STRIP,0,4); }

    static float a; a += 60.f * g_dt;
    glPushMatrix();
    glTranslatef(0.f, 0.1f, 0.f); glRotatef(a,0.3f,1.f,0.1f); glScalef(0.9f,0.9f,0.9f);
    glVertexPointer(3, GL_FLOAT, 32, g_cube_v);
    glNormalPointer(GL_FLOAT, 32, &g_cube_v[3]);
    glTexCoordPointer(2, GL_FLOAT, 32, &g_cube_v[6]);
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, g_cube_i);
    glPopMatrix();
    glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING);

    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE); glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);                    /* billboards are 2-sided — don't cull them */
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    for (int i=0;i<8;i++) {
        glPushMatrix();
        glRotatef(g_t*30.f + i*45.f, 0,1,0);   /* 8 quads, 45 deg apart, orbiting */
        glTranslatef(1.7f, 0.2f, 0.f);
        glRotatef(g_t*40.f, 0,0,1);             /* spin each quad face-on */
        float s=0.32f; float v[12]={-s,-s,0, s,-s,0, -s,s,0, s,s,0};
        glColor4f(0.3f+0.7f*(i&1), 0.5f, 1.f-0.6f*(i&1), 0.5f);
        glVertexPointer(3,GL_FLOAT,0,v); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        glPopMatrix();
    }
    glDisable(GL_BLEND); glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
}

/* 10 — textured + lit spinning torus (a second model). Exercises glClearDepthf,
 *      glLightf (scalar light param) and glGetFloatv. Drag orbits. */
static void screen_torus(int w, int h)
{
    orbit_from_drag();
    glClearColor(0.04f,0.05f,0.10f,1.f);
    begin3d(w, h, 1.5f, 14.f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
    glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); glEnable(GL_NORMALIZE);
    glShadeModel(GL_SMOOTH);
    GLfloat lp[4]={0.5f,1.f,0.8f,0.f}, ld[4]={1,0.96f,0.9f,1}, la[4]={0.3f,0.3f,0.38f,1};
    glLightfv(GL_LIGHT0,GL_POSITION,lp); glLightfv(GL_LIGHT0,GL_DIFFUSE,ld); glLightfv(GL_LIGHT0,GL_AMBIENT,la);
    glLightf(GL_LIGHT0, GL_CONSTANT_ATTENUATION, 1.0f);   /* scalar glLightf */
    GLfloat ma[4]={0.3f,0.3f,0.32f,1}, md[4]={0.85f,0.85f,0.9f,1}, ms[4]={0.7f,0.7f,0.7f,1};
    glMaterialfv(GL_FRONT_AND_BACK,GL_AMBIENT,ma);
    glMaterialfv(GL_FRONT_AND_BACK,GL_DIFFUSE,md);
    glMaterialfv(GL_FRONT_AND_BACK,GL_SPECULAR,ms);
    glMaterialf(GL_FRONT_AND_BACK,GL_SHININESS,20.f);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, g_tor_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    static float a; a += 60.f * g_dt;
    glTranslatef(0,0,-3.4f);
    glRotatef(25.f+g_pitch,1,0,0); glRotatef(a+g_yaw,0,0,1);
    glColor4f(1,1,1,1);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_NORMAL_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(3, GL_FLOAT, 32, g_tor_v);
    glNormalPointer(GL_FLOAT, 32, &g_tor_v[3]);
    glTexCoordPointer(2, GL_FLOAT, 32, &g_tor_v[6]);
    glDrawElements(GL_TRIANGLES, TIDX, GL_UNSIGNED_SHORT, g_tor_i);
    { GLfloat mv[16]; glGetFloatv(GL_MODELVIEW_MATRIX, mv); (void)mv; }   /* exercise glGetFloatv */
    glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_SCISSOR_TEST);
}

/* 11 — multitexture: two texture units combined on a cube. Exercises glActiveTexture,
 *      glClientActiveTexture and glTexEnvfv (env color). Unit0 = checker (MODULATE),
 *      unit1 = a sparse detail texture (ADD). Drag orbits. */
static void screen_multitex(int w, int h)
{
    orbit2(1.f, 1.f);
    glClearColor(0.05f,0.05f,0.07f,1.f);
    begin3d(w, h, 1.5f, 12.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
    glColor4f(1,1,1,1);

    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, g_chk_id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glClientActiveTexture(GL_TEXTURE0);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 32, &g_cube_v[6]);

    glActiveTexture(GL_TEXTURE1);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, g_det_id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
    { GLfloat envc[4]={0.2f,0.4f,0.6f,1.f}; glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, envc); }
    glClientActiveTexture(GL_TEXTURE1);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 32, &g_cube_v[6]);

    static float a; a += 48.f * g_dt;
    glTranslatef(0,0,-3.0f);
    glRotatef(20.f+g_pitch,1,0,0); glRotatef(a+g_yaw,0,1,0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_NORMAL_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(3, GL_FLOAT, 32, g_cube_v);
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, g_cube_i);

    /* restore single-texture state so later screens aren't affected */
    glActiveTexture(GL_TEXTURE1); glDisable(GL_TEXTURE_2D);
    glClientActiveTexture(GL_TEXTURE1); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glActiveTexture(GL_TEXTURE0); glClientActiveTexture(GL_TEXTURE0);
    glDisable(GL_TEXTURE_2D); glDisable(GL_SCISSOR_TEST);
}

/* =========================== entry =================================== */

static int g_inited;

static void upload_tex(GLuint id, int sz, const void *data, GLenum filt)
{
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

static void lazy_init(void)
{
    build_cube(); build_sphere(); build_torus(); build_grid_indices(); build_checker();
    for (int y=0;y<64;y++) for (int x=0;x<64;x++) {           /* base for the subtex screen */
        unsigned char *p=&g_tex_dyn[(y*64+x)*4];
        int grid = (x%16==0)||(y%16==0);
        p[0]=grid?90:(unsigned char)(20+x*2);
        p[1]=grid?90:(unsigned char)(30+y*2);
        p[2]=grid?120:130; p[3]=255;
    }
    for (int y=0;y<32;y++) for (int x=0;x<32;x++) {           /* detail tex (2nd unit) */
        unsigned char *p=&g_tex_detail[(y*32+x)*4];
        int dot=((x&7)<3)&&((y&7)<3);
        p[0]=dot?255:0; p[1]=dot?180:0; p[2]=dot?40:0; p[3]=255;
    }

    /* FIXED resource ids, reused every launch. The OS GL context persists across our
     * launches, so reusing the same ids rebinds the SAME objects -> no per-launch leak.
     * (glGen'ing fresh ids each launch leaked: the freed app arena forgets them but the
     * GL objects live on in the context, accumulating until the GPU runs out.) */
    g_grid_vbo = 60; g_chk_id = 240; g_dyn_id = 241; g_det_id = 242; g_tor_tex = 243;

    glBindBuffer(GL_ARRAY_BUFFER, g_grid_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(g_grid_v), g_grid_v, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    upload_tex(g_chk_id, 64, g_tex_checker, GL_LINEAR);
    upload_tex(g_dyn_id, 64, g_tex_dyn,     GL_NEAREST);
    upload_tex(g_det_id, 32, g_tex_detail,  GL_LINEAR);
    upload_tex(g_tor_tex,64, g_tex_checker, GL_LINEAR);

    /* exercise the remaining API surface once (results unused; proves the bindings). */
    { const GLubyte *s = glGetString(GL_VERSION); (void)s;
      GLint mx=0; glGetIntegerv(GL_MAX_TEXTURE_SIZE,&mx); (void)mx; }
    glClearStencil(0);
    glNormal3f(0.f, 0.f, 1.f);
    { GLfixed im[16]={0}; im[0]=im[5]=im[10]=im[15]=0x10000;   /* glLoadMatrixx: 16.16 identity */
      glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadMatrixx(im); glPopMatrix(); }
    { GLuint t=0,b=0; glGenTextures(1,&t); glDeleteTextures(1,&t);  /* gen/delete round-trip */
      glGenBuffers(1,&b); glDeleteBuffers(1,&b); }

    g_inited = 1;
}

void gl_app_init(void)
{
    /* No GL context current at load — defer all GL to the first gl_app_frame. */
}

extern unsigned hb_time_uptime_ms(void);   /* real wall clock (hardware timer) */

void gl_app_frame(int w, int h, uint32_t frame)
{
    (void)frame;
    if (!g_inited) lazy_init();

    /* Time-based animation, like the OS's own GL code (the system's GL apps sample
     * the millisecond-clock read() and interpolates over real durations). Driving motion
     * by REAL elapsed time means uneven timer firing / dropped frames don't change
     * speed — frame-count-based increments did, hence the jitter. */
    static unsigned s_last_ms;
    unsigned now_ms = hb_time_uptime_ms();
    g_dt = s_last_ms ? (float)(now_ms - s_last_ms) * 0.001f : 0.016f;
    s_last_ms = now_ms;
    if (g_dt > 0.1f) g_dt = 0.1f;       /* clamp after a stall so nothing leaps */
    g_t += g_dt * 3.0f;                  /* logical time at the previous average rate */

    touch_update(w, h);
    { static int ls = -1; if (g_screen != ls) { ls = g_screen; hb_trace_log("AGLSCRN ", (unsigned)g_screen, 0); } }

    glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST);
    glColorMask(1,1,1,1); glDepthMask(GL_TRUE);

    switch (g_screen) {
    case 0: screen_clear(w, h);   break;
    case 1: screen_cube(w, h);    break;
    case 2: screen_prims(w, h);   break;
    case 3: screen_indexed(w, h); break;
    case 4: screen_vbo(w, h);     break;
    case 5: screen_texture(w, h); break;
    case 6: screen_subtex(w, h);  break;
    case 7: screen_blend(w, h);   break;
    case 8: screen_light(w, h);    break;
    case 9: screen_combo(w, h);    break;
    case 10: screen_torus(w, h);   break;
    default: screen_multitex(w, h); break;
    }

    draw_nav(w, h);
    (void)glGetError();   /* drain the GL error each frame (exercise + keep it clean) */
}
