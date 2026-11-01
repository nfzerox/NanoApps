/* NanoApps GL entry. The SDK establishes the task's floating-point context
 * before calling the ARM game library. Each launch starts with fresh state. */
#include <stdint.h>
#include <string.h>
#include "hb_sdk.h"
#include "hb_shared.h"
#include "hb_heap.h"

#define APP_FILE "/Apps/Executables/Temple Run.hbapp"
extern void tr_nano_frame(int w,int h,uint32_t frame);
static uint32_t ran_before __attribute__((section(".noinit")));
extern uint8_t tr_fast_redraw;
void *payload_entry(int op,void *fb,int w,int h,hb_shared_t *sh);

/* A cached app image needs its own initial data restored on relaunch. */
static int restore_data(void) {
    extern uint8_t __data_start[],_edata[];
    uint32_t size=hb_fs_size(APP_FILE);
    if(size<24 || size>3u*1024u*1024u)return 0;
    uint32_t cap=(size+8191u)&~4095u;
    uint8_t *buf=hb_os_alloc(cap);
    if(!buf)return 0;
    int ok=0;
    if(hb_fs_read(APP_FILE,buf,cap)==size) {
        const uint32_t *hdr=(const uint32_t *)buf;
        if(hdr[0]==0x314c5248u && hdr[2]<=size-24 &&
           hdr[4]<=(size-24-hdr[2])/4 && hdr[1]<hdr[2]) {
            uint8_t *base=(uint8_t *)(((uintptr_t)payload_entry&~(uintptr_t)1)-hdr[1]);
            uint32_t d0=(uint32_t)(__data_start-base),d1=(uint32_t)(_edata-base);
            if(d0<=d1 && d1<=hdr[2] && !((uintptr_t)base&3)) {
                const uint32_t *rel=(const uint32_t *)(buf+24+hdr[2]);
                memcpy(__data_start,buf+24+d0,d1-d0);
                for(uint32_t i=0;i<hdr[4];i++)
                    if(rel[i]>=d0 && rel[i]<=d1 && d1-rel[i]>=4 && !(rel[i]&3))
                        *(uint32_t *)(base+rel[i])+=(uint32_t)(uintptr_t)base;
                ok=1;
            }
        }
    }
    hb_os_free(buf);return ok;
}
static void frame_callback(int w,int h,uint32_t frame) {
    hb_fpu_status_t status;
    if(hb_fpu_attach_current_task(&status)<0)return;
    tr_nano_frame(w,h,frame);
}
__attribute__((section(".text.entry"),used,noinline))
void *payload_entry(int op,void *fb,int w,int h,hb_shared_t *sh) {
    extern uint32_t __bss_start__[],__bss_end__[];
    (void)fb;(void)w;(void)h;
    if(op!=0 || !sh || sh->magic!=HB_SHARED_MAGIC || sh->version!=HB_SHARED_VERSION)return 0;
    if(ran_before && !restore_data())return 0;
    ran_before=1;
    for(uint32_t *p=__bss_start__;p<__bss_end__;p++)*p=0;
    hb_shared=sh;
    /* A resident that supports it (caps bit 0 in the second former padding byte of
     * hb_shared_t) redraws on a 2 ms heartbeat for us instead of 16 ms, so a dropped
     * redraw request is retried at once; the game then paces itself to the display.
     * With an older resident both bytes are zero and frames free-run as before. */
    tr_fast_redraw=0;
    if(sh->pad[1]&1u){sh->pad[0]=2;tr_fast_redraw=1;}
    return (void *)frame_callback;
}
