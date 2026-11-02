# Optional ports stay outside default builds/releases. Prepare the matching local
# IPA with tools/build_ios_game.py, then ./start package/install GAME normally.
# Set APP_NAME, ENTRY (the port/nano/app source stem), and ENGINE (the static
# library stem). port/nano builds port/build/nano/lib<ENGINE>_nano.a.
NANOAPPS := $(abspath ../..)
RELOC := 1
SRCS := port/nano/app/$(ENTRY).c $(NANOAPPS)/sdk/hb_surface_input.c
EXTRA_CFLAGS += -DHB_GL_SURFACE_APP -DHB_LV_RELOC
CHECK_NO_THUMB_FP := 1
GAME_LIBRARY := $(CURDIR)/port/build/nano/lib$(ENGINE)_nano.a
include ../../sdk/hb_app.mk
NEWLIB_FLAGS := -marm -march=armv5te+fp -mfloat-abi=softfp
LDLIBS := $(GAME_LIBRARY) \
          $(shell $(CC) $(NEWLIB_FLAGS) -print-file-name=libm.a) \
          $(shell $(CC) $(NEWLIB_FLAGS) -print-file-name=libc.a) $(LIBGCC) \
          -Wl,--wrap=memcpy,--wrap=memmove,--wrap=memset
$(BUILD)/$(APP_NAME).reloc.elf: $(GAME_LIBRARY) ../ios_app_path.h
.PHONY: game-library
$(GAME_LIBRARY): game-library
	$(MAKE) -C port/nano NANOAPPS=$(NANOAPPS) RECOMP=$(CURDIR)/build/recomp
