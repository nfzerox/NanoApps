/*
 * clock — show current date/time via the OS RTC mechanism.
 * Refreshes once per second. Whole app in a 64 KB pthread.
 */

#include "hb_sdk.h"

/* freestanding — compiler synthesizes calls to memset; provide one. */
void *memset(void *s, int c, unsigned int n)
{
    uint8_t *p = (uint8_t *)s;
    for (unsigned i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

#define PTHREAD_CREATE_ADDR (0x080226f8u | 1u)
typedef int (*pthread_create_t)(uint32_t *thread, void *attr,
                                void *(*start)(void *), void *arg);

static void dec2(char *out, int v)
{
    int t = v, tens = 0;
    while (t >= 10) { t -= 10; tens++; }
    while (tens >= 10) tens -= 10;
    out[0] = '0' + tens;
    out[1] = '0' + t;
    out[2] = 0;
}

static void dec4(char *out, int v)
{
    int t = v;
    int d4 = 0; while (t >= 1000) { t -= 1000; d4++; }
    int d3 = 0; while (t >= 100)  { t -= 100;  d3++; }
    int d2 = 0; while (t >= 10)   { t -= 10;   d2++; }
    out[0] = '0' + d4; out[1] = '0' + d3;
    out[2] = '0' + d2; out[3] = '0' + t; out[4] = 0;
}

static void draw_time(const hb_rtc_time_t *t)
{
    char hms[12];
    dec2(hms,   t->hours);    hms[2] = ':';
    dec2(hms+3, t->minutes);  hms[5] = ':';
    dec2(hms+6, t->seconds);

    char ymd[12];
    dec4(ymd,   t->year);         ymd[4] = '-';
    dec2(ymd+5, t->month);        ymd[7] = '-';
    dec2(ymd+8, t->day_of_month);

    hb_fill_screen(HB_BLACK);
    hb_draw_str(4, 0, "CLOCK", 2, HB_YELLOW, HB_BLACK);
    hb_draw_str(24, 110, hms, 3, HB_GREEN, HB_BLACK);
    hb_draw_str(48, 170, ymd, 2, HB_WHITE, HB_BLACK);

    char tail[40] = "wday=";
    dec2(tail+5, t->weekday);
    /* battery voltage too */
    uint32_t mv = hb_battery_voltage_mv();
    int ti = 7;
    tail[ti++] = ' '; tail[ti++] = 'b'; tail[ti++] = 'a'; tail[ti++] = 't';
    tail[ti++] = '=';
    int t1k = 0; while (mv >= 1000) { mv -= 1000; t1k++; }
    int t100 = 0; while (mv >= 100)  { mv -= 100;  t100++; }
    int t10  = 0; while (mv >= 10)   { mv -= 10;   t10++; }
    tail[ti++] = '0' + t1k;
    tail[ti++] = '0' + t100;
    tail[ti++] = '0' + t10;
    tail[ti++] = '0' + mv;
    tail[ti++] = 'm'; tail[ti++] = 'V';
    tail[ti] = 0;
    hb_draw_str(4, 220, tail, 1, HB_RGB(0x80,0x80,0x80), HB_BLACK);

    /* battery level + charger state */
    char b[40] = "lvl=";
    int bi = 4;
    uint32_t lvl = hb_battery_level_0_to_15();
    int lt = 0; while (lvl >= 10) { lvl -= 10; lt++; }
    if (lt) b[bi++] = '0' + lt;
    b[bi++] = '0' + lvl;
    b[bi++] = '/'; b[bi++] = '1'; b[bi++] = '5';
    b[bi++] = ' '; b[bi++] = 'c'; b[bi++] = 'h'; b[bi++] = 'g'; b[bi++] = '=';
    static const char *chg_names[] = {
        "Unkn","Off","CurOff","Susp","EnOff","LowChg","Chg"
    };
    uint32_t cs = hb_battery_charger_state();
    const char *cn = cs < 7 ? chg_names[cs] : "???";
    for (int i = 0; cn[i] && bi < 36; i++) b[bi++] = cn[i];
    b[bi] = 0;
    hb_draw_str(4, 240, b, 1,
                hb_battery_is_charging() ? HB_GREEN : HB_WHITE, HB_BLACK);

    hb_draw_str(4, HB_SCREEN_H - 14, "HOME / VOL = exit", 1,
                HB_RGB(0x80,0x80,0x80), HB_BLACK);
}

static void *app_main(void *arg)
{
    (void)arg;
    hb_ui_init();
    hb_rtc_time_t t;
    volatile uint8_t *bz = (volatile uint8_t *)&t;
    for (unsigned i = 0; i < sizeof(t); i++) bz[i] = 0;
    for (uint32_t frame = 0; frame < 20000000; frame++) {
        int16_t tx, ty;
        hb_ui_event_t e = hb_ui_poll(&tx, &ty);
        if (e == HB_UI_EXIT) break;

        /* Refresh ~ once per second. hb_ui_pace ~ a few ms per call,
           so ~500 iterations between reads. */
        static uint32_t last_refresh = 0;
        if (frame == 0 || frame - last_refresh >= 500) {
            hb_rtc_read(&t);
            draw_time(&t);
            last_refresh = frame;
        }
        hb_ui_pace();
    }
    hb_ui_done();
    return (void *)0;
}

HB_APP_ENTRY(payload_entry)
{
    static uint32_t attr[16];
    for (int i = 0; i < 16; i++) attr[i] = 0;
    attr[0] = 0x50544841u;
    attr[2] = 2;
    attr[4] = 0x10000;
    attr[6] = 1;
    attr[7] = 1;
    attr[8] = 1;
    attr[9] = 0;
    uint32_t tid = 0;
    ((pthread_create_t)PTHREAD_CREATE_ADDR)(&tid, attr, app_main, (void *)0);
}
