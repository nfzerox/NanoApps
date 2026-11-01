/*
 * hb_app_bundle.h — the `.app` bundle format + registrar.
 *
 * A homebrew app is one self-contained bundle: a manifest (identity + how to
 * launch) plus a set of sections (icon bitmap, label resource, screen DB, code
 * blob). The bundle just PACKAGES pieces the existing tools already build; the
 * registrar unpacks them and drives the existing SDK mechanisms (icon
 * cache-table extension, resource-DB publish, factory screen push, relocatable
 * code load). Bundles are produced by tools/mkapp.py.
 *
 * For fast iteration the runtime is handed a PACK (several bundles concatenated)
 * staged in memory; the on-disk form is identical bytes per bundle, so swapping
 * to on-disk /Apps enumeration only changes where the bytes come from.
 *
 * All offsets are byte offsets from the start of their containing bundle; all
 * fields little-endian.
 */
#ifndef HB_APP_BUNDLE_H
#define HB_APP_BUNDLE_H

#include "hb_sdk.h"

#define HB_APP_BUNDLE_MAGIC   0x50414248u   /* 'HBAP' */
#define HB_APP_PACK_MAGIC     0x4b504248u   /* 'HBPK' */
#define HB_APP_BUNDLE_VERSION 1u

/* app_kind — what launching the app does (mirrors silver_resident's notion). */
enum {
    HB_APP_KIND_OS_PLACEHOLDER = 0,   /* fire a stock OS nav handler          */
    HB_APP_KIND_CUSTOM_SCREEN  = 1,   /* push our compiled Silver screen */
    HB_APP_KIND_GL_SURFACE     = 2,   /* push our screen, inject a GL view (hardware GL) */
};

/* section kinds */
enum {
    HB_SEC_ICON   = 1,   /* a single bitmap header bitmap (ipodhax format)    */
    HB_SEC_LABEL  = 2,   /* a compiled Silver label-resource blob (home label text)  */
    HB_SEC_SCREEN = 3,   /* a compiled Silver screen-resource blob                   */
    HB_SEC_CODE   = 4,   /* a relocatable .hbapp blob (fills the custom-draw surface)  */
};

typedef struct {
    uint32_t kind;       /* HB_SEC_*                                          */
    uint32_t off;        /* section data offset from bundle start            */
    uint32_t len;        /* section data length (0 = absent)                 */
} hb_app_section_t;

typedef struct {
    uint32_t magic;          /* HB_APP_BUNDLE_MAGIC                          */
    uint16_t version;        /* HB_APP_BUNDLE_VERSION                        */
    uint16_t app_kind;       /* HB_APP_KIND_*                               */
    uint32_t app_id;         /* unique app id                               */
    uint32_t label_id;       /* Str resource id resolving the home label    */
    uint32_t screen_id;      /* CUSTOM_SCREEN: SilverDB screen id to push   */
    uint32_t layout_id;      /* CUSTOM_SCREEN: layout id                    */
    uint32_t os_handler;     /* OS_PLACEHOLDER: stock nav handler address   */
    uint32_t label_tag;      /* 4cc resource-DB tag for the label blob      */
    uint32_t screen_tag;     /* 4cc resource-DB tag for the screen blob     */
    uint32_t sbid_off;       /* SBID name (ASCII, NUL-terminated)           */
    uint32_t name_off;       /* CFBundleName (ASCII) — disk file name       */
    uint32_t bundle_len;     /* total bundle bytes                          */
    uint32_t section_count;  /* hb_app_section_t[] immediately follow        */
} hb_app_bundle_t;

typedef struct {
    uint32_t magic;          /* HB_APP_PACK_MAGIC                           */
    uint32_t version;
    uint32_t count;          /* number of bundles                           */
    /* uint32_t bundle_off[count] follow: each bundle's offset from pack start */
} hb_app_pack_t;

/*
 * Per-app launch record the registrar fills in for the tap router: enough to
 * launch the app when its home icon is tapped (without re-parsing the bundle).
 */
typedef struct {
    uint32_t  app_id;
    uint16_t  app_kind;     /* HB_APP_KIND_*                                  */
    uint16_t  _pad;
    long      screen_id, layout_id;
    uintptr_t os_handler;
    long      icon_id;      /* assigned bitmap-cache id (for reference)       */
    char      sbid[24];     /* SBID name                                      */
    char      name[24];     /* CFBundleName — disk file name for exe/icon     */
    uint32_t  code_addr;    /* in-memory app code (fixed-VA .bin) for CUSTOM_SCREEN */
    uint32_t  code_len;     /* code section length (bytes to copy to 0x09280000)   */
} hb_app_launch_t;

/*
 * Register every app in `pack` (a staged HBPK). Does ONE batched bitmap
 * cache-table extension for all icons, publishes each app's label + screen
 * blobs, adds the home items, and records launch info into `out_apps` (caller's
 * array of `max`). `alloc` = OS heap allocator; `trace` may be NULL.
 * Returns the number of apps registered. Call ONCE per boot, on the UI task.
 */
/*
 * `register_to_os`: when nonzero, do the once-per-boot OS-mutating work (batched
 * icon cache-table extension, label/screen publish, home items). When zero, ONLY
 * re-fill out_apps[] (the tap-routing table) — safe to run on every resident
 * re-deploy so taps keep working without a reboot. out_apps is always filled.
 */
struct hb_app_files;
/* register_to_os: 0 = routing only, 1 = initial registration, 2 = live refresh. */
int hb_app_register_pack(const void *pack, void *(*alloc)(uint32_t size),
                         void (*trace)(const char *, uint32_t, uint32_t),
                         hb_app_launch_t *out_apps, int max,
                         void *screen_obj, void *screen_node, int register_to_os,
                         const struct hb_app_files *files);

#endif /* HB_APP_BUNDLE_H */
