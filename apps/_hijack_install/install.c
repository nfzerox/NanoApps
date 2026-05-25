/*
 * install.c — opt-in hijack installer.
 *
 * Steps performed:
 *   1. Read /Apps/.hijack_stub into RAM at STUB_ADDR (0x09130000).
 *   2. Patch the Podcasts home-screen handler with a 12-byte
 *      Thumb-2 sequence:
 *           10 b5         push {r4, lr}
 *           01 48         ldr  r0, [pc, #4]   -> loads stub addr+1
 *           80 47         blx  r0
 *           10 bd         pop  {r4, pc}
 *           01 00 13 09   stub address (0x09130001 LE = thumb-bit)
 *   3. Save the original 12 bytes to /Apps/.hijack_backup so the
 *      uninstaller can restore them.
 *
 * After install, tap "Podcasts" on the home screen to launch the
 * Homebrew launcher. The original Podcasts UI is no longer
 * reachable until uninstalled (or rebooted — patches don't
 * persist across reboot).
 *
 * RISK: a wrong patch will crash SpringBoard or wedge the device
 * the next time the user taps Podcasts. To recover: reboot. The
 * RAM patch is GONE after reboot.
 */

#include "hb_sdk.h"

#define HANDLE_PODCASTS_ADDR  0x0844032cu
#define STUB_ADDR             0x09130000u
#define STUB_PATH             "/Apps/.hijack_stub"
#define BACKUP_PATH           "/Apps/.hijack_backup"
#define BG                    HB_RGB(0x00, 0x00, 0x40)
#define OK                    HB_GREEN
#define ERR                   HB_RED

/* Needed after writing executable bytes into RAM/ROM-mapped code VAs. */
static inline void cache_clean_invalidate(uint32_t addr, uint32_t size)
{
    register uint32_t r12 __asm__("r12") = 7;
    register uint32_t r0  __asm__("r0")  = addr;
    register uint32_t r1  __asm__("r1")  = size;
    __asm__ volatile("svc #70"
                     : "+r"(r12), "+r"(r0), "+r"(r1)
                     :
                     : "memory");
}

/* The 12-byte hook bytes. Built from the disasm above. The 4-byte
   literal at the end is the stub address with Thumb bit set. */
static const uint8_t k_hook_bytes[12] = {
    0x10, 0xb5,                          /* push {r4, lr}              */
    0x01, 0x48,                          /* ldr r0, [pc, #4]           */
    0x80, 0x47,                          /* blx r0                     */
    0x10, 0xbd,                          /* pop {r4, pc}               */
    0x01, 0x00, 0x13, 0x09,              /* literal: 0x09130001 LE     */
};

static void show(const char *msg, int16_t y, hb_color_t fg)
{
    hb_draw_str(8, y, msg, 2, fg, BG);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_trace_log("HJ_INST", 0, 0);

    hb_fill_screen(BG);
    show("HIJACK INSTALL", 4, HB_YELLOW);

    int16_t y = 50;

    /* Step 1: load stub bytes from disk */
    uint32_t stub_size = hb_fs_size(STUB_PATH);
    if (stub_size == 0 || stub_size > 0x10000) {
        show("stub missing", y, ERR);
        show("/Apps/.hijack_stub", y + 30, HB_WHITE);
        goto wait_exit;
    }

    /* Read stub directly into its destination at STUB_ADDR */
    uint32_t read = hb_fs_read(STUB_PATH, (void *)STUB_ADDR, stub_size);
    if (read != stub_size) {
        show("stub read FAIL", y, ERR);
        hb_trace_log("HJ_RDFL", read, stub_size);
        goto wait_exit;
    }
    cache_clean_invalidate(STUB_ADDR, stub_size);
    hb_trace_log("HJ_STBR", read, stub_size);
    show("stub loaded", y, OK); y += 30;

    /* Step 2: back up original 12 bytes */
    bool wrote_backup = hb_fs_write(BACKUP_PATH,
                                     (void *)HANDLE_PODCASTS_ADDR, 12);
    if (!wrote_backup) {
        show("backup FAIL", y, ERR);
        hb_trace_log("HJ_BKFL", 0, 0);
        goto wait_exit;
    }
    show("backup saved", y, OK); y += 30;

    /* Step 3: patch (RAM-only, lost on reboot) */
    volatile uint8_t *target = (volatile uint8_t *)HANDLE_PODCASTS_ADDR;
    for (int i = 0; i < 12; i++) target[i] = k_hook_bytes[i];
    cache_clean_invalidate(HANDLE_PODCASTS_ADDR, sizeof k_hook_bytes);
    hb_trace_log("HJ_PATCH", HANDLE_PODCASTS_ADDR, STUB_ADDR);

    show("PATCHED!", y, OK); y += 30;
    show("Tap Podcasts on home", y, HB_WHITE); y += 20;
    show("to run Homebrew.", y, HB_WHITE);

    for (volatile uint32_t i = 0; i < 250000; i++) { }
    return;

wait_exit:
    show("vol exits", HB_SCREEN_H - 20, HB_WHITE);
    for (uint32_t f = 0; f < 5000000; f++) {
        if (hb_button_pressed(HB_BTN_VOL_UP) ||
            hb_button_pressed(HB_BTN_VOL_DOWN)) return;
        for (volatile uint32_t i = 0; i < 1000; i++) { }
    }
}
