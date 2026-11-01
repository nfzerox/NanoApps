.syntax unified
.thumb

/* Nano 7 direct-cave NanoApps publisher with the C6/98 debug channel.
 * It runs after the stock cache wrapper, installs the C6/98 handler, then publishes
 * only the Settings-gated deferred ACTION hook. The resident records later
 * progress in the same report page. */

#define HOME_ACTION_SLOT        0x087cbb6c
#define STOCK_ACTION            0x082ae8dd
#define C6_DIRECT_CALL          0x081994f8
#define STOCK_C6_CALL           0xffaef002
#define CAVE                    0x086c97e4
#define STAGE_SIZE              0x600
#define RECORDER_STATE          (CAVE + 0x3a0)
#define REPORT_MAGIC            0x32505452
#define REPORT_MAGIC_OFFSET     0x2c
#define REPORT_STARTED_OFFSET   0x30
#define REPORT_CPSR_OFFSET      0x34
#define REPORT_PHASE_OFFSET     0x38
#define REPORT_REASON_OFFSET    0x3c
#define REPORT_DETAIL_OFFSET    0x40
#define REPORT_DETAIL_WORDS     13
#define SCSI_STUB_DST           0x0819c1d4
#define SCSI_STUB_SIZE          140

.section .text,"ax",%progbits
.balign 4
.global cff_deferred_install
.type cff_deferred_install,%function
.thumb_func
cff_deferred_install:
    /* Recover invocation-private saved r5/continuation and original frame. */
    sub.w r12, sp, #0x268
    ldmia.w r12, {r0-r3}
    sub   sp, #24
    stmia.w sp, {r2-r4, r6}
    str   r0, [sp, #16]
    str   r1, [sp, #20]
.global cff_entry_publish_branch
cff_entry_publish_branch:
    b     .Lpublish
.global cff_deferred_install_end
cff_deferred_install_end:
.size cff_deferred_install, .-cff_deferred_install

.Lclaim_install:
    adr   r0, cff_install_lock
    ldrex r1, [r0]
    cbnz  r1, .Lclaim_taken
    movs  r1, #1
    strex r2, r1, [r0]
    cmp   r2, #0
    bne   .Lclaim_taken
    dsb   sy
    b     .Lclaim_won
.Lclaim_taken:
    clrex
    movs  r0, #2
    b     .Lrecord_failure

.org 0x60
.global cff_install_lock
.type cff_install_lock,%object
cff_install_lock:
    .word 0
.size cff_install_lock, .-cff_install_lock

.org 0x70
.global cff_install_descriptor
cff_install_descriptor:
    .word 0x43534544
    .space 0x28
.global cff_install_flush_size
cff_install_flush_size:
    .word STAGE_SIZE + cff_deferred_image_end
.global cff_install_reserved
cff_install_reserved:
    .word 0
.global cff_install_cave
cff_install_cave:
    .word CAVE

.balign 4
.Lpublish:
    adr   r0, cff_install_lock
    ldr   r1, [r0]
    cmp   r1, #0
    bne   .Lreturn

    ldr   r4, =RECORDER_STATE
    ldr   r3, =REPORT_MAGIC
    str   r3, [r4, #REPORT_MAGIC_OFFSET]
    movs  r3, #1
    str   r3, [r4, #REPORT_STARTED_OFFSET]
    mrs   r3, cpsr
    str   r3, [r4, #REPORT_CPSR_OFFSET]
    movs  r3, #1
    str   r3, [r4, #REPORT_PHASE_OFFSET]
    /* The verified cave begins zeroed at boot. Preserve Apple's C6/96
     * function and install only the separate C6/98 function selected by qol. */
    adr   r1, scsi_stub
    ldr   r0, =SCSI_STUB_DST
    movs  r2, #(SCSI_STUB_SIZE / 4)
.Lc6_copy:
    ldmia r1!, {r3}
    stmia r0!, {r3}
    subs  r2, #1
    bne   .Lc6_copy
    dsb   sy
    ldr   r0, =SCSI_STUB_DST
    movs  r1, #SCSI_STUB_SIZE
    movs  r2, #7
    mov   r12, r2
    svc   #70
    movs  r3, #2
    str   r3, [r4, #REPORT_PHASE_OFFSET]

    ldr   r0, =HOME_ACTION_SLOT
    ldr   r1, [r0]
    ldr   r2, =STOCK_ACTION
    cmp   r1, r2
    bne   .Lowner_fail
    b     .Lclaim_install

.Lowner_fail:
    movs  r0, #3
    b     .Lrecord_failure
.Lclaim_won:
    ldr   r4, =RECORDER_STATE
    ldr   r0, =HOME_ACTION_SLOT
    ldr   r1, =(CAVE | 1)
    str   r1, [r0]
    dsb   sy
    movs  r1, #4
    movs  r2, #7
    mov   r12, r2
    svc   #70
    ldr   r0, =HOME_ACTION_SLOT
    ldr   r2, [r0]
    ldr   r1, =(CAVE | 1)
    cmp   r2, r1
    bne   .Lhook_fail
    movs  r3, #3
    str   r3, [r4, #REPORT_PHASE_OFFSET]
    adr   r0, cff_install_lock
    movs  r1, #2
    str   r1, [r0]
    dsb   sy
    b     .Lreturn
.Lhook_fail:
    movs  r0, #4
.Lrecord_failure:
    ldr   r2, =RECORDER_STATE
    str   r0, [r2, #REPORT_REASON_OFFSET]
    dsb   sy

.Lreturn:
.global cff_publish_return
cff_publish_return:
    ldr   r4, [sp, #8]
    ldr   r6, [sp, #12]
    ldr   r5, [sp, #0]
    ldr   r2, [sp, #4]
    add   sp, #16
    movs  r0, #0
    bx    r2

.ltorg
.balign 4
.global publisher_tables_end
publisher_tables_end:
.org 0x168
.global scsi_stub
.type scsi_stub,%object
scsi_stub:
    .incbin "scsi-stub.bin"
.global scsi_stub_end
scsi_stub_end:
.size scsi_stub, .-scsi_stub

.global cff_deferred_image_end
cff_deferred_image_end:
