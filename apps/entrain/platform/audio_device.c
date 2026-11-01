/*
 * audio_device.c — audio.h on the iPod under RetailOS.
 *
 * Generates audio a block at a time straight into RAM, and hands the blocks to
 * the OS as a CHAIN, so the sound hardware runs them together and there is no
 * join left for us to get wrong.
 *
 * The chain is the whole design, and it comes from RetailOS 1.1.2 itself. When
 * a voice finishes a buffer, its completion handler does this before any
 * teardown:
 *
 *     8631580: ldr r0, [r4, #0x78]   ; r0 = voice->descriptor
 *     8631584: ldr r1, [r0, #0x54]   ; r1 = descriptor+0x54
 *     8631588: cmp r1, #0
 *     863158c: beq <tear down>
 *     8631590: mov r0, r4
 *     8631594: bl  0x862fd24         ; voice::setSource(voice, next)
 *     863159c: bl  0x8630154         ; voice::start(voice)
 *
 * descriptor+0x54 is a NEXT pointer. A voice reaching the end of one buffer
 * re-arms itself from it immediately, inside its own completion handler, on the
 * audio task. That is a sample-exact join: not a short gap, not a small step —
 * the same continuous stream, concatenated by the hardware.
 *
 * Everything else follows from that:
 *
 *   - voice::setSource (VA 0x0862fd24) writes descriptor+0x64 = 1 when a voice
 *     takes a descriptor up, and the completion handler writes it back to 0. So
 *     each block carries its own "finished" flag, and which buffers are free to
 *     render into stops being a timing estimate and becomes a fact.
 *
 *   - setSource also applies rate, channels, bit depth and volume from the
 *     descriptor (VAs 0x8631610, 0x86306c8, 0x8630440, 0x86304a4), so a chained
 *     descriptor is set up as completely as one passed to play(). play() is
 *     only needed to get the first voice going.
 *
 *   - The UI tick no longer has to be punctual, only to keep up. It renders
 *     ahead and links blocks onto the tail; the join itself is the audio task's
 *     business. Nothing here spins on the clock any more.
 *
 * The two versions before this one tried to hide the join with a crossfade, and
 * both sounded wrong. Overlapping consecutive buffers does not blend two
 * sounds: consecutive buffers are consecutive samples of ONE signal, so an
 * overlap sums that signal with a time-shifted copy of itself, which is a comb
 * filter whose notches sweep as the timing drifts. The host tests measure it —
 * half a carrier period of misalignment, 2.5 ms, cancels the carrier outright.
 * No crossfade length fixes that, because the crossfade IS the defect.
 *
 * One field has to be right for any of this to work, and it is not obvious.
 * descriptor+0x34 feeds the voice's counter at voice+0x48:
 *
 *     862fd54: ldr r0, [r4, #0x34]    ; descriptor+0x34
 *     862fd58: ldr r6, [r5, #0x0C]    ; the voice's rate
 *     862fd5c: mul r0, r0, r6
 *     862fd60: bl  0x842d93c          ; / 1000
 *     862fd64: str r0, [r5, #0x48]
 *
 * The constructor leaves it zero and voice::start never touches it, so the
 * counter was zero. The exhaustion path then computes the shortfall to carry
 * into the next buffer as `samples_wanted - 0` — the whole mixer pass rather
 * than the true remainder — and the mixer re-enters at 0x86312e0 with that as
 * its target, emitting a pass worth of samples too many at every transition. A
 * few milliseconds out, once per block: a faint click.
 *
 * The counter has to hold the block's frame count. Getting there needs one more
 * fact, and assuming it wrongly is worth a block of silence: voice+0x0C is the
 * mixer's OUTPUT rate, not the descriptor's. VA 0x8631610 gives it away, since
 * it compares voice+0x0C against descriptor+0x14 to decide whether any
 * resampling is needed at all. So a block stated in its own milliseconds
 * converts to twice its length on a 44100 mixer, and the voice counts on for a
 * whole block after the PCM has run out — which is audible as exactly that: a
 * block of sound, a block of silence.
 *
 * So the duration is computed backwards from the sample count instead,
 * ms = frames * 1000 / output_rate, against the rate read off the live voice.
 * The value is only used when it converts back to exactly the frame count;
 * anything else falls back to zero, which is the behaviour that played
 * correctly all along, just with the mis-accounting above. Block lengths are
 * stated in frames and chosen to divide cleanly at any plausible mixer rate.
 *
 * Pause and stop do not wait for a block boundary. The voice reads our buffer
 * live, which is what makes the completion flag meaningful, and it cuts both
 * ways: they write a short fade to zero into the part of the sounding block the
 * voice has not reached yet, and silence the rest. A silent block is then
 * chained to ITSELF, so the voice loops silence for nothing while paused, the
 * DAC stays up, and resuming is a matter of pointing that block's next at real
 * audio again.
 *
 * Ownership: buffers come from hb_os_alloc and are never handed to loadFile, so
 * the OS never frees them. Descriptors live in this file for the life of the
 * app and are rebuilt before each handover, and the constructor zeroes the
 * buffer pointers without freeing, so there is no path to a double free.
 */

