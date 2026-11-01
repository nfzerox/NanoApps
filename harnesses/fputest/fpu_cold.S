.syntax unified
.text
.arm
.fpu vfpv3-d16
.align 2

.global fputest_cold_arm_vfp3
.type fputest_cold_arm_vfp3, %function
fputest_cold_arm_vfp3:
    vmov.f32 s0, #1.0
    vmov r0, s0
    bx lr
.size fputest_cold_arm_vfp3, .-fputest_cold_arm_vfp3

.thumb
.thumb_func
.align 2
.fpu vfp
.global fputest_cold_thumb_vfp2
.type fputest_cold_thumb_vfp2, %function
fputest_cold_thumb_vfp2:
    vmov s0, r0
    vmov r0, s0
    bx lr
.size fputest_cold_thumb_vfp2, .-fputest_cold_thumb_vfp2
