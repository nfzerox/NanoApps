# hb_app.mk — common build rules for homebrew apps.
#
# Include this from apps/<myapp>/Makefile after setting APP_NAME and SRCS.
#
#     APP_NAME := my_app
#     SRCS     := my_app.c
#     include ../../sdk/hb_app.mk
#
# Opt into LVGL by setting LVGL_ENABLE := 1 before the include line.
# LVGL apps gain ~150 KB of code; the basic SDK is ~6 KB. Make sure
# your app actually wants pretty widgets before flipping the switch.
#
# Optional overrides:
#     LINK_VA  ?= 0x0867f8e4
#     EXTRA_CFLAGS ?=
#     LVGL_ENABLE ?= 0       (1 = link LVGL + integration layer)
#     HB_CODE_MODE ?= thumb   (thumb or arm)
#     HB_FLOAT_ABI ?= soft    (soft keeps Thumb code free of FP instructions)
#     HB_FPU ?= neon
#     CHECK_NO_THUMB_FP ?= 0  (1 = reject VFP/NEON in every Thumb region)
#     THUMB_FP_ALLOW ?=       (diagnostic-only exact function allowlist)

SDK_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_DIR := $(abspath $(SDK_DIR)/..)
LVGL_DIR := $(REPO_DIR)/lvgl

SDK_SRCS := \
    $(SDK_DIR)/hb_mipi.c \
    $(SDK_DIR)/hb_gpu.c \
    $(SDK_DIR)/hb_text.c \
    $(SDK_DIR)/hb_font.c \
    $(SDK_DIR)/hb_touch.c \
    $(SDK_DIR)/hb_touch_mb.c \
    $(SDK_DIR)/hb_button.c \
    $(SDK_DIR)/hb_fs.c \
    $(SDK_DIR)/hb_ui.c \
    $(SDK_DIR)/hb_kb.c \
    $(SDK_DIR)/hb_audio.c \
    $(SDK_DIR)/hb_paged_list.c \
    $(SDK_DIR)/hb_brightness.c \
    $(SDK_DIR)/hb_rtc.c \
    $(SDK_DIR)/hb_battery.c \
    $(SDK_DIR)/hb_prefs.c \
    $(SDK_DIR)/hb_accel.c \
    $(SDK_DIR)/hb_media.c \
    $(SDK_DIR)/hb_orientation.c \
    $(SDK_DIR)/hb_image.c \
    $(SDK_DIR)/hb_screenshot.c \
    $(SDK_DIR)/hb_heap.c \
    $(SDK_DIR)/hb_record.c \
    $(SDK_DIR)/hb_sysfx.c \
    $(SDK_DIR)/hb_trace.c \
    $(SDK_DIR)/hb_shared.c \
    $(SDK_DIR)/hb_time.c \
    $(SDK_DIR)/hb_fpu.c \
    $(SDK_DIR)/hb_silver_app.c \
    $(SDK_DIR)/hb_silver_res.c \
    $(SDK_DIR)/hb_silver_uitask.c \
    $(SDK_DIR)/hb_silver_icon.c \
    $(SDK_DIR)/hb_silver_gfx.c \
    $(SDK_DIR)/hb_compositor.c \
    $(SDK_DIR)/gl.c \
    $(SDK_DIR)/hb_reloc.c \
    $(SDK_DIR)/hb_reloc_file.c \
    $(SDK_DIR)/hb_app_bundle.c

# Most apps use the complete SDK above and let --gc-sections discard unused
# modules. Small boot-critical images may provide SDK_SRCS_ONLY before
# including this file so their layout does not depend on host linker GC.
ifneq ($(strip $(SDK_SRCS_ONLY)),)
  SDK_SRCS := $(SDK_SRCS_ONLY)
endif

CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
SIZE    := arm-none-eabi-size
AR      := arm-none-eabi-ar
PYTHON  ?= python3

LVGL_ENABLE ?= 0
# LV_SURFACE := 1 — a fixed-VA LVGL Silver-app (hb_lv_surface runtime + the
# app's lv_app_main()); the resident loads it to 0x09280000 and drives it. Pulls
# in the runtime, forces LVGL, and parks .bss below the 0x09200000 compositor
# heap so the live OS can't stomp lv_global.
# RELOC := 1 — produce a relocatable .hbapp (vs a flat fixed-VA .bin); set by the
# surface app modes below. The resident loads a .hbapp into an operator-new arena.
RELOC ?= 0

