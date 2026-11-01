/* Bounded atlas loader. Every page is uploaded before the first gameplay draw. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "platform.h"
#include "gfx.h"
#include "sprites.h"
typedef struct { char name[40]; uint16_t w,h,start,count; } Sprite;
typedef struct { uint16_t page,x,y,w,h,dx,dy,pad; } Piece;
static Sprite images[80];
static Piece pieces[300];
static int textures[32], count, page_count;
static float asset_scale;
static uint16_t u16(const unsigned char *p) { return p[0] | p[1]<<8; }
static const Sprite *lookup(const char *s) {
    int i;
    for(i=0;i<count;i++) if(!strcmp(images[i].name,s)) return images+i;
    return NULL;
}
void sprites_forget(void) { memset(textures,0,sizeof textures); count=page_count=0; }
int sprites_load(void) {
    uint32_t size=0; int i,j,ns,np,nc;
    unsigned char *data=plat_read_file("sprites.bin",&size,0), *p;
    if(!data || size<16 || memcmp(data,"DJA1",4)) { free(data);return -1; }
    np=u16(data+4);ns=u16(data+6);nc=u16(data+8);
    memcpy(&asset_scale,data+12,4);
    if(u16(data+10)!=2||np<1||np>32||ns<1||ns>80||nc>300||size!=16u+48u*ns+16u*nc||
       !(asset_scale>=.25f && asset_scale<=1.f)) {free(data);return -1;}
    p=data+16;
    for(i=0;i<ns;i++,p+=48) {
        Sprite *s=images+i;memcpy(s->name,p,40);s->name[39]=0;
        s->w=u16(p+40);s->h=u16(p+42);s->start=u16(p+44);s->count=u16(p+46);
        if(!s->w||!s->h||s->w>1024||s->h>1024||s->start+s->count>nc) {free(data);return -1;}
    }
    for(i=0;i<nc;i++,p+=16) {
        uint16_t *dst=(uint16_t *)(pieces+i);
        for(j=0;j<8;j++) dst[j]=u16(p+j*2);
        Piece *t=pieces+i;
        if(t->page>=np || !t->w || !t->h || t->x+t->w>256 || t->y+t->h>128) {free(data);return -1;}
    }
    free(data);
    gfx_set_texture_budget(2u<<20);
    for(i=0;i<np;i++) {
        char path[32];snprintf(path,sizeof path,"page%02d.bin",i);
        data=plat_read_file(path,&size,0);
        if(data && size==131072) textures[i]=gfx_tex_create(256,128,GFX_FMT_RGBA8888,data,1,0);
        free(data);
        if(!textures[i]) {
            for(j=0;j<i;j++) gfx_tex_destroy(textures[j]);
            sprites_forget();return -1;
        }
    }
    count=ns;page_count=np;
    plat_log("sprites: %d images, %d pages, %d KiB",ns,np,np*128);
    return 0;
}
void sprite_size(const char *name,float *w,float *h) {
    const Sprite *s=lookup(name);*w=s?s->w:0;*h=s?s->h:0;
}
void sprite_rect(const char *name,float x,float y,float width,float height,float alpha) {
    const Sprite *s=lookup(name); int i;
    if(!s||width<=0||height<=0||alpha<=0)return;
    unsigned a=(unsigned)((alpha>1?1:alpha)*255.f);
    uint32_t color=a*0x01010101u;
    /* Convert the original y-up canvas to y-down drawing coordinates. The
     * platform stretches this full canvas to the panel, without margins. */
    x-=width*.5f;y=480.f-y-height*.5f;
    float ux=width/(s->w*asset_scale),uy=height/(s->h*asset_scale);
    for(i=0;i<s->count;i++) {
        const Piece *p=pieces+s->start+i;
        float l=x+p->dx*ux,t=y+p->dy*uy,r=l+p->w*ux,b=t+p->h*uy;
        float xy[8]={l,t,r,t,r,b,l,b};
        float uv[8]={p->x,p->y,p->x+p->w,p->y,p->x+p->w,p->y+p->h,p->x,p->y+p->h};
        gfx_quad(textures[p->page],256,128,xy,uv,color);
    }
}
void sprite(const char *name,float x,float y,float scale,float alpha) {
    const Sprite *s=lookup(name);
    if(s)sprite_rect(name,x,y,s->w*scale,s->h*scale,alpha);
}
/* Texture2D::pixelsWide is the padded power-of-two width, unlike contentSize.
 * All three score readouts in mainGameLoop use this exact advance formula. */
void draw_score(int value,float x,float y) {
    char text[16],name[2]={0,0};
    snprintf(text,sizeof text,"%d",value<0?0:value);
    for(int i=0;text[i];i++) {
        name[0]=text[i];const Sprite *s=lookup(name);if(!s)continue;
        unsigned padded=1;while(padded<s->w)padded*=2;
        x+=5.f+padded*.5f;sprite(name,x,y,1,1);
    }
}
void draw_number(int value,float x,float y,int centered) {
    char text[16],name[2]={0,0};float total=0,w,h;int i;
    snprintf(text,sizeof text,"%d",value<0?0:value);
    for(i=0;text[i];i++){name[0]=text[i];sprite_size(name,&w,&h);total+=w+1;}
    if(centered)x-=total*.5f;
    for(i=0;text[i];i++){name[0]=text[i];sprite_size(name,&w,&h);sprite(name,x+w*.5f,y,1,1);x+=w+1;}
}
