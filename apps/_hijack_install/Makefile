# Hijack installer — opt-in. Patches the OS to redirect home-screen
# "Podcasts" tap to the Homebrew launcher.
#
# This Makefile depends on _hijack_stub being built first (its .bin
# is read at runtime from /Apps/.hijack_stub).

APP_NAME := _hijack_install
SRCS     := install.c

include ../../sdk/hb_app.mk

# Make sure the stub is built (this app depends on the stub binary
# being deployed alongside, but doesn't link with it).
$(BUILD)/_hijack_install.elf: stub_check

.PHONY: stub_check
stub_check:
	@if [ ! -f ../_hijack_stub/build/_hijack_stub.bin ]; then \
	    echo "Building _hijack_stub first..."; \
	    $(MAKE) -s -C ../_hijack_stub; \
	fi
