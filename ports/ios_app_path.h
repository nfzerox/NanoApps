/* A cached port restores its initialized data on relaunch. Resolve its stable
 * identity instead of assuming the .nanoapp folder has never been renamed.
 * Finish directory enumeration before reading: nano filesystem reads may yield.
 */
#include "hb_app_catalog.h"
#include <string.h>

static void ios_path(char *out, const char *folder, const char *file)
{
    const char *prefix = "/Apps/";
    while (*prefix) *out++ = *prefix++;
    while (*folder) *out++ = *folder++;
    *out++ = '/';
    while (*file) *out++ = *file++;
    *out = 0;
}

static const char *ios_app_file(const char *identity)
{
    static char path[96];
    char names[64][64], name[256];
    unsigned count = 0;
    bool is_dir;
    hb_dir_t dir;
    if (!hb_fs_dir_open(&dir, "/Apps", false)) return NULL;
    while (count < 64 && hb_fs_dir_next(&dir, name, sizeof name, &is_dir)) {
        size_t n = strlen(name);
        if (!is_dir || n <= 8 || n >= 64 || name[0] == '.' ||
            strchr(name, '/') || strchr(name, '\\')) continue;
        unsigned k;
        for (k = 0; k < 8; k++) {
            unsigned char c = name[n - 8 + k];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != (unsigned char)".nanoapp"[k]) break;
        }
        if (k != 8) continue;
        memcpy(names[count++], name, n + 1);
    }
    hb_fs_dir_close(&dir);
    for (unsigned i = 0; i < count; i++) {
        ios_path(path, names[i], "manifest.napp");
        uint32_t n = hb_fs_size(path);
        if (n < 36 + sizeof(hb_app_bundle_t) || n > 65536) continue;
        uint32_t cap = (n + 8191u) & ~4095u;
        uint8_t *data = hb_os_alloc(cap);
        if (!data) return NULL;
        int found = 0;
        if (hb_fs_read(path, data, cap) == n) {
            const uint32_t *h = (const uint32_t *)data;
            const hb_app_bundle_t *b = (const hb_app_bundle_t *)(data + 36);
            size_t len = strlen(identity) + 1;
            if (h[0] == HB_APP_MANIFEST_MAGIC && h[1] == HB_APP_MANIFEST_VERSION &&
                h[2] == HB_APP_MANIFEST_ABI && h[3] == HB_APP_MANIFEST_FIRMWARE &&
                h[5] == n - 36 && b->magic == HB_APP_BUNDLE_MAGIC &&
                b->sbid_off >= sizeof(*b) && b->sbid_off <= n - 36 &&
                len <= n - 36 - b->sbid_off &&
                !memcmp(data + 36 + b->sbid_off, identity, len)) found = 1;
        }
        hb_os_free(data);
        if (found) {
            ios_path(path, names[i], "app.hbapp");
            return path;
        }
    }
    return NULL;
}
