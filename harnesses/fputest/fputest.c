/*
 * fputest -- genuine first-FP-at-launch probe for nano 7G retailos.
 *
 * No floating-point instruction executes before the selected diagnostic
 * instruction. A persistent two-stage state file makes separate launches test:
 *   1. ARM VFPv3 immediate while another task owns FP
 *   2. Thumb VFPv2 register transfer while another task owns FP
 *
 * If NanoApps already owns FP, the probe records NOT COLD and does not consume
 * the stage. Reboot (or deliberately run the other FP task) and launch again.
 */
#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define OUT_DIR    "/Data/FPUTest"
#define OUT_PATH   OUT_DIR "/result.txt"
#define STATE_PATH OUT_DIR "/cold-state.bin"

#define CURRENT_TCB_PTR_ADDR 0x0890b950u
#define TASK_TABLE_BASE      0x08a9cdc8u
#define TCB_TASK_ID_OFF      0x20u
#define TCB_FP_CONTEXT_OFF   0x30u
#define TCB_FP_ENABLED_OFF   0x34u

#define STATE_MAGIC   0x32445046u /* "FPD2" */
#define STATE_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t launch_count;
    uint32_t next_stage;
    uint32_t pending_stage;
    uint32_t check;
} cold_state_t;

static uint32_t *s_fb;
static int s_w, s_h;
static char g_lines[80][80];
static int g_nlines;
static int g_done;
static int g_prev_down;

extern uint32_t fputest_cold_arm_vfp3(void);
extern uint32_t fputest_cold_thumb_vfp2(uint32_t bits);

static void text(int x, int y, const char *str, int scale, uint32_t fg)
{
    fg |= 0xff000000u;
    for (; *str; str++, x += 8 * scale) {
        const uint8_t *glyph = hb_font8x8_glyph(*str);
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++) {
                if (!(glyph[row] & (0x80 >> col))) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;
                        if (px >= 0 && px < s_w && py >= 0 && py < s_h)
                            s_fb[py * s_w + px] = fg;
                    }
            }
    }
}

static char *put_s(char *out, const char *s)
{
    while (*s) *out++ = *s++;
    *out = 0;
    return out;
}

static char *put_u(char *out, uint32_t value)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (n) *out++ = tmp[--n];
    *out = 0;
    return out;
}

static char *put_x(char *out, uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4)
        *out++ = hex[(value >> shift) & 15u];
    *out = 0;
    return out;
}

static void line_s(const char *value)
{
    if (g_nlines < (int)(sizeof(g_lines) / sizeof(g_lines[0])))
        put_s(g_lines[g_nlines++], value);
}

static void line_u(const char *name, uint32_t value)
{
    if (g_nlines < (int)(sizeof(g_lines) / sizeof(g_lines[0])))
        put_u(put_s(g_lines[g_nlines++], name), value);
}

static void line_x(const char *name, uint32_t value)
{
    if (g_nlines < (int)(sizeof(g_lines) / sizeof(g_lines[0])))
        put_x(put_s(g_lines[g_nlines++], name), value);
}

static void save_result(const char *status)
{
    static char buffer[8192];
    char *out = buffer;
    for (int i = 0; i < g_nlines; i++) {
        out = put_s(out, g_lines[i]);
        *out++ = '\n';
    }
    out = put_s(out, status);
    *out++ = '\n';
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(OUT_DIR);
    hb_fs_write(OUT_PATH, buffer, (uint32_t)(out - buffer));
}

static uint32_t state_check(const cold_state_t *state)
{
    return state->magic ^ state->version ^ state->launch_count ^
           state->next_stage ^ state->pending_stage ^ 0xa5c31f27u;
}

static void state_init(cold_state_t *state)
{
    state->magic = STATE_MAGIC;
    state->version = STATE_VERSION;
    state->launch_count = 0;
    state->next_stage = 1;
    state->pending_stage = 0;
    state->check = state_check(state);
}

