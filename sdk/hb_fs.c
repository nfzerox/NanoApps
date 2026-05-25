/*
 * hb_fs.c — thin wrappers over the iPod OS filesystem APIs, callable
 * from plain C.
 *
 * USAGE NOTE — the iPod must be on the home screen with the
 * filesystem mounted. Trigger this once per boot:
 *     tools/eject.sh
 * Then any SCSI deploy has FS access.
 */

#include "hb_sdk.h"

#define FS_MAIN_VOLUME 0

/* OS file object entry points. The object buffer is larger than the
   observed firmware object size to leave a little stack slack. */
typedef void (*fs_file_ctor_c_t)(void *this, const char *path,
                                 int readOnly, int volume,
                                 uint32_t cacheSize,
                                 uint32_t numCaches, void *cachePtr);
typedef void (*fs_file_dtor_t)(void *this);
typedef int  (*fs_file_isopen_t)(void *this);
typedef int  (*fs_file_read_t)(void *this, uint32_t numBytes,
                               void *buf, uint32_t *bytesOut);
typedef int  (*fs_file_write_t)(void *this, uint32_t numBytes,
                                const void *buf, uint32_t *bytesOut);
typedef int  (*fs_file_seteof_t)(void *this, uint32_t eof);
typedef int  (*fs_file_flush_t)(void *this);
typedef bool (*fs_exists_t)(const char *path, int volume);

#define FILE_CTOR_C ((fs_file_ctor_c_t)(0x084137a8u | 1u))
#define FILE_DTOR   ((fs_file_dtor_t)  (0x08423be0u | 1u))
#define FILE_ISOPEN ((fs_file_isopen_t)(0x08417e18u | 1u))
#define FILE_READ   ((fs_file_read_t)  (0x0841cddcu | 1u))
#define FILE_WRITE  ((fs_file_write_t) (0x0841ba42u | 1u))
#define FILE_SETEOF ((fs_file_seteof_t)(0x0841b09eu | 1u))
#define FILE_FLUSH  ((fs_file_flush_t) (0x0840718cu | 1u))
#define EXISTS_FN   ((fs_exists_t)     (0x0841bb7cu | 1u))

#define FILEOBJ_SIZE  0x60

bool hb_fs_exists(const char *path)
{
    return EXISTS_FN(path, FS_MAIN_VOLUME);
}

/* Static cache: stack-allocating this inside an app call chain can
   overflow the task stack. Not thread-safe, which is fine for now. */
#define FILE_CACHE_SIZE 0x1010
static uint8_t g_fs_cache[FILE_CACHE_SIZE];

bool hb_fs_write(const char *path, const void *data, uint32_t size)
{
    uint8_t file_obj[FILEOBJ_SIZE];

    /* Wipe cache so stale data from a prior op doesn't bleed into
       the new file's deblock state. */
    for (uint32_t i = 0; i < FILE_CACHE_SIZE; i++) g_fs_cache[i] = 0;

    FILE_CTOR_C(file_obj, path, /*readOnly=*/0, FS_MAIN_VOLUME,
                /*cacheSize=*/0x1000, /*numCaches=*/1, g_fs_cache);

    bool ok = false;
    if (FILE_ISOPEN(file_obj)) {
        uint32_t bytes_out = 0;
        int rc = FILE_WRITE(file_obj, size, data, &bytes_out);
        if (rc == 0 && bytes_out == size) {
            FILE_SETEOF(file_obj, bytes_out);
            FILE_FLUSH(file_obj);
            ok = true;
        }
    }

    FILE_DTOR(file_obj);
    return ok;
}

/* Returns the number of bytes actually read. Pass 0 to query size
   without consuming file data. */
