/*
 * hb_silver_uitask.h — run resident code on the UI task by hooking an OS
 * class-method slot.
 *
 * The home/menu controller's action-dispatch method runs on the UI task,
 * which is where mutating UI-owned singletons, the home grid, and the
 * resource chain is safe (these must run on the UI task). Patch that slot to point at
 * your own function and it will run in that context on every dispatch; forward
 * the calls you don't consume to the original.
 *
 * hb_silver_uitask_hook patches one method pointer in a class vtable, flushes
 * the I-cache over it, and returns the previous pointer (the original method).
 */
#ifndef HB_SILVER_UITASK_H
#define HB_SILVER_UITASK_H

#include "hb_sdk.h"

/* Home/menu controller dispatch slot (the UI-task entry point we hook). */
#define HB_SILVER_HOME_VTABLE        0x087cb9dcu   /* controller class vtable      */
#define HB_SILVER_HOME_DISPATCH_SLOT 0x190u        /* action-dispatch slot offset  */
#define HB_SILVER_HOME_DISPATCH_ORIG 0x082ae8ddu   /* stock dispatcher (forward to)*/

#define HB_SILVER_HOME_MESSAGE_SLOT  0x40u          /* complete message handler     */
#define HB_SILVER_HOME_MESSAGE_ORIG  0x085b49adu

#define HB_SILVER_HOME_EVENT_SLOT    0x17cu         /* queued named-event slot      */
#define HB_SILVER_HOME_EVENT_ORIG    0x081c4675u    /* stock handler (thumb)        */

/* Untranslated-message slot: receives raw input messages (incl. icon taps),
 * translated to an event name and dispatched internally. Hook point for taps
 * on custom home items whose SBID has no built-in action binding. */
#define HB_SILVER_HOME_RAWMSG_SLOT   0x188u         /* raw-input handler slot    */
#define HB_SILVER_HOME_RAWMSG_ORIG   0x081c4505u    /* stock handler (thumb)        */

/* 1.1.2 sends this model notification when rebuilding Home after USB eject.
 * TParamsMessage: type 30 at +4, payload at +8, notification code at payload+4.
 * RAWMSG receives it inside the stock Home message handler (0x085b49ac).
 * Defer UI changes to a later queued message so native view teardown can finish. HandleGotoMainScreen is not sent on this return path.
 * Verified by an on-device message trace and no-tap add/remove cycles. */
#define HB_SILVER_HOME_REFRESH_NOTICE 0x6385u

/*
 * Read the event-name C-string out of a params message (as delivered to the
 * named-event slot). Returns NULL if unavailable. The string is owned by the
 * message; copy it if you need it past the call.
 */
const char *hb_silver_event_name(void *params_msg);

/*
 * Recover the event name that a raw input message (as delivered to the
 * raw-input handler slot) translates to — e.g. "sbitem.<SBID>.clicked" for
 * an icon tap. Copies up to buf_len-1 chars + NUL into out_buf and returns it
 * (empty string if the message doesn't translate). `self` is the controller
 * the slot was invoked on. Manages its own scratch strings; safe to call per
 * message.
 */
const char *hb_silver_translate_msg(void *self, void *raw_msg, char *out_buf, int buf_len);

/* Return value from hb_silver_rawmsg when the event name matched the prefix. */
#define HB_SILVER_RAWMSG_CLAIMED 2

int hb_silver_rawmsg(void *self, void *raw_msg, const char *claim_prefix,
                     const char *claim_suffix, char *out_buf, int buf_len);
/* Optional breadcrumb sink for hb_silver_rawmsg (NULL = off). The resident
 * points it at hb_trace_log to record sbitem event names and dispatch path. */
extern void (*hb_silver_rawmsg_trace)(const char *tag, uint32_t v1, uint32_t v2);

/*
 * Patch class_vtable[slot_off] to `handler` (thumb bit set automatically),
 * flush the I-cache over the slot, and return the previous slot value (the
 * original method, for forwarding). Call once at load.
 */
uint32_t hb_silver_uitask_hook(uint32_t class_vtable, uint32_t slot_off, void *handler);

/*
 * Redirect a NON-virtual function (called directly via `bl`, so there is no
 * vtable slot to swap) to `hook` by rewriting its entry. `tramp` is >=24 bytes
 * of resident, executable memory that becomes a callable "original function":
 * the 10 overwritten prologue bytes + a jump back to func+10. Returns the
 * trampoline (thumb) so the hook can still invoke the original.
 *
 * REQUIREMENT: the first 10 bytes of `func` must be simple, position-independent
 * instructions (no PC-relative load, no branch) — verify by disassembly. Apply
 * once on a fresh boot (re-patching a patched function corrupts the trampoline);
 * guard with a check that the original prologue is still present.
 */
void *hb_silver_patch_function(uint32_t func, void *hook, void *tramp);
/* Variant copying `n` prologue bytes (must land on an instruction boundary). */
void *hb_silver_patch_function_n(uint32_t func, void *hook, void *tramp, int n);

/* Deterministic variants for reboot-persistent patches. `stock` contains the
 * known original prologue bytes for this firmware. They rebuild the trampoline
 * from those bytes even if `func` is already patched, then aim the entry at the
 * current hook. This avoids trusting scratch RAM left by a warm reboot. */
void *hb_silver_patch_function_stock(uint32_t func, void *hook, void *tramp,
                                     const void *stock, int n);
/* Like _n but uses a 4-byte B.W entry jump (stays in Thumb, ±16 MB), so n can be
 * as small as 4 — for functions with a PC-relative op inside the first 10 bytes
 * that can't fit the 10-byte jump. The relocated n bytes must NOT be PC-relative;
 * the rest (from func+n) stays put (and may be PC-relative). */
void *hb_silver_patch_function_bw(uint32_t func, void *hook, void *tramp, int n);
void *hb_silver_patch_function_bw_stock(uint32_t func, void *hook, void *tramp,
                                        const void *stock, int n);

/*
 * Re-aim an already-patched function's entry at a new `hook` (e.g. on a re-push
 * where the hook moved) without rebuilding its trampoline. Only valid if `func`
 * was previously patched by hb_silver_patch_function and its trampoline is intact.
 */
void hb_silver_repoint_function(uint32_t func, void *hook);

/* Re-aim a function patched with hb_silver_patch_function_bw (rewrites only the
 * 4-byte B.W entry). Use this — NOT hb_silver_repoint_function — for B.W patches. */
void hb_silver_repoint_function_bw(uint32_t func, void *hook);

/* Register-preserving absolute Thumb jump: ldr.w pc,[pc]; .word target|1.
 * No branch-distance limit. Entry and trampoline must be 4-byte aligned;
 * n >= 8 must be a multiple of 4, with position-independent instructions. */
void *hb_silver_patch_function_abs_stock(uint32_t func, void *hook, void *tramp,
                                        const void *stock, int n);
void hb_silver_repoint_function_abs(uint32_t func, void *hook);

#endif /* HB_SILVER_UITASK_H */
