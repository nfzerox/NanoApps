/*
 * sys_device.c — sys.h on the iPod nano 7G.
 */

#include "sys.h"

#include "hb_sdk.h"
#include "hb_heap.h"

#define EN_DATA_DIR     "/Data/Entrain"
#define EN_CACHE_DIR    EN_DATA_DIR "/cache"
#define EN_PROGRAMS_DIR EN_DATA_DIR "/programs"
#define EN_PREFS_PATH   EN_DATA_DIR "/prefs.bin"

static bool s_exit;
static bool s_dirs_ready;

static void ensure_dirs(void)
{
    if (s_dirs_ready) return;
    hb_fs_mkdir(EN_DATA_DIR);
    hb_fs_mkdir(EN_CACHE_DIR);
    hb_fs_mkdir(EN_PROGRAMS_DIR);
    s_dirs_ready = true;
}

int en_sys_battery_percent(void)
{
    /* The OS reports 0..15; the header shows a percentage because a number out
       of fifteen means nothing to anyone. */
    uint32_t level = hb_battery_level_0_to_15();
    if (level > 15) level = 15;
    return (int)((level * 100u) / 15u);
}

bool en_sys_battery_charging(void)
{
    return hb_battery_is_charging();
}

void en_sys_wake_lock(bool on)
{
    /* This is what kept relighting the screen. The lock resets the OS idle
       clock roughly every ten seconds, so with it held the backlight came
       straight back on after the app blanked it. The UI now drops the lock
       when it blanks and takes it again when it wakes. */
    hb_wake_lock(on);
}

bool en_sys_has_system_volume(void)
{
    /* RetailOS owns volume: hardware buttons and its own HUD. */
    return true;
}

void en_sys_backlight(int percent)
{
    /* Zero means the backlight driver off, not the minimum level — the level
       setter clamps to the lowest visible step and stays lit. Crucially this
       touches only the backlight: LVGL keeps compositing, because on this
       device stopping the render loop can take the audio subsystem down with
       it (see AUDIO_NOTES.md). */
    if (percent <= 0) {
        hb_brightness_power(false);
    } else {
        hb_brightness_power(true);
        hb_brightness_set_percent(percent);
    }
}

uint32_t en_sys_millis(void)
{
    return hb_time_uptime_ms();
}

bool en_sys_prefs_load(void *blob, uint32_t size)
{
    return hb_fs_read(EN_PREFS_PATH, blob, size) == size;
}

bool en_sys_prefs_save(const void *blob, uint32_t size)
{
    ensure_dirs();
    return hb_fs_write(EN_PREFS_PATH, blob, size);
}

const char *en_sys_cache_dir(void)
{
    ensure_dirs();
    return EN_CACHE_DIR;
}

const char *en_sys_programs_dir(void)
{
    ensure_dirs();
    return EN_PROGRAMS_DIR;
}

const char *en_sys_data_dir(void)
{
    ensure_dirs();
    return EN_DATA_DIR;
}

uint32_t en_sys_read_file(const char *path, void *buf, uint32_t max)
{
    return hb_fs_read(path, buf, max);
}

bool en_sys_list_dir(const char *dir, int index, char *name_out, int name_cap)
{
    /* Reopens for each index. That is O(n^2) over the listing, but n is the
       number of user program files a person has hand-written — a dozen at
       most — and it avoids holding an open directory handle across UI frames. */
    hb_dir_t d;
    if (!hb_fs_dir_open(&d, dir, false)) return false;

    char name[128];
    bool is_dir;
    int i = 0;
    bool found = false;
    while (hb_fs_dir_next(&d, name, (int)sizeof name, &is_dir)) {
        if (is_dir) continue;
        if (i == index) {
            int k = 0;
            while (name[k] && k < name_cap - 1) { name_out[k] = name[k]; k++; }
            name_out[k] = 0;
            found = true;
            break;
        }
        i++;
    }
    hb_fs_dir_close(&d);
    return found;
}

void *en_sys_alloc(uint32_t size)
{
    /* hb_os_alloc wraps the OS nothrow allocator. The other one panics and
       reboots the device on failure, so it must never be used for a buffer
       whose size depends on a preset. */
    return hb_os_alloc(size);
}

void en_sys_free(void *p)
{
    if (p) hb_os_free(p);
}

void en_sys_request_exit(void) { s_exit = true; }
bool en_sys_exit_requested(void) { return s_exit; }

const char *en_sys_platform_name(void) { return "iPod nano 7G"; }
