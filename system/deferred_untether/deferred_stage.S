.syntax unified
.thumb

/*
 * Boot-safe Nano 7 untether stage.
 *
 * The CFF exploit writes this image into an aligned executable payload window
 * inside the OS image's 0x800-byte ISO-BMFF `free` atom, then points only the
 * Home action slot at the entry below. Nothing here runs in the font decoder.
 * Boot lifecycle actions also use this slot, so they must remain completely
 * stock unless their bounded action name is exactly `HandleGotoMainScreen`
 * (automatic path) or `HandleGotoSettings` (manual fallback).  Both paths
 * still have to pass the same heap/filesystem readiness gates before the
 * loader touches resident state.
 *
 * The build supplies LINK_VA and the six HRL1 header values.  This code is
 * deliberately freestanding and owns no fixed writable RAM other than its
 * firmware patch window and the three vtable slots used during handoff.
 */

#ifndef LINK_VA
#define LINK_VA 0x086c97e4
#endif
#ifndef RES_FILE_SIZE
#error RES_FILE_SIZE is required
#endif
#ifndef RES_IMAGE_SIZE
#error RES_IMAGE_SIZE is required
#endif
#ifndef RES_SPAN
#error RES_SPAN is required
#endif
#ifndef RES_ENTRY_OFF
#error RES_ENTRY_OFF is required
#endif
#ifndef RES_RELOC_COUNT
#error RES_RELOC_COUNT is required
#endif
#ifndef RES_ALIGN
#error RES_ALIGN is required
#endif
#ifndef RES_FNV1A
#error RES_FNV1A is required
#endif

#if RES_ALIGN > 32
#error deferred stage supports resident alignment up to 32 bytes
#endif

#define RES_SRC_OFF       (RES_SPAN - RES_FILE_SIZE)
#define RES_RELOCS_OFF    (RES_SRC_OFF + 24 + RES_IMAGE_SIZE)
#define FILE_OBJ_OFF      ((RES_SPAN + 7) & ~7)
#define FILE_CACHE_OFF    ((FILE_OBJ_OFF + 0x60 + 31) & ~31)
#define FILE_CACHE_SIZE   0x1010
#define ARENA_ALLOC_SIZE  (FILE_CACHE_OFF + FILE_CACHE_SIZE + 31)

#define HOME_RAW_SLOT     0x087cbb64
#define HOME_ACTION_SLOT  0x087cbb6c
#define TEMPLATE_SLOT     0x087ab8a4
#define STOCK_RAW         0x081c4505
#define STOCK_ACTION      0x082ae8dd
#define STOCK_TEMPLATE    0x081e0a03

#define HEAP_OBJECT_CELL  0x08909134
#define RAM_LO            0x08000000
#define RAM_HI            0x0a000000

/* Verified byte-for-byte in the decrypted 39A10023 OS.  The names have
 * multiple addresses in the real image, so identity must be content based. */
#define MAX_ACTION_BYTES  64
#define SETTINGS_ACTION_LEN 18
#define SETTINGS_ACTION_FNV 0x5ada82a5
#define MAIN_ACTION_LEN   20
#define MAIN_ACTION_FNV   0xe688f681

/* The publisher owns this page and the C6 reader returns it verbatim.  The
 * resident/selector use through +0x80; the stage owns the words below.
 * ACTION_HISTORY is a two-bit shift register (1=Main, 2=Settings), so every
 * recognised action in the current boot remains visible, bounded to the last
 * sixteen.  READY_MASK describes the latest recognised action:
 * 1=recognised, 2=heap, 4=enabled, 8=arena, 16=file ready. */
#define REPORT_EVENTS     0x086c9c04
#define REPORT_ACTION_HISTORY 0x04
#define REPORT_ACTION_COUNT   0x08
#define REPORT_LAST_ACTION    0x0c
#define REPORT_READY_MASK     0x10
#define REPORT_UNKNOWN_HASH   0x14
#define REPORT_UNKNOWN_LENGTH 0x18
#define REPORT_UNKNOWN_COUNT  0x1c

