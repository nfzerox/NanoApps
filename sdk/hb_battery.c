/*
 * hb_battery.c — read battery state from the OS PMU cache.
 *
 * The convenience helper below treats the low-rate and normal 
 * charging states as "charging".
 */

#include "hb_sdk.h"

#define BATTERY_CACHE_BASE          0x0890c070u
#define OFF_CACHED_CHARGE_STATE     0x00
#define OFF_CACHED_VOLTAGE          0x08
#define OFF_CACHED_LEVEL            0x0c

#define POWER_MANAGER_BATTERY_VOLTAGE_ADDR (0x080109acu | 1u)
typedef void (*battery_voltage_t)(uint32_t *out_mv);

uint32_t hb_battery_voltage_mv(void)
{
    /* Use the OS wrapper rather than direct memread — slight extra
       safety since the function reads the cache under whatever lock 
       the PMU code holds. */
    uint32_t mv = 0;
    ((battery_voltage_t)POWER_MANAGER_BATTERY_VOLTAGE_ADDR)(&mv);
    return mv;
}

uint32_t hb_battery_level_0_to_15(void)
{
    return *(volatile uint32_t *)(BATTERY_CACHE_BASE + OFF_CACHED_LEVEL);
}

uint32_t hb_battery_charger_state(void)
{
    return *(volatile uint32_t *)(BATTERY_CACHE_BASE + OFF_CACHED_CHARGE_STATE);
}

bool hb_battery_is_charging(void)
{
    uint32_t s = hb_battery_charger_state();
    return s == 5 /* slow charger? */ || s == 6 /* regular charging */;
}

/* Diagnostic — read raw struct word for layout verification. */
uint32_t hb_battery_cache_read(int word_offset)
{
    volatile uint32_t *p = (volatile uint32_t *)BATTERY_CACHE_BASE;
    return p[word_offset];
}
