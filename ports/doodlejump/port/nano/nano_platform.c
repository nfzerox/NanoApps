/* NanoApps SDK adapter. All gameplay runs in ARM state after SDK FPU setup. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_surface_input.h"
#include "../src/platform.h"
#include "../src/gfx.h"
#include "../src/game.h"

#define DATA_DIR "/Data/DoodleJump"
extern void *memalign(size_t,size_t);
extern void port_log_flush(const char *path);
static struct { char name[48];uint32_t size; } files[64];
static int file_count,initialized,failed;
static float tilt;
static struct { uint64_t periods,cpu;uint32_t count,max_period,slow;int written; } perf;
uint8_t dj_fast_redraw;          /* set by the entry: the resident honours a short heartbeat */
#define PACE_US 16667u            /* frame spacing when it does: the display's 60 Hz */
/* Platform time between our return and the next callback. It redraws in pairs, so the
 * gap is short (~5 ms) then long (~8.5 ms) in strict alternation: predict the next one. */
static uint32_t gap_estimate=6500,gap_short=5200,gap_long=8600;
static int last_short;
uint64_t plat_time_us(void) {
    static uint32_t last;static uint64_t high;
    uint32_t now=hb_time_uptime_us();if(now<last)high+=1ull<<32;last=now;return high|now;
}
float plat_tilt_x(void){return tilt;}
/* Same aligned, oversized read pattern used by the tested nano game ports. */
static void *read_file(const char *path,uint32_t expected,uint32_t *size) {
    if(expected>4u*1024u*1024u)return NULL;
    uint32_t cap=((expected+1u+4095u)&~4095u)+4096u;
    unsigned char *data=memalign(64,cap);if(!data)return NULL;
    uint32_t got=hb_fs_read(path,data,cap);
    if(!got||got>expected){free(data);return NULL;}
    data[got]=0;if(size)*size=got;return data;
}
static int manifest(void) {
    uint32_t size;char *text=read_file(DATA_DIR "/files.lst",8192,&size),*p;
    if(!text)return -1;
    file_count=0;
    for(p=text;*p&&file_count<64;) {
        unsigned len=0;uint32_t n=0;
        while(*p&&*p!=' '&&*p!='\n'&&len<47)files[file_count].name[len++]=*p++;
        files[file_count].name[len]=0;
        while(*p==' ')p++;
        while(*p>='0'&&*p<='9'){n=n*10+(unsigned)(*p++-'0');if(n>4u*1024u*1024u){free(text);return -1;}}
        while(*p&&*p!='\n')p++;
        if(*p)p++;
        if(len&&n){files[file_count].size=n;file_count++;}
    }
    free(text);return file_count?0:-1;
}
void *plat_read_file(const char *name,uint32_t *size,int save) {
    char path[112];uint32_t expected=16;
    if(strstr(name,"..")||name[0]=='/')return NULL;
    if(!save) {
        int i;for(i=0;i<file_count;i++)if(!strcmp(files[i].name,name))break;
        if(i==file_count)return NULL;
        expected=files[i].size;
    }
    snprintf(path,sizeof path,DATA_DIR "/%s",name);
    return read_file(path,expected,size);
}
int plat_write_file(const char *name,const void *data,uint32_t size) {
    char path[112];if(!size||size>65536||strstr(name,"..")||name[0]=='/')return -1;
    uint32_t cap=(size+4095u)&~4095u;void *buf=memalign(64,cap);if(!buf)return -1;
    memset(buf,0,cap);memcpy(buf,data,size);snprintf(path,sizeof path,DATA_DIR "/%s",name);
    hb_fs_mkdir(DATA_DIR);int result=hb_fs_write(path,buf,size)?0:-1;free(buf);return result;
}
void plat_log(const char *format,...) {
    va_list args;va_start(args,format);vprintf(format,args);va_end(args);putchar('\n');
}
/* Sound events stay present in gameplay. The SDK's single-shot audio API cannot
 * yet reproduce the original overlapping effects and looping UFO track. */
float plat_audio_play(const char *name,float volume,int loop,int track) {
    (void)name;(void)volume;(void)loop;(void)track;return 0;
}
void plat_audio_stop(const char *name){(void)name;}
void plat_audio_track_volume(int track,float volume){(void)track;(void)volume;}
void plat_audio_clip_volume(const char *name,float volume){(void)name;(void)volume;}
void port_crumb(const char *tag,uint32_t a,uint32_t b){(void)tag;(void)a;(void)b;}

