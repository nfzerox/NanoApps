/*
 * hb_media.c — direct OS media-player state + control.
 *
 * This file exposes some control surface useful for:
 *   - reading current play state without triggering action handlers
 *   - building a custom now-playing UI that talks straight to the
 *     player without round-tripping through the UI action dispatcher
 */

#include "hb_sdk.h"

#define FN_THUMB(addr) ((addr) | 1u)

typedef void *(*media_get_t)(void);
#define ADDR_MEDIA_PLAYER_INSTANCE 0x083f3590u

static inline void *media_player(void) {
    return ((media_get_t)FN_THUMB(ADDR_MEDIA_PLAYER_INSTANCE))();
}

typedef int  (*int_arg0_t)(void *self);
typedef void (*void_arg0_t)(void *self);
typedef void (*void_arg1_t)(void *self, int v);

int hb_media_state(void)
{
    void *p = media_player();
    if (!p) return -1;
    void **vtbl = *(void ***)p;
    return ((int_arg0_t)vtbl[0x80 / 4])(p);
}

bool hb_media_has_session(void)
{
    void *p = media_player();
    if (!p) return false;
    void **vtbl = *(void ***)p;
    return ((int_arg0_t)vtbl[0x88 / 4])(p) != 0;
}

bool hb_media_is_video(void)
{
    void *p = media_player();
    if (!p) return false;
    void **vtbl = *(void ***)p;
    return ((int_arg0_t)vtbl[0x90 / 4])(p) != 0;
}

void hb_media_set_paused(bool paused)
{
    void *p = media_player();
    if (!p) return;
    void **vtbl = *(void ***)p;
    ((void_arg1_t)vtbl[0x94 / 4])(p, paused ? 1 : 0);
}

void hb_media_next(void)
{
    void *p = media_player();
    if (!p) return;
    void **vtbl = *(void ***)p;
    ((void_arg1_t)vtbl[0xb4 / 4])(p, 3);
}

void hb_media_prev(void)
{
    void *p = media_player();
    if (!p) return;
    void **vtbl = *(void ***)p;
    ((void_arg1_t)vtbl[0xb8 / 4])(p, 3);
}

void hb_media_show_now_playing(void)
{
    void *p = media_player();
    if (!p) return;
    void **vtbl = *(void ***)p;
    ((void_arg0_t)vtbl[0x70 / 4])(p);
}
