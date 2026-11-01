/*
 * lv_conf_n31.h — LVGL config for the N31 Linux port.
 *
 * Same base as the device build, so the widget set, the fonts and the 32-bit
 * colour depth are identical and the layout is the one already checked in the
 * screenshots. Only the plumbing differs: this target renders to /dev/fb0,
 * which the kernel already exposes at exactly 240x432, 32 bpp.
 *
 * fbdev rather than DRM on purpose. The DRM path needs libdrm and gbm, and
 * this binary has to be fully static against musl to run on a 26 MB initramfs
 * with no shared libraries to speak of. fbdev needs nothing but an ioctl.
 */

#ifndef ENTRAIN_LV_CONF_N31_H
#define ENTRAIN_LV_CONF_N31_H

#include "../../../sdk/lv_conf.h"

/* The device build swaps in freestanding replacements for two libc headers.
   musl has the real ones. */
#undef  LV_INTTYPES_INCLUDE
#define LV_INTTYPES_INCLUDE     <inttypes.h>
#undef  LV_LIMITS_INCLUDE
#define LV_LIMITS_INCLUDE       <limits.h>

/* The landscape readout draws its text on rotated layers, and LVGL renders a
   transformed layer into an alpha format before compositing it. The shared SDK
   config compiles ARGB8888 out - correct for every other screen in the repo,
   which is opaque XRGB8888 throughout - and without it a rotated layer draws
   nothing at all. Turned on here rather than in sdk/lv_conf.h so that one
   screen in one app does not grow every app in the repo.
*/
#undef  LV_DRAW_SW_SUPPORT_ARGB8888
#define LV_DRAW_SW_SUPPORT_ARGB8888 1


/* LVGL's TLSF pool is pinned to a fixed high-RAM address in the RetailOS
   build, which is a wild pointer under Linux. Use LVGL's own static array —
   the same thing the relocatable surface build does. Size stays at the
   device's 640 KB: this machine has 55 MB total and about 15 MB free, so
   there is no room to be generous and no reason to be. */
#undef  LV_MEM_ADR
#define LV_MEM_ADR              0

/* No SDL, no DRM, no GPU. Framebuffer and evdev only. */
#undef  LV_USE_SDL
#define LV_USE_SDL              0
/*
 * DRM, because the kernel driver is a real DRM/KMS driver and its fbdev
 * emulation is capped by fb_deferred_io's timer - 75 compositor kicks for 150
 * repaints, measured, where the DRM path gives one kick per commit at 64 fps.
 *
 * The note this replaces said the DRM path "needs libdrm and gbm" and was half
 * wrong: GBM is opt-in behind LV_USE_LINUX_DRM_GBM_BUFFERS and LVGL uses dumb
 * buffers without it, so Mesa never enters into it. libdrm alone is five files
 * with no dependency beyond libc - see tools/linux-n31/build-n31-libdrm.sh in
 * the ipod repo - and links statically like everything else here.
 *
 * fbdev stays compiled in as the fallback: a kernel without the DRM driver
 * must not mean a black screen.
 */
#undef  LV_USE_LINUX_DRM
#define LV_USE_LINUX_DRM        1

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

#undef  LV_USE_EVDEV
#define LV_USE_EVDEV            1

/* The SoC is a Cortex-A8 without NEON — the CPU reports vfpv3/vfpv4 and no
   neon flag — so LVGL's assembly blend paths must stay off. They are already
   off in the base config; this is here so nobody turns them on by accident. */
#undef  LV_USE_DRAW_SW_ASM
#define LV_USE_DRAW_SW_ASM      LV_DRAW_SW_ASM_NONE

/* 30 Hz is plenty for this UI and leaves the CPU to the audio thread, which
   matters much more here than a faster ring animation. */
#undef  LV_DEF_REFR_PERIOD
#define LV_DEF_REFR_PERIOD      33

#undef  LV_USE_LOG
#define LV_USE_LOG              0

#undef  LV_USE_DEMO_WIDGETS
#define LV_USE_DEMO_WIDGETS         0
#undef  LV_USE_DEMO_BENCHMARK
#define LV_USE_DEMO_BENCHMARK       0
#undef  LV_USE_DEMO_STRESS
#define LV_USE_DEMO_STRESS          0
#undef  LV_USE_DEMO_KEYPAD_AND_ENCODER
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0

#endif /* ENTRAIN_LV_CONF_N31_H */
