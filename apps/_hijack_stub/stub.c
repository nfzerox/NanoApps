/*
 * stub.c — hijack landing stub.
 *
 * Lives at 0x09130000. Patched call to launch Podcasts app calls this
 * stub (BL); the stub spawns a worker task which loads the Homebrew
 * launcher from disk into the standard app VA and BLX's it. The stub
 * itself returns quickly so the SpringBoard/Podcasts UI task is not
 * stuck running the whole launcher.
 *
 * Returns to its caller ONLY if the load fails — in that case the OS 
 * just silently does nothing (no Podcasts UI shown), which is acceptable 
 * for an opt-in hijack.
 */

#include "hb_sdk.h"

#define HOMEBREW_PATH  "/Apps/Homebrew.app/Homebrew"
#define HIJACK_STUB_VA 0x09130000u
#define PTHREAD_CREATE_ADDR  (0x080226f8u | 1u)
#define PTHREAD_ATTR_MAGIC   0x50544841u
#define PTHREAD_STACK_64K    0x10000u

typedef int (*pthread_create_t)(uint32_t *thread, void *attr,
                                void *(*start)(void *), void *arg);

static uint32_t g_attr[16];

__attribute__((section(".text.task_entry"), used, noinline))
static void *launcher_worker(void *arg)
{
    (void)arg;
    hb_trace_init();
    hb_trace_log("HJ_WORK", HIJACK_STUB_VA, 0);

    /* This worker runs from the hijack VA, which must stay separate
       from /Apps/.load_stub's trampoline VA (0x09100000). The launcher
       may later load that trampoline while this frame is still waiting
       for hb_app_load_direct() to return. */
    hb_app_load_direct(HOMEBREW_PATH);
    hb_trace_log("HJ_FAIL", 0, 0);
    return (void *)0;
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_trace_log("HJ_STUB", HIJACK_STUB_VA, 0);

    for (int i = 0; i < 16; i++) g_attr[i] = 0;
    g_attr[0] = PTHREAD_ATTR_MAGIC;
    g_attr[2] = 2;                 /* DETACHED */
    g_attr[4] = PTHREAD_STACK_64K;
    g_attr[6] = 1;                 /* SCHED_FIFO */
    g_attr[7] = 1;                 /* INHERIT_SCHED */
    g_attr[8] = 1;                 /* SCOPE_SYSTEM */
    g_attr[9] = 0;                 /* default priority */

    uint32_t tid = 0;
    int rc = ((pthread_create_t)PTHREAD_CREATE_ADDR)(
        &tid, g_attr, launcher_worker, (void *)0);
    hb_trace_log("HJ_SPAWN", (uint32_t)rc, tid);

    /* If load failed, fall through and return to the OS. The
       hijack call site does `pop {r4,pc}` so we go back up the
       call chain cleanly. */
}