#include "audio.h"
#include "sys.h"
#include "../core/render.h"

#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_shared.h"
#include <stddef.h>

#define SFX_CTOR_ADDR        (0x08417efcu | 1u)
#define SFX_PLAYER_INST_ADDR (0x08417eb8u | 1u)
#define SFX_PLAYER_PLAY_ADDR (0x0841828cu | 1u)

/* Descriptor layout, from SoundEffectDescriptor::ctor (VA 0x08417efc),
   voice::setSource (VA 0x0862fd24) and the completion handler (VA 0x08631580).
   The last two are the ones this file leans on. */
#define SFX_OFF_BUF_A    0x04
#define SFX_OFF_BUF_B    0x08
#define SFX_OFF_BUF_LEN  0x0C
#define SFX_OFF_TYPE     0x10   /* 0 = linear PCM */
#define SFX_OFF_RATE     0x14
#define SFX_OFF_CHANNELS 0x18
#define SFX_OFF_BITS     0x1C
#define SFX_OFF_VOLUME   0x24
#define SFX_OFF_DURATION 0x34   /* MILLISECONDS -> voice+0x48 sample count */
#define SFX_OFF_TRIM_LO  0x38
#define SFX_OFF_TRIM_HI  0x3C
#define SFX_OFF_VOICE    0x48   /* setSource writes the voice here */
#define SFX_OFF_PLAYMODE 0x51
#define SFX_OFF_FLAGS    0x52
#define SFX_OFF_NEXT     0x54   /* the chain: followed at end of buffer */
#define SFX_OFF_PLAYING  0x64   /* byte: 1 while a voice holds it, 0 when done */

#define SFX_TYPE_LPCM 0

/* 22050 Hz: proven by harness T9, and half the render cost and half the memory
   of 44.1k for carriers that never go above a few hundred hertz. */
#define DEVICE_RATE 22050u

/* Block length, in FRAMES rather than milliseconds, because frames are what
   has to come out exact.
 *
 * 28224 divides cleanly at every mixer rate worth worrying about: it is 64x441,
 * so 22050 and 44100 both give whole milliseconds, and 588x48, so 48000 does
 * too. (The previous 26460 was a multiple of 441 but not of 48, so it would
 * have been inexact on a 48k mixer.) At 22050 it is 1.28 seconds.
 *
 * With the chain doing the joining, the length is no longer an audio quality
 * knob at all — it is the app's worst-case response time to a retune or a
 * preset change, and how much rendering has to fit between one block being
 * linked and the voice reaching it. */
#define BLOCK_FRAMES 28224u

/* Blocks in the ring. Two would be enough never to leave the chain empty — one
   sounding, one queued behind it — and the third is slack for a frame that runs
   long. */
#define SLOTS 3

/* How much audio one tick will render. A block is over a second of lead time
   but only tens of milliseconds of work, so the work wants spreading rather
   than hurrying: rendering a whole block in one callback hitched the UI every
   time, which is the visible rendering stage this design exists to avoid. */
#define CHUNK_MS 250

/* How far ahead of the estimated play cursor an in-place write has to stay. The
   cursor comes from the wall clock, so it is only good to a frame period or so;
   this is the margin for that plus anything the DAC has prefetched. */
#define SAFETY_MS 250

/* Fade to and from silence for pause, resume and stop. Long enough not to
   click, short enough to feel immediate. */
#define FADE_MS 80

/* The block chained to itself while paused, purely to keep the voice chain and
   the DAC alive so resuming is a ramp and not a cold start. 7056 = 16x441 =
   147x48, exact for the same rates as BLOCK_FRAMES; 320 ms at 22050. */
#define QUIET_FRAMES 7056u

#define MS_FRAMES(ms) ((DEVICE_RATE * (uint32_t)(ms)) / 1000u)

#define SAFETY_FRAMES MS_FRAMES(SAFETY_MS)
#define FADE_FRAMES   MS_FRAMES(FADE_MS)
#define CHUNK_FRAMES  MS_FRAMES(CHUNK_MS)
#define BLOCK_MS      ((BLOCK_FRAMES * 1000u) / DEVICE_RATE)

/* Nothing has run us for this long and the chain has certainly run dry: the OS
   put another app in front. Whatever was queued finished long ago and unheard,
   so start clean rather than carrying on from stale state. */
#define STARVED_MS (BLOCK_MS * SLOTS * 3)

typedef void *(*sfx_ctor_t)(void *self);
typedef void *(*sfx_player_inst_t)(void);
typedef void  (*sfx_player_play_t)(void *player, void *desc,
                                   void *cb, void *cbdata);

/* Slot states. */
#define SLOT_FREE   0       /* nothing in it; may be rendered into */
#define SLOT_BUILD  1       /* part-rendered */
#define SLOT_READY  2       /* rendered, not yet handed to the OS */
#define SLOT_QUEUED 3       /* handed over: sounding, or waiting in the chain */

typedef struct {
    uint8_t  desc[0x80];
    int16_t *pcm;
    uint32_t frames;        /* always a multiple of 441 — see FRAMES_MS */
    uint32_t fill;          /* how much is rendered so far */
    uint8_t  state;
    bool     seen_playing;  /* the OS has had it: its flag was seen set */
} slot_t;

static slot_t g_slot[SLOTS];
static slot_t g_quiet;              /* self-chained silence, for pause */

/* The queue is short and strictly FIFO — the chain plays blocks in the order
   they were linked — so it is an array and a count, not a ring. */
static uint8_t g_q[SLOTS];
static uint8_t g_qn;

static uint32_t g_head_start_ms;    /* when g_q[0] was first seen sounding */
static uint32_t g_last_tick_ms;
static uint64_t g_frames_done;      /* frames of blocks that have FINISHED */

static en_audio_pull_fn s_pull;
static void            *s_pull_ctx;
static bool             s_source_done;

static bool g_paused;
static bool g_stopping;
static uint32_t g_out_rate;         /* the mixer's rate, read off a live voice */
static bool     g_duration_bad;     /* the OS disagreed; stop stating one */
static uint8_t  g_overrun;          /* consecutive blocks that took too long */
static bool g_quiet_armed;          /* the silence block is looping */
static bool g_fade_in;              /* next block handed over ramps up */
static int  g_volume = 45;
static en_audio_state_t g_state = EN_AUDIO_IDLE;

/* ---- descriptors --------------------------------------------------------- */

static uint32_t os_volume(void)
{
    int v = g_volume < 0 ? 0 : (g_volume > 100 ? 100 : g_volume);
    return (0x7fffu * (uint32_t)v) / 100u;
}

/* What to put in descriptor+0x34 so that voice+0x48 comes out holding exactly
 * `frames`.
 *
 * setSource computes ms * output_rate / 1000, so this inverts that — and only
 * answers when the inversion is exact. Zero is the honest fallback: it is what
 * the constructor leaves and what played correctly before any of this, just
 * with a pass worth of mis-accounting at each transition. A wrong non-zero
 * value is far worse, because the voice keeps counting after the PCM has run
 * out and pads the difference with silence. */
static uint32_t duration_ms(uint32_t frames)
{
    if (g_duration_bad || !g_out_rate || !frames) return 0;

    uint32_t num = frames * 1000u;          /* 28224000 — no overflow */
    if (num % g_out_rate) return 0;         /* would not round-trip */

    uint32_t ms = num / g_out_rate;
    if ((ms * g_out_rate) / 1000u != frames) return 0;   /* belt and braces */
    return ms;
}

/* Take the mixer's rate off the voice the OS actually handed us, rather than
   assuming it. setSource leaves the voice at descriptor+0x48, and the voice
   keeps the output rate at +0x0C — 0x8631610 compares the two to decide whether
   to resample, which is what makes it the output rate and not ours. */
static void learn_rate(const slot_t *s)
{
    if (g_out_rate) return;

    uint32_t v = *(volatile const uint32_t *)(s->desc + SFX_OFF_VOICE);
    if (!v || (v & 3u)) return;

    uint32_t r = *(volatile const uint32_t *)(uintptr_t)(v + 0x0Cu);
    if (r >= 8000u && r <= 192000u) g_out_rate = r;
}

/* Set a descriptor up to describe a slot's buffer. Everything the OS needs is
   here: setSource applies the format and the volume itself, so a descriptor
   reached through the chain is as complete as one passed to play(). */
static void desc_build(slot_t *s)
{
    uint8_t *d = s->desc;
    ((sfx_ctor_t)SFX_CTOR_ADDR)(d);

    *(volatile uint32_t *)(d + SFX_OFF_BUF_A)    = (uint32_t)(uintptr_t)s->pcm;
    *(volatile uint32_t *)(d + SFX_OFF_BUF_B)    = (uint32_t)(uintptr_t)s->pcm;
    *(volatile uint32_t *)(d + SFX_OFF_BUF_LEN)  = s->frames * 4u;
    d[SFX_OFF_TYPE] = SFX_TYPE_LPCM;
    *(volatile uint32_t *)(d + SFX_OFF_RATE)     = DEVICE_RATE;
    *(volatile uint32_t *)(d + SFX_OFF_CHANNELS) = 2;
    *(volatile uint32_t *)(d + SFX_OFF_BITS)     = 16;
    *(volatile uint32_t *)(d + SFX_OFF_TRIM_LO)  = 0;
    *(volatile uint32_t *)(d + SFX_OFF_TRIM_HI)  = 0;
    /* The one the constructor leaves at zero. setSource turns it into the
       voice's remaining-sample count, and the chain transition subtracts the
       mixer pass from it to work out how much of that pass the NEXT buffer
       owes. Zero means every transition over-produces by a pass; too large
       means the voice pads with silence once the PCM runs out. */
    *(volatile uint32_t *)(d + SFX_OFF_DURATION) = duration_ms(s->frames);
    *(volatile uint32_t *)(d + SFX_OFF_VOLUME)   = os_volume();
    d[SFX_OFF_PLAYMODE] = 1;
    d[SFX_OFF_FLAGS] = 0;
    *(volatile uint32_t *)(d + SFX_OFF_NEXT) = 0;
    d[SFX_OFF_PLAYING] = 0;
}

/* The OS sets this when a voice takes the descriptor up and clears it when the
   buffer has been played out. Volatile because the writer is the audio task. */
static bool desc_playing(const slot_t *s)
{
    return *(volatile const uint8_t *)(s->desc + SFX_OFF_PLAYING) != 0;
}

/* Link `next` onto the end of `s`. A single aligned 32-bit store, so the audio
   task reading it at completion sees either the old value or the new one and
   never a torn pointer — which is what makes it safe to do this from the tick
   with no lock. Worst case the store lands just too late, the voice tears down,
   and the tick notices an empty chain and restarts. */
static void desc_chain(slot_t *s, slot_t *next)
{
    *(volatile uint32_t *)(s->desc + SFX_OFF_NEXT) =
        next ? (uint32_t)(uintptr_t)next->desc : 0u;
}

/* ---- buffers ------------------------------------------------------------- */

static bool reserve(int16_t **pcm, uint32_t bytes)
{
    if (*pcm) return true;
    /* Refuse rather than risk the OS allocator, which panics and reboots the
       device on failure. Starting a preset must never be able to do that. */
    if (hb_os_heap_largest() < bytes + (128u * 1024u)) return false;
    *pcm = (int16_t *)hb_os_alloc(bytes);
    return *pcm != (int16_t *)0;
}

static bool buffers_ready(void)
{
    for (int i = 0; i < SLOTS; i++)
        if (!reserve(&g_slot[i].pcm, BLOCK_FRAMES * 4u)) return false;

    if (!reserve(&g_quiet.pcm, QUIET_FRAMES * 4u)) return false;
    for (uint32_t i = 0; i < QUIET_FRAMES * 2u; i++) g_quiet.pcm[i] = 0;
    g_quiet.frames = QUIET_FRAMES;
    return true;
}

/* ---- rendering ----------------------------------------------------------- */

/* One slice of building a block. Returns true once it is ready to hand over.
 *
 * Straight through now: the block is a fixed number of frames and the chain
 * joins sample-exactly, so there is nothing to choose about where it ends and
 * nothing has to be rendered twice. */
static bool build_step(int idx)
{
    slot_t *b = &g_slot[idx];
    if (b->state == SLOT_READY) return true;
    if (!s_pull || s_source_done) return false;

    b->state = SLOT_BUILD;

    uint32_t want = BLOCK_FRAMES - b->fill;
    if (want > CHUNK_FRAMES) want = CHUNK_FRAMES;

    uint32_t got = s_pull(b->pcm + (uint32_t)b->fill * 2u, want, want,
                          s_pull_ctx);
    if (got == 0) {
        /* The program ended part-way through. Whatever is already rendered is
           real audio and gets played at its true length — duration_ms() will
           decline to state a duration that does not convert exactly, and a
           block with no successor has nothing to hand over to anyway. */
        s_source_done = true;
        if (b->fill == 0) { b->state = SLOT_FREE; return false; }
        b->frames = b->fill;
        b->state = SLOT_READY;
        return true;
    }

    b->fill += got;
    if (b->fill < BLOCK_FRAMES) return false;

    b->frames = BLOCK_FRAMES;
    b->fill = 0;
    b->state = SLOT_READY;
    return true;
}

/* Build a whole block now, hitch and all. Only for the moment the user presses
   play, where there is no lead time to spread the work over and a few tens of
   milliseconds is the difference between starting and not. */
static bool build_now(int idx)
{
    g_slot[idx].fill = 0;
    g_slot[idx].state = SLOT_FREE;
    for (int guard = 0; guard < 64; guard++)
        if (build_step(idx)) return true;
    return false;
}

/* Ramp the head of a block up from silence, in place, so the first block of a
   run and the first after a resume do not start as a step. */
static void fade_in_head(slot_t *b)
{
    uint32_t n = FADE_FRAMES < b->frames ? FADE_FRAMES : b->frames;
    for (uint32_t i = 0; i < n; i++) {
        int32_t g = (int32_t)((i * 4096u) / n);
        b->pcm[2 * i + 0] = (int16_t)((b->pcm[2 * i + 0] * g) >> 12);
        b->pcm[2 * i + 1] = (int16_t)((b->pcm[2 * i + 1] * g) >> 12);
    }
}

/* ---- handing blocks to the OS -------------------------------------------- */

static void enqueue(int idx)
{
    g_slot[idx].state = SLOT_QUEUED;
    g_slot[idx].seen_playing = false;
    g_q[g_qn++] = (uint8_t)idx;
    /* Deliberately not counted towards elapsed here. Handing a block to the OS
       is not the same as playing it, and counting it here made the clock jump a
       block at a time and run ahead of the sound. */
}

/* Start a voice on a block. Only needed to get going, or to recover after the
   chain has run dry — everything after that is chained. */
static bool play_slot(int idx)
{
    slot_t *b = &g_slot[idx];
    if (b->state != SLOT_READY || !b->pcm || !b->frames) return false;

    if (g_fade_in) { fade_in_head(b); g_fade_in = false; }
    desc_build(b);

    void *player = ((sfx_player_inst_t)SFX_PLAYER_INST_ADDR)();
    if (!player) return false;
    ((sfx_player_play_t)SFX_PLAYER_PLAY_ADDR)(player, b->desc,
                                              (void *)0, (void *)0);

    /* Only now does a voice exist to read the mixer rate from, so the
       descriptor just handed over went out with a zero duration - the safe
       value, but the one that leaves this block's join mis-counted. Every
       later block gets an exact duration through the descriptor; this one is
       corrected by writing the counter the OS would have computed straight
       into the voice.

       Safe to do while it sounds: voice+0x48 is written only by setSource and
       touched only by the exhaustion path, which this block will not reach for
       another second. */
    learn_rate(b);

    uint32_t v = *(volatile uint32_t *)(b->desc + SFX_OFF_VOICE);
    if (g_out_rate && v && !(v & 3u) && duration_ms(b->frames))
        *(volatile uint32_t *)(uintptr_t)(v + 0x48u) = b->frames;

    enqueue(idx);
    g_head_start_ms = hb_time_uptime_ms();
    g_state = EN_AUDIO_PLAYING;
    return true;
}

/* Link a ready block onto the end of the chain. This is the normal path: the
   voice picks it up itself when it reaches the end of the block in front. */
static bool chain_slot(int idx)
{
    slot_t *b = &g_slot[idx];
    if (b->state != SLOT_READY || !g_qn) return false;

    if (g_fade_in) { fade_in_head(b); g_fade_in = false; }
    desc_build(b);
    desc_chain(&g_slot[g_q[g_qn - 1]], b);
    enqueue(idx);
    return true;
}

/* Where the sounding block has most likely got to. Derived from the wall clock,
   so it is an estimate — every use of it keeps SAFETY_FRAMES clear. */
static uint32_t play_cursor(void)
{
    return MS_FRAMES(hb_time_uptime_ms() - g_head_start_ms);
}

/* Break the chain and fade the sounding block to zero in place, ahead of the
 * play cursor.
 *
 * This is what makes pause and stop immediate. The voice reads our buffer live,
 * so rewriting the part it has not reached yet takes effect within a fade
 * rather than at the end of a block — and it does that without starting a
 * second voice, which would sum rather than replace, because nothing can
 * silence a voice that is already sounding. */
static void silence_now(void)
{
    for (uint8_t i = 0; i < g_qn; i++) {
        slot_t *b = &g_slot[g_q[i]];
        desc_chain(b, (slot_t *)0);

        if (i > 0) {
            /* Queued but not reached yet: zero it outright. */
            for (uint32_t k = 0; k < b->frames * 2u; k++) b->pcm[k] = 0;
            continue;
        }

        uint32_t from = play_cursor() + SAFETY_FRAMES;
        if (from >= b->frames) continue;   /* ends before a fade could finish */

        uint32_t n = FADE_FRAMES;
        if (from + n > b->frames) n = b->frames - from;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t j = from + k;
            int32_t g = 4096 - (int32_t)((k * 4096u) / n);
            b->pcm[2 * j + 0] = (int16_t)((b->pcm[2 * j + 0] * g) >> 12);
            b->pcm[2 * j + 1] = (int16_t)((b->pcm[2 * j + 1] * g) >> 12);
        }
        for (uint32_t j = from + n; j < b->frames; j++) {
            b->pcm[2 * j + 0] = 0;
            b->pcm[2 * j + 1] = 0;
        }
    }
}

/* Chain the silent block to ITSELF, so the voice loops it for nothing. The DAC
   and the voice chain stay up while paused, which is why resuming is a ramp
   rather than a cold start — and it costs no rendering at all. */
static void arm_quiet(void)
{
    if (g_quiet_armed && desc_playing(&g_quiet)) return;

    desc_build(&g_quiet);
    desc_chain(&g_quiet, &g_quiet);

    if (g_qn) {
        /* Hand over from whatever is still sounding rather than starting a
           second voice on top of it: the pool mixes, it does not replace. */
        desc_chain(&g_slot[g_q[g_qn - 1]], &g_quiet);
    } else {
        void *player = ((sfx_player_inst_t)SFX_PLAYER_INST_ADDR)();
        if (!player) return;
        ((sfx_player_play_t)SFX_PLAYER_PLAY_ADDR)(player, g_quiet.desc,
                                                  (void *)0, (void *)0);
    }
    g_quiet_armed = true;
}

static void release_quiet(void)
{
    if (!g_quiet_armed) return;
    desc_chain(&g_quiet, (slot_t *)0);   /* let it run out */
    g_quiet_armed = false;
}

static void drop_queue(void)
{
    for (uint8_t i = 0; i < g_qn; i++) {
        slot_t *b = &g_slot[g_q[i]];
        b->state = SLOT_FREE;
        b->fill = 0;
    }
    g_qn = 0;
}

/* ---- the interface ------------------------------------------------------- */

static int prepare_unload(void)
{
    g_stopping = true;
    s_pull = (en_audio_pull_fn)0;
    silence_now();
    for (unsigned i = 0; i < SLOTS; i++) desc_chain(&g_slot[i], NULL);
    desc_chain(&g_quiet, NULL);
    int busy = desc_playing(&g_quiet);
    for (unsigned i = 0; i < SLOTS; i++) busy |= desc_playing(&g_slot[i]);
    if (busy) return 0;
    for (unsigned i = 0; i < SLOTS; i++) {
        hb_os_free(g_slot[i].pcm);
        g_slot[i].pcm = NULL;
    }
    hb_os_free(g_quiet.pcm);
    g_quiet.pcm = NULL;
    return 1;
}

