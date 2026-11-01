/*
 * hb_fs.c — filesystem access for plain-C apps, layered over the iPod OS
 * file and directory primitives.
 *
 * The OS exposes a file as an opaque fixed-size object with a handful of
 * member functions at fixed code addresses. Rather than open-code the
 * open / append / truncate / close dance in every writer, the low-level
 * object is wrapped in a tiny handle (fobj_t) with begin / append / finish
 * helpers; the one-shot writers, the multi-buffer writer and the persistent
 * streaming writer all share that one path.
 *
 * USAGE NOTE — the iPod must be on the home screen with the filesystem
 * mounted. Trigger this once per boot:
 *     start eject
 * Then any SCSI deploy has FS access.
 */

#include "hb_sdk.h"

#define FS_MAIN_VOLUME  0
#define FS_WRITE_CHUNK  0x40000u   /* one write call is capped; loop in <=256 KB pieces */

/* ---- low-level OS file object -------------------------------------------
 * The object is a plain memory block the OS member functions operate on.
 * The block is sized a little over the firmware object to leave stack slack. */
#define FOBJ_SIZE        0x60
/* One-shot reads/writes use a static, generously sized object instead of 0x60 bytes of
 * stack: on hardware the OS read path wrote past a 0x60 stack object into the
 * callers' saved registers, causing a crash on return. Not reentrant, like
 * the shared cache below. */
#define FOBJ_ONESHOT_SIZE 0x400
static uint8_t s_oneshot_obj[FOBJ_ONESHOT_SIZE] __attribute__((aligned(8)));
#define FOBJ_CACHE_BYTES 0x1010

typedef void (*fobj_open_fn) (void *o, const char *path, int a2, int a3,
                              uint32_t a4, uint32_t a5, void *a6);
typedef void (*fobj_close_fn)(void *o);
typedef int  (*fobj_ready_fn)(void *o);
typedef int  (*fobj_read_fn) (void *o, uint32_t n, void *buf, uint32_t *got);
typedef int  (*fobj_write_fn)(void *o, uint32_t n, const void *buf, uint32_t *put);
typedef int  (*fobj_trunc_fn)(void *o, uint32_t len);
typedef int  (*fobj_sync_fn) (void *o);
typedef bool (*fs_exists_fn) (const char *path, int volume);

#define FOBJ_OPEN  ((fobj_open_fn) (0x084137a8u | 1u))
#define FOBJ_CLOSE ((fobj_close_fn)(0x08423be0u | 1u))
#define FOBJ_READY ((fobj_ready_fn)(0x08417e18u | 1u))
#define FOBJ_READ  ((fobj_read_fn) (0x0841cddcu | 1u))
#define FOBJ_WRITE ((fobj_write_fn)(0x0841ba42u | 1u))
#define FOBJ_TRUNC ((fobj_trunc_fn)(0x0841b09eu | 1u))
#define FOBJ_SYNC  ((fobj_sync_fn) (0x0840718cu | 1u))
#define FS_EXISTS  ((fs_exists_fn) (0x0841bb7cu | 1u))

/* Shared scratch cache. Stack-allocating it inside an app call chain can
   overflow the task stack, so it is static — in this image's .bss, which lives
   in the image's own heap block (relocatable apps and the heap-loaded
   resident). It used to sit at fixed 0x09118000, which is live OS heap. Not
   reentrant — fine for our use. */
#ifdef HB_FS_FIXED_CACHE
/* The flashed firmware bootstrap only: its .bss window is 0x100 bytes, and its
   source must keep matching the image in firmware. */
static uint8_t *fs_scratch_cache(void)
{
    return (uint8_t *)HB_FS_FIXED_CACHE;
}
#else
static uint8_t s_fs_cache[FOBJ_CACHE_BYTES] __attribute__((aligned(32)));

static uint8_t *fs_scratch_cache(void)
{
    return s_fs_cache;
}
#endif

