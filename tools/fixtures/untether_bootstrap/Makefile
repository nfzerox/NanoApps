# Historical CFF-stack bootstrap fixture for emulator regression tests only.
# The shipped firmware uses system/disk_bootstrap.
# Heap-resident CFF bootstrap and its small position-independent stack stage.
# The CFF decoder writes the stage only into its active caller frame.  The stage
# allocates an OS-heap arena and relocates untether_bootstrap.hbapp into it.
APP_NAME := untether_bootstrap
SRCS := cff_entry.S untether_bootstrap.c
RELOC := 1
# Like system/disk_bootstrap: a firmware-embedded loader, without the heavy-image
# gate and chunked reads (see sdk/hb_reloc_file.c).
EXTRA_CFLAGS += -DHB_RELOC_BOOT_ONLY
include ../../../sdk/hb_app.mk

STAGE_ELF := build/cff_heap_stage.elf
STAGE_BIN := build/cff_heap_stage.bin

all: $(STAGE_BIN)

$(STAGE_ELF): cff_heap_stage.S | build
	$(CC) -mcpu=cortex-a8 -mthumb -nostdlib -Wl,--build-id=none \
		-Wl,-Ttext,0 -Wl,-e,cff_heap_stage -o $@ $<

$(STAGE_BIN): $(STAGE_ELF)
	$(OBJCOPY) -O binary $< $@
	@echo "=== $@ ===" && wc -c $@