#define OS_ALLOC          0x0842b441
#define OS_FREE           0x0842d86d
#define FOBJ_OPEN         0x084137a9
#define FOBJ_CLOSE        0x08423be1
#define FOBJ_READY        0x08417e19
#define FOBJ_READ         0x0841cddd
#define FS_EXISTS         0x0841bb7d

.section .text,"ax",%progbits
.balign 4

.global deferred_action
.type deferred_action,%function
.thumb_func
deferred_action:
    /* Ten saved words keep every firmware call 8-byte aligned. */
    stmdb sp!, {r0-r8, lr}

    /* The bounded content matcher lives in the existing 96-byte gap after the
     * 512-byte report page. It returns 0=stock, 1=Main, 2=Settings. */
    bl    action_match_helper
    cmp   r2, #0
    beq.w .Lforward_stock

    ldr   r7, =REPORT_EVENTS
    ldr   r0, [r7, #REPORT_ACTION_HISTORY]
    lsls  r0, r0, #2
    orrs  r0, r0, r2
    str   r0, [r7, #REPORT_ACTION_HISTORY]
    ldr   r0, [r7, #REPORT_ACTION_COUNT]
    adds  r0, r0, #1
    str   r0, [r7, #REPORT_ACTION_COUNT]
    str   r2, [r7, #REPORT_LAST_ACTION]
    movs  r0, #1
    str   r0, [r7, #REPORT_READY_MASK]

    adr   r4, stage_busy
    ldr   r3, [r4]
    cmp   r3, #0
    bne.w .Lforward_stock
    movs  r3, #1
    str   r3, [r4]

    /* The action vtable exists before the general OS heap does.  A cold-boot
     * action arriving during that interval must remain stock and retry later;
     * calling OS_ALLOC before this cell is a plausible heap object is unsafe. */
    ldr   r3, =HEAP_OBJECT_CELL
    ldr   r3, [r3]
    ldr   r2, =RAM_LO
    cmp   r3, r2
    blo.w .Lfail_no_arena
    ldr   r2, =RAM_HI
    cmp   r3, r2
    bhs.w .Lfail_no_arena
    tst   r3, #3
    bne.w .Lfail_no_arena
    movs  r0, #3
    str   r0, [r7, #REPORT_READY_MASK]

    /* Disk Mode can create this marker without running any NanoApps code.
     * Check it only after the OS heap/filesystem are live. FS_EXISTS is a
     * direct lookup and does not consume the resident file object's cache. */
    adr   r0, disabled_path
    movs  r1, #0
    ldr   r3, =FS_EXISTS
    blx   r3
    cmp   r0, #0
    bne.w .Ldisabled
    movs  r0, #7
    str   r0, [r7, #REPORT_READY_MASK]

    /* One arena owns the resident plus the temporary file object/cache. */
    movw  r0, #ARENA_ALLOC_SIZE
    movs  r1, #2
    ldr   r3, =OS_ALLOC
    blx   r3
    mov   r4, r0                /* original allocation, for failure cleanup */
    cmp   r4, #0
    beq.w .Lfail_no_arena
    movs  r0, #15
    str   r0, [r7, #REPORT_READY_MASK]
    adds  r5, r4, #31
    bic   r5, r5, #31           /* resident base (>= HRL1 alignment) */
    movw  r0, #FILE_OBJ_OFF
    add   r6, r5, r0            /* file object */
    movw  r0, #FILE_CACHE_OFF
    add   r7, r5, r0            /* 32-byte-aligned file cache */

    /* File object cache must start clean. */
    mov   r0, r7
    movs  r1, #0
    movw  r2, #FILE_CACHE_SIZE
    add   r2, r7, r2
.Lzero_cache:
    str   r1, [r0], #4
    cmp   r0, r2
    blo   .Lzero_cache

    /* The exact-bound selector lives in the IPSW resource volume, so boot
     * waits for the disk resident without an extra startup tree. */
    sub   sp, #16
    movw  r3, #0x1000
    str   r3, [sp, #0]
    movs  r3, #1
    str   r3, [sp, #4]
    str   r7, [sp, #8]
    mov   r0, r6
    adr   r1, resident_path
    movs  r2, #1
    movs  r3, #4
    ldr   r12, =FOBJ_OPEN
    blx   r12
    add   sp, #16
    mov   r0, r6
    ldr   r3, =FOBJ_READY
    blx   r3
    cmp   r0, #0
    beq.w .Lopen_failed
    movs  r0, #31
    ldr   r3, =REPORT_EVENTS
    str   r0, [r3, #REPORT_READY_MASK]

    /* Read the packed HRL1 at the top of its future image span. */
    movw  r0, #RES_SRC_OFF
    add   r8, r5, r0
    sub   sp, #8
    movs  r0, #0
    str   r0, [sp, #0]
    mov   r0, r6
    movw  r1, #RES_FILE_SIZE
    mov   r2, r8
    mov   r3, sp
    ldr   r12, =FOBJ_READ
    blx   r12
    mov   r7, r0                /* read return code */
    mov   r0, r6
    ldr   r3, =FOBJ_CLOSE
    blx   r3
    ldr   r0, [sp, #0]          /* bytes read */
    add   sp, #8
    cmp   r7, #0
    bne   .Lfail_free
    movw  r1, #RES_FILE_SIZE
    cmp   r0, r1
    bne   .Lfail_free

    /* Bind the loader to the exact packed resident used to build this stage.
     * Header/layout checks alone cannot detect same-size code corruption. */
    mov   r1, r8
    movw  r2, #RES_FILE_SIZE
    ldr   r0, action_fnv_offset
    ldr   r3, action_fnv_prime
.Lhash_resident:
    ldrb  r7, [r1], #1
    eor   r0, r0, r7
    mul   r0, r0, r3
    subs  r2, #1
    bne   .Lhash_resident
    ldr   r1, =RES_FNV1A
    cmp   r0, r1
    bne   .Lfail_free

    ldr   r0, [r8]
    ldr   r1, =0x314c5248       /* HRL1 */
    cmp   r0, r1
    bne   .Lfail_free

    /* The loader was compiled for one exact resident layout.  Reject a file
     * whose self-described layout differs even if its byte count still fits. */
    ldr   r0, [r8, #4]
    movw  r1, #RES_ENTRY_OFF
    cmp   r0, r1
    bne   .Lfail_free
    ldr   r0, [r8, #8]
    movw  r1, #RES_IMAGE_SIZE
    cmp   r0, r1
    bne   .Lfail_free
    ldr   r0, [r8, #12]
    movw  r1, #RES_SPAN
    cmp   r0, r1
    bne   .Lfail_free
    ldr   r0, [r8, #16]
    movw  r1, #RES_RELOC_COUNT
    cmp   r0, r1
    bne   .Lfail_free
    ldr   r0, [r8, #20]
    movw  r1, #RES_ALIGN
    cmp   r0, r1
    bne   .Lfail_free

    /* Validate the complete relocation table before changing executable
     * bytes.  Canonical HRL1 offsets are aligned, sorted, non-overlapping and
     * each ABS32 word is wholly contained in PROGBITS. */
#if RES_RELOC_COUNT > 0
    movw  r0, #RES_RELOCS_OFF
    add   r1, r5, r0
    movw  r2, #RES_RELOC_COUNT
    movs  r6, #0                /* lowest offset allowed this iteration */
.Lvalidate_reloc:
    ldr   r3, [r1], #4
    tst   r3, #3
    bne   .Lfail_free
    movw  r0, #(RES_IMAGE_SIZE - 4)
    cmp   r3, r0
    bhi   .Lfail_free
    cmp   r3, r6
    blo   .Lfail_free
    adds  r6, r3, #4
    subs  r2, #1
    bne   .Lvalidate_reloc
#endif

    /* Copy PROGBITS down, then apply the trusted build's ABS32 relocs. */
    mov   r0, r5
    adds  r1, r8, #24
    movw  r2, #RES_IMAGE_SIZE
.Lcopy:
    ldrb  r3, [r1], #1
    strb  r3, [r0], #1
    subs  r2, #1
    bne   .Lcopy

#if RES_RELOC_COUNT > 0
    movw  r0, #RES_RELOCS_OFF
    add   r1, r5, r0
    movw  r2, #RES_RELOC_COUNT
.Lreloc:
    ldr   r3, [r1], #4
    ldr   r0, [r5, r3]
    adds  r0, r0, r5
    str   r0, [r5, r3]
    subs  r2, #1
    bne   .Lreloc
#endif

    /* Clear inter-section gaps and BSS. */
    movw  r0, #RES_IMAGE_SIZE
    add   r0, r5, r0
    movw  r1, #RES_SPAN
    add   r1, r5, r1
    movs  r2, #0
    cmp   r0, r1
    bhs   .Lzero_bss_done
.Lzero_bss:
    strb  r2, [r0], #1
    cmp   r0, r1
    blo   .Lzero_bss
.Lzero_bss_done:

    mov   r0, r5
    movw  r1, #RES_SPAN
    movs  r2, #7
    mov   r12, r2
    svc   #70

    /* Give the resident a clean three-slot baseline.  The fixed stage itself
     * was installed in ACTION only; RAW and TEMPLATE stay factory until here. */
    ldr   r6, =HOME_RAW_SLOT
    ldr   r0, =STOCK_RAW
    str   r0, [r6]
    ldr   r0, =STOCK_ACTION
    str   r0, [r6, #8]
    mov   r0, r6
    movs  r1, #12
    movs  r2, #7
    mov   r12, r2
    svc   #70

    ldr   r6, =TEMPLATE_SLOT
    ldr   r0, =STOCK_TEMPLATE
    str   r0, [r6]
    mov   r0, r6
    movs  r1, #4
    movs  r2, #7
    mov   r12, r2
    svc   #70

    mov   r0, r5
    movw  r1, #RES_SPAN
    movw  r3, #RES_ENTRY_OFF
    add   r3, r5, r3
    orr   r3, r3, #1
    blx   r3

    /* The disk-only bootstrap retains its own waiting handlers until media
     * is available. Its exact file/hash/relocations were checked above. Keep
     * the one small arena alive so those UI handlers can complete safely. */
    movw  r7, #RES_IMAGE_SIZE
    add   r7, r7, r5
    ldr   r3, =HOME_ACTION_SLOT
    ldr   r6, [r3]              /* preserved across the final stack restore */
    tst   r6, #1
    beq   .Lresident_failed
    ldr   r3, =STOCK_ACTION
    cmp   r6, r3
    beq   .Lresident_failed
    bic   r0, r6, #1
    cmp   r0, r5
    blo   .Lresident_failed
    cmp   r0, r7
    bhs   .Lresident_failed

    ldr   r3, =HOME_RAW_SLOT
    ldr   r0, [r3]
    tst   r0, #1
    beq   .Lresident_failed
    ldr   r3, =STOCK_RAW
    cmp   r0, r3
    beq   .Lresident_failed
    bic   r0, r0, #1
    cmp   r0, r5
    blo   .Lresident_failed
    cmp   r0, r7
    bhs   .Lresident_failed

    ldr   r3, =TEMPLATE_SLOT
    ldr   r0, [r3]
    tst   r0, #1
    beq   .Lresident_failed
    ldr   r3, =STOCK_TEMPLATE
    cmp   r0, r3
    beq   .Lresident_failed
    bic   r0, r0, #1
    cmp   r0, r5
    blo   .Lresident_failed
    cmp   r0, r7
    bhs   .Lresident_failed

    /* Forward this same action through the resident dispatcher so
     * registration happens in the already proven UI-task context. */
    adr   r3, stage_busy
    movs  r2, #0
    str   r2, [r3]
    mov   r12, r6
    ldmia sp!, {r0-r8, lr}
    bx    r12

.Lresident_failed:
    /* Never expose a partial install or retry another resident this boot.
     * Return all three hooks to stock and free only the exact-bound selector.
     * If resident code ran, the selector deliberately retained its arena. */
    ldr   r6, =HOME_RAW_SLOT
    ldr   r0, =STOCK_RAW
    str   r0, [r6]
    ldr   r0, =STOCK_ACTION
    str   r0, [r6, #8]
    mov   r0, r6
    movs  r1, #12
    movs  r2, #7
    mov   r12, r2
    svc   #70

    ldr   r6, =TEMPLATE_SLOT
    ldr   r0, =STOCK_TEMPLATE
    str   r0, [r6]
    mov   r0, r6
    movs  r1, #4
    movs  r2, #7
    mov   r12, r2
    svc   #70
    b     .Lfail_free

.Lopen_failed:
    mov   r0, r6
    ldr   r3, =FOBJ_CLOSE
    blx   r3
.Lfail_free:
    mov   r0, r4
    ldr   r3, =OS_FREE
    blx   r3
.Lfail_no_arena:
    adr   r3, stage_busy
    movs  r2, #0
    str   r2, [r3]
.Lforward_stock:
    ldr   r12, =STOCK_ACTION
    ldmia sp!, {r0-r8, lr}
    bx    r12

.Ldisabled:
    /* One successful marker lookup permanently returns ACTION to stock for
     * this boot. The installer never changed RAW or TEMPLATE. */
    ldr   r6, =HOME_ACTION_SLOT
    ldr   r0, =STOCK_ACTION
    str   r0, [r6]
    mov   r0, r6
    movs  r1, #4
    movs  r2, #7
    mov   r12, r2
    svc   #70
    adr   r3, stage_busy
    movs  r2, #0
    str   r2, [r3]
    b     .Lforward_stock
.size deferred_action, .-deferred_action

.balign 4
.global stage_busy
stage_busy:
    .word 0
resident_path:
    .asciz "/NanoApps/loader.hbapp"
disabled_path:
    .asciz "/Library/disabled"
.balign 4

/* Keep matcher literals in the primary stage so the 96-byte helper island is
 * entirely executable code and cannot silently grow through its boundary. */
action_ram_hi_limit:
    .word (RAM_HI - MAX_ACTION_BYTES)
action_fnv_offset:
    .word 0x811c9dc5
action_fnv_prime:
    .word 0x01000193
action_settings_fnv:
    .word SETTINGS_ACTION_FNV
action_main_fnv:
    .word MAIN_ACTION_FNV
action_report_events:
    .word REPORT_EVENTS
.ltorg

.global deferred_primary_end
deferred_primary_end:

/* This island occupies only CAVE+0x5a0..+0x600. The preceding 0x200 bytes are
 * the reporter page and no reporter writer reaches this section. Its unused
 * tail at report+0x180..+0x200 is reserved for the host's one-shot reset stub. */
.section .match_helper,"ax",%progbits
.balign 4
.global action_match_helper
.type action_match_helper,%function
.thumb_func
action_match_helper:
    mov   r2, r1
    ldr   r7, =action_ram_hi_limit
    ldr   r3, =RAM_LO
    cmp   r2, r3
    blo   .Lunknown_pointer
    ldr   r3, [r7, #0]
    cmp   r2, r3
    bhi   .Lunknown_pointer
    ldr   r6, [r7, #4]
    ldr   r5, [r7, #8]
    movs  r4, #0
.Lhash_action:
    cmp   r4, #MAX_ACTION_BYTES
    beq   .Lunknown_action
    ldrb  r0, [r2], #1
    cmp   r0, #0
    beq   .Laction_hashed
    eor   r6, r6, r0
    mul   r6, r6, r5
    adds  r4, #1
    b     .Lhash_action
.Laction_hashed:
    cmp   r4, #SETTINGS_ACTION_LEN
    bne   .Lcheck_main_action
    ldr   r3, [r7, #12]
    cmp   r6, r3
    bne   .Lunknown_action
    movs  r2, #2                /* manual Settings fallback */
    bx    lr
.Lcheck_main_action:
    cmp   r4, #MAIN_ACTION_LEN
    bne   .Lunknown_action
    ldr   r3, [r7, #16]
    cmp   r6, r3
    bne   .Lunknown_action
    movs  r2, #1                /* automatic Home path */
    bx    lr
.Lunknown_action:
    ldr   r3, [r7, #20]
    str   r6, [r3, #REPORT_UNKNOWN_HASH]
    str   r4, [r3, #REPORT_UNKNOWN_LENGTH]
    ldr   r0, [r3, #REPORT_UNKNOWN_COUNT]
    adds  r0, #1
    str   r0, [r3, #REPORT_UNKNOWN_COUNT]
.Lunknown_pointer:
    movs  r2, #0
    bx    lr
.size action_match_helper, .-action_match_helper
.balign 4
.ltorg

/* Build-time guards in the linker script enforce both code windows. */
.global deferred_stage_end
deferred_stage_end:
