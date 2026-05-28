/*
 * load_stub.c — parametric app loader at fixed RAM address.
 *
 * Linked at 0x09100000. Reads its target path from 0x09100400.
 * Spawns a detached worker which performs the read+jump to the
 * standard app load VA (0x0867f8e4).
 *
 * Because we run from 0x09100000, NOT 0x0867f8e4, our code survives
 * the write into 0x0867f8e4. The original caller (launcher) was
 * crashing because it tried to overwrite its OWN .text while still
 * executing — this stub solves that.
 *
 * Used by hb_app_load_and_exec via the trampoline pattern:
 *   launcher loads /Apps/.load_stub into 0x09100000,
 *   writes target path to 0x09100400,
 *   BLX 0x09100001.
 *
 * Important: this stub must not synchronously call the target app and
 * then return through the launcher. The target app overwrites the
 * launcher's text at 0x0867f8e4, so that return path is dead. Instead
 * the stub starts a worker, returns to the launcher, and the launcher
 * exits immediately. When the launched app later returns, the worker
 * exits. The filesystem cache lives outside this stub image, so the
 * worker no longer corrupts itself during app loads.
 */

#include "hb_sdk.h"

#define APP_LOAD_VA   0x0867f8e4u
#define PATH_ADDR     0x09110000u   /* safe scratch, OUTSIDE stub binary */
#define PTHREAD_CREATE_ADDR  (0x080226f8u | 1u)
#define PTHREAD_ATTR_MAGIC   0x50544841u
#define PTHREAD_STACK_64K    0x10000u

typedef void (*entry_t)(void);
typedef int (*pthread_create_t)(uint32_t *thread, void *attr,
                                void *(*start)(void *), void *arg);

static uint32_t g_attr[16];

__attribute__((section(".text.task_entry"), used, noinline))
static void *app_worker(void *arg)
{
    (void)arg;
    hb_trace_log("STB_WORK", PATH_ADDR, 0);

    if (hb_app_load_direct((const char *)PATH_ADDR)) {
        hb_trace_log("APP_RET", 0, 0);
    } else {
        hb_trace_log("APP_FAIL", 0, 0);
    }

    hb_trace_log("APP_DONE", 0, 0);
    return (void *)0;
}

HB_APP_ENTRY(payload_entry)
{
    /* Diagnostic: log a known-magic value FIRST so we can verify the
       BLX 0x09100001 actually entered the stub correctly. */
    hb_trace_log("STBHERE", 0xCAFE1234, 0xC0DEFACE);
    hb_trace_log("STB_ENT", PATH_ADDR,   0);

    /* Delegate to hb_app_load_direct (which lives in OUR text at
       0x09100xxx). It does the unsafe overwrite-and-jump into
       0x0867f8e4 — safe because we're at 0x09100000, not there. */
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
        &tid, g_attr, app_worker, (void *)0);
    hb_trace_log("STB_SPAW", (uint32_t)rc, tid);
}
