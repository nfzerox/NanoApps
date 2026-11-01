/*
 * hb_silver_uitask.c — patch an OS class-method slot so resident code runs in
 * that object's context. See hb_silver_uitask.h.
 *
 * Mechanism (verified on iPod nano 7 OS 1.1.2): the controller's method table is
 * a plain array of code pointers in the OS image; overwriting one slot
 * redirects every virtual call through it. Patching the home controller's
 * action-dispatch slot lands our code on the UI task (we picked that slot by which
 * patch made our handler run, not from a labelled table). We flush the I-cache
 * over the patched word so the new pointer is seen, and return the prior value
 * for forwarding.
 */

#include "hb_silver_uitask.h"

#define FN_THUMB(addr) ((uintptr_t)((addr) | 1u))

uint32_t hb_silver_uitask_hook(uint32_t class_vtable, uint32_t slot_off, void *handler)
{
    uint32_t *vt = (uint32_t *)(uintptr_t)class_vtable;
    uint32_t  slot = slot_off / 4u;
    uint32_t  prev = vt[slot];

    vt[slot] = ((uint32_t)(uintptr_t)handler) | 1u;   /* thumb */
    hb_icache_invalidate(class_vtable, slot_off + 8u);
    return prev;
}

/* Emit an alignment-independent absolute jump (movw r12,#lo; movt r12,#hi;
 * bx r12 — 10 bytes) to `target` (thumb bit set) at dst. r12 is the
 * intra-procedure scratch register, safe to clobber at a function boundary. */
static void hb__emit_jump(uint8_t *dst, uint32_t target)
{
    uint32_t lo = target & 0xFFFFu, hi = (target >> 16) & 0xFFFFu, hw1, hw2;
    hw1 = 0xF240u | (((lo >> 11) & 1u) << 10) | ((lo >> 12) & 0xFu);     /* movw r12,#lo */
    hw2 = (((lo >> 8) & 7u) << 12) | (0xCu << 8) | (lo & 0xFFu);
    dst[0]=(uint8_t)hw1; dst[1]=(uint8_t)(hw1>>8); dst[2]=(uint8_t)hw2; dst[3]=(uint8_t)(hw2>>8);
    hw1 = 0xF2C0u | (((hi >> 11) & 1u) << 10) | ((hi >> 12) & 0xFu);     /* movt r12,#hi */
    hw2 = (((hi >> 8) & 7u) << 12) | (0xCu << 8) | (hi & 0xFFu);
    dst[4]=(uint8_t)hw1; dst[5]=(uint8_t)(hw1>>8); dst[6]=(uint8_t)hw2; dst[7]=(uint8_t)(hw2>>8);
    dst[8]=0x60; dst[9]=0x47;                                            /* bx r12 */
}

/* Thumb-2 B.W (unconditional, ±16 MB). Stays in Thumb (not a BX), so `target`'s
 * low bit is ignored. Only 4 bytes — for functions whose prologue can't spare the
 * 10 bytes hb__emit_jump needs (e.g. a PC-relative op within the first 10 bytes).
 * Encoding verified by round-trip disassembly. */
static void hb__emit_bw(uint8_t *dst, uint32_t target)
{
    uint32_t t = target & ~1u;
    uint32_t off = t - ((uint32_t)(uintptr_t)dst + 4u);
    uint32_t S  = (off >> 24) & 1u;
    uint32_t I1 = (off >> 23) & 1u, I2 = (off >> 22) & 1u;
    uint32_t J1 = (~(I1 ^ S)) & 1u, J2 = (~(I2 ^ S)) & 1u;
    uint32_t imm10 = (off >> 12) & 0x3FFu, imm11 = (off >> 1) & 0x7FFu;
    uint32_t hw1 = 0xF000u | (S << 10) | imm10;
    uint32_t hw2 = 0x9000u | (J1 << 13) | (J2 << 11) | imm11;
    dst[0]=(uint8_t)hw1; dst[1]=(uint8_t)(hw1>>8); dst[2]=(uint8_t)hw2; dst[3]=(uint8_t)(hw2>>8);
}

