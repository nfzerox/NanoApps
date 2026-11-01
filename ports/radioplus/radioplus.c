/*
 * radioplus.c — Radio+ on the iPod under RetailOS.
 *
 * The whole app is elsewhere. This is the entry point, the frame callback and
 * the wake lock, which is all the SDK asks for; the screens are in ui.c and
 * read a model that this file never touches.
 *
 * The tuner is not driven on this platform yet, and the reason is in
 * model_device.c: the OS Bluetooth stack owns the HCI transport the tuner is
 * reached through, and HCI is flow controlled, so injecting commands underneath
 * it risks stalling its command queue. What runs here is the interface, the
 * settings and the presets - all of which are the same code the Linux build
 * uses, which is the point of having a model layer at all.
 */

#include "hb_sdk.h"
#include "lvgl/lvgl.h"

#include "ui.h"
#include "model.h"

/* The model has nothing to poll on this platform, but the screens still animate
   and the clock in a recording timer still moves, so the refresh runs at a
   sensible rate rather than every frame. */
#define REFRESH_MS 200

static uint32_t s_last_ms;

static void on_tick(void)
{
    uint32_t now = hb_time_uptime_ms();
    if (now - s_last_ms < REFRESH_MS) return;
    s_last_ms = now;

    rp_model_refresh();
    rp_ui_tick();
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    /* Build the model before the screens, so the first frame draws real
       settings rather than defaults that are replaced a moment later. */
    rp_model_refresh();
    rp_ui_init();

    hb_lv_set_frame_cb(on_tick);
}
