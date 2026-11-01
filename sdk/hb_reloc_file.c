/* hb_reloc_file.c — load a relocatable .hbapp file into its own OS-heap block.
 * See hb_reloc_load_file in hb_reloc.h.
 *
 * Nothing here may touch fixed DRAM: 0x0910xxxx-0x094xxxxx is all live OS heap
 * (the old 0x09280000 staging slot sat inside an arena the OS had handed out).
 * So the file goes straight into the block that will hold the image:
 *
 *   arena = alloc(align + need),  need = max(span, file)
 *   file is read to the TOP of the block (src = base + need - file >= base),
 *   so the image moves DOWN (forward copy is safe), the reloc table sits above
 *   everything the image copy writes, and bss is zeroed only after relocation.
 */
#include "hb_reloc.h"
#include "hb_heap.h"
#include "hb_runtime_compat.h"

/* Selector diagnostics: high byte = last validation step (0 on success),
 * low 24 bits = file length. Missing files/header reads stop at step 1.
 * This distinguishes unavailable disk files from incompatible/corrupt slots
 * in the boot report without running any candidate code. */
static uint32_t s_file_status;
uint32_t hb_reloc_file_status(void) { return s_file_status; }

static void file_step(uint32_t step)
{
    s_file_status = (s_file_status & 0x00ffffffu) | (step << 24);
}