bool en_audio_init(void)
{
    hb_shared->prepare_unload = prepare_unload;
    g_state = EN_AUDIO_IDLE;
    return true;
}

void en_audio_shutdown(void)
{
    silence_now();
    release_quiet();
    g_stopping = true;
    s_pull = (en_audio_pull_fn)0;
    drop_queue();
    g_state = EN_AUDIO_IDLE;
    /* Buffers are deliberately left allocated: the app is going away, and
       freeing one while a voice may still be reading it would be worse than
       leaking for the moment until the heap goes too. */
}

bool en_audio_can_stream(void) { return true; }

uint32_t en_audio_preferred_rate(void) { return DEVICE_RATE; }

bool en_audio_start_stream(uint32_t sample_rate, en_audio_pull_fn pull,
                           void *ctx)
{
    (void)sample_rate;             /* the descriptor is told DEVICE_RATE */
    if (!pull) return false;
    if (!buffers_ready()) { g_state = EN_AUDIO_FAILED; return false; }

    /* Whatever is queued belongs to the old source. Fade it out where it stands
       and take the blocks back: letting it run would sum with the new audio,
       since the pool mixes and nothing can cut a sounding voice short. */
    silence_now();
    drop_queue();

    s_pull = pull;
    s_pull_ctx = ctx;
    s_source_done = false;
    g_frames_done = 0;
    g_paused = false;
    g_stopping = false;
    g_fade_in = true;

    for (int i = 0; i < SLOTS; i++) {
        g_slot[i].state = SLOT_FREE;
        g_slot[i].fill = 0;
    }

    if (!build_now(0)) { g_state = EN_AUDIO_FAILED; return false; }

    /* Silence is chainable, so hand over from it rather than starting a second
       voice underneath it. */
    if (g_quiet_armed) {
        if (g_fade_in) { fade_in_head(&g_slot[0]); g_fade_in = false; }
        desc_build(&g_slot[0]);
        desc_chain(&g_quiet, &g_slot[0]);
        enqueue(0);
        g_head_start_ms = hb_time_uptime_ms();
        g_quiet_armed = false;
        g_state = EN_AUDIO_PLAYING;
        return true;
    }

    if (!play_slot(0)) { g_state = EN_AUDIO_FAILED; return false; }
    return true;
}

/* The file-shaped half of the interface is unreachable: the engine checks
   en_audio_can_stream() and takes the pull path. Defined so the backend
   satisfies audio.h in full. */
bool en_audio_submit(const char *key, const int16_t *pcm,
                     uint32_t frames, uint32_t advance_frames,
                     uint32_t sample_rate, bool loop)
{
    (void)key; (void)pcm; (void)frames; (void)advance_frames;
    (void)sample_rate; (void)loop;
    return false;
}

bool en_audio_queue(const char *key, const int16_t *pcm,
                    uint32_t frames, uint32_t advance_frames,
                    uint32_t sample_rate)
{
    (void)key; (void)pcm; (void)frames; (void)advance_frames;
    (void)sample_rate;
    return false;
}

bool en_audio_wants_next(void) { return false; }

double en_audio_remaining(void) { return s_pull && !s_source_done ? 1e9 : 0.0; }

void en_audio_stop(uint32_t fade_ms)
{
    (void)fade_ms;                 /* the fade length is fixed at FADE_MS */
    silence_now();
    g_stopping = true;
}

void en_audio_set_paused(bool paused)
{
    if (paused == g_paused) return;
    g_paused = paused;

    if (paused) {
        /* Immediate, and without stopping the voice: the sounding block is
           faded to zero in place ahead of the cursor and the chain is broken,
           then silence is chained to itself so the DAC never goes down. */
        silence_now();
        drop_queue();
        arm_quiet();
        g_state = EN_AUDIO_PAUSED;
    } else {
        g_fade_in = true;          /* come back up, do not step */
        g_state = EN_AUDIO_PLAYING;
    }
}

void en_audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_volume = percent;
    /* Read when a descriptor is built, so it takes hold within a block. */
}

int en_audio_get_volume(void) { return g_volume; }

