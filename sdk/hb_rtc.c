/*
 * hb_rtc.c — read and set current date/time through the OS clock
 * service.
 */

#include "hb_sdk.h"

#define TIME_MANAGER_INSTANCE_ADDR (0x0842bf60u | 1u)
#define TIME_MANAGER_VTABLE_GET_TIME_OFFSET 0x38

typedef void *(*time_inst_t)(void);
typedef void  (*time_get_t)(void *out, void *self);

void hb_rtc_read(hb_rtc_time_t *out)
{
    void *tmgr = ((time_inst_t)TIME_MANAGER_INSTANCE_ADDR)();
    if (!tmgr) return;
    void **vtable = *(void ***)tmgr;
    time_get_t getfn = (time_get_t)vtable[TIME_MANAGER_VTABLE_GET_TIME_OFFSET / 4];
    getfn(out, tmgr);
}

#define TIME_MANAGER_VTABLE_SET_TIME_OFFSET 0x34

typedef void (*time_set_t)(void *self, void *time_struct);

bool hb_rtc_set(uint16_t year, uint8_t month, uint8_t day,
                uint8_t hour, uint8_t minute, uint8_t second)
{
    if (year < 2000 || year >= 2100) return false;
    if (month < 1 || month > 12) return false;
    if (day   < 1 || day   > 31) return false;
    if (hour > 23 || minute > 59 || second > 59) return false;

    void *tmgr = ((time_inst_t)TIME_MANAGER_INSTANCE_ADDR)();
    if (!tmgr) return false;
    void **vtable = *(void ***)tmgr;

    /* Read existing clock struct to preserve timezone/DST fields. */
    uint8_t rtc[16] = {0};
    time_get_t getfn = (time_get_t)vtable[TIME_MANAGER_VTABLE_GET_TIME_OFFSET / 4];
    getfn(rtc, tmgr);

    *(uint16_t *)(rtc + 0) = year;
    rtc[2]  = month;
    rtc[3]  = day;
    /* leave dayOfYear at +4 alone — OS recalculates from y/m/d */
    rtc[8]  = hour;
    rtc[9]  = minute;
    rtc[10] = second;

    time_set_t setfn = (time_set_t)vtable[TIME_MANAGER_VTABLE_SET_TIME_OFFSET / 4];
    setfn(tmgr, rtc);
    return true;
}