static void state_load(cold_state_t *state)
{
    uint32_t got = hb_fs_read(STATE_PATH, state, sizeof(*state));
    if (got != sizeof(*state) || state->magic != STATE_MAGIC ||
        state->version != STATE_VERSION || state->check != state_check(state) ||
        state->next_stage < 1 || state->next_stage > 3 ||
        state->pending_stage > 2)
        state_init(state);
}

static bool state_save(cold_state_t *state)
{
    state->check = state_check(state);
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(OUT_DIR);
    return hb_fs_write(STATE_PATH, state, sizeof(*state));
}

static bool ram_pointer(uint32_t value)
{
    return (value & 3u) == 0 && value >= 0x08000000u && value < 0x0c000000u;
}

static void snapshot_owner(uint32_t owner, uint32_t tcb_words[16],
                           uint32_t slot_words[8], uint32_t *task_id)
{
    *task_id = 0xffffffffu;
    for (int i = 0; i < 16; i++) tcb_words[i] = 0;
    for (int i = 0; i < 8; i++) slot_words[i] = 0;
    if (!ram_pointer(owner)) return;

    volatile uint32_t *tcb = (volatile uint32_t *)(uintptr_t)owner;
    for (int i = 0; i < 16; i++) tcb_words[i] = tcb[i];
    *task_id = *(volatile uint32_t *)(uintptr_t)(owner + TCB_TASK_ID_OFF);
    if (*task_id > 0 && *task_id <= 110) {
        volatile uint32_t *slot = (volatile uint32_t *)(uintptr_t)
            (TASK_TABLE_BASE + *task_id * 32u);
        for (int i = 0; i < 8; i++) slot_words[i] = slot[i];
    }
}

static void record_snapshot(uint32_t task_id, const uint32_t tcb_words[16],
                            const uint32_t slot_words[8])
{
    char name[24];
    line_u("OTHER TASK ID ", task_id);
    for (int i = 0; i < 16; i++) {
        char *out = put_s(name, "OTHER TCB+");
        out = put_x(out, (uint32_t)i * 4u);
        put_s(out, " ");
        line_x(name, tcb_words[i]);
    }
    for (int i = 0; i < 8; i++) {
        char *out = put_s(name, "OTHER SLOT+");
        out = put_x(out, (uint32_t)i * 4u);
        put_s(out, " ");
        line_x(name, slot_words[i]);
    }
}

static uint32_t current_task(void)
{
    return *(volatile uint32_t *)CURRENT_TCB_PTR_ADDR;
}

static uint32_t fp_enabled(uint32_t task)
{
    return ram_pointer(task)
        ? *(volatile uint8_t *)(uintptr_t)(task + TCB_FP_ENABLED_OFF)
        : 0xffffffffu;
}

static uint32_t saved_context_word(uint32_t task, uint32_t index)
{
    uint32_t context;
    if (!ram_pointer(task)) return 0xffffffffu;
    context = *(volatile uint32_t *)(uintptr_t)(task + TCB_FP_CONTEXT_OFF);
    if (!ram_pointer(context) || index >= 37) return 0xffffffffu;
    return *(volatile uint32_t *)(uintptr_t)(context + index * 4u);
}

