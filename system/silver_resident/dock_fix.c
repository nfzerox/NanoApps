/* 1.1.2 A1717 dock fix, adapted from Raphmatt/nano7-a1717-dock (MIT).
 * See LICENSE.compat. All hook code/state belongs to the resident's heap.
 * The stock ID writer is only 30 bytes plus two literals. Reproduce its
 * checked behavior below instead of relocating its PC-relative instructions.
 * An absolute entry jump works even when the resident is >16 MB away. */
#include "hb_sdk.h"
#include "hb_silver_uitask.h"
#include "compat_fixes.h"

#define ID_WRITER    0x084df4c4u
#define ID_RECORD    0x08ae77a0u
#define ID_NOTIFY    0x08427a61u
#define ID_NOTIFY_OBJ 0x089187d4u
#define MEMCPY      0x0842d6e0u /* ARM entry */
#define UI_POST     0x0802fbd9u
#define UI_POST_OBJ 0x0842a995u
#define PORT_STATE  0x08918558u
#define POWER       0x080d0409u
#define SLEEP       0x0842b1ddu

typedef uint32_t (*fn0)(void);
typedef uint32_t (*fn1)(uint32_t);
typedef uint32_t (*fn2)(uint32_t, uint32_t);
typedef uint32_t (*fn5)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void *(*copy_fn)(void *, const void *, uint32_t);

/* Full function and literal pool, read back through C6/98 on stock 39A10023. */
static const uint8_t original[] = {
    0x10,0xb5,0x41,0x78,0x02,0x29,0x09,0xd1,
    0x81,0x1c,0x05,0x48,0x1a,0x22,0x4e,0xf7,
    0x06,0xe9,0xbd,0xe8,0x10,0x40,0x03,0x48,
    0x48,0xf7,0xc0,0xba,0x10,0xbd,0x00,0x00,
    0xa0,0x77,0xae,0x08,0xd4,0x87,0x91,0x08
};
static uint8_t installed;
static volatile uint8_t dock_seen;

static void dock_power_on(void)
{
    for (unsigned i = 0; i < 40; i++) {
        if (!dock_seen) return;
        uint8_t state = *(volatile uint8_t *)PORT_STATE;
        if (state == 3) {
            ((fn2)POWER)(4, 1);
            return;
        }
        if (state == 0 || state >= 4) return;
        ((fn2)SLEEP)(25, 0);
    }
    /* No power call after timeout or detach. Stock detach turns power off. */
}

static uint32_t dock_id_writer(uint32_t address)
{
    volatile uint8_t *p = (volatile uint8_t *)address;
    dock_seen = 0;
    if (p[1] != 2) return address; /* stock path does not consume this packet */
    dock_seen = p[15] == 0x05 && p[16] == 0xac && p[18] == 0 && p[19] == 5;
    if (dock_seen) p[19] = 2; /* accept this Apple dock, not every accessory */
    ((copy_fn)MEMCPY)((void *)ID_RECORD, (const void *)(address + 2), 26);
    uint32_t result = ((fn1)ID_NOTIFY)(ID_NOTIFY_OBJ);
    if (dock_seen)
        ((fn5)UI_POST)(((fn0)UI_POST_OBJ)(),
                      (uint32_t)(uintptr_t)dock_power_on | 1u, 0, 0, 0);
    return result;
}

int hb_dock_fix(void)
{
    if (installed) return 2;
    const volatile uint8_t *p = (const volatile uint8_t *)ID_WRITER;
    for (unsigned i = 0; i < sizeof original; i++)
        if (p[i] != original[i]) return 0;
    hb_silver_repoint_function(ID_WRITER, (void *)dock_id_writer);
    installed = 1;
    return 1;
}
