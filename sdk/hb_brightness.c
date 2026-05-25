/*
 * hb_brightness.c — set LCD backlight level through the OS power stack.
 *
 * Values below the visible minimum clamp to the minimum, so level=0 is
 * not the same as powering the backlight off. The SDK caches the most
 * recent value it set so callers can query it back.
 */

#include "hb_sdk.h"

#define BRIGHTNESS_SET_RAW_ADDR (0x08083068u | 1u)
typedef void (*brightness_set_t)(uint32_t level_u16);

#define BACKLIGHT_POWER_ADDR (0x0800acf4u | 1u)
typedef int (*backlight_power_t)(int on);

/* N7G brightness stops used by the OS. */
#define BRIGHTNESS_9BIT_MIN     100u
#define BRIGHTNESS_9BIT_DEFAULT 384u
#define BRIGHTNESS_9BIT_MAX     475u

static uint16_t g_last_set = (BRIGHTNESS_9BIT_DEFAULT << 7);  /* OS default */

void hb_brightness_set(uint16_t level)
{
    ((brightness_set_t)BRIGHTNESS_SET_RAW_ADDR)((uint32_t)level);
    g_last_set = level;
}

uint16_t hb_brightness_get(void)
{
    /* Best-effort: returns what we last wrote. The OS may have changed
       it since (auto-dim, etc.) but there's no way to query. */
    return g_last_set;
}

/* True backlight power, separate from level clamping. */
void hb_brightness_power(bool on)
{
    ((backlight_power_t)BACKLIGHT_POWER_ADDR)(on ? 1 : 0);
}

void hb_brightness_set_percent(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    unsigned int level9;
    if (percent <= 40) {
        /* Map 0..40% to MIN..DEFAULT. */
        level9 = BRIGHTNESS_9BIT_MIN +
                 (percent * (BRIGHTNESS_9BIT_DEFAULT - BRIGHTNESS_9BIT_MIN));
        /* /40 without libgcc divmod: */
        unsigned int q = 0;
        while (level9 >= 40) { level9 -= 40; q++; }
        level9 = BRIGHTNESS_9BIT_MIN + q;
    } else {
        /* Map 40..100% to DEFAULT..MAX. */
        unsigned int inv = (100 - percent) *
                           (BRIGHTNESS_9BIT_MAX - BRIGHTNESS_9BIT_DEFAULT);
        /* /60 without libgcc divmod: */
        unsigned int q = 0;
        while (inv >= 60) { inv -= 60; q++; }
        level9 = BRIGHTNESS_9BIT_MAX - q;
    }
    hb_brightness_set((uint16_t)(level9 << 7));
}
