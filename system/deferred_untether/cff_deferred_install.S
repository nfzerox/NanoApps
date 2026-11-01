.syntax unified
.thumb

/*
 * Nano 7 / 39A10023 post-CFF publisher.
 *
 * The CFF decoder writes both deferred_stage.bin and this image directly into
 * the verified firmware cave before endchar.  cff_slot_load may release its
 * charstring frame immediately after the decoder returns, so this code never
 * retains or dereferences a pointer into that frame.
 *
 * The ordinary cff_slot_load epilogue restores r4 as the descriptor below, r5
 * as the middle of the stock cache wrapper, and returns through:
 *
 *   0x08005202  mov   r0, r4
 *   0x08005204  blx   r5
 *
 * The wrapper publishes the complete cave image, then pops original r4 and
 * this entry PC from the two temporary caller-stack words.  This publisher
 * repairs those words, publishes only the ACTION slot, restores saved r5, and
 * resumes the captured firmware continuation.
 */

#ifndef DEFERRED_SIZE
#error DEFERRED_SIZE is required
#endif

#define HOME_ACTION_SLOT 0x087cbb6c
#define STOCK_ACTION     0x082ae8dd
#define DEFERRED_CAVE    0x086c97e4

.section .text.entry,"ax",%progbits
.balign 4
.global cff_deferred_install
.type cff_deferred_install,%function
.thumb_func
cff_deferred_install:
    /* Undo the wrapper's two-word pop and repair its temporary stack words. */
    sub   sp, #8
    adr   r12, cff_saved_stack152
    ldmia r12!, {r0-r1}
    strd  r0, r1, [sp]

    ldr   r3, =HOME_ACTION_SLOT
    ldr   r1, [r3]
    ldr   r2, =STOCK_ACTION
    cmp   r1, r2
    bne   .Lreturn

    ldr   r1, =(DEFERRED_CAVE | 1)
    str   r1, [r3]
    dsb   sy

    /* Publish the hook through the same cache-maintenance interface. */
    mov   r0, r3
    movs  r1, #4
    movs  r2, #7
    mov   r12, r2
    svc   #70

.Lreturn:
    /* SVC 70 may destroy every volatile register; reload from coherent cave. */
    adr   r3, cff_saved_r5
    ldrd  r1, r2, [r3]          /* original r5, original saved PC */
    mov   r5, r1
    /* A successful endchar/cff_slot_load result is zero. */
    movs  r0, #0
    bx    r2
.global cff_deferred_install_end
cff_deferred_install_end:
.size cff_deferred_install, .-cff_deferred_install

.ltorg

/* The code and its literal pool must stay below the fixed capture block. */
.org 0x60
.global cff_saved_stack152
.type cff_saved_stack152,%object
cff_saved_stack152:
    .word 0x53313532
.global cff_saved_stack153
cff_saved_stack153:
    .word 0x53313533
.size cff_saved_stack152, 8

.global cff_saved_r5
.type cff_saved_r5,%object
cff_saved_r5:
    .word 0x53415635
.size cff_saved_r5, 4

.global cff_install_continuation
.type cff_install_continuation,%object
cff_install_continuation:
    .word 0x5245544e
.size cff_install_continuation, 4

/* The stock wrapper reads descriptor+0x2c as size and descriptor+0x34 as
 * address.  The leading magic lets the CFF generator locate the descriptor in
 * the raw publisher artifact without relying on an ELF symbol table. */
.org 0x70
.global cff_install_descriptor
.type cff_install_descriptor,%object
cff_install_descriptor:
    .word 0x43534544              /* "DESC" */
    .space 0x28
.global cff_install_flush_size
.type cff_install_flush_size,%object
cff_install_flush_size:
    .word DEFERRED_SIZE + cff_deferred_image_end
.size cff_install_flush_size, 4

.global cff_install_reserved
cff_install_reserved:
    .word 0

.global cff_install_cave
.type cff_install_cave,%object
cff_install_cave:
    .word DEFERRED_CAVE
.size cff_install_cave, 4
.size cff_install_descriptor, .-cff_install_descriptor

.global cff_deferred_image_end
cff_deferred_image_end:
