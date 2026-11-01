/* Integer-only libc wrappers for the app's bulk copies. The SDK's default
 * memcpy/memset and newlib's backward memmove use byte-at-a-time loops.
 * Never read outside the requested range or issue an unaligned word access.
 * Integer-only code also works during entry, before the task's FP setup. */
#include <stddef.h>
#include <stdint.h>

typedef uint32_t CopyWord __attribute__((__may_alias__));

void *__wrap_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0) {
        while (n && ((uintptr_t)d & 3u)) { *d++ = *s++; n--; }
        while (n >= 16) {
            CopyWord *dw = (CopyWord *)d;
            const CopyWord *sw = (const CopyWord *)s;
            dw[0] = sw[0]; dw[1] = sw[1]; dw[2] = sw[2]; dw[3] = sw[3];
            d += 16; s += 16; n -= 16;
        }
        while (n >= 4) {
            *(CopyWord *)d = *(const CopyWord *)s;
            d += 4; s += 4; n -= 4;
        }
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *__wrap_memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || !n) return dst;
    if ((uintptr_t)d < (uintptr_t)s || (uintptr_t)d - (uintptr_t)s >= n)
        return __wrap_memcpy(dst, src, n);
    d += n; s += n;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0) {
        while (n && ((uintptr_t)d & 3u)) { *--d = *--s; n--; }
        while (n >= 16) {
            d -= 16; s -= 16;
            CopyWord *dw = (CopyWord *)d;
            const CopyWord *sw = (const CopyWord *)s;
            /* Descending stores preserve overlapping sources, even 4 bytes apart. */
            dw[3] = sw[3]; dw[2] = sw[2]; dw[1] = sw[1]; dw[0] = sw[0];
            n -= 16;
        }
        while (n >= 4) {
            d -= 4; s -= 4;
            *(CopyWord *)d = *(const CopyWord *)s;
            n -= 4;
        }
    }
    while (n--) *--d = *--s;
    return dst;
}

void *__wrap_memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;
    unsigned char b = (unsigned char)value;
    while (n && ((uintptr_t)d & 3u)) { *d++ = b; n--; }
    CopyWord word = (CopyWord)b * 0x01010101u;
    while (n >= 16) {
        CopyWord *dw = (CopyWord *)d;
        dw[0] = word; dw[1] = word; dw[2] = word; dw[3] = word;
        d += 16; n -= 16;
    }
    while (n >= 4) { *(CopyWord *)d = word; d += 4; n -= 4; }
    while (n--) *d++ = b;
    return dst;
}