static void run(void)
{
    cold_state_t state;
    hb_fpu_status_t attach_status;
    uint32_t tcb_words[16], slot_words[8];
    uint32_t task_id, task, stage, result;
    uint32_t owner_before, owner_after;
    uint32_t fpexc_before, fpscr_before, fpexc_after, fpscr_after;
    int attach;

    g_nlines = 0;
    g_done = 0;
    state_load(&state);
    state.launch_count++;

    line_s("BUILD COLD-FIRST V1");
    line_u("LAUNCH ", state.launch_count);
    if (state.pending_stage != 0) {
        line_u("PRIOR INCOMPLETE STAGE ", state.pending_stage);
        state.next_stage = state.pending_stage + 1u;
        state.pending_stage = 0;
    }
    stage = state.next_stage;
    line_u("STAGE ", stage);

    attach = hb_fpu_attach_current_task(&attach_status);
    line_u("ATTACH ", attach < 0 ? (uint32_t)-attach : (uint32_t)attach);
    line_x("CPSR 0x", attach_status.cpsr);
    line_x("TASK 0x", attach_status.task);
    line_x("CONTEXT 0x", attach_status.context);
    line_x("OWNER AT ENTRY 0x", attach_status.owner_before);
    if (attach < 0) {
        save_result("ATTACH FAILED");
        return;
    }

    if (stage > 2) {
        g_done = 1;
        state_save(&state);
        save_result("BOTH STAGES COMPLETE");
        return;
    }

    task = current_task();
    if (task != attach_status.task) {
        line_x("TASK CHANGED 0x", task);
        save_result("TASK CHANGED");
        return;
    }

    state.pending_stage = stage;
    if (!state_save(&state)) {
        save_result("STATE WRITE FAILED");
        return;
    }

    owner_before = hb_fpu_owner_task();
    line_x("OWNER BEFORE MARKER 0x", owner_before);
    line_x("ENABLE BEFORE 0x", fp_enabled(task));
    line_x("SAVED FPEXC BEFORE 0x", saved_context_word(task, 1));
    line_x("SAVED FPSCR BEFORE 0x", saved_context_word(task, 2));
    snapshot_owner(owner_before, tcb_words, slot_words, &task_id);
    save_result(stage == 1 ? "START ARM VFP3 COLD" :
                             "START THUMB VFP2 COLD");

    fpexc_before = saved_context_word(task, 1);
    fpscr_before = saved_context_word(task, 2);

    /* This is the decisive check. No function call occurs between this owner
     * read and the diagnostic FP instruction. */
    owner_before = hb_fpu_owner_task();
    if (owner_before == task) {
        state.pending_stage = 0;
        state_save(&state);
        line_x("OWNER EXACT 0x", owner_before);
        save_result("NOT COLD - REBOOT AND RELAUNCH");
        return;
    }

    if (stage == 1)
        result = fputest_cold_arm_vfp3();
    else
        result = fputest_cold_thumb_vfp2(0x3f000000u);
    owner_after = hb_fpu_owner_task();
    fpexc_after = saved_context_word(task, 1);
    fpscr_after = saved_context_word(task, 2);

    line_x("OWNER EXACT BEFORE 0x", owner_before);
    line_x("OWNER EXACT AFTER 0x", owner_after);
    line_x("FPEXC SAVED PRE 0x", fpexc_before);
    line_x("FPSCR SAVED PRE 0x", fpscr_before);
    line_x("FPEXC SAVED POST 0x", fpexc_after);
    line_x("FPSCR SAVED POST 0x", fpscr_after);
    line_x("RESULT 0x", result);
    record_snapshot(task_id, tcb_words, slot_words);

    state.pending_stage = 0;
    state.next_stage = stage + 1u;
    state_save(&state);
    if (owner_after == task &&
        ((stage == 1 && result == 0x3f800000u) ||
         (stage == 2 && result == 0x3f000000u))) {
        g_done = 1;
        save_result(stage == 1 ? "ARM COLD OK" : "THUMB COLD OK");
    } else {
        save_result("COLD RESULT WRONG");
    }
}

static void draw(void)
{
    hb_raw_fill(0x14102a);
    text(12, 20, "FPU COLD", 3, 0xffffff);
    int first = g_nlines > 9 ? g_nlines - 9 : 0;
    for (int i = first; i < g_nlines; i++)
        text(12, 75 + (i - first) * 30, g_lines[i], 1, 0xdde3ee);
    text(12, 360, g_done ? "DONE" : "INCOMPLETE", 2,
         g_done ? 0x7ee0a8 : 0xff7070);
    text(12, s_h - 24, "TAP: RUN AGAIN", 1, 0x8a93a8);
}

void hb_raw_init(int w, int h)
{
    s_fb = hb_raw_fb();
    s_w = w;
    s_h = h;
    run();
    draw();
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    if (!touch->down && g_prev_down) {
        run();
        draw();
    }
    g_prev_down = touch->down;
}
