/* Atlas loader: the original's one 1024x1024 texture, repacked into 256x128 pages.
 * Every page is uploaded before the first frame is drawn. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "platform.h"
#include "gfx.h"
#include "sprites.h"

#define MAX_IMAGES 96
#define MAX_PIECES 160
#define MAX_PAGES  40
#define PAGE_W 256
#define PAGE_H 128
typedef struct { char name[32]; uint16_t w,h,start,count,opaque; } Image;
typedef struct { uint16_t page,x,y,w,h,dx,dy,pad; } Piece;
static Image images[MAX_IMAGES];
static Piece pieces[MAX_PIECES];
static int textures[MAX_PAGES], count, page_count;
static uint16_t u16(const unsigned char *p) { return (uint16_t)(p[0] | p[1]<<8); }

void sprites_forget(void) { memset(textures,0,sizeof textures); count=page_count=0; }
int sprites_load(void) {
    uint32_t size=0; int i,j,ns,np,nc;
    unsigned char *data=plat_read_file("sprites.bin",&size,0), *p, formats[MAX_PAGES];
    if(!data || size<12 || memcmp(data,"FBA1",4)) { free(data);return -1; }
    np=u16(data+4);ns=u16(data+6);nc=u16(data+8);
    uint32_t table=(uint32_t)(np+3)&~3u;
    if(np<1||np>MAX_PAGES||ns<1||ns>MAX_IMAGES||nc>MAX_PIECES||size!=12u+table+44u*ns+16u*nc) {free(data);return -1;}
    memcpy(formats,data+12,(size_t)np);
    p=data+12+table;
    for(i=0;i<ns;i++,p+=44) {
        Image *s=images+i;memcpy(s->name,p,32);s->name[31]=0;
        s->w=u16(p+32);s->h=u16(p+34);s->start=u16(p+36);s->count=u16(p+38);s->opaque=u16(p+40);
        if(!s->w||!s->h||s->w>1024||s->h>1024||s->start+s->count>nc) {free(data);return -1;}
    }
    for(i=0;i<nc;i++,p+=16) {
        uint16_t *dst=(uint16_t *)(pieces+i);
        for(j=0;j<8;j++) dst[j]=u16(p+j*2);
        Piece *t=pieces+i;
        if(t->page>=np || !t->w || !t->h || t->x+t->w>PAGE_W || t->y+t->h>PAGE_H) {free(data);return -1;}
    }
    free(data);
    gfx_set_texture_budget(3u<<20);
    for(i=0;i<np;i++) {
        char path[32];snprintf(path,sizeof path,"page%02d.bin",i);
        int rgb=formats[i]==0;
        data=plat_read_file(path,&size,0);
        if(data && size==(uint32_t)PAGE_W*PAGE_H*(rgb?2u:4u))
            textures[i]=gfx_tex_create(PAGE_W,PAGE_H,rgb?GFX_FMT_RGB565:GFX_FMT_RGBA8888,data,1,0);
        free(data);
        if(!textures[i]) {
            for(j=0;j<i;j++) gfx_tex_destroy(textures[j]);
            sprites_forget();return -1;
        }
    }
    count=ns;page_count=np;
    plat_log("sprites: %d images, %d pages, %u KiB",ns,np,(unsigned)(gfx_stat_tex_bytes/1024u));
    return 0;
}
int spr_find(const char *prefix) {
    size_t n=strlen(prefix);
    for(int i=0;i<count;i++) if(!strncmp(images[i].name,prefix,n)) return i;
    return -1;
}
int spr_w(int id) { return id>=0&&id<count?images[id].w:0; }
int spr_h(int id) { return id>=0&&id<count?images[id].h:0; }

static uint32_t premultiplied(float alpha) {
    if(alpha>1.f)alpha=1.f;
    return (uint32_t)(alpha*255.f)*0x01010101u;
}
/* Everything drawn in the last game step, kept so the platform can show the same picture
 * again when it is called back sooner than the game's 60 steps a second. */
typedef struct { int id; float q[8],alpha; } Drawn;
static Drawn drawn[160];
static int drawn_count,replaying;
void spr_frame_begin(void){drawn_count=0;}
static void remember(int id,const float q[8],float alpha) {
    if(replaying||drawn_count==(int)(sizeof drawn/sizeof drawn[0]))return;
    Drawn *d=&drawn[drawn_count++];d->id=id;memcpy(d->q,q,sizeof d->q);d->alpha=alpha;
}
static void fill(const float xy[8],int white,float alpha);
void spr_frame_replay(void) {
    replaying=1;
    for(int i=0;i<drawn_count;i++) {
        const Drawn *d=&drawn[i];
        if(d->id<0)fill(d->q,d->id==-2,d->alpha);else spr_quad(d->id,d->q,d->alpha);
    }
    replaying=0;
}
void spr_quad(int id,const float q[8],float alpha) {
    if(id<0||id>=count||alpha<=0.f)return;
    remember(id,q,alpha);
    const Image *s=images+id;
    uint32_t color=premultiplied(alpha);
    /* The backgrounds and the ground have no transparency: skip the blend. */
    gfx_set_blend(s->opaque && alpha>=1.f ? GFX_BLEND_NONE : GFX_BLEND_ALPHA);
    float iw=1.f/s->w,ih=1.f/s->h;
    for(int i=0;i<s->count;i++) {
        const Piece *p=pieces+s->start+i;
        /* the piece's corners inside the image, 0..1, mapped through the quad */
        float u0=p->dx*iw,v0=p->dy*ih,u1=(p->dx+p->w)*iw,v1=(p->dy+p->h)*ih;
        float uu[4]={u0,u1,u1,u0},vv[4]={v0,v0,v1,v1},xy[8];
        for(int k=0;k<4;k++) {
            float u=uu[k],v=vv[k];
            float tx=q[0]+(q[2]-q[0])*u,ty=q[1]+(q[3]-q[1])*u;      /* along the top edge */
            float bx=q[6]+(q[4]-q[6])*u,by=q[7]+(q[5]-q[7])*u;      /* along the bottom edge */
            xy[k*2]=tx+(bx-tx)*v;xy[k*2+1]=ty+(by-ty)*v;
        }
        float uv[8]={p->x,p->y,p->x+p->w,p->y,p->x+p->w,p->y+p->h,p->x,p->y+p->h};
        gfx_quad(textures[p->page],PAGE_W,PAGE_H,xy,uv,color);
    }
}
void spr_rect(int id,int x0,int y0,int x1,int y1,float alpha) {
    float q[8]={(float)x0,(float)y0,(float)x1,(float)y0,(float)x1,(float)y1,(float)x0,(float)y1};
    spr_quad(id,q,alpha);
}
static void fill(const float xy[8],int white,float alpha) {
    if(alpha>1.f)alpha=1.f;
    uint32_t a=(uint32_t)(alpha*255.f),color=white?a*0x01010101u:a<<24;
    float uv[8]={0};
    gfx_set_blend(GFX_BLEND_ALPHA);
    gfx_quad(0,1,1,xy,uv,color);
}
void spr_fill(int x0,int y0,int x1,int y1,int white,float alpha) {
    if(alpha<=0.f)return;
    float xy[8]={(float)x0,(float)y0,(float)x1,(float)y0,(float)x1,(float)y1,(float)x0,(float)y1};
    remember(white?-2:-1,xy,alpha);
    fill(xy,white,alpha);
}
