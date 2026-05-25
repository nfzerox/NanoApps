/*
 * hb_audio.c — play WAV sound effects through the OS audio stack.
 *
 * Why it isn't a single call: the OS loader uses a large stack frame.
 * The USB-MSC task that SCSI exec runs in does not have enough stack,
 * so a direct call crashes/reboots. Workaround: spawn a pthread with
 * an explicit 64 KB stack, do the audio work there.
 *
 * Even with a separate pthread, the new task only gets CPU once our
 * caller relinquishes it. Calling hb_audio_play_wav and then
 * returning from the app entry works (the pthread runs after we
 * return). Staying in a busy UI loop after the call may starve the
 * pthread.
 *
 * Known resource WAV paths:
 *   Resources/Sounds/volumebeep.wav
 *   Resources/Sounds/shake.wav
 */

#include "hb_sdk.h"

#define SFX_CTOR_ADDR        (0x08417efcu | 1u)
#define SFX_LOADFILE_ADDR    (0x08417f78u | 1u)
#define SFX_PLAYER_INST_ADDR (0x08417eb8u | 1u)
#define SFX_PLAYER_PLAY_ADDR (0x0841828cu | 1u)
#define PTHREAD_CREATE_ADDR  (0x080226f8u | 1u)

#define SFX_VOL_RESOURCE_IMAGE 4

#define SFX_OFF_VOLUME     0x24
#define SFX_OFF_PLAYMODE   0x51
#define SFX_OFF_FLAGS      0x52
#define SFX_OFF_NEXTSFX    0x54
#define SFX_MODE_PLAY      1
#define SFX_FLAG_NONE      0

#define PTHREAD_ATTR_MAGIC 0x50544841u
#define PTHREAD_STACK_64K  0x10000u

typedef void *(*sfx_ctor_t)(void *this);
typedef int   (*sfx_loadfile_t)(void *this, const char *filename,
                                int volume, uint32_t offset, uint32_t size);
typedef void *(*sfx_player_instance_t)(void);
typedef void  (*sfx_player_play_t)(void *player, void *desc,
                                   void *callback, void *cbdata);
typedef int   (*pthread_create_t)(uint32_t *thread, void *attr,
                                  void *(*start)(void *), void *arg);

/* Per-call state — static because the pthread must outlive the
   spawn site, and the descriptor must outlive playback. Single-shot
   only; a concurrent call clobbers the in-flight sound. */
typedef struct {
    char     path[96];
    int      volume_id;
    uint32_t play_volume;
    uint8_t  desc[0x78];
} audio_job_t;

static audio_job_t g_job;
static uint32_t    g_attr[16];

static void *audio_worker(void *arg)
{
    audio_job_t *job = (audio_job_t *)arg;
    ((sfx_ctor_t)SFX_CTOR_ADDR)(job->desc);
    int rc = ((sfx_loadfile_t)SFX_LOADFILE_ADDR)(
        job->desc, job->path, job->volume_id, 0, 0);
    if (rc != 0) return (void *)(uintptr_t)rc;

    *(volatile uint32_t *)(job->desc + SFX_OFF_VOLUME) = job->play_volume;
    job->desc[SFX_OFF_PLAYMODE] = SFX_MODE_PLAY;
    job->desc[SFX_OFF_FLAGS]    = SFX_FLAG_NONE;
    *(volatile void **)(job->desc + SFX_OFF_NEXTSFX) = (void *)0;

    void *player = ((sfx_player_instance_t)SFX_PLAYER_INST_ADDR)();
    if (player) {
        ((sfx_player_play_t)SFX_PLAYER_PLAY_ADDR)(player, job->desc,
                                                  (void *)0, (void *)0);
    }
    return (void *)0;
}