static void fs_zero_cache(uint8_t *cache)
{
    /* Stale deblock state from a prior op must not bleed into the new file. */
    for (uint32_t i = 0; i < FOBJ_CACHE_BYTES; i++) cache[i] = 0;
}

/* A live handle around one OS file object. `obj` may point at a stack buffer
   (one-shot ops) or a static object (the persistent stream). */
typedef struct {
    void    *obj;
    uint32_t put;     /* bytes appended so far — used to set EOF on finish */
} fobj_t;

/* Open `path`: write=true creates/truncates for writing, false = read-only.
   On failure the object is closed and false is returned, so the caller never
   has to clean up after a failed open. */
static bool fobj_begin(fobj_t *f, void *obj, uint8_t *cache,
                       const char *path, bool write, int volume)
{
    f->obj = obj;
    f->put = 0;
    fs_zero_cache(cache);
    FOBJ_OPEN(obj, path, write ? 0 : 1, volume, 0x1000, 1, cache);
    if (FOBJ_READY(obj)) return true;
    FOBJ_CLOSE(obj);
    return false;
}

/* Append `len` bytes, splitting into FS_WRITE_CHUNK pieces. Returns false on
   the first short/failed write; the handle stays open so the caller decides
   whether to abort or keep appending. */
static bool fobj_append(fobj_t *f, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len) {
        uint32_t chunk = len > FS_WRITE_CHUNK ? FS_WRITE_CHUNK : len;
        uint64_t put = 0;   /* 64-bit slot: see hb_fs_size (low word holds the count) */
        if (FOBJ_WRITE(f->obj, chunk, p, (uint32_t *)&put) != 0 || (uint32_t)put != chunk) return false;
        p += chunk; len -= chunk; f->put += chunk;
    }
    return true;
}

/* Set EOF to the running byte count, flush to disk, close. */
static void fobj_finish(fobj_t *f)
{
    FOBJ_TRUNC(f->obj, f->put);
    FOBJ_SYNC(f->obj);
    FOBJ_CLOSE(f->obj);
}

bool hb_fs_exists(const char *path)
{
    return FS_EXISTS(path, FS_MAIN_VOLUME);
}

bool hb_fs_write(const char *path, const void *data, uint32_t size)
{
    uint8_t *obj = s_oneshot_obj;
    fobj_t f;
    if (!fobj_begin(&f, obj, fs_scratch_cache(), path, true, FS_MAIN_VOLUME)) return false;
    bool ok = fobj_append(&f, data, size);
    if (ok) fobj_finish(&f); else FOBJ_CLOSE(obj);
    return ok;
}

/* Write several buffers to ONE file, in order, with a single open — for data
   assembled from multiple allocations (e.g. a chunked recording ring). */
bool hb_fs_write_parts(const char *path, void *const *ptrs,
                       const uint32_t *lens, int nparts)
{
    uint8_t *obj = s_oneshot_obj;
    fobj_t f;
    if (!fobj_begin(&f, obj, fs_scratch_cache(), path, true, FS_MAIN_VOLUME)) return false;
    bool ok = true;
    for (int i = 0; i < nparts && ok; i++)
        ok = fobj_append(&f, ptrs[i], lens[i]);
    if (ok) fobj_finish(&f); else FOBJ_CLOSE(obj);
    return ok;
}

/* ---- streaming writer: one file kept open across many appends (e.g. a screen
   recording flushed buffer-by-buffer as it fills, so it isn't RAM-bound). One
   stream at a time, on a static object + cache (a stack buffer can't survive
   between the open and the appends). ---- */
static uint8_t s_stream_obj[FOBJ_ONESHOT_SIZE] __attribute__((aligned(8)));
static uint8_t s_stream_cache[FOBJ_CACHE_BYTES] __attribute__((aligned(32)));
static fobj_t s_stream;
static bool   s_stream_live;

bool hb_fs_stream_open(const char *path)
{
    if (s_stream_live) hb_fs_stream_close();
    if (!fobj_begin(&s_stream, s_stream_obj, s_stream_cache, path, true, FS_MAIN_VOLUME))
        return false;
    s_stream_live = true;
    return true;
}

