APP_NAME := doodlejump
SRCS := dj_entry.c ../../sdk/hb_surface_input.c
RELOC := 1
EXTRA_CFLAGS += -DHB_GL_SURFACE_APP -DHB_LV_RELOC
CHECK_NO_THUMB_FP := 1
DJ_DIR ?= $(abspath ../../ports/doodlejump/port)
include ../../sdk/hb_app.mk
NEWLIB_FLAGS := -marm -march=armv5te+fp -mfloat-abi=softfp
LDLIBS := $(DJ_DIR)/build/nano/libdj_nano.a \
          $(shell $(CC) $(NEWLIB_FLAGS) -print-file-name=libc.a) $(LIBGCC) \
          -Wl,--wrap=memcpy,--wrap=memmove,--wrap=memset
$(BUILD)/$(APP_NAME).reloc.elf: $(DJ_DIR)/build/nano/libdj_nano.a