/* Like hb_silver_patch_function_n but the entry jump is a 4-byte B.W (so n can be
 * as small as 4 — one Thumb-2 op). Valid only when |hook-func| and |func-tramp|
 * are within B.W's ±16 MB and the relocated `n` prologue bytes are not PC-relative
 * (the rest of the original, from func+n, stays in place and may be PC-relative). */
void *hb_silver_patch_function_bw_stock(uint32_t func, void *hook, void *tramp,
                                        const void *stock, int n)
{
    uint8_t *f = (uint8_t *)(uintptr_t)func;
    uint8_t *t = (uint8_t *)tramp;
    const uint8_t *s = (const uint8_t *)stock;
    int i;

    for (i = 0; i < n; i++)
        t[i] = s[i];
    hb__emit_bw(t + n, func + (uint32_t)n);          /* trampoline -> func+n (rest)  */
    hb__emit_bw(f, (uint32_t)(uintptr_t)hook);       /* func entry -> our hook       */
    hb_icache_invalidate((uint32_t)(uintptr_t)tramp, (uint32_t)n + 6u);
    hb_icache_invalidate(func, 8u);
    return (void *)((uintptr_t)tramp | 1u);          /* callable "original"          */
}

void *hb_silver_patch_function_bw(uint32_t func, void *hook, void *tramp, int n)
{
    return hb_silver_patch_function_bw_stock(func, hook, tramp,
                                              (const void *)(uintptr_t)func, n);
}

/* Same as hb_silver_patch_function but copies `n` prologue bytes (the entry jump
 * is 10 bytes, so the prologue must be >= 10 and `n` must land on an instruction
 * boundary — a 4-byte Thumb-2 op spanning byte 10 needs n=12, etc.). */
void *hb_silver_patch_function_stock(uint32_t func, void *hook, void *tramp,
                                     const void *stock, int n)
{
    uint8_t *f = (uint8_t *)(uintptr_t)func;
    uint8_t *t = (uint8_t *)tramp;
    const uint8_t *s = (const uint8_t *)stock;
    int i;

    for (i = 0; i < n; i++)
        t[i] = s[i];
    hb__emit_jump(t + n, (func + (uint32_t)n) | 1u); /* trampoline -> func+n (rest of original) */
    hb__emit_jump(f, ((uint32_t)(uintptr_t)hook) | 1u);  /* func entry -> our hook */
    hb_icache_invalidate((uint32_t)(uintptr_t)tramp, (uint32_t)n + 14u);
    hb_icache_invalidate(func, 16u);
    return (void *)((uintptr_t)tramp | 1u);          /* callable "original" */
}

void *hb_silver_patch_function_n(uint32_t func, void *hook, void *tramp, int n)
{
    return hb_silver_patch_function_stock(func, hook, tramp,
                                           (const void *)(uintptr_t)func, n);
}

void *hb_silver_patch_function(uint32_t func, void *hook, void *tramp)
{
    return hb_silver_patch_function_n(func, hook, tramp, 10);
}

void hb_silver_repoint_function(uint32_t func, void *hook)
{
    /* Re-aim an already-patched function's entry at a new hook WITHOUT touching
     * its trampoline (its saved prologue + jump-back stay valid). Use on a
     * re-deploy where the hook code moved but the function is still patched. */
    hb__emit_jump((uint8_t *)(uintptr_t)func, ((uint32_t)(uintptr_t)hook) | 1u);
    hb_icache_invalidate(func, 16u);
}

void hb_silver_repoint_function_bw(uint32_t func, void *hook)
{
    /* Re-aim a B.W-patched function (only the 4-byte entry branch is rewritten;
     * its trampoline stays). MUST be used instead of hb_silver_repoint_function
     * for a function patched with hb_silver_patch_function_bw — a 10-byte repoint
     * would clobber the bytes the trampoline jumps back to. */
    hb__emit_bw((uint8_t *)(uintptr_t)func, (uint32_t)(uintptr_t)hook);
    hb_icache_invalidate(func, 8u);
}

static void hb__emit_abs(uint32_t address, void *target)
{
    uint32_t *p = (uint32_t *)(uintptr_t)address;
    p[0] = 0xf000f8dfu; /* ldr.w pc,[pc,#0], literal at address+4 */
    p[1] = (uint32_t)(uintptr_t)target | 1u;
}

