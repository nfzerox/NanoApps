/*
 * hb_app_loader.c — load and execute another homebrew app from disk.
 *
 * Apps are linked at HB_APP_LOAD_VA (0x0867f8e4, see hb_app.ld). A
 * launcher can't safely overwrite its own .text while still executing,
 * so we use a trampoline: /Apps/.load_stub is loaded to HB_STUB_VA
 * (0x09100000), the target path is parked at HB_STUB_PATH_AT, and we
 * BLX the stub. The stub then reads the target into HB_APP_LOAD_VA
 * (which is now safe since the stub itself is at 0x09100000) and BLX's.
 *
 * I-cache coherency for freshly loaded bytes goes through a firmware
 * cache-maintenance service. Without this, jumping into the new code
 * can hit stale I-cache lines from the previous app.
 *
 * Returns false on FS error / size mismatch; doesn't return on success.
 */

#include "hb_sdk.h"

#define HB_APP_LOAD_VA  0x0867f8e4u
#define HB_STUB_VA      0x09100000u   /* parametric loader stub here */
/* Path scratch — must be OUTSIDE the stub's binary region (stub is
   ~5 KB starting at HB_STUB_VA). 0x09110000 is 64 KB above, well
   clear, and below the trace buffer at 0x09120000. */
#define HB_STUB_PATH_AT 0x09110000u
#define HB_STUB_FILE    "/Apps/.load_stub"

typedef void (*entry_t)(void);

/* Required before jumping into freshly-loaded code. */
static inline void hb_icache_invalidate(uint32_t addr, uint32_t size)
{
    register uint32_t r12 __asm__("r12") = 7;     /* funccode */
    register uint32_t r0  __asm__("r0")  = addr;  /* arg1 */
    register uint32_t r1  __asm__("r1")  = size;  /* arg2 */
    /* All in/out — kernel may clobber. */
    __asm__ volatile("svc #70"
                     : "+r"(r12), "+r"(r0), "+r"(r1)
                     :
                     : "memory");
}

/* Load+jump for callers that ARE NOT running at HB_APP_LOAD_VA — safe
   to overwrite that region directly. Used by the stub itself, never
   from a normal app. */
bool hb_app_load_direct(const char *path)
{
    hb_trace_log("DIR_ENT", (uint32_t)(uintptr_t)path, 0);
    uint32_t size = hb_fs_size(path);
    hb_trace_log("DIR_SZ", size, 0);
    if (size == 0 || size > 0x80000) return false;

    uint32_t read = hb_fs_read(path, (void *)HB_APP_LOAD_VA, size);
    hb_trace_log("DIR_RD", read, size);
    if (read != size) return false;

    /* Flush instruction cache before jumping into loaded code. */
    hb_icache_invalidate(HB_APP_LOAD_VA, size);
    hb_trace_log("DIR_JMP", HB_APP_LOAD_VA, 0);

    ((entry_t)(HB_APP_LOAD_VA | 1u))();
    return true;
}

/* Trampoline load. Used by anything running at HB_APP_LOAD_VA (i.e.
   most apps) — we can't overwrite our own .text while executing it,
   so we hand off to /Apps/.load_stub which lives at HB_STUB_VA and
   does the actual self-overwriting load+jump.

   Requires /Apps/.load_stub to exist on disk (deployed by
   `make push-scsi` / `push-usb` as part of the bundle stage). */
bool hb_app_load_and_exec(const char *path)
{
    hb_trace_log("LDR_ENT", (uint32_t)(uintptr_t)path, 0);
    if (!path || !path[0]) return false;

    uint32_t stub_size = hb_fs_size(HB_STUB_FILE);
    hb_trace_log("LDR_SZ", stub_size, 0);
    if (stub_size == 0 || stub_size > 0x100000) return false;

    uint32_t rd = hb_fs_read(HB_STUB_FILE, (void *)HB_STUB_VA, stub_size);
    hb_trace_log("LDR_RD", rd, stub_size);
    if (rd != stub_size) return false;

    /* Write target path to HB_STUB_PATH_AT */
    char *dst = (char *)HB_STUB_PATH_AT;
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = 0;
    hb_trace_log("LDR_PTH", HB_STUB_PATH_AT, (uint32_t)i);

    /* Flush instruction cache before jumping into the stub. */
    hb_icache_invalidate(HB_STUB_VA, stub_size);
    hb_trace_log("LDR_BLX", HB_STUB_VA, 0);

    /* BLX to stub */
    ((entry_t)(HB_STUB_VA | 1u))();
    hb_trace_log("LDR_BAK", 0xdead, 0);   /* unreachable on success */
    return true;
}

