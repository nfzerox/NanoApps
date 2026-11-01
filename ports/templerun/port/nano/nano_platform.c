/* NanoApps SDK adapter for Temple Run: files, textures, touch, tilt, timing and the log. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_surface_input.h"
#include <setjmp.h>
#include "gl.h"
#include "../src/platform.h"
#include "../src/game.h"
#include "../src/glfe.h"
#include "../src/rt.h"

#define DATA_DIR "/Data/TempleRun"
#define MAX_FILES 256
extern void *memalign(size_t,size_t);
extern void port_log_flush(const char *path);
static struct { char name[48];uint32_t size; } files[MAX_FILES];
static int file_count,initialized,failed;
uint8_t tr_fast_redraw;          /* set by the entry: the resident honours a short heartbeat */
uint64_t plat_time_us(void) {
    static uint32_t last;static uint64_t high;
    uint32_t now=hb_time_uptime_us();if(now<last)high+=1ull<<32;last=now;return high|now;
}
/* Same aligned, oversized read pattern used by the tested nano game ports. */
/* File buffers come straight from the OS heap and go back as soon as the file is used, so
 * the arena need not have room for the largest file. The header before the data is the one
 * the arena's free() reads to find where a block came from. */
typedef struct { void *base; uint32_t size, pad[2]; } alloc_header;
static void *os_buffer(uint32_t cap) {
    unsigned char *raw=hb_os_alloc(cap+64u);if(!raw)return NULL;
    unsigned char *p=(unsigned char *)(((uintptr_t)raw+16u+63u)&~(uintptr_t)63u);
    alloc_header *h=(alloc_header *)(void *)p-1;h->base=raw;h->size=cap;
    return p;
}
static void *read_file(const char *path,uint32_t expected,uint32_t *size) {
    if(expected>4u*1024u*1024u)return NULL;
    uint32_t cap=((expected+1u+4095u)&~4095u)+4096u;
    unsigned char *data=os_buffer(cap);if(!data)return NULL;
    uint32_t got=hb_fs_read(path,data,cap);
    if(!got||got>expected){free(data);return NULL;}
    data[got]=0;if(size)*size=got;return data;
}
static int manifest(void) {
    uint32_t size;char *text=read_file(DATA_DIR "/files.lst",16384,&size),*p;
    if(!text)return -1;
    file_count=0;
    for(p=text;*p&&file_count<MAX_FILES;) {
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
    char path[112];uint32_t expected=65536;
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
    hb_fs_mkdir(DATA_DIR);int result=hb_fs_write(path,buf,size)?0:-1;
    /* the newest file is the one lost if the iPod restarts soon after: let that be this one */
    memset(buf,0,cap);memcpy(buf,"saved\n",6);hb_fs_write(DATA_DIR "/sync.txt",buf,6);
    free(buf);return result;
}
void plat_log(const char *format,...) {
    va_list args;va_start(args,format);vprintf(format,args);va_end(args);putchar('\n');
}
/* Breadcrumbs, as in the Angry Birds port: a small RAM ring tagged with a magic. RAM
 * survives a panic reboot, so the next launch finds the previous run's ring and saves it
 * as prevboot.txt. Never written to disk while running. */
#define CRUMB_MAGIC0 0x54525542u
#define CRUMB_MAGIC1 0x43524d42u
typedef struct { uint32_t magic0,magic1,len,seq;char text[8176]; } crumb_ring_t;
static crumb_ring_t *s_ring;
void port_crumb(const char *tag,uint32_t a,uint32_t b) {
    static const char hex[]="0123456789abcdef";
    static uint32_t s_t0;
    char line[40];int n=0,i;
    if(!s_t0)s_t0=hb_time_uptime_us();
    if(b==0)b=(hb_time_uptime_us()-s_t0)/1000u;   /* ms since the first crumb */
    while(*tag&&n<12)line[n++]=*tag++;
    line[n++]=' ';
    for(i=28;i>=0;i-=4)line[n++]=hex[(a>>i)&15];
    line[n++]=' ';
    for(i=28;i>=0;i-=4)line[n++]=hex[(b>>i)&15];
    line[n++]='\n';
    if(!s_ring) {
        s_ring=malloc(sizeof(crumb_ring_t));
        if(!s_ring)return;
        s_ring->magic0=CRUMB_MAGIC0;s_ring->magic1=CRUMB_MAGIC1;s_ring->len=0;s_ring->seq=hb_time_uptime_us();
    }
    if(s_ring->len>sizeof(s_ring->text))s_ring->len=0;
    if(s_ring->len+(uint32_t)n>sizeof(s_ring->text)) {
        uint32_t cut=s_ring->len/2;
        while(cut<s_ring->len&&s_ring->text[cut-1]!='\n')cut++;
        memmove(s_ring->text,s_ring->text+cut,s_ring->len-cut);s_ring->len-=cut;
    }
    for(i=0;i<n;i++)s_ring->text[s_ring->len++]=line[i];
    __asm__ volatile("dsb":::"memory");
}
/* Previous run's ring: scan the heap region for the magic (16-byte aligned blocks). */
static void save_previous_crumbs(const char *file) {
    for(uint32_t a=0x08800000u;a<0x0B800000u;a+=16u) {
        crumb_ring_t *r=(crumb_ring_t *)(uintptr_t)a;
        if(r->magic0==CRUMB_MAGIC0&&r->magic1==CRUMB_MAGIC1&&r!=s_ring&&r->len<=sizeof(r->text)) {
            uint32_t cap=(r->len+4095u)&~4095u;char *buf=memalign(64,cap?cap:4096u);
            if(buf) {
                memcpy(buf,r->text,r->len);
                hb_fs_mkdir("/Data");hb_fs_mkdir(DATA_DIR);
                char path[112];snprintf(path,sizeof path,DATA_DIR "/%s",file);
                hb_fs_write(path,buf,r->len);free(buf);
            }
            r->magic0=0;   /* consumed */
            return;
        }
    }
}

static void keep_awake(void) {          /* as the other nano game ports (firmware 39579dba addresses) */
    static uint64_t last;
    uint64_t now=plat_time_us();
    if(last && now-last<10000000ull)return;
    last=now;
    typedef void *(*instance_fn)(void);
    typedef void (*event_fn)(void *,int);
    void *manager=((instance_fn)(0x0842ae80u|1u))();
    if(manager)((event_fn)(0x084069d8u|1u))(manager,4);
}

/* The newest file written is lost if the iPod reboots soon after (the log came back empty
 * three times), so the log is followed by a second small file. */
static void flush_log(void) {
    port_log_flush(DATA_DIR "/log.txt");
    char *mark=memalign(64,4096);
    if(mark){memset(mark,0,4096);memcpy(mark,"log written\n",12);hb_fs_write(DATA_DIR "/sync.txt",mark,12);free(mark);}
}

void plat_log_flush(void) { flush_log(); }

/* Fatal errors unwind to the frame driver, which stops the game; spinning would freeze the
 * iPod's UI task. */
static jmp_buf fatal_jump;
static int fatal_armed;
void port_fatal(int code) {
    (void)code;
    failed=1;
    if(fatal_armed)longjmp(fatal_jump,1);
    for(;;){}
}
void plat_fatal(const char *message) {
    plat_log("fatal: %s",message);port_crumb("fatal",0,0);flush_log();port_fatal(1);
}

/* ---- textures: the original PVRTC files as they are; the PNG sheets as RGBA4444 ---- */
#ifndef GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG
#define GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG 0x8C00
#endif
#ifndef GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG 0x8C02
#endif
#ifndef GL_LINEAR_MIPMAP_NEAREST
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
static uint32_t texture_bytes;
static uint32_t u32le(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
unsigned rt_host_load_texture(const char *name,const char *file,int repeat) {
    char base[48],path[56];uint32_t size=0;GLuint t=0;int ok=0;
    snprintf(base,sizeof base,"%s",file);
    char *dot=strrchr(base,'.');
    if(dot)*dot=0;
    port_crumb("texture",(uint32_t)name[0]<<8|(uint32_t)name[1],0);
    glGenTextures(1,&t);if(!t){plat_log("texture %s: no texture name",name);return 0;}
    glBindTexture(GL_TEXTURE_2D,t);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,repeat?GL_REPEAT:GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,repeat?GL_REPEAT:GL_CLAMP_TO_EDGE);
    snprintf(path,sizeof path,"%s.pvr",base);          /* the PNG sheets were re-encoded as PVRTC too */
    uint8_t *d=plat_read_file(path,&size,0);
    while(glGetError()){}
    if(d && size>52 && u32le(d)==52 && !memcmp(d+44,"PVR!",4)) {
        /* a 52-byte PVR v2 header, then PVRTC 4bpp levels, largest first */
        uint32_t h=u32le(d+4),w=u32le(d+8),mips=u32le(d+12),total=u32le(d+20),level=0,lw=w,lh=h,used=0,skip=0,sent=0;
        const uint8_t *p=d+52,*end=d+52+total;
        /* The panel is 240 pixels wide: a 1024-pixel texture's largest level is never the one
         * shown, so it is left out (a quarter of the memory, and less for the GPU to read). */
        if(mips>=2 && (w>512||h>512))skip=1;
        GLenum format=u32le(d+40)?GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG:GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG;
        if(end<=d+size) {
            for(;level<=mips&&p<end;level++) {
                uint32_t bw=lw<8?8:lw,bh=lh<8?8:lh,bytes=bw*bh/2;
                if(p+bytes>end)break;
                if(level>=skip) {
                    port_crumb("level",(level<<16)|lw,0);
                    glCompressedTexImage2D(GL_TEXTURE_2D,(GLint)(level-skip),format,(GLsizei)lw,(GLsizei)lh,0,(GLsizei)bytes,p);
                    GLenum error=glGetError();
                    if(error){plat_log("texture %s level %u (%ux%u, format %x): GL error %x",name,(unsigned)level,(unsigned)lw,(unsigned)lh,(unsigned)format,(unsigned)error);break;}
                    used+=bytes;sent++;
                }
                p+=bytes;
                if(lw==1&&lh==1){level++;break;}
                lw=lw>1?lw/2:1;lh=lh>1?lh/2:1;
            }
            /* smaller levels only if the file has them all (a lone level with a mipmap filter draws white) */
            if(mips>0 && level>mips)glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_NEAREST);
            ok=sent>=1;texture_bytes+=used;
        }
    }
    free(d);
    if(!ok){plat_log("texture %s (%s): could not be loaded",name,path);glDeleteTextures(1,&t);return 0;}
    return t;
}
void rt_host_sound(const char *name,int loop,float pitch,int stop){(void)name;(void)loop;(void)pitch;(void)stop;}

