.syntax unified
.thumb

/* Entry of the relocatable heap bootstrap.  The CFF stack stage tail-branches
 * here with the firmware continuation in r8 and its original r4-r8/lr saved
 * in an aligned six-word frame.  Finish directly from heap code: the allocator
 * and this C call may overwrite the reclaimed stack-stage instructions. */
.section .text.entry,"ax",%progbits
.balign 4
.global payload_entry
.type payload_entry,%function
.thumb_func
payload_entry:
    bl untether_bootstrap_main
    mov r12, r8
    pop {r4-r8, lr}
    bx r12
.size payload_entry, .-payload_entry
