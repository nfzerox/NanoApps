/*
 * lv_conf_host.h — LVGL config for the desktop build.
 *
 * It is the device config, verbatim, with only the host display and input
 * drivers switched on. That is the point: if the host build used a different
 * colour depth, font set or widget set, the layout you tuned here would not be
 * the layout the iPod renders, and the whole reason for having a host target
 * would evaporate.
 */

#ifndef ENTRAIN_LV_CONF_HOST_H
#define ENTRAIN_LV_CONF_HOST_H

#include "../../../sdk/lv_conf.h"

/* The device build swaps in freestanding replacements for two libc headers it
   has no room for. The host has the real ones. */
#undef  LV_INTTYPES_INCLUDE
#define LV_INTTYPES_INCLUDE     <inttypes.h>
#undef  LV_LIMITS_INCLUDE
#define LV_LIMITS_INCLUDE       <limits.h>

/* Host display backends. SDL is what you develop against; fbdev and DRM are
   what the brief actually targets, and all three are compiled in so the same
   binary runs on a dev laptop and on a tinydrm panel. */
#undef  LV_USE_SDL
#define LV_USE_SDL              1
#undef  LV_SDL_INCLUDE_PATH
#define LV_SDL_INCLUDE_PATH     <SDL2/SDL.h>
#undef  LV_SDL_RENDER_MODE
#define LV_SDL_RENDER_MODE      LV_DISPLAY_RENDER_MODE_DIRECT
#undef  LV_SDL_BUF_COUNT
#define LV_SDL_BUF_COUNT        1
#undef  LV_SDL_FULLSCREEN
#define LV_SDL_FULLSCREEN       0
#undef  LV_SDL_DIRECT_EXIT
#define LV_SDL_DIRECT_EXIT      1
#undef  LV_SDL_MOUSEWHEEL_MODE
#define LV_SDL_MOUSEWHEEL_MODE  LV_SDL_MOUSEWHEEL_MODE_ENCODER

#undef  LV_USE_LINUX_FBDEV
#define LV_USE_LINUX_FBDEV      1
#undef  LV_LINUX_FBDEV_BSD
#define LV_LINUX_FBDEV_BSD      0
#undef  LV_LINUX_FBDEV_RENDER_MODE
#define LV_LINUX_FBDEV_RENDER_MODE LV_DISPLAY_RENDER_MODE_PARTIAL
#undef  LV_LINUX_FBDEV_BUFFER_COUNT
#define LV_LINUX_FBDEV_BUFFER_COUNT 0
#undef  LV_LINUX_FBDEV_BUFFER_SIZE
#define LV_LINUX_FBDEV_BUFFER_SIZE  60

/* DRM needs libdrm and gbm headers. The Makefile defines ENTRAIN_HAVE_DRM
   only when pkg-config finds them, so a machine without them still builds the
   SDL, fbdev and headless backends. */
#undef  LV_USE_LINUX_DRM
#ifdef ENTRAIN_HAVE_DRM
#define LV_USE_LINUX_DRM        1
#undef  LV_LINUX_DRM_GBM_FORMAT
#define LV_LINUX_DRM_GBM_FORMAT GBM_FORMAT_ARGB8888
#else
#define LV_USE_LINUX_DRM        0
#endif

#undef  LV_USE_EVDEV
#define LV_USE_EVDEV            1

/* The device build pins LVGL's TLSF pool to a fixed high-RAM address, which is
   a wild pointer here. Point it at LVGL's own static array instead — which is
   exactly what the device's relocatable surface build does.

   The SIZE deliberately stays at the device's 640 KB rather than being raised.
   If the UI ever exhausts LVGL's pool it should do so on the desktop, where
   there is a debugger, and not first on the iPod. */
#undef  LV_MEM_ADR
#define LV_MEM_ADR              0

/* Useful while developing the UI, off on the device. */
#undef  LV_USE_LOG
#define LV_USE_LOG              0

/* Nothing here needs the bundled demos. */
#undef  LV_USE_DEMO_WIDGETS
#define LV_USE_DEMO_WIDGETS         0
#undef  LV_USE_DEMO_BENCHMARK
#define LV_USE_DEMO_BENCHMARK       0
#undef  LV_USE_DEMO_STRESS
#define LV_USE_DEMO_STRESS          0
#undef  LV_USE_DEMO_KEYPAD_AND_ENCODER
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0

#endif /* ENTRAIN_LV_CONF_HOST_H */
