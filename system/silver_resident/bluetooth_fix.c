/* 1.1.2 Bluetooth timestamp fix, adapted from Merquice/NanoBtFix (MIT).
 * See LICENSE.compat. Runs once from the resident's first UI action. */
#include "hb_sdk.h"
#include "compat_fixes.h"

#define PATCH_ADDR 0x082b2bd6u
static const uint8_t original[] = {
    0x01,0xee,0x10,0xca, 0x91,0xed,0x01,0x0a, 0xb8,0xee,0x40,0x0b,
    0xb8,0xee,0x41,0x1b, 0x08,0xee,0x01,0x0b, 0xbc,0xee,0xc0,0x0b,
    0x81,0xed,0x01,0x0a
};
/* timestamp += SBC frame_count * 128; pad the old FP sequence with NOPs. */
static const uint8_t patched[] = {
    0x48,0x68,0x00,0xeb,0xcc,0x10,0x48,0x60,
    0x00,0xbf,0x00,0xbf,0x00,0xbf,0x00,0xbf,0x00,0xbf,
    0x00,0xbf,0x00,0xbf,0x00,0xbf,0x00,0xbf,0x00,0xbf
};

static int matches(const uint8_t *bytes)
{
    const volatile uint8_t *p = (const volatile uint8_t *)PATCH_ADDR;
    for (unsigned i = 0; i < sizeof original; i++)
        if (p[i] != bytes[i]) return 0;
    return 1;
}

int hb_bluetooth_fix(void)
{
    if (matches(patched)) return 2;
    if (!matches(original)) return 0;
    for (unsigned i = 0; i < sizeof patched; i++)
        ((volatile uint8_t *)PATCH_ADDR)[i] = patched[i];
    __asm__ volatile("dsb sy" : : : "memory");
    hb_icache_invalidate(PATCH_ADDR, sizeof patched);
    return matches(patched);
}
