NANOAPPS ?= $(abspath ../../../..)
CC := arm-none-eabi-gcc
AR := arm-none-eabi-ar
BUILD := ../build/nano
ARCH := -marm -mcpu=cortex-a5 -mfloat-abi=softfp -mfpu=vfpv3-d16 -mword-relocations
# The translated engine (RECOMP, made by tools/recomp.py from the original executable) keeps
# the guest stack pointer and memory base in r10 and r11; see port/src/rt.h.
RECOMP ?= $(HOME)/nano-tr/recomp
CFLAGS := $(ARCH) -O2 -ffp-contract=off -std=gnu99 -Wall -ffunction-sections -fdata-sections \
          -fno-common -fno-strict-aliasing -fwrapv -fno-builtin -MMD -MP -DAB_NANO -DPORT_ARENA_SIZE=0x330000u \
          -I$(NANOAPPS)/sdk -I../src -I$(RECOMP)
ENGINE := game glfe rt rt_gl
PLATFORM := nano_platform nano_alloc nano_libc nano_memory
GENERATED := $(basename $(notdir $(wildcard $(RECOMP)/recomp_*.c)))
OBJS := $(ENGINE:%=$(BUILD)/src/%.o) $(PLATFORM:%=$(BUILD)/nano/%.o) $(GENERATED:%=$(BUILD)/recomp/%.o)
all: $(BUILD)/libtr_nano.a
$(BUILD)/libtr_nano.a: $(OBJS)
	$(AR) crs $@ $(OBJS)
	@arm-none-eabi-size -t $@ | tail -1
$(BUILD)/src/%.o: ../src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/recomp/%.o: $(RECOMP)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@
$(BUILD)/nano/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
-include $(OBJS:.o=.d)
.PHONY: all