void hb_silver_repoint_function_abs(uint32_t func, void *hook)
{
    if (func & 3u) return;
    hb__emit_abs(func, hook);
    hb_icache_invalidate(func, 8u);
}

void *hb_silver_patch_function_abs_stock(uint32_t func, void *hook, void *tramp,
                                        const void *stock, int n)
{
    if (n < 8 || ((func | (uint32_t)(uintptr_t)tramp | (uint32_t)n) & 3u))
        return 0;
    const uint8_t *s = (const uint8_t *)stock;
    uint8_t *t = (uint8_t *)tramp;
    for (int i = 0; i < n; i++) t[i] = s[i];
    hb__emit_abs((uint32_t)(uintptr_t)(t + n), (void *)(uintptr_t)(func + n));
    hb_icache_invalidate((uint32_t)(uintptr_t)t, (uint32_t)n + 8u);
    hb_silver_repoint_function_abs(func, hook);
    return (void *)((uintptr_t)tramp | 1u);
}

/*
 * The params message holds its payload object pointer in a fixed field, and
 * the payload stores the data pointer in a fixed field; for a named UI event
 * that data is the event-name C-string. (iPod nano 7 OS 1.1.2, read straight out
 * of the named-event handler: payload object at msg+0x08; data pointer at
 * payload+0x0c, after the payload's vtable / type / size words.) These are
 * plain field reads — no method call — so it is safe to call on every event.
 */
#define MSG_PAYLOAD_OBJ_OFF 0x08u
#define PAYLOAD_DATA_OFF    0x0cu

const char *hb_silver_event_name(void *params_msg)
{
    void *payload;
    const char *name;

    if (!params_msg)
        return 0;
    payload = *(void **)((char *)params_msg + MSG_PAYLOAD_OBJ_OFF);
    if ((uintptr_t)payload <= 0x1000u)
        return 0;
    name = *(const char **)((char *)payload + PAYLOAD_DATA_OFF);
    return ((uintptr_t)name > 0x1000u) ? name : 0;
}

/*
 * The controller turns a raw input message into an event-name string via a
 * method at 0x084aa17a that takes `this`, the raw message, and two output
 * string slots. Each string slot is a single word: a pointer to the character
 * data, with an "empty" slot pointing at a fixed shared buffer (0x08d164e4).
 * So we init two scratch words to that shared-empty pointer, call the method,
 * copy out the resulting characters, then release each slot via the string
 * release fn at 0x0842d8f4 — no constructor needed.
 */
#define A_TRANSLATE   0x084aa17au
#define A_STR_UNLINK  0x0842d8f4u    /* string release(this)            */
#define STR_EMPTY_REP 0x08d164e4u    /* shared empty-string data pointer */

typedef void (*translate_t)(void *self, void *msg, void *out, void *out_alt);
typedef void (*str_unlink_t)(void *str_obj);

const char *hb_silver_translate_msg(void *self, void *raw_msg, char *out_buf, int buf_len)
{
    void *s1 = (void *)STR_EMPTY_REP;       /* empty string slot */
    void *s2 = (void *)STR_EMPTY_REP;
    const char *p;
    int i = 0;

    if (buf_len > 0)
        out_buf[0] = 0;
    if (!self || !raw_msg || buf_len < 1)
        return out_buf;

    ((translate_t)FN_THUMB(A_TRANSLATE))(self, raw_msg, &s1, &s2);

    p = (const char *)s1;                   /* s1 word = translated data ptr */
    if ((uintptr_t)p > 0x1000u)
        for (; i < buf_len - 1 && p[i]; i++)
            out_buf[i] = p[i];
    out_buf[i] = 0;

    ((str_unlink_t)FN_THUMB(A_STR_UNLINK))(&s1);
    ((str_unlink_t)FN_THUMB(A_STR_UNLINK))(&s2);
    return out_buf;
}

#define A_DISPATCH_SE 0x081c42e4u
#define VT_GETPARENTAPP 0x154u    /* self->vtable[VT_GETPARENTAPP] = parent-app-controller getter */
#define VT_HANDLEMSG    0x40u     /* parent->vtable[VT_HANDLEMSG]  = message handler          */

