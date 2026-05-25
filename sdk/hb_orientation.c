/*
 * hb_orientation.c — query / set screen rotation.
 *
 * Built on top of hb_settings: the OS persists rotation in the
 * "General.UIRotation" preference (degrees: 0, 90, 180, 270).
 *
 * Homebrew apps that draw directly to the LCD (via hb_fill_rect etc)
 * see the raw 240×432 framebuffer regardless of rotation. If you
 * want to follow the OS rotation, read hb_orientation_get() and
 * transform your draw coordinates yourself.
 */

#include "hb_sdk.h"

#define ROTATION_CHANGE_ADDR (0x08240ab8u | 1u)
#define SILVER_GET_ADDR      (0x083fb524u | 1u)

typedef void *(*silver_get_t)(void);
typedef void  (*rotation_change_t)(void *cntlr);

static void apply_rotation_change(void)
{
    void *cntlr = ((silver_get_t)SILVER_GET_ADDR)();
    if (cntlr) {
        ((rotation_change_t)ROTATION_CHANGE_ADDR)(cntlr);
    }
}

/* Returns current rotation in degrees: 0, 90, 180, 270.
   Defaults to 0 if the setting hasn't been written yet. */
int32_t hb_orientation_get(void)
{
    int32_t v = hb_settings_get_int(HB_PREF_GENERAL_UI_ROTATION, 0);
    /* Normalize negative / wrap */
    while (v < 0)    v += 360;
    while (v >= 360) v -= 360;
    /* Snap to 90-degree increments */
    if      (v >= 315 || v < 45)  return 0;
    else if (v < 135)             return 90;
    else if (v < 225)             return 180;
    else                          return 270;
}

/* Write the rotation setting and ask the OS UI to apply it.
   degrees must be one of {0, 90, 180, 270}; other values normalized. */
void hb_orientation_set(int32_t degrees)
{
    while (degrees < 0)    degrees += 360;
    while (degrees >= 360) degrees -= 360;
    hb_settings_set_int(HB_PREF_GENERAL_UI_ROTATION, degrees);
    apply_rotation_change();
}

/* Tell the OS to query the accelerometer and rotate if needed.
   Useful for homebrew apps that want to react to physical tilt. */
void hb_orientation_apply_from_accel(void)
{
    apply_rotation_change();
}