/* ---- touch ---- */
/* The resident keeps only the finger's latest state, and a frame here can take 40 ms: a
 * quick tap would come and go between two looks. So the state is sampled all through the
 * frame (plat_poll) and the changes are queued for the game. */
#define TOUCH_QUEUE 24
static struct { int16_t x,y;uint8_t down; } touch_queue[TOUCH_QUEUE];
static int touch_count,touch_last_down;static int16_t touch_last_x,touch_last_y;
static uint32_t touch_presses;static int touch_report;
/* The finger from the OS's own touch list (looked at, not altered). The resident's mailbox
 * is only refreshed when the UI task gets round to its touch events, and this game's long
 * frames leave it little time; the list itself is filled by the touch driver. */
#define OS_TOUCH_HEAD ((volatile uint32_t *)0x089a5298u)
static uint32_t touch_from_list,touch_from_mailbox,touch_stale,touch_nodes_most,touch_lifts;
/* 1 with the finger's place if the list holds a touch that is down, 2 if all it holds have
 * lifted, 0 if it is empty or cannot be read. Every entry is looked at: a touch that has
 * lifted can still be at the front when the next one is already down. */
static int list_touch(int16_t *x,int16_t *y) {
    volatile uint32_t *head=OS_TOUCH_HEAD;
    if(head[5]==0)return 0;
    uint32_t end=head[4],nodes=0;int down=0;
    if(end<0x08000000u||end>=0x10000000u)return 0;
    for(uint32_t node=*(volatile uint32_t *)end;node!=end && nodes<8;node=*(volatile uint32_t *)node,nodes++) {
        if(node<0x08000000u||node>=0x10000000u)return 0;
        volatile int32_t *value=(volatile int32_t *)(node+8);
        if(*((volatile uint8_t *)(node+8)+4)==1)continue;      /* status 1: lifted */
        *x=(int16_t)value[2];*y=(int16_t)value[3];down=1;
    }
    if(nodes>touch_nodes_most)touch_nodes_most=nodes;
    return down?1:nodes?2:0;
}
/* The finger now. The OS list is what the touch driver keeps and is right at once; the
 * resident's mailbox follows only when the UI task has handled its events, which during a
 * 40 ms frame can be a good while later. Going by the mailbox whenever it said "down" held a
 * finger down after it had lifted, and the engine takes one swipe per touch: the next swipe
 * was then not a new touch and did nothing. So when the list shows the touch lifted, the
 * mailbox's "down" is not believed until it has caught up: until it has shown the finger up,
 * or down where the list last had it. */