/* Keep the screen lit: the game is played by tilting, so the OS sees no touches and
 * dims after ~20-40 s. Reset its idle clock like a touch every 10 s
 * (firmware 39579dba addresses). */
static void keep_awake(void) {
    static uint64_t last;
    uint64_t now=plat_time_us();
    if(last && now-last<10000000ull)return;
    last=now;
    typedef void *(*instance_fn)(void);
    typedef void (*event_fn)(void *,int);
    void *manager=((instance_fn)(0x0842ae80u|1u))();
    if(manager)((event_fn)(0x084069d8u|1u))(manager,4);
}

void dj_nano_frame(int w,int h,uint32_t frame) {
    static uint64_t last;static uint32_t previous_frame;static int previous_state,previous_live;
    if(w<1||h<1||failed)return;
    if(initialized && frame<previous_frame) {game_gl_lost();initialized=0;last=0;}
    previous_frame=frame;
    if(!initialized) {
        if(manifest() || game_init(320,480)) {
            plat_log("Doodle Jump initialization failed");port_log_flush(DATA_DIR "/log.txt");failed=1;return;
        }
        initialized=1;plat_log("ready: heap=%u largest=%u texture=%u",hb_os_heap_free(),hb_os_heap_largest(),gfx_stat_tex_bytes);
        port_log_flush(DATA_DIR "/log.txt");
    }
    keep_awake();
    static uint64_t previous_end;
    uint64_t now=plat_time_us();uint32_t period=last?(uint32_t)(now-last):16667;
    uint32_t gap=previous_end?(uint32_t)(now-previous_end):0;
    if(gap>2000u && gap<16000u) {
        last_short=gap<(gap_short+gap_long)/2u;
        if(last_short)gap_short=(gap_short*7u+gap)/8u;else gap_long=(gap_long*7u+gap)/8u;
        gap_estimate=last_short?gap_long:gap_short;   /* the next gap */
    }
    float dt=period*1e-6f;last=now;
    int live=G.state==DJ_PLAY && G.death==DJ_ALIVE && !G.player_frozen;
    int sample=live && previous_live;
    if(G.state==DJ_PLAY && previous_state!=DJ_PLAY)memset(&perf,0,sizeof perf);
    if(sample) {
        perf.periods+=period;perf.count++;perf.slow+=period>20000;
        if(period>perf.max_period)perf.max_period=period;
    }
    previous_live=live;previous_state=G.state;
    /* The nano's portrait X axis has the opposite sign to the iPhone input. */
    int32_t acceleration[3];hb_accel_read_milli_g(acceleration);tilt=(float)acceleration[0]*-.001f;
    hb_spoint_t finger;hb_surface_touch_read(&finger);
    PlatTouches touch={0};if(finger.down){touch.count=1;touch.pt[0].x=finger.x*320.f/w;touch.pt[0].y=finger.y*480.f/h;}
    uint32_t bg=game_bg_color();
    gfx_begin_frame(w,h,320,480,0,((bg>>16)&255)/255.f,((bg>>8)&255)/255.f,(bg&255)/255.f);
    game_frame(dt,&touch);gfx_end_frame();
    if(sample)perf.cpu+=plat_time_us()-now;
    /* With the short heartbeat the platform shows frames as fast as we finish them;
     * hold each one to its 60 Hz slot (a spin: the SDK has no sleep for the UI task). */
    if(dj_fast_redraw && PACE_US>gap_estimate) {
        /* a fixed beat: an overrun is made up by the next frame; start again when
         * more than a frame off it */
        static uint64_t slot;
        if(!slot || now>slot+PACE_US || now+PACE_US<slot)slot=now;
        slot+=PACE_US;
        uint64_t until=slot-gap_estimate;
        while(plat_time_us()<until){}
    }
    previous_end=plat_time_us();
    if(G.state==DJ_OVER && !perf.written) {
        perf.written=1;
        uint32_t fps10=perf.periods?(uint32_t)((uint64_t)perf.count*10000000/perf.periods):0;
        plat_log("run score=%d frames=%u fps=%u.%u avg_submit_us=%u max_period_us=%u periods_over_20ms=%u fast_redraw=%u gap=%u",
            G.score,perf.count,fps10/10,fps10%10,perf.count?(uint32_t)(perf.cpu/perf.count):0,perf.max_period,perf.slow,
            (unsigned)dj_fast_redraw,(unsigned)((gap_short+gap_long)/2u));
        port_log_flush(DATA_DIR "/log.txt"); /* Once after a run; no on-screen counter or per-frame disk writes. */
    }
}