typedef int   (*dispatch_se_t)(void *self, void *ev, void *alt, int from_param, void *msg);
typedef void *(*get_parent_t)(void *self);
typedef int   (*handle_msg_t)(void *self, void *msg);

/* Forward an unconsumed message to the controller's parent app controller,
 * as the stock raw-input handler does. Returns its result (0 if no
 * parent). Virtual dispatch through each object's own vtable. */
static int hb__forward_to_parent_app(void *self, void *raw_msg)
{
    void **self_vt = *(void ***)self;
    void *parent = ((get_parent_t)self_vt[VT_GETPARENTAPP / 4u])(self);
    void **par_vt;

    if (!parent)
        return 0;
    par_vt = *(void ***)parent;
    return ((handle_msg_t)par_vt[VT_HANDLEMSG / 4u])(parent, raw_msg);
}

static int hb__prefix(const char *s, const char *p)
{ while (*p) { if (*s != *p) return 0; s++; p++; } return 1; }

static int hb__suffix(const char *s, const char *suf)
{
    int ls = 0, lf = 0;
    while (s[ls]) ls++;
    while (suf[lf]) lf++;
    if (lf > ls) return 0;
    for (int i = 0; i < lf; i++)
        if (s[ls - lf + i] != suf[i]) return 0;
    return 1;
}

/* Breadcrumbs are compiled only into the resident (HB_RAWMSG_TRACE). The
 * untether bootstrap links this file too, but it lives inside the firmware
 * image and its BSS is not cleared across a warm reboot, so it must not gain
 * a function-pointer global. */
#ifdef HB_RAWMSG_TRACE
void (*hb_silver_rawmsg_trace)(const char *tag, uint32_t v1, uint32_t v2);
#endif

int hb_silver_rawmsg(void *self, void *raw_msg, const char *claim_prefix,
                     const char *claim_suffix, char *out_buf, int buf_len)
{
    void *s1 = (void *)STR_EMPTY_REP;
    void *s2 = (void *)STR_EMPTY_REP;
    const char *p;
    int i = 0, claimed = 0, r = 0;

    if (buf_len > 0)
        out_buf[0] = 0;
    if (!self || !raw_msg)
        return 0;

    ((translate_t)FN_THUMB(A_TRANSLATE))(self, raw_msg, &s1, &s2);

    p = (const char *)s1;
    if ((uintptr_t)p > 0x1000u && buf_len > 0) {
        for (; i < buf_len - 1 && p[i]; i++)
            out_buf[i] = p[i];
        out_buf[i] = 0;
    }

    /* claim only events that match BOTH the prefix and the suffix, so e.g.
     * a tap on one of our items is ours; everything else (other suffixes, other
     * items) passes through to the OS unchanged. */
    if (out_buf[0] && claim_prefix && hb__prefix(out_buf, claim_prefix)
                   && (!claim_suffix || hb__suffix(out_buf, claim_suffix)))
        claimed = 1;

    /* Record sbitem event names in 8-char chunks (tag = text, v1 = offset). */
#ifdef HB_RAWMSG_TRACE
    if (hb_silver_rawmsg_trace && hb__prefix(out_buf, "sbitem.")) {
        for (int k = 0; k < 32 && out_buf[k]; k += 8)   /* icon taps only */
            hb_silver_rawmsg_trace(out_buf + k, (uint32_t)k, (uint32_t)claimed);
    }
#endif

    if (!claimed) {
        if (out_buf[0])                     /* decoded but not ours: dispatch once */
            r = ((dispatch_se_t)FN_THUMB(A_DISPATCH_SE))(self, &s1, &s2, 0, raw_msg);
        /* untranslatable input, or an event this controller didn't consume:
         * hand it to the parent app controller (stock behaviour — this is what
         * carries scroll/swipe). Strings are released after, as in the original. */
        if (r == 0)
            r = hb__forward_to_parent_app(self, raw_msg);
    }

    ((str_unlink_t)FN_THUMB(A_STR_UNLINK))(&s1);
    ((str_unlink_t)FN_THUMB(A_STR_UNLINK))(&s2);
    return claimed ? HB_SILVER_RAWMSG_CLAIMED : r;
}
