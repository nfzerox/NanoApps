/*
 * sys_host.c — sys.h on a Linux desktop.
 *
 * The battery is faked (slowly draining, so the header's readout is visibly
 * alive rather than a frozen 100%), the backlight is a no-op, and everything
 * else is ordinary POSIX.
 */

#include "sys.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

static uint64_t s_start_us;
static bool     s_exit;
static char     s_cache[512];
static char     s_programs[512];

/* Read a small sysfs value. Returns -1 if the file is not there, which is the
   normal case on a desktop and the signal to fall back. */
static long read_sysfs_long(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static bool read_sysfs_str(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    if (ok) {
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    }
    return ok;
}

/* The N31 kernel exposes d1830-battery and s5l8740-backlight. Both are found
   by probing rather than hardcoded, so the same binary still runs on a
   desktop where neither exists. */
#define EN_BATTERY_CAPACITY "/sys/class/power_supply/d1830-battery/capacity"
#define EN_BATTERY_STATUS   "/sys/class/power_supply/d1830-battery/status"
#define EN_BACKLIGHT_DIR    "/sys/class/backlight/s5l8740-backlight"

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

uint32_t en_sys_millis(void)
{
    if (!s_start_us) s_start_us = now_us();
    return (uint32_t)((now_us() - s_start_us) / 1000ull);
}

int en_sys_battery_percent(void)
{
    long v = read_sysfs_long(EN_BATTERY_CAPACITY);
    if (v >= 0) return v > 100 ? 100 : (int)v;

    /* No battery here — a desktop. Drift down one percent a minute from 87 so
       the readout still moves and layout bugs in the header show up. */
    uint32_t mins = en_sys_millis() / 60000u;
    int f = 87 - (int)(mins % 80u);
    return f < 7 ? f + 80 : f;
}

bool en_sys_battery_charging(void)
{
    char st[32];
    if (read_sysfs_str(EN_BATTERY_STATUS, st, sizeof st))
        return strcmp(st, "Charging") == 0 || strcmp(st, "Full") == 0;
    return false;
}

bool en_sys_has_system_volume(void)
{
    /* A bare Linux port has no system volume UI of its own, so the app's
       slider is the only one there is. */
    return false;
}

void en_sys_wake_lock(bool on)
{
    /* Nothing to hold on Linux: there is no OS idle timer competing for the
       panel here. The app's own blank timer is the only thing that dims it. */
    (void)on;
}

void en_sys_backlight(int percent)
{
    static long max_bright = -2;
    char path[256];

    if (max_bright == -2) {
        snprintf(path, sizeof path, "%s/max_brightness", EN_BACKLIGHT_DIR);
        max_bright = read_sysfs_long(path);
    }
    if (max_bright < 0) return;          /* no backlight: a desktop */

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    /* bl_power first: 0 is FB_BLANK_UNBLANK, 4 is FB_BLANK_POWERDOWN. Turning
       the backlight off this way leaves the compositor running, which is the
       whole point of the blank-while-playing feature. */
    snprintf(path, sizeof path, "%s/bl_power", EN_BACKLIGHT_DIR);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%d\n", percent == 0 ? 4 : 0); fclose(f); }

    if (percent == 0) return;

    snprintf(path, sizeof path, "%s/brightness", EN_BACKLIGHT_DIR);
    f = fopen(path, "w");
    if (f) {
        long v = (max_bright * percent) / 100;
        if (v < 1) v = 1;
        fprintf(f, "%ld\n", v);
        fclose(f);
    }
}

static const char *base_dir(void)
{
    const char *e = getenv("ENTRAIN_HOME");
    if (e && *e) return e;
    static char buf[512];
    const char *home = getenv("HOME");
    snprintf(buf, sizeof buf, "%s/.entrain", home ? home : "/tmp");
    return buf;
}

static void ensure_dir(const char *p)
{
    mkdir(p, 0755);
}

const char *en_sys_cache_dir(void)
{
    if (!s_cache[0]) {
        ensure_dir(base_dir());
        snprintf(s_cache, sizeof s_cache, "%s/cache", base_dir());
        ensure_dir(s_cache);
    }
    return s_cache;
}

const char *en_sys_programs_dir(void)
{
    /* ENTRAIN_PROGRAMS lets this point at the iPod's mounted volume, which is
       read-only — hence a separate variable from ENTRAIN_HOME, which has to be
       somewhere writable. */
    const char *env = getenv("ENTRAIN_PROGRAMS");
    if (env && *env) return env;

    if (!s_programs[0]) {
        ensure_dir(base_dir());
        snprintf(s_programs, sizeof s_programs, "%s/programs", base_dir());
        ensure_dir(s_programs);
    }
    return s_programs;
}

const char *en_sys_data_dir(void)
{
    /* ENTRAIN_DATA is what the device sets, to the app's own folder on the
       read-only volume. Falling back to ENTRAIN_HOME rather than to the
       programs directory keeps a host run working with nothing exported: drop
       frequencies.set in ~/.entrain and it is found. */
    const char *env = getenv("ENTRAIN_DATA");
    if (env && *env) return env;
    return base_dir();
}

bool en_sys_prefs_load(void *blob, uint32_t size)
{
    char path[600];
    snprintf(path, sizeof path, "%s/prefs.bin", base_dir());
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(blob, 1, size, f);
    fclose(f);
    return n == size;
}

bool en_sys_prefs_save(const void *blob, uint32_t size)
{
    ensure_dir(base_dir());
    char path[600];
    snprintf(path, sizeof path, "%s/prefs.bin", base_dir());
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = fwrite(blob, 1, size, f);
    fclose(f);
    return n == size;
}

uint32_t en_sys_read_file(const char *path, void *buf, uint32_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, max, f);
    fclose(f);
    return (uint32_t)n;
}

bool en_sys_list_dir(const char *dir, int index, char *name_out, int name_cap)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    int i = 0;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (i == index) {
            snprintf(name_out, (size_t)name_cap, "%s", e->d_name);
            found = true;
            break;
        }
        i++;
    }
    closedir(d);
    return found;
}

void *en_sys_alloc(uint32_t size) { return malloc(size); }
void  en_sys_free(void *p) { free(p); }

void en_sys_request_exit(void) { s_exit = true; }
bool en_sys_exit_requested(void) { return s_exit; }

const char *en_sys_platform_name(void) { return "Linux host"; }