bool hb_fs_stream_write(const void *data, uint32_t len)
{
    if (!s_stream_live) return false;
    return fobj_append(&s_stream, data, len);
}

bool hb_fs_stream_close(void)
{
    if (!s_stream_live) return false;
    fobj_finish(&s_stream);
    s_stream_live = false;
    return true;
}

/* Read up to `max_size` bytes from `path` into `buf`. Returns the number of
   bytes actually read (0 if the file is missing or the read failed). */
uint32_t hb_fs_read(const char *path, void *buf, uint32_t max_size)
{
    return hb_fs_read_at(path, buf, max_size, FS_MAIN_VOLUME);
}

uint32_t hb_fs_read_at(const char *path, void *buf, uint32_t max_size, int volume)
{
    uint8_t *obj = s_oneshot_obj;
    fobj_t f;
    if (!fobj_begin(&f, obj, fs_scratch_cache(), path, false, volume)) return 0;
    uint64_t got64 = 0;   /* 64-bit slot: see hb_fs_size (low word holds the count) */
    uint32_t got;
    if (FOBJ_READ(obj, max_size, buf, (uint32_t *)&got64) != 0) got64 = 0;
    got = (uint32_t)got64;
    FOBJ_CLOSE(obj);
    return got;
}

/* ----- directory iteration -----
 *
 * Directory iteration and path helper entry points.
 */
typedef void (*fs_dir_ctor_t)(void *o, const char *path,
                              int recursive, int volume);
typedef bool (*fs_dir_next_t)(void *o, void *out_path, bool *out_is_dir);
typedef void (*fs_dir_dtor_t)(void *o);

typedef void        (*fs_path_ctor_t)  (void *o);
typedef void        (*fs_path_dtor_t)  (void *o);
typedef const char *(*fs_path_aschar_t)(void *o);

#define DIR_CTOR    ((fs_dir_ctor_t)   (0x08417b94u | 1u))
#define DIR_NEXT    ((fs_dir_next_t)   (0x08417392u | 1u))
#define DIR_DTOR    ((fs_dir_dtor_t)   (0x084178a8u | 1u))
#define PATH_CTOR   ((fs_path_ctor_t)  (0x08423a6cu | 1u))
#define PATH_DTOR   ((fs_path_dtor_t)  (0x0842d96cu | 1u))
#define PATH_ASCHAR ((fs_path_aschar_t)(0x0842d510u | 1u))

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
typedef int (*fs_remove_t) (const char *path, int volume);
typedef int (*fs_mkdir_t)  (const char *path, int volume);
typedef int (*fs_getsize_t)(const char *path, uint64_t *size_out, int volume);   /* 64-bit size */
typedef int (*fs_getattr_t)(const char *path, uint8_t *attr_out, int volume);
typedef int (*fs_rmdir_t)  (const char *path, int a2, int volume, int a4);
typedef int (*fs_setattr_t)(const char *path, uint8_t attr, int volume);

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
    /* 0 = newly created, 0xd = already existed — both mean the dir is there. */
    return rc == 0 || rc == 0xd;
}

uint32_t hb_fs_size(const char *path)
{
    return hb_fs_size_at(path, FS_MAIN_VOLUME);
}

uint32_t hb_fs_size_at(const char *path, int volume)
{
    /* The OS stores a 64-bit size: with a 32-bit local its high word overwrote the
     * next stack slot (the caller's saved r4 at -Os), causing a later crash. */
    uint64_t sz = 0;
    int rc = FS_GETSIZE(path, &sz, volume);
    if (rc != 0) return 0;
    return (uint32_t)sz;
}

bool hb_fs_is_dir(const char *path)
{
    uint8_t attr = 0;
    int rc = FS_GETATTR(path, &attr, FS_MAIN_VOLUME);
    if (rc != 0) return false;
    /* bit 0 = directory bit */
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

/* Smart unlink — file OR empty directory. Picks rmdir vs remove based on
   whether the path is a directory. */
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
