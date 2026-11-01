/* Headless dependency adapters for gameplay-only reference comparisons. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
uint64_t plat_time_us(void){return 0;}
void *plat_read_file(const char *p,uint32_t *n,int s){
    if(s||strcmp(p,"scenes.bin"))return NULL;
    FILE *f=fopen("build/data/scenes.bin","rb");if(!f)return NULL;
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    void *data=malloc(size);if(!data||fread(data,1,size,f)!=(size_t)size){free(data);fclose(f);return NULL;}
    fclose(f);*n=(uint32_t)size;return data;
}
int plat_write_file(const char *p,const void *d,uint32_t n){(void)p;(void)d;(void)n;return 0;}
void plat_log(const char *f,...){va_list a;va_start(a,f);vfprintf(stderr,f,a);va_end(a);}
float plat_tilt_x(void){return 0;}
int sprites_load(void){return 0;}
void sprites_forget(void){}
void sprite(const char *n,float x,float y,float s,float a){(void)n;(void)x;(void)y;(void)s;(void)a;}
void sprite_rect(const char *n,float x,float y,float w,float h,float a){(void)n;(void)x;(void)y;(void)w;(void)h;(void)a;}
void draw_score(int v,float x,float y){(void)v;(void)x;(void)y;}
void draw_number(int v,float x,float y,int c){(void)v;(void)x;(void)y;(void)c;}
void gfx_set_clip(float x,float y,float w,float h){(void)x;(void)y;(void)w;(void)h;}
void gfx_forget_all(void){}
void gfx_quad(int t,int w,int h,const float *x,const float *u,uint32_t c){(void)t;(void)w;(void)h;(void)x;(void)u;(void)c;}
float plat_audio_play(const char *n,float v,int l,int t){(void)n;(void)v;(void)l;(void)t;return 0;}
void plat_audio_stop(const char *n){(void)n;}