# RAW_SURFACE := 1 — a direct-draw Silver app (hb_raw_surface runtime): same
# surface plumbing as an LVGL app (HBAppKind 1, 60fps composite, real touch +
# lifecycle) but NO LVGL — the app draws pixels straight into the framebuffer
# (fast immediate-mode painting / pixel games). App provides hb_raw_init/_frame.
RAW_SURFACE ?= 0
ifeq ($(RAW_SURFACE),1)
  RELOC := 1
  SRCS += $(SDK_DIR)/hb_raw_surface.c $(SDK_DIR)/hb_surface_input.c
  EXTRA_CFLAGS += -DHB_LV_RELOC
endif

# GL_SURFACE := 1 — a hardware-GL Silver app (HB_APP_KIND_GL_SURFACE). Links the GL
# surface runtime + the GLES 1.1 bindings (hb_gl.c/gl.c, already in SDK_SRCS); the
# app provides gl_app_init/gl_app_frame. No LVGL. Relocatable like LVGL surface apps.
GL_SURFACE ?= 0
ifeq ($(GL_SURFACE),1)
  RELOC := 1
  # shared touch source, available if a GL app wants it (gc-sections drops it if not)
  SRCS += $(SDK_DIR)/hb_gl_surface.c $(SDK_DIR)/hb_surface_input.c
  EXTRA_CFLAGS += -DHB_GL_SURFACE_APP -DHB_LV_RELOC
endif

LV_SURFACE ?= 0
ifeq ($(LV_SURFACE),1)
  RELOC := 1
  LVGL_ENABLE := 1
  SRCS += $(SDK_DIR)/hb_lv_surface.c $(SDK_DIR)/hb_blob_mem.c $(SDK_DIR)/hb_surface_input.c \
          $(SDK_DIR)/hb_glyph.c $(SDK_DIR)/generated/hb_glyphs.c
  # HB_LV_SURFACE_APP: HB_APP_ENTRY -> lv_app_main. HB_LV_RELOC: LVGL's TLSF pool
  # is its own in-.bss static array (LV_MEM_ADR=0), so the whole app — code +
  # pool — is one relocatable image the resident loads into an operator-new
  # arena (no fixed VA). See tools/mkrelocapp.py + sdk/hb_reloc.c.
  EXTRA_CFLAGS += -DHB_LV_SURFACE_APP -DHB_LV_RELOC
  # LV_COMPOSITOR := 1 — render into a compositor-addressable GPU pixel buffer and (stage 1b+)
  # offload fills/blits to the GPU hardware 2D engine. Off by default.
  ifeq ($(LV_COMPOSITOR),1)
    EXTRA_CFLAGS += -DHB_LV_COMPOSITOR
    SRCS += $(SDK_DIR)/lv_draw_compositor.c
  endif
endif

# LV_PERF := 1 — opt into LVGL perf instrumentation (sysmon + FPS/CPU/mem
# overlays; lv_demo_benchmark needs the sysmon subject). Off by default (per-frame
# overhead). Uses a SEPARATE LVGL archive (build_n7g_perf) so it doesn't taint the
# regular apps' shared archive.
LV_PERF ?= 0
ifeq ($(LV_PERF),1)
  EXTRA_CFLAGS += -DHB_LV_PERF
endif

# All apps load at a high-DRAM window starting at 0x09280000 — far
# above the SDK's high-RAM allocations (stub 0x09100000, file cache
# 0x09118000, trace 0x09120000, shadow FB 0x09140000–0x09180000,
# LVGL pool/buf 0x09190000–0x091E8000, .bss 0x09200000) and clear
# of OS .text. One load address means one loader path for everything:
# every app — LVGL or legacy SDK — goes through tools/push_lvgl.sh
# and the same _lvgl_loader stub.
LINK_VA ?= 0x09280000
# .bss parks at 0x09200000 by default; override (e.g. below the live compositor
# heap) for coexist binaries that run alongside the OS.
BSS_VA ?= 0x09200000

HB_CODE_MODE ?= thumb
HB_FLOAT_ABI ?= soft
HB_FPU ?= neon
CHECK_NO_THUMB_FP ?= 0
THUMB_FP_ALLOW ?=
HB_ARCH_FLAGS ?= -mcpu=cortex-a8 -m$(HB_CODE_MODE) \
                 -mfloat-abi=$(HB_FLOAT_ABI) -mfpu=$(HB_FPU)

CFLAGS := \
    $(HB_ARCH_FLAGS) \
    -fno-pic -fno-builtin -ffreestanding -nostdlib \
    -fno-jump-tables -fno-common -fno-exceptions \
    -Os -Wall -Wextra -fdata-sections -ffunction-sections \
    -fno-strict-aliasing \
    -I$(SDK_DIR) \
    -I$(SDK_DIR)/freestanding \
    $(EXTRA_CFLAGS)

