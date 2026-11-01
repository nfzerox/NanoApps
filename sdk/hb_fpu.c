/*
 * hb_fpu.c -- opt the current retailos task into Apple's lazy VFP switching.
 *
 * Firmware: iPod nano 7G retailos 1.1.2 (39A10023) only.
 *
 * The undefined-instruction handler at 0x0862d294 already implements lazy VFP
 * ownership. It panics when current_tcb->fp_context (+0x30) is NULL. Native
 * FP-capable tasks receive the context below during task creation; NanoApps may
 * enter on a UI task which already has one, so reuse it when present and attach
 * a native-layout context only when absent. The firmware remains responsible
 * for the actual enable/save/restore work.
 *
 * Do not replace this with a direct FPEXC write.  Apps run in User mode, and
 * leaving FPEXC enabled without joining the scheduler's ownership protocol
 * would allow another task to silently corrupt the FP registers.
 */
#include "hb_sdk.h"
#include "hb_heap.h"

#define HB_CURRENT_TCB_PTR_ADDR  0x0890b950u
#define HB_FP_OWNER_PTR_ADDR      0x0890b97cu
#define HB_TCB_FP_CONTEXT_OFF    0x30u
#define HB_TCB_FP_ENABLED_OFF    0x34u

#define HB_FP_CONTEXT_WORDS      37u /* 5 control words + d0-d15 (32 words) */
#define HB_FPEXC_EN              0x40000000u
#define HB_FPSCR_NATIVE_DEFAULT  0x03000f00u

static uint32_t read_cpsr(void)
{
    uint32_t value;
    __asm__ volatile("mrs %0, cpsr" : "=r"(value));
    return value;
}

uint32_t hb_fpu_owner_task(void)
{
    return *(volatile uint32_t *)HB_FP_OWNER_PTR_ADDR;
}

int hb_fpu_attach_current_task(hb_fpu_status_t *status)
{
    volatile uint8_t *task;
    uint32_t *context;
    uint32_t i;
    int result = HB_FPU_ATTACH_EXISTING;

    if (status) {
        status->cpsr = read_cpsr();
        status->task = 0;
        status->previous_context = 0;
        status->context = 0;
        status->previous_enabled = 0;
        status->enabled = 0;
        status->owner_before = hb_fpu_owner_task();
    }

    task = *(volatile uint8_t * volatile *)HB_CURRENT_TCB_PTR_ADDR;
    if (!task)
        return HB_FPU_ATTACH_NO_TASK;

    context = *(uint32_t * volatile *)(task + HB_TCB_FP_CONTEXT_OFF);
    if (status) {
        status->task = (uint32_t)(uintptr_t)task;
        status->previous_context = (uint32_t)(uintptr_t)context;
        status->previous_enabled = *(volatile uint8_t *)(task + HB_TCB_FP_ENABLED_OFF);
    }

    if (!context) {
        context = (uint32_t *)hb_os_alloc(HB_FP_CONTEXT_WORDS * sizeof(uint32_t));
        if (!context)
            return HB_FPU_ATTACH_NO_MEMORY;
        if (((uintptr_t)context & 7u) != 0u) {
            hb_os_free(context);
            return HB_FPU_ATTACH_BAD_ALIGNMENT;
        }

        for (i = 0; i < HB_FP_CONTEXT_WORDS; i++)
            context[i] = 0;
        context[1] = HB_FPEXC_EN;
        context[2] = HB_FPSCR_NATIVE_DEFAULT;

        /* Publish a fully initialized context before advertising it to the
         * lazy undefined-instruction handler. */
        __asm__ volatile("dmb sy" ::: "memory");
        *(uint32_t * volatile *)(task + HB_TCB_FP_CONTEXT_OFF) = context;
        __asm__ volatile("dmb sy" ::: "memory");
        result = HB_FPU_ATTACH_NEW;
    }

    /* The native lazy handler passes this byte to its FPEXC.EN helper. */
    *(volatile uint8_t *)(task + HB_TCB_FP_ENABLED_OFF) = 1;
    __asm__ volatile("dmb sy" ::: "memory");

    if (status) {
        status->context = (uint32_t)(uintptr_t)context;
        status->enabled = *(volatile uint8_t *)(task + HB_TCB_FP_ENABLED_OFF);
    }
    return result;
}
