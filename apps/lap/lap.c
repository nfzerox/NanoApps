/*
 * lap - raw-surface stopwatch (from nano7kit by Shimku, MIT) for Helvetica / silver_resident.
 *
 * Design (runs well on this MKN52):
 *   - RAW_SURFACE, no LVGL (~6 KB SDK, not +150 KB widgets)
 *   - HBAppKind 1, well under the ~0.5 MB launch cap and 576 KB GL slot
 *   - heap free/largest on screen so a later edit cannot hide a leak
 *   - one file in /Data/Lap; missing dir is an empty state, not a crash
 *
 * TAP start/stop. Hold about 1 s to reset. Last elapsed is saved.
 */
#include "hb_raw_surface.h"
#include "hb_sdk.h"
#include "hb_heap.h"

#define DATA_DIR  "/Data/Lap"
#define SAVE_PATH "/Data/Lap/last.txt"

static uint32_t *s_fb;
static int s_w, s_h;

static uint32_t s_free, s_largest;
static uint32_t s_base_ms;
static uint32_t s_elapsed;
static int s_running;
static int s_prev_down;
static uint32_t s_hold_ms;
static int s_dirty = 1;
static int s_save_ok = 1;

static char *put_s(char *o, const char *s)
{
    while (*s) *o++ = *s++;
    *o = 0;
    return o;
}

static char *put_u(char *o, uint32_t u)
{
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + (u % 10u)); u /= 10u; } while (u);
    while (n) *o++ = t[--n];
    *o = 0;
    return o;
}

static void text(int x, int y, const char *str, int scale, uint32_t fg)
{
    fg |= 0xff000000u;
    for (; *str; str++, x += 8 * scale) {
        const uint8_t *g = hb_font8x8_glyph(*str);
        int r, c, sy, sx, px, py;
        for (r = 0; r < 8; r++)
            for (c = 0; c < 8; c++) {
                if (!(g[r] & (0x80 >> c))) continue;
                for (sy = 0; sy < scale; sy++)
                    for (sx = 0; sx < scale; sx++) {
                        px = x + c * scale + sx;
                        py = y + r * scale + sy;
                        if (px >= 0 && px < s_w && py >= 0 && py < s_h)
                            s_fb[py * s_w + px] = fg;
                    }
            }
    }
}

static uint32_t shown_ms(void)
{
    if (!s_running) return s_elapsed;
    return s_elapsed + (hb_time_uptime_ms() - s_base_ms);
}

static void load_last(void)
{
    uint8_t buf[32];
    uint32_t n, i, v = 0;
    s_elapsed = 0;
    n = hb_fs_read(SAVE_PATH, buf, sizeof buf);
    if (n == 0) {
        s_save_ok = hb_fs_exists(DATA_DIR) ? 1 : 0;
        return;
    }
    for (i = 0; i < n; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') v = v * 10u + (uint32_t)(buf[i] - '0');
        else break;
    }
    s_elapsed = v;
    s_save_ok = 1;
}

static void save_last(void)
{
    char buf[32];
    char *o = put_u(buf, s_elapsed);
    *o++ = '\n';
    *o = 0;
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    s_save_ok = hb_fs_write(SAVE_PATH, buf, (uint32_t)(o - buf)) ? 1 : 0;
}

static void format_time(char *out, uint32_t ms)
{
    uint32_t cs = (ms / 100u) % 10u;
    uint32_t s = (ms / 1000u) % 60u;
    uint32_t m = (ms / 60000u) % 100u;
    char *o = out;
    if (m < 10) *o++ = '0';
    o = put_u(o, m);
    *o++ = ':';
    if (s < 10) *o++ = '0';
    o = put_u(o, s);
    *o++ = '.';
    o = put_u(o, cs);
}

static void draw(void)
{
    char line[48];
    char *o;
    uint32_t ms = shown_ms();

    hb_raw_fill(0x10141c);
    text(12, 16, "LAP", 3, 0xe8c547);

    o = put_s(line, "HEAP ");
    o = put_u(o, s_free / 1024u);
    put_s(o, "K");
    text(12, 56, line, 1, 0x8a93a8);

    o = put_s(line, "LARGEST ");
    o = put_u(o, s_largest / 1024u);
    put_s(o, "K");
    text(12, 72, line, 1, 0x8a93a8);

    format_time(line, ms);
    text(20, 140, line, 4, 0xffffff);

    if (s_running) text(12, 220, "RUNNING", 2, 0x7ee0a8);
    else if (s_elapsed) text(12, 220, "PAUSED", 2, 0xe8c547);
    else text(12, 220, "STOPPED", 2, 0x8a93a8);

    if (!s_save_ok) text(12, 260, "NO /APPS/DATA (OK)", 1, 0xf4a261);
    text(12, s_h - 56, "TAP  START/STOP", 1, 0x8a93a8);
    text(12, s_h - 36, "HOLD RESET", 1, 0x8a93a8);
    text(12, s_h - 16, "RAW SURFACE  SLOT A", 1, 0x5a6270);
}

void hb_raw_init(int w, int h)
{
    s_fb = hb_raw_fb();
    s_w = w;
    s_h = h;
    s_free = hb_os_heap_free();
    s_largest = hb_os_heap_largest();
    load_last();
    hb_trace_init();
    hb_trace_log("LAPINIT", s_free, s_largest);
    draw();
}

void hb_raw_frame(const hb_spoint_t *t)
{
    uint32_t now = hb_time_uptime_ms();
    if (t->down) {
        if (!s_prev_down) s_hold_ms = now;
        else if (now - s_hold_ms >= 1000u && s_elapsed) {
            s_running = 0;
            s_elapsed = 0;
            save_last();
            s_hold_ms = now;
            s_dirty = 1;
        }
    } else if (s_prev_down) {
        if (now - s_hold_ms < 1000u) {
            if (s_running) {
                s_elapsed += now - s_base_ms;
                s_running = 0;
                save_last();
            } else {
                s_base_ms = now;
                s_running = 1;
            }
            s_dirty = 1;
        }
    }
    s_prev_down = t->down;

    if (s_running || s_dirty) {
        if ((now % 250u) < 20u) {
            s_free = hb_os_heap_free();
            s_largest = hb_os_heap_largest();
        }
        draw();
        s_dirty = 0;
    }
}
