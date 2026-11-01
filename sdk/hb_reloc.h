/*
 * hb_reloc.h — load a relocatable homebrew app blob (.hbapp) into an OS-heap
 * arena and rebase it, so position-independent app/LVGL code can run from any
 * heap address (no fixed VA, no chainload).
 *
 * The blob is produced by tools/mkrelocapp.py from a non-PIC, base-0, -Wl,-q
 * ELF: a header, a flat image, and a table of ABS32 relocation offsets. This
 * loader allocates an arena, copies the image, zeroes bss, adds the arena base
 * to every ABS32 location, flushes the I-cache, and returns the entry point.
 *
 * The blob is self-contained (no external symbols) and reaches the OS/SDK only
 * through the hb_app_api_t table the caller passes to its entry (see
 * hb_app_api.h).
 */
#ifndef HB_RELOC_H
#define HB_RELOC_H

#include "hb_sdk.h"

#define HB_RELOC_MAGIC 0x314c5248u   /* 'HRL1' */

/*
 * Load + relocate a packed .hbapp at `packed` (already resident in memory).
 * `alloc(size)` returns `size` bytes of OS heap (the OS allocator). Returns the
 * blob's entry point (thumb, cast to hb_blob_main_t and call), or NULL on a bad
 * header / failed allocation.
 *
 * If `out_arena` is non-NULL it receives the underlying allocation pointer, so
 * the caller can free the app's arena (matched free) when done — e.g. before
 * loading the next app. Pass NULL to keep the arena for the session.
 */
void *hb_reloc_load(const void *packed, void *(*alloc)(uint32_t size),
                    void **out_arena);

/*
 * Load + relocate the .hbapp FILE at `path` straight into its own OS-heap block
 * (hb_reloc_file.c): the file is read to the top of the block, then moved down,
 * rebased and its bss zeroed, so nothing outside the block is ever written.
 * Returns the thumb entry, or NULL (arena freed) on any failure. *out_arena gets
 * the allocation to free later; *out_len the file size (0 if the file is missing
 * or not relocatable). If `out_base`/`out_span` are non-NULL they receive the
 * image's rebased base address and its span (image + bss) in bytes.
 */
void *hb_reloc_load_file(const char *path, void **out_arena, uint32_t *out_len,
                         uint8_t **out_base, uint32_t *out_span);
/* Verify standalone app bytes against the manifest before publishing code. */
void *hb_reloc_load_file_verified(const char *path, void **out_arena, uint32_t *out_len,
                                  uint32_t expected_hash);

/* Checked resident image. The on-disk file is one canonical HRL1 image followed
 * by a little-endian FNV-1a checksum of every preceding byte. This is an
 * accidental-corruption check, not authentication. The loader validates the
 * complete relocation table before changing executable bytes. */
void *hb_reloc_load_file_checked(const char *path, void **out_arena,
                                 uint32_t *out_len, uint8_t **out_base,
                                 uint32_t *out_span, uint32_t *out_image_size);

/* Versioned resident from the selected filesystem volume (disk = 0).
 * HBVR/ABI/firmware/source-timestamp metadata is covered by the slot checksum.
 * Legacy checksummed slots report version zero. No resident code is executed. */
void *hb_reloc_load_file_checked_at(const char *path, int volume,
                                    void **out_arena, uint32_t *out_len,
                                    uint8_t **out_base, uint32_t *out_span,
                                    uint32_t *out_image_size, uint32_t *out_version);
/* Last file-load validation step (high byte) and length (low 24 bits).
 * 0=valid, 1=missing/header read, 2=layout, 3=allocation, 4=read, 5=changed
 * header, 6=checksum, 7=ABI/firmware/version, 8=relocations. UI-task serialized. */
uint32_t hb_reloc_file_status(void);

#endif /* HB_RELOC_H */