/* --- App bundle scanner ---
 *
 * Bundle layout (convention-based, no plist parser needed):
 *
 *   /Apps/<Name>.app/
 *       <Name>          — the executable .bin (filename matches dir
 *                         minus .app suffix)
 *       Icon.bmp        — optional 64x64 BMP, loaded by launcher
 *       Name.txt        — optional UTF-8 display name; if absent,
 *                         we use <Name> (dir name minus .app).
 *
 * hb_app_scan walks /Apps, collects up to `max_apps` valid bundles,
 * fills the caller's array with {dir_name, exec_path, label}. Returns
 * the count found.
 */

static int my_strlen(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

static void my_strcpy_max(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static bool ends_with_app(const char *s)
{
    int n = my_strlen(s);
    if (n < 4) return false;
    return s[n - 4] == '.' && s[n - 3] == 'a' &&
           s[n - 2] == 'p' && s[n - 1] == 'p';
}

int hb_app_scan(hb_app_info_t *out, int max_apps)
{
    hb_dir_t d;
    if (!hb_fs_dir_open(&d, "/Apps", false)) return 0;

    int count = 0;
    char name[64];
    bool is_dir = false;

    while (count < max_apps && hb_fs_dir_next(&d, name, sizeof name, &is_dir)) {
        if (!is_dir) continue;
        if (!ends_with_app(name)) continue;

        /* Strip .app to get the base name */
        int n = my_strlen(name);
        char base[64];
        for (int i = 0; i < n - 4 && i < (int)sizeof base - 1; i++) base[i] = name[i];
        base[n - 4] = 0;

        /* Build exec path: /Apps/<Name>.app/<Name> */
        hb_app_info_t *a = &out[count];
        my_strcpy_max(a->dir, name, sizeof a->dir);
        my_strcpy_max(a->label, base, sizeof a->label);

        /* /Apps/<name>/<base> */
        int j = 0;
        const char *prefix = "/Apps/";
        while (prefix[j]) { a->exec[j] = prefix[j]; j++; }
        for (int i = 0; name[i] && j < (int)sizeof a->exec - 2; i++)
            a->exec[j++] = name[i];
        a->exec[j++] = '/';
        for (int i = 0; base[i] && j < (int)sizeof a->exec - 1; i++)
            a->exec[j++] = base[i];
        a->exec[j] = 0;

        /* Try to read Name.txt for a friendlier label */
        char name_path[160];
        int k = 0;
        while (prefix[k]) { name_path[k] = prefix[k]; k++; }
        for (int i = 0; name[i] && k < (int)sizeof name_path - 16; i++)
            name_path[k++] = name[i];
        const char *suffix = "/Name.txt";
        for (int i = 0; suffix[i] && k < (int)sizeof name_path - 1; i++)
            name_path[k++] = suffix[i];
        name_path[k] = 0;

        if (hb_fs_exists(name_path)) {
            char label_buf[64];
            uint32_t rd = hb_fs_read(name_path, label_buf, sizeof label_buf - 1);
            if (rd > 0) {
                label_buf[rd] = 0;
                /* trim trailing whitespace */
                while (rd > 0 && (label_buf[rd-1] == '\n' ||
                                  label_buf[rd-1] == '\r' ||
                                  label_buf[rd-1] == ' '  ||
                                  label_buf[rd-1] == '\t')) {
                    label_buf[--rd] = 0;
                }
                if (rd > 0) my_strcpy_max(a->label, label_buf, sizeof a->label);
            }
        }

        /* Verify the exec exists, else skip */
        if (!hb_fs_exists(a->exec)) continue;

        count++;
    }
    hb_fs_dir_close(&d);
    return count;
}
