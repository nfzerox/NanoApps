#include "hb_app_catalog.h"
#include "hb_heap.h"

#define MAX_APPS 64u
#define MAX_PACK (1024u * 1024u)
#define MAX_MANIFEST (64u * 1024u)
#define APP_DIR "/Apps"

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
}

static void path(char *out, const char *dir, const char *name, const char *suffix)
{
    while (*dir) *out++ = *dir++;
    *out++ = '/';
    while (*name) *out++ = *name++;
    while (*suffix) *out++ = *suffix++;
    *out = 0;
}

static int valid_identity(const char *name)
{
    uint32_t i;
    if (name[0] != 'h' || name[1] != 'b' || name[2] != '.' || !name[3]) return 0;
    for (i = 3; name[i]; i++) {
        unsigned char c = name[i];
        if (i >= 23u || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* FAT names may contain spaces and UTF-8; keep Windows-forbidden characters out
 * of distributable folders. The folder is independent of the manifest SBID. */
static int valid_folder(const char *name, uint32_t len)
{
    uint32_t i;
    const char *ext = ".nanoapp";
    if (len <= 8u || len >= 64u || name[0] == '.') return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = name[i];
        if (c < 32u || c == 127u || c == '/' || c == '\\' || c == ':' || c == '"' ||
            c == '<' || c == '>' || c == '|' || c == '?' || c == '*') return 0;
    }
    for (i = 0; i < 8u; i++) {
        unsigned char c = name[len - 8u + i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (unsigned char)ext[i]) return 0;
    }
    return 1;
}

static int safe_string(const uint8_t *b, uint32_t n, uint32_t off, int identity)
{
    uint32_t i;
    if (off < sizeof(hb_app_bundle_t) || off >= n) return 0;
    for (i = 0; i < 24u && i < n - off; i++) {
        unsigned char c = b[off + i];
        if (!c) return i > 0u;
        if (c < 32u || c > 126u || c == '/' || c == '\\') return 0;
        if (identity && !((c >= 'a' && c <= 'z') ||
                         (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 0;
}

static int valid_bundle(const uint8_t *b, uint32_t n)
{
    const hb_app_bundle_t *h = (const hb_app_bundle_t *)b;
    const hb_app_section_t *s;
    uint32_t i, found = 0, start;
    if (n < sizeof(*h) || h->magic != HB_APP_BUNDLE_MAGIC ||
        h->version != 1u || h->bundle_len != n || (n & 3u) ||
        (h->app_kind != 1u && h->app_kind != 2u) ||
        h->app_id < 0x0dad8100u || h->label_id != h->app_id ||
        h->screen_id != 0x0dad8002u || h->layout_id != 0x0dad8001u ||
        h->os_handler || h->label_tag != 0x48426170u || h->screen_tag != 0x48425344u ||
        h->section_count != 2u) return 0;
    start = sizeof(*h) + h->section_count * sizeof(*s);
    if (start > n || h->sbid_off < start || h->name_off < start ||
        !safe_string(b, n, h->sbid_off, 1) || !safe_string(b, n, h->name_off, 0)) return 0;
    const char *sbid = (const char *)b + h->sbid_off;
    if (!valid_identity(sbid)) return 0;
    s = (const hb_app_section_t *)(b + sizeof(*h));
    for (i = 0; i < h->section_count; i++) {
        if ((s[i].kind != HB_SEC_LABEL && s[i].kind != HB_SEC_SCREEN) ||
            (found & (1u << s[i].kind)) || s[i].off < start || (s[i].off & 3u) ||
            s[i].off > n || s[i].len < 12u || s[i].len > n - s[i].off) return 0;
        /* SilverDB's OS reader trusts section/resource offsets. Check the
         * complete metadata ranges before handing downloaded bytes to it. */
        const uint32_t *db = (const uint32_t *)(b + s[i].off);
        uint32_t k, entries, header = db[1], size = s[i].len;
        if (db[0] != 3u || header > size || db[2] > 64u ||
            header < 12u + 16u * db[2]) return 0;
        for (k = 0; k < db[2]; k++) {
            const uint32_t *sec = db + 3u + k * 4u;
            uint32_t r;
            entries = sec[1];
            if (entries > 64u || sec[2] != 1u || (sec[3] & 3u) ||
                sec[3] > header || entries * 12u > header - sec[3]) return 0;
            for (r = 0; r < entries; r++) {
                const uint32_t *res = (const uint32_t *)((const uint8_t *)db + sec[3]) + r * 3u;
                if (res[1] > size - header || res[2] > size - header - res[1]) return 0;
            }
        }
        if (s[i].kind == HB_SEC_LABEL) {
            if (db[2] != 1u || db[3] != 0x53747220u || db[4] != 1u) return 0;
            const uint32_t *res = (const uint32_t *)((const uint8_t *)db + db[6]);
            if (res[0] != h->label_id || !res[2] || res[2] > 24u ||
                ((const uint8_t *)db)[header + res[1] + res[2] - 1u] != 0) return 0;
        }
        found |= 1u << s[i].kind;
    }
    return found == ((1u << HB_SEC_LABEL) | (1u << HB_SEC_SCREEN));
}

static uint8_t *read_file(const char *p, uint32_t max, uint32_t *size)
{
    uint8_t *data;
    uint32_t n = hb_fs_size(p);
    if (!n || n > max) return 0;
    data = hb_os_alloc(n);
    if (!data) return 0;
    if (hb_fs_read(p, data, n) != n) { hb_os_free(data); return 0; }
    *size = n;
    return data;
}

void *hb_app_catalog_load(const char *legacy_path, uint32_t resident_version,
                         hb_app_files_t *out_files,
                         void (*trace)(const char *, uint32_t, uint32_t))
{
    /* UI-task scratch; caller serializes refreshes, including across yielding
     * filesystem calls. Keep the large arrays off the firmware task stack. */
    static const uint8_t *items[MAX_APPS];
    static uint8_t *owned[MAX_APPS];
    static hb_app_files_t metadata[MAX_APPS];
    static char names[MAX_APPS][64];
    static char name[256];
    char p[96];
    uint32_t count = 0, files = 0, i, j, n = 0, bytes = 12u, pos;
    uint8_t *legacy = read_file(legacy_path, MAX_PACK, &n), *out = 0;
    hb_dir_t dir;
    bool is_dir;
    for (i = 0; i < MAX_APPS; i++) {
        uint8_t *m = (uint8_t *)&metadata[i];
        owned[i] = 0;
        for (j = 0; j < sizeof(metadata[i]); j++) m[j] = 0;
    }
    if (legacy && n >= 12u) {
        const uint32_t *h = (const uint32_t *)legacy;
        /* The unversioned legacy catalog is known only for runtime ABI 2.
         * Current releases ship per-app manifests, including in collections. */
        if (HB_RUNTIME_ABI == 2u && h[0] == HB_APP_PACK_MAGIC && h[1] == 1u && h[2] <= MAX_APPS &&
            12u + h[2] * 4u <= n) {
            for (i = 0; i < h[2]; i++) {
                uint32_t off = h[3 + i];
                const hb_app_bundle_t *b;
                if ((off & 3u) || off < 12u + h[2] * 4u || off > n ||
                    sizeof(*b) > n - off) continue;
                b = (const hb_app_bundle_t *)(legacy + off);
                if (b->bundle_len <= n - off && valid_bundle(legacy + off, b->bundle_len)) {
                    for (j = 0; j < count; j++) {
                        const hb_app_bundle_t *old = (const hb_app_bundle_t *)items[j];
                        if (old->app_id == b->app_id ||
                            !same((const char *)(items[j] + old->sbid_off), (const char *)b + b->sbid_off) ||
                            !same((const char *)(items[j] + old->name_off), (const char *)b + b->name_off)) break;
                    }
                    uint32_t exe_size;
                    path(p, "/Apps/Executables", (const char *)b + b->name_off, ".hbapp");
                    exe_size = hb_fs_size(p);
                    path(p, "/Apps/Icons", (const char *)b + b->name_off, ".bin");
                    if (j == count && exe_size >= 24u && hb_fs_size(p) == 112u * 112u * 4u + 32u &&
                        bytes + 4u + b->bundle_len <= MAX_PACK) {
                        metadata[count].executable_size = exe_size;
                        items[count++] = legacy + off;
                        bytes += 4u + b->bundle_len;
                    }
                }
            }
        }
    }
    /* Enumerate before reading files: filesystem operations may yield. Select
     * the first 64 names alphabetically, independent of FAT directory order. */
    if (hb_fs_dir_open(&dir, APP_DIR, false)) {
        while (hb_fs_dir_next(&dir, name, sizeof name, &is_dir)) {
            uint32_t len = 0, at;
            while (name[len]) len++;
            if (!is_dir || !valid_folder(name, len)) continue;
            for (at = 0; at < files && same(names[at], name) < 0; at++);
            if (at == MAX_APPS) continue;
            if (files < MAX_APPS) files++;
            for (j = files - 1u; j > at; j--) copy(names[j], names[j - 1u], 64u);
            for (j = 0; j < 64u; j++) names[at][j] = 0;
            copy(names[at], name, len + 1u);
        }
        hb_fs_dir_close(&dir);
    }
    for (i = 0; i < files; i++) {
        uint8_t *data;
        const uint32_t *h;
        const hb_app_bundle_t *b;
        const uint8_t *bb;
        const char *sbid;
        uint32_t at, replace;
        path(p, APP_DIR, names[i], "/manifest.napp");
        data = read_file(p, MAX_MANIFEST, &n);
        if (!data) continue;
        h = (const uint32_t *)data;
        if (n < 36u || h[0] != HB_APP_MANIFEST_MAGIC || h[1] != HB_APP_MANIFEST_VERSION ||
            h[2] != HB_APP_MANIFEST_ABI || h[3] != HB_APP_MANIFEST_FIRMWARE ||
            h[4] > resident_version || h[5] != n - 36u ||
            !valid_bundle(data + 36u, n - 36u)) goto reject;
        bb = data + 36u;
        b = (const hb_app_bundle_t *)bb;
        sbid = (const char *)(bb + b->sbid_off);
        if (same(sbid, (const char *)(bb + b->name_off))) goto reject;
        path(p, APP_DIR, names[i], "/app.hbapp");
        if (h[6] < 24u || hb_fs_size(p) != h[6]) goto reject;
        path(p, APP_DIR, names[i], "/icon.bin");
        if (hb_fs_size(p) != 112u * 112u * 4u + 32u) goto reject;
        replace = count;
        for (at = 0; at < count; at++) {
            const hb_app_bundle_t *old = (const hb_app_bundle_t *)items[at];
            if (!same(sbid, (const char *)(items[at] + old->sbid_off))) {
                /* One folder per identity; don't let a second copied folder
                 * silently override another. A legacy entry may be replaced. */
                if (metadata[at].standalone) goto reject;
                replace = at;
            }
        }
        for (at = 0; at < count; at++) {
            const hb_app_bundle_t *old = (const hb_app_bundle_t *)items[at];
            if (at != replace && (old->app_id == b->app_id ||
                !same((const char *)(bb + b->name_off),
                      (const char *)(items[at] + old->name_off)))) goto reject;
        }
        if (replace == MAX_APPS) goto reject;
        {
            uint32_t old_len = replace < count ?
                ((const hb_app_bundle_t *)items[replace])->bundle_len : 0u;
            uint32_t next = bytes - old_len + b->bundle_len + (replace == count ? 4u : 0u);
            if (next > MAX_PACK) goto reject;
            bytes = next;
        }
        if (owned[replace]) hb_os_free(owned[replace]);
        owned[replace] = data;
        items[replace] = bb;
        metadata[replace].executable_size = h[6];
        metadata[replace].executable_hash = h[7];
        metadata[replace].icon_hash = h[8];
        metadata[replace].standalone = 1;
        copy(metadata[replace].folder, names[i], 64u);
        if (replace == count) count++;
        if (trace) trace("APPADD  ", b->app_id, replace);
        continue;
reject:
        if (trace) trace("APPBAD  ", i, n);
        hb_os_free(data);
    }
    bytes = 12u + count * 4u;
    for (i = 0; i < count; i++) bytes += ((const hb_app_bundle_t *)items[i])->bundle_len;
    if (bytes <= MAX_PACK) out = hb_os_alloc(bytes);
    if (out) {
        uint32_t *h = (uint32_t *)out;
        h[0] = HB_APP_PACK_MAGIC; h[1] = 1; h[2] = count;
        pos = 12u + count * 4u;
        for (i = 0; i < count; i++) {
            uint32_t len = ((const hb_app_bundle_t *)items[i])->bundle_len;
            h[3 + i] = pos;
            copy(out + pos, items[i], len);
            pos += len;
            if (out_files) out_files[i] = metadata[i];
        }
    }
    for (i = 0; i < count; i++) if (owned[i]) hb_os_free(owned[i]);
    if (legacy) hb_os_free(legacy);
    if (trace) trace("APPCAT  ", count, out ? bytes : 0u);
    return out;
}
