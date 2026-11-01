NANOAPPS ?= $(abspath ../../../..)
CC := arm-none-eabi-gcc
AR := arm-none-eabi-ar
BUILD := ../build/nano
ARCH := -marm -mcpu=cortex-a5 -mfloat-abi=softfp -mfpu=vfpv3-d16 -mword-relocations
CFLAGS := $(ARCH) -O2 -ffp-contract=off -std=gnu99 -Wall -ffunction-sections -fdata-sections \
          -fno-common -fno-strict-aliasing -fno-builtin -MMD -MP -DAB_NANO \
          -I$(NANOAPPS)/sdk -I../src
ENGINE := gfx sprites game objects random
PLATFORM := nano_platform nano_alloc nano_libc nano_memory
OBJS := $(ENGINE:%=$(BUILD)/src/%.o) $(PLATFORM:%=$(BUILD)/nano/%.o)
all: $(BUILD)/libdj_nano.a
$(BUILD)/libdj_nano.a: $(OBJS)
	$(AR) crs $@ $(OBJS)
	@arm-none-eabi-size -t $@ | tail -1
$(BUILD)/src/%.o: ../src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/nano/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
-include $(OBJS:.o=.d)
.PHONY: all