void plat_poll(void) {
    static int distrust;static int16_t listed_x,listed_y;
    hb_spoint_t f;hb_surface_touch_read(&f);
    int16_t lx=0,ly=0;
    int listed=list_touch(&lx,&ly),down;
    if(!f.down)distrust=0;
    else if(distrust) {
        int dx=f.x-listed_x,dy=f.y-listed_y;
        if(dx>-12 && dx<12 && dy>-12 && dy<12 && listed==1)distrust=0;
    }
    if(listed==1){down=1;f.x=lx;f.y=ly;listed_x=lx;listed_y=ly;touch_from_list++;}
    else if(listed==2){down=0;if(touch_last_down)touch_lifts++;if(f.down){distrust=1;touch_stale++;}}
    else if(f.down && !distrust){down=1;touch_from_mailbox++;}
    else {down=0;if(f.down)touch_stale++;}
    if(!down){f.x=touch_last_x;f.y=touch_last_y;}       /* a touch ends where it was last seen */
    if(down==touch_last_down && (!down || (f.x==touch_last_x && f.y==touch_last_y)))return;
    if(down && !touch_last_down) {
        touch_presses++;
        if(touch_presses<=3)touch_report=1;              /* the first few are logged, to check input on hardware */
    }
    touch_last_down=down;touch_last_x=f.x;touch_last_y=f.y;
    if(touch_count<TOUCH_QUEUE) {
        touch_queue[touch_count].x=f.x;touch_queue[touch_count].y=f.y;touch_queue[touch_count].down=(uint8_t)down;touch_count++;
    } else if(!down) {                                   /* never lose a release */
        touch_queue[TOUCH_QUEUE-1].x=f.x;touch_queue[TOUCH_QUEUE-1].y=f.y;touch_queue[TOUCH_QUEUE-1].down=0;
    }
}

