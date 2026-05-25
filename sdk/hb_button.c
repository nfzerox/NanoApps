/*
 * hb_button.c — button state via the OS button-state cache.
 *
 * The OS keeps a static keyboard-state array. Each physical button has
 * a 24-byte entry; offset 0 holds a state flag byte whose bit 0 means
 * "currently down".
 *
 * Replaces a lower-level PMU polling path that was less reliable while
 * the OS was also using the bus.
 */

#include "hb_sdk.h"

#define SKEYS_BASE          0x089A54A0u
#define KEYDATA_SIZE        24u
#define KSTATE_DOWN_BIT     0x01u

#define KEY_HOME            1
#define KEY_PLAYPAUSE       6
#define KEY_POWER           9

static inline bool skey_down(unsigned idx)
{
    volatile uint8_t *p = (volatile uint8_t *)(SKEYS_BASE + idx * KEYDATA_SIZE);
    return (*p & KSTATE_DOWN_BIT) != 0;
}

/* Volume buttons: read raw GPIO because the OS button cache treats
   some transport buttons as mutually exclusive. Active-low:
   pressed = bit clear. */
#define GPIO_BASE        0x3CF00000u
#define GPIO_PORT_STRIDE 0x20u
#define PDAT_OFFSET      0x04u

static inline uint32_t pdat(uint32_t port)
{
    return *(volatile uint32_t *)(GPIO_BASE + port * GPIO_PORT_STRIDE + PDAT_OFFSET);
}

bool hb_button_pressed(hb_button_t btn)
{
    switch (btn) {
    case HB_BTN_VOL_UP:     return !(pdat(5) & (1u << 0));
    case HB_BTN_VOL_DOWN:   return !(pdat(5) & (1u << 1));
    case HB_BTN_HOME:       return skey_down(KEY_HOME);
    case HB_BTN_POWER:      return skey_down(KEY_POWER);
    case HB_BTN_PLAY_PAUSE: return skey_down(KEY_PLAYPAUSE);
    default:                return false;
    }
}