uint32_t hb_fs_read(const char *path, void *buf, uint32_t max_size)
{
    uint8_t file_obj[FILEOBJ_SIZE];

    for (uint32_t i = 0; i < FILE_CACHE_SIZE; i++) g_fs_cache[i] = 0;

    FILE_CTOR_C(file_obj, path, /*readOnly=*/1, FS_MAIN_VOLUME,
                /*cacheSize=*/0x1000, /*numCaches=*/1, g_fs_cache);

    uint32_t bytes_out = 0;
    if (FILE_ISOPEN(file_obj)) {
        int rc = FILE_READ(file_obj, max_size, buf, &bytes_out);
        if (rc != 0) bytes_out = 0;
    }

    FILE_DTOR(file_obj);
    return bytes_out;
}

/* ----- directory iteration -----
 *
 * Directory iteration and path helper entry points.
 */
typedef void (*fs_dir_ctor_t)(void *this, const char *path,
                              int recursive, int volume);
typedef bool (*fs_dir_next_t)(void *this, void *out_path, bool *out_is_dir);
typedef void (*fs_dir_dtor_t)(void *this);

typedef void        (*fs_path_ctor_t)  (void *this);
typedef void        (*fs_path_dtor_t)  (void *this);
typedef const char *(*fs_path_aschar_t)(void *this);

#define DIR_CTOR    ((fs_dir_ctor_t)   (0x08417b94u | 1u))
#define DIR_NEXT    ((fs_dir_next_t)   (0x08417392u | 1u))
#define DIR_DTOR    ((fs_dir_dtor_t)   (0x084178a8u | 1u))
#define PATH_CTOR   ((fs_path_ctor_t)  (0x08423a6cu | 1u))
#define PATH_DTOR   ((fs_path_dtor_t)  (0x0842d96cu | 1u))
#define PATH_ASCHAR ((fs_path_aschar_t)(0x0842d510u | 1u))

#define DIROBJ_SIZE   0x50

bool hb_fs_dir_open(hb_dir_t *iter, const char *path, bool recursive)
{
    /* Caller's hb_dir_t opaque buffer must be large enough. */
    DIR_CTOR(iter, path, recursive ? 1 : 0, FS_MAIN_VOLUME);
    return true;
}

bool hb_fs_dir_open_at(hb_dir_t *iter, const char *path, bool recursive,
                       int volume_id)
{
    DIR_CTOR(iter, path, recursive ? 1 : 0, volume_id);
    return true;
}

bool hb_fs_dir_next(hb_dir_t *iter, char *out_name, int out_size,
                    bool *out_is_dir)
{
    /* Small temporary path object used by the OS directory iterator. */
    uint8_t path_obj[8];
    PATH_CTOR(path_obj);

    bool dummy_dir = false;
    if (!out_is_dir) out_is_dir = &dummy_dir;

    bool ok = DIR_NEXT(iter, path_obj, out_is_dir);
    if (ok && out_name && out_size > 0) {
        const char *s = PATH_ASCHAR(path_obj);
        int i = 0;
        while (s && i < out_size - 1 && s[i]) {
            out_name[i] = s[i];
            i++;
        }
        out_name[i] = 0;
    }
    PATH_DTOR(path_obj);
    return ok;
}

void hb_fs_dir_close(hb_dir_t *iter)
{
    DIR_DTOR(iter);
}

/* ----- modify / stat ----- */
typedef int      (*fs_remove_t) (const char *path, int volume);
typedef int      (*fs_mkdir_t)  (const char *path, int volume);
typedef int      (*fs_getsize_t)(const char *path, uint32_t *size_out, int volume);
typedef int      (*fs_getattr_t)(const char *path, uint8_t *attr_out, int volume);
typedef int      (*fs_rmdir_t)  (const char *path, int unused, int volume, int z);
typedef int      (*fs_setattr_t)(const char *path, uint8_t attr, int volume);

#define FS_REMOVE    ((fs_remove_t) (0x0840ad9cu | 1u))
#define FS_MKDIR     ((fs_mkdir_t)  (0x083fbf22u | 1u))
#define FS_GETSIZE   ((fs_getsize_t)(0x08049598u | 1u))
#define FS_GETATTR   ((fs_getattr_t)(0x083fa026u | 1u))
#define FS_RMDIR     ((fs_rmdir_t)  (0x08157f3eu | 1u))
#define FS_SETATTR   ((fs_setattr_t)(0x0814595eu | 1u))