LDFLAGS := -Wl,--gc-sections -Wl,--build-id=none \
           -Wl,-T,$(SDK_DIR)/hb_app.ld \
           -Wl,--defsym=LINK_VA=$(LINK_VA) -Wl,--defsym=BSS_VA=$(BSS_VA) -nostdlib

# libgcc supplies ARM EABI helpers (__aeabi_idiv, __aeabi_uidivmod,
# __aeabi_lmul, etc). The basic SDK rarely needs them, but anything
# touching division or 64-bit math (notably LVGL) does. Linking libgcc
# after -nostdlib gets the helpers without pulling in libc.
LIBGCC := $(shell $(CC) $(HB_ARCH_FLAGS) -print-libgcc-file-name)
LDLIBS := $(LIBGCC)

ifeq ($(LVGL_ENABLE),1)
  # LVGL paths: lv_conf.h lives in sdk/, lvgl headers under lvgl/include.
  LVGL_CPPFLAGS := -DLV_CONF_INCLUDE_SIMPLE \
                   -DLV_CONF_PATH=\"$(SDK_DIR)/lv_conf.h\" \
                   -I$(LVGL_DIR) \
                   -I$(LVGL_DIR)/include \
                   -I$(LVGL_DIR)/src
  LVGL_QUIET_WARNS := -Wno-unused-parameter -Wno-sign-compare \
                      -Wno-maybe-uninitialized -Wno-implicit-fallthrough \
                      -Wno-unused-variable -Wno-stringop-overread
  CFLAGS += $(LVGL_CPPFLAGS) $(LVGL_QUIET_WARNS)

  # Grab LVGL sources by glob — relying on LV_USE_* gates inside each
  # file plus -ffunction-sections+--gc-sections to drop unused code.
  # Drivers / heavyweight libs are explicitly omitted (the host-side
  # backends would never link against our freestanding toolchain).
  LVGL_INCLUDE_DIRS := \
      core display draw font indev layouts misc others stdlib themes tick widgets debugging
  LVGL_SRCS := $(foreach d,$(LVGL_INCLUDE_DIRS),$(wildcard $(LVGL_DIR)/src/$(d)/*.c)) \
               $(foreach d,$(LVGL_INCLUDE_DIRS),$(wildcard $(LVGL_DIR)/src/$(d)/*/*.c)) \
               $(foreach d,$(LVGL_INCLUDE_DIRS),$(wildcard $(LVGL_DIR)/src/$(d)/*/*/*.c)) \
               $(foreach d,$(LVGL_INCLUDE_DIRS),$(wildcard $(LVGL_DIR)/src/$(d)/*/*/*/*.c)) \
               $(LVGL_DIR)/src/lv_init.c \
               $(LVGL_DIR)/src/libs/bin_decoder/lv_bin_decoder.c \
               $(LVGL_DIR)/src/libs/qrcode/lv_qrcode.c \
               $(LVGL_DIR)/src/libs/qrcode/qrcodegen.c \
               $(LVGL_DIR)/src/libs/barcode/lv_barcode.c \
               $(LVGL_DIR)/src/libs/barcode/code128.c \
               $(LVGL_DIR)/src/osal/lv_os.c \
               $(LVGL_DIR)/src/osal/lv_os_none.c

  # Strip out files that pull in host-only headers even with LV_USE_X=0.
  LVGL_SRCS := $(filter-out \
      $(LVGL_DIR)/src/osal/lv_cmsis_rtos2.c \
      $(LVGL_DIR)/src/osal/lv_freertos.c \
      $(LVGL_DIR)/src/osal/lv_linux.c \
      $(LVGL_DIR)/src/osal/lv_mqx.c \
      $(LVGL_DIR)/src/osal/lv_pthread.c \
      $(LVGL_DIR)/src/osal/lv_rtthread.c \
      $(LVGL_DIR)/src/osal/lv_sdl2.c \
      $(LVGL_DIR)/src/osal/lv_windows.c \
      , $(LVGL_SRCS))

  # Pre-built LVGL archive shared across all LVGL apps. .o files are
  # cached in $(LVGL_DIR)/build_n7g/ so the second app's build is fast.
  # The integration layer (sdk/hb_lvgl.c) is NOT in the archive — apps
  # link it directly so changes there don't force an archive rebuild.
  # Perf-instrumented apps compile LVGL differently (sysmon), so they get their
  # own archive — never mix -DHB_LV_PERF .o files into the regular one.
  LVGL_BUILD := $(LVGL_DIR)/build_n7g$(if $(filter 1,$(LV_PERF)),_perf)
  LVGL_AR    := $(LVGL_BUILD)/liblvgl.a
  LVGL_OBJS  := $(patsubst $(LVGL_DIR)/%.c,$(LVGL_BUILD)/%.o,$(LVGL_SRCS))

  # LVGL apps are relocatable surface apps: the hb_lv_surface runtime (added via
  # SRCS in the LV_SURFACE block) owns display/input/lifecycle, so no takeover
  # hb_lvgl.c / hb_ui.c here.
  SDK_SRCS += $(SDK_DIR)/hb_t9.c $(SDK_DIR)/hb_swipe_row.c \
              $(SDK_DIR)/hb_status_bar.c \
              $(SDK_DIR)/hb_confirm.c $(SDK_DIR)/hb_vlist.c
  LDLIBS_LVGL := $(LVGL_AR)
  # Surface apps don't use the basic-SDK input layer (hb_ui.c) — the hb_lv_surface
  # runtime owns the touch indev. (Non-LVGL apps keep hb_ui.c for hb_ui_poll.)
  ifeq ($(LV_SURFACE),1)
    SDK_SRCS := $(filter-out $(SDK_DIR)/hb_ui.c,$(SDK_SRCS))
  endif
endif

BUILD := build

# A surface app links to a RELOCATABLE blob (.hbapp): base-0 with -Wl,-q so
# mkrelocapp can extract the ABS32 reloc offsets; entry = payload_entry; no
# fixed-VA linker script. Everything else links fixed-VA to a flat .bin.
ifeq ($(RELOC),1)
  CFLAGS += -mword-relocations
  RELOC_LDFLAGS := $(HB_ARCH_FLAGS) -nostdlib \
                   -Wl,--gc-sections -Wl,--build-id=none \
                   -Wl,-q -Wl,-e,payload_entry -Wl,-Ttext,0
  APP_OUT := $(BUILD)/$(APP_NAME).hbapp
else
  APP_OUT := $(BUILD)/$(APP_NAME).bin
endif

.PHONY: all clean inspect

all: $(APP_OUT)

$(BUILD):
	@mkdir -p $(BUILD)

ifeq ($(LVGL_ENABLE),1)
$(LVGL_BUILD)/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $(notdir $<)"
	@$(CC) $(filter-out -I$(SDK_DIR),$(CFLAGS)) -I$(SDK_DIR) -c -o $@ $<

$(LVGL_AR): $(LVGL_OBJS)
	@echo "  AR  $@"
	@$(AR) crs $@ $^
endif

$(BUILD)/$(APP_NAME).elf: $(SRCS) $(SDK_SRCS) $(LDLIBS_LVGL) $(wildcard $(SDK_DIR)/*.h) $(wildcard *.h) $(SDK_DIR)/hb_app.ld $(REPO_DIR)/tools/check_no_thumb_fp.py | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(SDK_SRCS) $(LDLIBS_LVGL) $(LDLIBS)
	@if [ "$(CHECK_NO_THUMB_FP)" = "1" ]; then \
		$(PYTHON) $(REPO_DIR)/tools/check_no_thumb_fp.py \
			$(foreach s,$(THUMB_FP_ALLOW),--allow-symbol $(s)) $@; \
	fi
	@$(SIZE) $@
	@echo "linked at $(LINK_VA)"

$(BUILD)/$(APP_NAME).bin: $(BUILD)/$(APP_NAME).elf
	$(OBJCOPY) -O binary $< $@
	@echo "=== $@ ==="
	@wc -c $@

# Relocatable surface blob: link base-0 (-Wl,-q keeps relocations), then
# mkrelocapp flattens the image + emits the ABS32 reloc table as a .hbapp.
$(BUILD)/$(APP_NAME).reloc.elf: $(SRCS) $(SDK_SRCS) $(LDLIBS_LVGL) $(wildcard $(SDK_DIR)/*.h) $(wildcard *.h) $(REPO_DIR)/tools/check_no_thumb_fp.py | $(BUILD)
	$(CC) $(CFLAGS) $(RELOC_LDFLAGS) -o $@ $(SRCS) $(SDK_SRCS) $(LDLIBS_LVGL) $(LDLIBS)
	@if [ "$(CHECK_NO_THUMB_FP)" = "1" ]; then \
		$(PYTHON) $(REPO_DIR)/tools/check_no_thumb_fp.py \
			$(foreach s,$(THUMB_FP_ALLOW),--allow-symbol $(s)) $@; \
	fi
	@$(SIZE) $@

$(BUILD)/$(APP_NAME).hbapp: $(BUILD)/$(APP_NAME).reloc.elf
	$(PYTHON) $(REPO_DIR)/tools/mkrelocapp.py $< $@ payload_entry
	@echo "=== $@ ===" && wc -c $@

inspect: $(BUILD)/$(APP_NAME).elf
	arm-none-eabi-objdump -d -M force-thumb $<

clean:
	rm -rf $(BUILD)