static uint32_t fnv1a32(const uint8_t *p, uint32_t n)
{
    uint32_t value = 0x811c9dc5u;
    while (n--)
        value = (value ^ *p++) * 0x01000193u;
    return value;
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void *load_file(const char *path, void **out_arena, uint32_t *out_len,
                       uint8_t **out_base, uint32_t *out_span,
                       uint32_t *out_image_size, int checked, int volume,
                       uint32_t *out_version, uint32_t expected_hash)
{
    uint32_t h[6], fsize, packed_size, need, i, rbytes, prior;
    uint8_t *arena, *base, *src;
    const uint8_t *image;
    const uint8_t *relocs;

    *out_arena = 0;
    *out_len = 0;
    if (out_image_size)
        *out_image_size = 0;
    if (out_version) *out_version = 0;
    fsize = hb_fs_size_at(path, volume);
    s_file_status = 0x01000000u | (fsize & 0x00ffffffu);
    if (fsize < sizeof h + (checked == 1 ? 4u : 0u) ||
        hb_fs_read_at(path, h, sizeof h, volume) != sizeof h ||
        h[0] != HB_RELOC_MAGIC)
        return 0;
    *out_len = fsize;
    file_step(2);
    packed_size = fsize - (checked == 1 ? 4u : 0u);
    hb_trace_log("RLHDR   ", h[3], fsize);
    hb_trace_log("HEAPMAX ", hb_os_heap_largest(), hb_os_heap_free());
    /* Reject corrupt headers before any arithmetic can wrap: reloc count must fit
     * the file, the entry must lie inside the image. */
    if (h[5] == 0 || h[5] > 32u || (h[5] & (h[5] - 1u)) ||
        h[2] < 4u || h[2] > h[3] || h[1] >= h[2] || (h[1] & 1u) ||
        h[4] > (packed_size - sizeof h) / 4u)
        return 0;
    rbytes = 4u * h[4];
    if (sizeof h + h[2] < h[2] || sizeof h + h[2] + rbytes < rbytes)
        return 0;
    if (checked == 1 && sizeof h + h[2] + rbytes + 16u == packed_size)
        packed_size -= 16u;
    if (sizeof h + h[2] + rbytes != packed_size || (checked == 1 && h[3] > 1024u * 1024u))
        return 0;
    /* Only the historical ABI-2 disk format lacked version metadata. A future
     * ABI bump must not accidentally execute an unversioned incompatible slot. */
    if (checked == 1 && fsize == packed_size + 4u && HB_RUNTIME_ABI != 2u)
        return 0;
    need = h[3] > fsize ? h[3] : fsize;
    if (need > 0xffffffffu - h[5])
        return 0;

    /* nothrow allocator: a heap too fragmented for this image returns NULL (it
     * just doesn't load) instead of the operator-new panic-reboot. */
    file_step(3);
    arena = (uint8_t *)hb_os_alloc(need + h[5]);
    hb_trace_log("RLARENA ", (uint32_t)(uintptr_t)arena, need + h[5]);
    if (!arena)
        return 0;
    base = (uint8_t *)(((uintptr_t)arena + (h[5] - 1u)) & ~((uintptr_t)h[5] - 1u));
    src = base + (need - fsize);
    file_step(4);
    if (hb_fs_read_at(path, src, fsize, volume) != fsize) {
        hb_os_free(arena);
        return 0;
    }
    file_step(5);
    for (i = 0; i < 6; i++) {
        if (read_le32(src + 4u * i) != h[i]) {
            hb_os_free(arena);
            return 0;
        }
    }
    file_step(6);
    if ((checked == 1 && fnv1a32(src, fsize - 4u) != read_le32(src + fsize - 4u)) ||
        (checked == 2 && fnv1a32(src, fsize) != expected_hash)) {
        hb_os_free(arena);
        return 0;
    }
    file_step(7);
    if (checked == 1 && fsize == packed_size + 20u) {
        const uint8_t *v = src + packed_size;
        if (read_le32(v) != 0x52564248u || read_le32(v + 4u) != HB_RUNTIME_ABI ||
            read_le32(v + 8u) != HB_RUNTIME_FIRMWARE || !read_le32(v + 12u)) {
            hb_os_free(arena);
            return 0;
        }
        if (out_version) *out_version = read_le32(v + 12u);
    }
    image  = src + sizeof h;
    relocs = image + h[2];
    hb_trace_log("RLBASE  ", (uint32_t)(uintptr_t)base, (uint32_t)(uintptr_t)src);

    /* Canonical relocations are aligned, strictly sorted/non-overlapping, and
     * wholly inside PROGBITS. Validate them all before modifying code. */
    file_step(8);
    prior = 0;
    for (i = 0; i < h[4]; i++) {
        uint32_t off = read_le32(relocs + 4u * i);
        if ((off & 3u) || off > h[2] - 4u || (i && off < prior + 4u)) {
            hb_os_free(arena);
            return 0;
        }
        prior = off;
    }

    for (i = 0; i < h[2]; i++)               /* dest <= src: forward copy is safe */
        base[i] = image[i];
    for (i = 0; i < h[4]; i++) {             /* relocs still intact above image  */
        uint32_t off = read_le32(relocs + 4u * i);
        *(uint32_t *)(base + off) += (uint32_t)(uintptr_t)base;
    }
    for (i = h[2]; i < h[3]; i++)            /* bss last; may overwrite src      */
        base[i] = 0;
    hb_icache_invalidate((uint32_t)(uintptr_t)base, h[3]);
    hb_trace_log("RLDONE  ", (uint32_t)(uintptr_t)base + h[1], h[4]);
    *out_arena = arena;
    if (out_base)
        *out_base = base;
    if (out_span)
        *out_span = h[3];
    if (out_image_size)
        *out_image_size = h[2];
    file_step(0);
    return (void *)((uintptr_t)(base + h[1]) | 1u);
}

void *hb_reloc_load_file(const char *path, void **out_arena, uint32_t *out_len,
                         uint8_t **out_base, uint32_t *out_span)
{
    return load_file(path, out_arena, out_len, out_base, out_span, 0, 0, 0, 0, 0);
}

void *hb_reloc_load_file_verified(const char *path, void **out_arena, uint32_t *out_len,
                                  uint32_t expected_hash)
{
    return load_file(path, out_arena, out_len, 0, 0, 0, 2, 0, 0, expected_hash);
}

void *hb_reloc_load_file_checked(const char *path, void **out_arena,
                                 uint32_t *out_len, uint8_t **out_base,
                                 uint32_t *out_span, uint32_t *out_image_size)
{
    return load_file(path, out_arena, out_len, out_base, out_span,
                     out_image_size, 1, 0, 0, 0);
}

void *hb_reloc_load_file_checked_at(const char *path, int volume,
                                    void **out_arena, uint32_t *out_len,
                                    uint8_t **out_base, uint32_t *out_span,
                                    uint32_t *out_image_size, uint32_t *out_version)
{
    return load_file(path, out_arena, out_len, out_base, out_span,
                     out_image_size, 1, volume, out_version, 0);
}
