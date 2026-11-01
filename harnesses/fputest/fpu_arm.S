.syntax unified
.text
.arm
.fpu vfpv3-d16
.align 2
.global fputest_arm_vfp_loop
.type fputest_arm_vfp_loop, %function
fputest_arm_vfp_loop:
    /* Load constants through core registers: unlike vmov.f32 #imm this is
     * available on the oldest VFP implementations relevant here. */
    movw r1, #0x0000
    movt r1, #0x3f80
    vmov s0, r1
    movw r1, #0x0000
    movt r1, #0x3f00
    vmov s1, r1
    eor r1, r1, r1
    vmov s2, r1
    lsr r0, r0, #3
1:
    .rept 8
    vmla.f32 s2, s0, s1
    .endr
    subs r0, r0, #1
    bne 1b
    vcvt.u32.f32 s2, s2
    vmov r0, s2
    bx lr
.size fputest_arm_vfp_loop, .-fputest_arm_vfp_loop

.global fputest_arm_neon_loop
.type fputest_arm_neon_loop, %function
.fpu neon
fputest_arm_neon_loop:
    movw r1, #0x0000
    movt r1, #0x3f80
    vmov d0, r1, r1
    vmov d1, r1, r1
    movw r1, #0x0000
    movt r1, #0x3f00
    vmov d2, r1, r1
    vmov d3, r1, r1
    veor q2, q2, q2
    lsr r0, r0, #3
2:
    vmla.f32 q2, q0, q1
    vmla.f32 q2, q0, q1
    subs r0, r0, #1
    bne 2b
    vcvt.u32.f32 q2, q2
    vmov.32 r0, d4[0]
    lsl r0, r0, #2
    bx lr
.size fputest_arm_neon_loop, .-fputest_arm_neon_loop

/* AAPCS-valid call-boundary probe.
 *
 * r0 = uint32_t (*fn)(void *)
 * r1 = opaque argument
 * r2 = uint32_t fpscr_pair[2]
 *
 * Preserve the caller's d8-d15, plant sentinels, call fn, check the sentinels,
 * then restore the original d8-d15 before returning. The result is 1 when the
 * tested call preserved every callee-saved VFP register, otherwise 0.
 */
.global fputest_arm_probe_call
.type fputest_arm_probe_call, %function
.fpu vfpv3-d16
fputest_arm_probe_call:
    push {r4-r8, lr}
    vpush {d8-d15}
    mov r4, r0
    mov r5, r1
    mov r6, r2
    vmrs r7, fpscr
    str r7, [r6]

    ldr r0, =0x13579bdf
    ldr r1, =0x2468ace0
    ldr r2, =0x0badf00d
    ldr r3, =0xc001d00d
    vmov d8,  r0, r1
    vmov d9,  r2, r3
    vmov d10, r0, r2
    vmov d11, r1, r3
    vmov d12, r0, r1
    vmov d13, r2, r3
    vmov d14, r0, r2
    vmov d15, r1, r3

    mov r0, r5
    blx r4
    vmrs r7, fpscr
    str r7, [r6, #4]
    mov r8, #1

    vmov r0, r1, d8
    ldr r2, =0x13579bdf
    cmp r0, r2
    bne 9f
    ldr r2, =0x2468ace0
    cmp r1, r2
    bne 9f
    vmov r0, r1, d9
    ldr r2, =0x0badf00d
    cmp r0, r2
    bne 9f
    ldr r2, =0xc001d00d
    cmp r1, r2
    bne 9f
    vmov r0, r1, d10
    ldr r2, =0x13579bdf
    cmp r0, r2
    bne 9f
    ldr r2, =0x0badf00d
    cmp r1, r2
    bne 9f
    vmov r0, r1, d11
    ldr r2, =0x2468ace0
    cmp r0, r2
    bne 9f
    ldr r2, =0xc001d00d
    cmp r1, r2
    bne 9f
    vmov r0, r1, d12
    ldr r2, =0x13579bdf
    cmp r0, r2
    bne 9f
    ldr r2, =0x2468ace0
    cmp r1, r2
    bne 9f
    vmov r0, r1, d13
    ldr r2, =0x0badf00d
    cmp r0, r2
    bne 9f
    ldr r2, =0xc001d00d
    cmp r1, r2
    bne 9f
    vmov r0, r1, d14
    ldr r2, =0x13579bdf
    cmp r0, r2
    bne 9f
    ldr r2, =0x0badf00d
    cmp r1, r2
    bne 9f
    vmov r0, r1, d15
    ldr r2, =0x2468ace0
    cmp r0, r2
    bne 9f
    ldr r2, =0xc001d00d
    cmp r1, r2
    bne 9f
    b 10f
9:
    mov r8, #0
10:
    mov r0, r8
    vpop {d8-d15}
    pop {r4-r8, pc}
.size fputest_arm_probe_call, .-fputest_arm_probe_call
.ltorg

.fpu vfpv3-d16
.global fputest_arm_vfp3_smoke
.type fputest_arm_vfp3_smoke, %function
fputest_arm_vfp3_smoke:
    vmov.f32 s0, #1.0
    vmov r0, s0
    bx lr
.size fputest_arm_vfp3_smoke, .-fputest_arm_vfp3_smoke