static void copy_path(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Worker: spawn audio worker with the volume_id chosen by caller.
   Uses pthread priority 50 (normalized, range -62..63) so the worker
   actually preempts our USB-MSC caller task — otherwise audio only
   plays after the caller returns from its app entry. */
static bool play_wav_at_volume(const char *path, int volume_id,
                               uint32_t play_volume)
{
    copy_path(g_job.path, path, (int)sizeof(g_job.path));
    g_job.volume_id   = volume_id;
    g_job.play_volume = play_volume;

    for (int i = 0; i < 16; i++) g_attr[i] = 0;
    g_attr[0] = PTHREAD_ATTR_MAGIC;
    g_attr[2] = 2;                /* DETACHED        */
    g_attr[4] = PTHREAD_STACK_64K;
    g_attr[6] = 1;                /* SCHED_FIFO      */
    g_attr[7] = 1;                /* INHERIT_SCHED   */
    g_attr[8] = 1;                /* SCOPE_SYSTEM    */
    g_attr[9] = 0;                /* default priority (sized so caller controls) */

    uint32_t tid = 0;
    int rc = ((pthread_create_t)PTHREAD_CREATE_ADDR)(
        &tid, g_attr, audio_worker, &g_job);
    return rc == 0;
}

bool hb_audio_play_wav(const char *path, uint32_t volume_0_to_7fff)
{
    return play_wav_at_volume(path, SFX_VOL_RESOURCE_IMAGE, volume_0_to_7fff);
}

/* For sounds on the main filesystem, e.g. files you've written there, like
   /WAV/click.wav. Path should start with '/'. */
bool hb_audio_play_wav_main(const char *path, uint32_t volume_0_to_7fff)
{
    return play_wav_at_volume(path, 0, volume_0_to_7fff);
}

/* FIXME: The OS audio subsystem somehow requires MIPI bus activity between
   each of these calls or the device panics and reboots. Needs investigation.
   Empirical investigation results:
     - Pure CPU delay (svc_yield x 30 ~30ms):  FAILS
     - 1x1 hb_fill_rect (~6 MIPI register writes): unreliable
     - 240x4 hb_fill_rect (~960 px MIPI bursts): FAILS
     - scale-3 hb_draw_str(10+ char string): WORKS reliably
   So the trigger isn't time, isn't simple MIPI activity, but
   something specific about scale-3 text rendering (lots of
   per-strip MIPI FIFO polls? Some OS dispatch event when text is
   drawn?). Hypothesis: the audio task only wakes on certain scheduler
   events; need to verify with traces or scope.
   For now: callers MUST draw a scale-3 hb_draw_str between steps.

   Audio playback step pointers — expose to callers so they can
   interleave their own MIPI activity (e.g. drawing a status label)
   between each phase.

   The simplest "it just works" recipe for an app:

       hb_audio_ctor(desc);
       hb_draw_str(...,"step1",...);         // MIPI activity
       hb_audio_loadfile(desc, path, vol);
       hb_draw_str(...,"step2",...);
       hb_audio_setfields(desc, vol);
       hb_draw_str(...,"step3",...);
       hb_audio_play_now(desc);

   Don't try to skip the draws between calls — it WILL reboot. */
void hb_audio_ctor(void *desc)
{
    ((sfx_ctor_t)SFX_CTOR_ADDR)(desc);
}

int hb_audio_loadfile(void *desc, const char *path, int volume_id)
{
    return ((sfx_loadfile_t)SFX_LOADFILE_ADDR)(desc, path, volume_id, 0, 0);
}

void hb_audio_setfields(void *desc, uint32_t volume_0_to_7fff)
{
    *(volatile uint32_t *)((uint8_t *)desc + SFX_OFF_VOLUME) = volume_0_to_7fff;
    ((uint8_t *)desc)[SFX_OFF_PLAYMODE] = SFX_MODE_PLAY;
    ((uint8_t *)desc)[SFX_OFF_FLAGS]    = SFX_FLAG_NONE;
    *(volatile void **)((uint8_t *)desc + SFX_OFF_NEXTSFX) = (void *)0;
}

bool hb_audio_play_now(void *desc)
{
    void *player = ((sfx_player_instance_t)SFX_PLAYER_INST_ADDR)();
    if (!player) return false;
    ((sfx_player_play_t)SFX_PLAYER_PLAY_ADDR)(player, desc,
                                              (void *)0, (void *)0);
    return true;
}
