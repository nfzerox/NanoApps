/*
 * sys.h — the non-audio platform surface the shared UI needs.
 *
 * Kept as small as the audio interface, and for the same reason: ui.c is the
 * same file on both targets, so anything that only exists on one of them has
 * to come through here.
 */

#ifndef ENTRAIN_SYS_H
#define ENTRAIN_SYS_H

#include <stdint.h>
#include <stdbool.h>

/* 0..100, or -1 if the platform has no battery to report. */
int en_sys_battery_percent(void);
bool en_sys_battery_charging(void);

/* Hold the screen on. The device's OS dims and sleeps on its own idle timer,
   which would be wrong in the middle of a 45-minute program. */
void en_sys_wake_lock(bool on);

/* True when the platform has its own volume control that the user already
   knows about — RetailOS has hardware volume buttons and an on-screen HUD, so
   a second slider inside the app is just a second thing to get out of sync.
   A bare Linux port has no such thing and needs ours. */
bool en_sys_has_system_volume(void);

/* Backlight only, 0..100. Zero means the backlight is off but the compositor
   keeps running — which matters more than it looks: on the device, stopping
   the render loop can take the audio subsystem down with it (AUDIO_NOTES.md),
   so blanking must never mean "stop drawing". */
void en_sys_backlight(int percent);

/* Milliseconds since the app started. Monotonic. */
uint32_t en_sys_millis(void);

/* Persist a small settings blob. Returns false if unavailable, in which case
   the app runs on defaults rather than refusing to start. */
bool en_sys_prefs_load(void *blob, uint32_t size);
bool en_sys_prefs_save(const void *blob, uint32_t size);

/* Where rendered loops are cached. Device: under /Data. Host: a temp dir.
   Returns a path with no trailing slash. */
const char *en_sys_cache_dir(void);

/* Where user program files live. */
const char *en_sys_programs_dir(void);

/* Where read-only data shipped with the app lives - the frequency bundle, and
   anything else added later that is neither a setting nor a cache. Separate
   from the programs directory because that one is a directory the user drops
   files into, and separate from the cache because this is never written. */
const char *en_sys_data_dir(void);

/* Read a whole small file. Returns bytes read, 0 on failure. */
uint32_t en_sys_read_file(const char *path, void *buf, uint32_t max);

/* Enumerate files in a directory. `index` walks from 0; returns false past the
   end. Deliberately index-based rather than callback-based so the UI can page
   through without holding an open handle. */
bool en_sys_list_dir(const char *dir, int index, char *name_out, int name_cap);

/* Allocate the renderer's working buffer. The device has no libc: it borrows
   the OS heap through a nothrow allocator that returns NULL rather than
   panicking, which is why this is a platform call and not malloc. Returns NULL
   when there is not enough room, and the caller must cope rather than assume. */
void *en_sys_alloc(uint32_t size);
void  en_sys_free(void *p);

/* Leave the app. On the device this must also stop audio — nothing is worse
   than a loop still playing after the user has gone home. */
void en_sys_request_exit(void);
bool en_sys_exit_requested(void);

const char *en_sys_platform_name(void);

#endif /* ENTRAIN_SYS_H */
