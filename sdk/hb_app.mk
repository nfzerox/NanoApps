# hb_app.mk — common build rules for homebrew apps.
#
# Include this from apps/<myapp>/Makefile after setting APP_NAME and SRCS.
#
#     APP_NAME := my_app
#     SRCS     := my_app.c
#     include ../../sdk/hb_app.mk
#
# Optional overrides:
#     LINK_VA  ?= 0x0867f8e4
#     EXTRA_CFLAGS ?=

SDK_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

PUBLIC_SDK_SRCS := \
    $(SDK_DIR)/hb_mipi.c \
    $(SDK_DIR)/hb_text.c \
    $(SDK_DIR)/hb_font.c \
    $(SDK_DIR)/hb_touch.c \
    $(SDK_DIR)/hb_button.c \
    $(SDK_DIR)/hb_fs.c \
    $(SDK_DIR)/hb_ui.c \
    $(SDK_DIR)/hb_kb.c \
    $(SDK_DIR)/hb_audio.c \
    $(SDK_DIR)/hb_paged_list.c \
    $(SDK_DIR)/hb_brightness.c \
    $(SDK_DIR)/hb_rtc.c \
    $(SDK_DIR)/hb_battery.c \
    $(SDK_DIR)/hb_helvetica.c \
    $(SDK_DIR)/hb_settings.c \
    $(SDK_DIR)/hb_accel.c \
    $(SDK_DIR)/hb_media.c \
    $(SDK_DIR)/hb_app_loader.c \
    $(SDK_DIR)/hb_orientation.c \
    $(SDK_DIR)/hb_image.c \
    $(SDK_DIR)/hb_screenshot.c \
    $(SDK_DIR)/hb_trace.c \
    $(SDK_DIR)/generated/hb_font_helvetica.c

SDK_SRCS := $(PUBLIC_SDK_SRCS)
SDK_RELEASE_CFLAG := -DNANOAPPS_RELEASE=1

CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
SIZE    := arm-none-eabi-size

LINK_VA ?= 0x0867f8e4

CFLAGS := \
    -mcpu=cortex-a8 -mthumb -mfpu=neon \
    -fno-pic -fno-builtin -ffreestanding -nostdlib \
    -fno-jump-tables -fno-common -fno-exceptions \
    -Os -Wall -Wextra -fdata-sections -ffunction-sections \
    -fno-strict-aliasing \
    -I$(SDK_DIR) \
    $(SDK_RELEASE_CFLAG) \
    $(EXTRA_CFLAGS)

LDFLAGS := -Wl,--gc-sections -Wl,--build-id=none \
           -Wl,-T,$(SDK_DIR)/hb_app.ld \
           -Wl,--defsym=LINK_VA=$(LINK_VA) -nostdlib

BUILD := build

.PHONY: all clean inspect

all: $(BUILD)/$(APP_NAME).bin

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/$(APP_NAME).elf: $(SRCS) $(SDK_SRCS) $(SDK_DIR)/hb_sdk.h $(SDK_DIR)/hb_app.ld | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(SDK_SRCS)
	@$(SIZE) $@
	@echo "linked at $(LINK_VA)"

$(BUILD)/$(APP_NAME).bin: $(BUILD)/$(APP_NAME).elf
	$(OBJCOPY) -O binary $< $@
	@echo "=== $@ ==="
	@wc -c $@

inspect: $(BUILD)/$(APP_NAME).elf
	arm-none-eabi-objdump -d -M force-thumb $<

clean:
	rm -rf $(BUILD)
