APP_NAME := flappybird
SRCS := fb_entry.c ../../sdk/hb_surface_input.c
RELOC := 1
EXTRA_CFLAGS += -DHB_GL_SURFACE_APP -DHB_LV_RELOC
CHECK_NO_THUMB_FP := 1
FB_DIR ?= $(abspath ../../ports/flappybird/port)
include ../../sdk/hb_app.mk
NEWLIB_FLAGS := -marm -march=armv5te+fp -mfloat-abi=softfp
LDLIBS := $(FB_DIR)/build/nano/libfb_nano.a \
          $(shell $(CC) $(NEWLIB_FLAGS) -print-file-name=libc.a) $(LIBGCC) \
          -Wl,--wrap=memcpy,--wrap=memmove,--wrap=memset
$(BUILD)/$(APP_NAME).reloc.elf: $(FB_DIR)/build/nano/libfb_nano.a