bool hb_fs_remove(const char *path)
{
    int rc = FS_REMOVE(path, FS_MAIN_VOLUME);
    return rc == 0;
}

bool hb_fs_mkdir(const char *path)
{
    int rc = FS_MKDIR(path, FS_MAIN_VOLUME);
    /* 0 = newly created, 0xd = already existed — both are "the dir
       is now there" (the OS uses the same check in its own callers). */
    return rc == 0 || rc == 0xd;
}

uint32_t hb_fs_size(const char *path)
{
    uint32_t sz = 0;
    int rc = FS_GETSIZE(path, &sz, FS_MAIN_VOLUME);
    if (rc != 0) return 0;
    return sz;
}

bool hb_fs_is_dir(const char *path)
{
    uint8_t attr = 0;
    int rc = FS_GETATTR(path, &attr, FS_MAIN_VOLUME);
    if (rc != 0) return false;
    /* bit 0 = directory bit (observed in FUN_0821516c's
       directory check in the firmware. */
    return (attr & 1) != 0;
}

bool hb_fs_rmdir(const char *path)
{
    int rc = FS_RMDIR(path, 0, FS_MAIN_VOLUME, 0);
    return rc == 0;
}

bool hb_fs_set_attr(const char *path, uint8_t attr_byte)
{
    int rc = FS_SETATTR(path, attr_byte, FS_MAIN_VOLUME);
    return rc == 0;
}

/* Smart unlink — file OR empty directory. Mirrors the OS helper
   checks whether the path is a directory before choosing an operation. */
bool hb_fs_unlink(const char *path)
{
    uint8_t attr = 0;
    int rc = FS_GETATTR(path, &attr, FS_MAIN_VOLUME);
    if (rc != 0) return false;
    if (attr & 1) return hb_fs_rmdir(path);
    return hb_fs_remove(path);
}

/* Recursive directory removal — equivalent to `rm -rf path`.
   Walks children first (post-order), removes each, then removes the
   parent dir. Returns true if path was fully removed.

   Implementation note: uses a stack-allocated 256-byte path buffer.
   Sub-paths beyond that depth are silently skipped (returns false).
   For our app-bundle use case (depth 2-3), this is plenty. */
static bool join_path(char *out, int out_size, const char *parent, const char *name)
{
    int i = 0;
    while (parent[i] && i < out_size - 1) { out[i] = parent[i]; i++; }
    if (i > 0 && out[i - 1] != '/' && i < out_size - 1) { out[i++] = '/'; }
    int j = 0;
    while (name[j] && i < out_size - 1) { out[i++] = name[j++]; }
    out[i] = 0;
    return name[j] == 0;
}

bool hb_fs_rmrf(const char *path)
{
    uint8_t attr = 0;
    int rc = FS_GETATTR(path, &attr, FS_MAIN_VOLUME);
    if (rc != 0) return false;
    if (!(attr & 1)) return hb_fs_remove(path);

    hb_dir_t d;
    if (!hb_fs_dir_open(&d, path, false)) return false;
    char name[256];
    bool is_dir = false;
    bool ok = true;
    while (hb_fs_dir_next(&d, name, sizeof name, &is_dir)) {
        /* Skip . and .. */
        if (name[0] == '.' && (name[1] == 0 ||
            (name[1] == '.' && name[2] == 0))) continue;
        char child[256];
        if (!join_path(child, sizeof child, path, name)) { ok = false; continue; }
        if (is_dir) {
            if (!hb_fs_rmrf(child)) ok = false;
        } else {
            if (!hb_fs_remove(child)) ok = false;
        }
    }
    hb_fs_dir_close(&d);
    if (ok) ok = hb_fs_rmdir(path);
    return ok;
}