void en_audio_tick(void)
{
    if (!s_pull) return;

    uint32_t now = hb_time_uptime_ms();
    uint32_t since = now - g_last_tick_ms;
    g_last_tick_ms = now;

    /* Starved: the frame callback stopped being run, which is what happens when
       RetailOS brings its own Music app to the front. Everything queued has
       long since played out unheard, so start clean rather than carrying on
       from stale state. */
    if (g_qn && since > STARVED_MS) {
        drop_queue();
        g_fade_in = true;
    }

    /* Reclaim. The chain is strictly FIFO — blocks finish in the order they
       were linked — so only the front of the queue can be the one to retire.
       The flag is the OS's own: set by setSource when a voice takes the
       descriptor up, cleared by the completion handler when the buffer has been
       played out. Waiting to see it set first is what distinguishes a block
       that has finished from one that has not started. */
    while (g_qn) {
        slot_t *h = &g_slot[g_q[0]];
        if (desc_playing(h)) { h->seen_playing = true; break; }
        if (!h->seen_playing) break;      /* queued, not reached yet */

        /* How long the block took against how long its audio actually is.
         *
         * Stating a duration is the one thing here that depends on reading the
         * OS correctly, and getting it wrong is expensive: too large and the
         * voice pads with silence once the PCM runs out, which is a block of
         * sound followed by a block of nothing. Rather than trust the reading,
         * measure it. A block that takes half again as long as its own audio
         * has been padded, and two in a row is not a stalled frame callback.
         *
         * Falling back to no duration at all costs a faint click per join and
         * is what played correctly before any of this - much the better of the
         * two failures to land on. */
        if (!g_duration_bad && duration_ms(h->frames)) {
            uint32_t expect = (h->frames * 1000u) / DEVICE_RATE;
            uint32_t took = now - g_head_start_ms;
            if (h->seen_playing && took > expect + expect / 2u) {
                if (++g_overrun >= 2) g_duration_bad = true;
            } else {
                g_overrun = 0;
            }
        }

        h->state = SLOT_FREE;
        h->fill = 0;
        g_frames_done += h->frames;       /* this one really has played */
        for (uint8_t i = 1; i < g_qn; i++) g_q[i - 1] = g_q[i];
        g_qn--;
        g_head_start_ms = now;
    }

    if (g_paused) { arm_quiet(); return; }

    if (g_stopping) {
        if (!g_qn) {
            release_quiet();
            s_pull = (en_audio_pull_fn)0;
            g_state = EN_AUDIO_IDLE;
        }
        return;
    }

    /* Build one slice into whichever block is free, then hand over anything
       ready. A block of lead time against a few milliseconds of work per tick,
       so the chain is extended long before the voice reaches its end and no
       single frame carries enough of the rendering to be seen. */
    int ready = -1;
    for (int i = 0; i < SLOTS; i++) {
        if (g_slot[i].state == SLOT_READY) { ready = i; break; }
        if (g_slot[i].state == SLOT_FREE || g_slot[i].state == SLOT_BUILD) {
            if (build_step(i)) ready = i;
            break;                        /* one slice per tick, no more */
        }
    }

    if (ready >= 0) {
        if (g_qn) {
            chain_slot(ready);
        } else {
            /* The chain ran dry — a very long stall, or the very first block.
               A fresh voice has to be started, and that join is covered by the
               fade-in rather than by anything clever. */
            release_quiet();
            play_slot(ready);
        }
        return;
    }

    if (!g_qn && s_source_done) {
        release_quiet();
        s_pull = (en_audio_pull_fn)0;
        g_state = EN_AUDIO_IDLE;          /* the program is over */
    }
}

en_audio_state_t en_audio_state(void) { return g_state; }

double en_audio_elapsed(void)
{
    /* Blocks that have finished, plus however far into the sounding one the
       wall clock says we are - capped at that block's length so a stall cannot
       push the clock past audio that has not been played. */
    uint64_t f = g_frames_done;

    if (g_qn) {
        uint32_t cap = g_slot[g_q[0]].frames;
        uint32_t in = MS_FRAMES(hb_time_uptime_ms() - g_head_start_ms);
        f += (in > cap) ? cap : in;
    }
    return (double)f / (double)DEVICE_RATE;
}

const char *en_audio_backend_name(void) { return "RetailOS sfx, chained PCM"; }
