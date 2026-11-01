#ifndef HB_APP_CATALOG_H
#define HB_APP_CATALOG_H

#include "hb_app_bundle.h"
#include "hb_runtime_compat.h"

/* Independent .napp manifests: HBNP, format version, required ABI, firmware,
 * minimum resident version, bundle bytes, executable bytes, executable FNV-1a,
 * icon FNV-1a (nine LE words),
 * followed by one HBAP bundle. Unknown formats or unsupported ABI/firmware are
 * skipped before app code runs. A minimum resident version of zero imposes no
 * version requirement; a higher value requests a compatible feature/fix (see
 * hb_runtime_compat.h). Executable bytes are checked before relocation/execution
 * and icon bytes before rendering.
 * Each app is one folder /Apps/Calculator.nanoapp/ containing manifest.napp,
 * app.hbapp and icon.bin. Folder names may contain capitals, spaces and UTF-8
 * (up to 63 bytes including .nanoapp) and are independent of the stable SBID.
 * Copy/remove the whole folder to install/uninstall; saved data stays in Data.
 * The display label remains in the bundle's label resource.
 */
#define HB_APP_MANIFEST_MAGIC 0x504e4248u
#define HB_APP_MANIFEST_VERSION 1u
#define HB_APP_MANIFEST_ABI HB_RUNTIME_ABI
#define HB_APP_MANIFEST_FIRMWARE HB_RUNTIME_FIRMWARE

typedef struct hb_app_files {
    uint32_t executable_size, executable_hash, icon_hash, standalone;
    char folder[64]; /* actual on-disk folder; never inferred from SBID/title */
} hb_app_files_t;

/* Assemble a validated catalog at boot or live refresh. Independent manifests
 * override legacy entries with the same SBID; removing an override exposes the
 * legacy entry again. Unversioned AllApps.pack is accepted only as known ABI 2.
 * Limits: 64 registered apps, 64 manifests, 64 KiB per manifest, 1 MiB combined
 * catalog. Manifests are considered alphabetically, independent of FAT order.
 * Invalid, incompatible, incomplete or resource-ID-colliding packages are
 * skipped with APPBAD trace records. Returned memory must remain live while
 * registered resource DBs reference it. No disk files or saved data are changed.
 */
void *hb_app_catalog_load(const char *legacy_path, uint32_t resident_version,
                         hb_app_files_t *out_files,
                         void (*trace)(const char *, uint32_t, uint32_t));

#endif