/* ---- frames ---- */
/* The engine advances by the time that has really passed (as on the phone), so the frame
 * rate only sets how smooth it looks. Frames are held to a 30 Hz beat when the work fits. */
#define BEAT_US 33333u
#define ENGINE_HEAP 0x110000u
static uint32_t gap_short=5200,gap_long=8600,gap_estimate=6500;
static struct { uint32_t frames,max_period,max_work,over40,triangles,draws,dropped,vertices;uint64_t period,work,engine,draw,transform,submit,clear,first; } perf;
extern unsigned fe_time_draw_us;

void tr_nano_frame(int w,int h,uint32_t frame) {
    static uint64_t last,previous_end,started;static uint32_t previous_frame,count;static int reports,touching;
    if(w<1||h<1||failed)return;
    fatal_armed=1;
    if(setjmp(fatal_jump)){fatal_armed=0;return;}
    if(initialized && frame<previous_frame) {           /* a new GL view: our textures are gone */
        plat_log("the GL view was recreated; stopping");flush_log();failed=1;fatal_armed=0;return;
    }
    previous_frame=frame;
    if(!initialized) {
        static int prepared,splash_frames;
        if(!prepared) {
            prepared=1;
            plat_log("Temple Run: panel %dx%d, heap free %u, largest %u, redraw %s",w,h,hb_os_heap_free(),
                     hb_os_heap_largest(),tr_fast_redraw?"2 ms heartbeat":"16 ms heartbeat");
            {   /* the previous run's trail, if it ended in a reboot: kept under a new name each time */
                char trail[64];int n=0;
                for(;n<9;n++){snprintf(trail,sizeof trail,DATA_DIR "/prevboot-%d.txt",n);if(!hb_fs_size(trail))break;}
                snprintf(trail,sizeof trail,"prevboot-%d.txt",n);
                save_previous_crumbs(trail);
            }
            port_crumb("init",0,0);
            if(manifest()){plat_log("files.lst is missing");flush_log();failed=1;fatal_armed=0;return;}
        }
        /* The loading picture goes up before the two seconds of loading: its texture in the
         * first frame, drawn in the next two, and it stays on the panel while the rest loads. */
        if(splash_frames<3) {
            port_crumb("splash",(uint32_t)splash_frames,0);
            game_splash(w,h,splash_frames>0);
            splash_frames++;fatal_armed=0;return;
        }
        uint64_t t0=plat_time_us();
        uint32_t marker=0;void *flag=NULL;
        flag=plat_read_file("autopilot.txt",&marker,1);
        if(flag){free(flag);game_autopilot(1);plat_log("autopilot on");}
        port_crumb("heap0",hb_os_heap_free(),0);
        if(!game_init(w,h,ENGINE_HEAP,hb_time_uptime_us()|1u)) {
            plat_log("initialization failed");flush_log();failed=1;fatal_armed=0;return;
        }
        initialized=1;
        port_crumb("heap1",hb_os_heap_free(),hb_os_heap_largest());
        plat_log("loaded in %u ms: heap free %u, largest %u; engine heap %u of %u; models %u; textures %u",
                 (unsigned)((plat_time_us()-t0)/1000u),hb_os_heap_free(),hb_os_heap_largest(),(unsigned)rt_heap_peak(),ENGINE_HEAP,
                 fe_buffer_bytes,(unsigned)texture_bytes);
        flush_log();
        last=0;previous_end=0;started=plat_time_us();
    }
    keep_awake();
    uint64_t now=plat_time_us();
    uint32_t period=last?(uint32_t)(now-last):BEAT_US;
    uint32_t gap=previous_end?(uint32_t)(now-previous_end):0;
    if(gap>2000u && gap<16000u) {                       /* the platform redraws in pairs: a short gap, then a long one */
        int was_short=gap<(gap_short+gap_long)/2u;
        if(was_short)gap_short=(gap_short*7u+gap)/8u;else gap_long=(gap_long*7u+gap)/8u;
        gap_estimate=was_short?gap_long:gap_short;
    }
    last=now;

    plat_poll();
    for(int i=0;i<touch_count;i++) {
        if(touch_queue[i].down){game_touch(touching?1:0,(float)touch_queue[i].x,(float)touch_queue[i].y);touching=1;}
        else if(touching){game_touch(2,(float)touch_queue[i].x,(float)touch_queue[i].y);touching=0;}
    }
    touch_count=0;
    if(touch_report) {
        touch_report=0;
        plat_log("press %u at %d,%d (mailbox %u, list %u samples); state %d",(unsigned)touch_presses,(int)touch_last_x,(int)touch_last_y,
                 (unsigned)touch_from_mailbox,(unsigned)touch_from_list,game_state());
    }
    port_crumb("input",count,0);
    int32_t g[3]={0,0,0};hb_accel_read_milli_g(g);
    float tilt=(float)-g[0]*0.001f;                     /* the phone reports gravity; the nano the opposite */
    game_tilt(tilt>1.f?1.f:tilt<-1.f?-1.f:tilt);

    /* The first frames after the textures go up only clear the screen. */
    game_set_drawing(count>=4);
    /* (Measured on the iPod with one frame in sixteen drawn the old way, each triangle with
     * its own vertices: 8.2-8.3 ms a frame indexed against 9.5-10.0 ms.) */
    port_crumb("heap",hb_os_heap_free(),hb_os_heap_largest());
    port_crumb("frame",count,0);
    uint64_t t1=plat_time_us();
    game_frame((float)(period>250000u?250000u:period)*1e-6f);
    uint32_t work=(uint32_t)(plat_time_us()-now);
    port_crumb("drawn",count++,0);
    if(last!=now)period=0;
    perf.frames++;perf.period+=period;perf.work+=work;perf.engine+=(uint32_t)(fe_time_engine_us);
    perf.draw+=fe_time_draw_us;perf.transform+=fe_time_transform_us;perf.submit+=fe_time_submit_us;perf.clear+=fe_time_clear_us;perf.first+=fe_time_first_draw_us;perf.vertices+=(uint32_t)fe_stat_vertices_in;
    perf.triangles+=(uint32_t)fe_stat_triangles_out;perf.draws+=(uint32_t)fe_stat_draws;perf.dropped+=(uint32_t)fe_stat_dropped;
    if(period>perf.max_period)perf.max_period=period;
    if(work>perf.max_work)perf.max_work=work;
    perf.over40+=period>40000u;
    static uint32_t most_triangles,trimmed_frames,trimmed,array_frames;static uint64_t vertex_us,boxed,vertices_out,submit_indexed,submit_arrays;
    vertex_us+=fe_time_vertex_us;boxed+=(uint32_t)fe_stat_box_culled;vertices_out+=(uint32_t)fe_stat_vertices_out;
    if(fe_option_indexed)submit_indexed+=fe_time_submit_us;else {submit_arrays+=fe_time_submit_us;array_frames++;}
    if((uint32_t)fe_stat_triangles_out>most_triangles)most_triangles=(uint32_t)fe_stat_triangles_out;
    if(fe_stat_trimmed){trimmed_frames++;trimmed+=(uint32_t)fe_stat_trimmed;}
    (void)t1;

    if(tr_fast_redraw && work+gap_estimate<BEAT_US) {    /* hold the 30 Hz beat (a spin: the UI task has no sleep) */
        static uint64_t slot;
        if(!slot || now>slot+BEAT_US || now+BEAT_US<slot)slot=now;
        slot+=BEAT_US;
        uint64_t until=slot-gap_estimate;
        while(plat_time_us()<until){}
    }
    /* A line of timings at a few moments (never per frame: file writes stall the iPod). */
    static const uint16_t marks[8]={5,15,30,60,120,300,600,1200};
    uint32_t seconds=(uint32_t)((plat_time_us()-started)/1000000u);
    if(reports<8 && seconds>=marks[reports] && perf.frames) {
        reports++;
        uint32_t fps10=perf.period?(uint32_t)((uint64_t)perf.frames*10000000u/perf.period):0;
        plat_log("%u s: %u frames, %u.%u fps, work %u us = simulate %u + draw %u (transform %u, vertices %u) + submit %u (first %u); max work %u, max period %u, over 40 ms %u",
                 (unsigned)seconds,perf.frames,fps10/10,fps10%10,(unsigned)(perf.work/perf.frames),(unsigned)(perf.engine/perf.frames),
                 (unsigned)(perf.draw/perf.frames),(unsigned)(perf.transform/perf.frames),(unsigned)(vertex_us/perf.frames),(unsigned)(perf.submit/perf.frames),
                 (unsigned)(perf.first/perf.frames),perf.max_work,perf.max_period,perf.over40);
        plat_log("  in %u vertices (+%u boxed); out %u triangles, %u vertices (most %u triangles; %u left out in %u frames), %u draws; dropped %u",
                 perf.vertices/perf.frames,(unsigned)(boxed/perf.frames),perf.triangles/perf.frames,(unsigned)(vertices_out/perf.frames),
                 (unsigned)most_triangles,(unsigned)trimmed,(unsigned)trimmed_frames,perf.draws/perf.frames,perf.dropped);
        plat_log("  submit indexed %u us, arrays %u us (%u frames); state %d, distance %d, best %d",
                 (unsigned)(perf.frames>array_frames?submit_indexed/(perf.frames-array_frames):0),(unsigned)(array_frames?submit_arrays/array_frames:0),
                 (unsigned)array_frames,game_state(),game_distance(),game_best());
        plat_log("  presses %u (mailbox %u, list %u, stale %u, lifts %u, entries %u); heap free %u, engine heap %u",
                 (unsigned)touch_presses,(unsigned)touch_from_mailbox,(unsigned)touch_from_list,(unsigned)touch_stale,(unsigned)touch_lifts,
                 (unsigned)touch_nodes_most,hb_os_heap_free(),(unsigned)rt_heap_peak());
        flush_log();
        memset(&perf,0,sizeof perf);most_triangles=trimmed=trimmed_frames=array_frames=0;vertex_us=boxed=vertices_out=submit_indexed=submit_arrays=0;
        last=0;
    }
    previous_end=plat_time_us();
    fatal_armed=0;
}
