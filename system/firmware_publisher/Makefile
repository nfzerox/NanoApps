CC := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
ROOT := $(CURDIR)
BUILD := build
.PHONY: all clean
all: $(BUILD)/recorder_publisher.bin
$(BUILD):
	mkdir -p $@
$(BUILD)/scsi-stub.bin: ../../firmware/scsi_shellcode/src/main.rs ../../firmware/scsi_shellcode/Cargo.toml | $(BUILD)
	cd ../../firmware/scsi_shellcode && cargo build --release --target thumbv6m-none-eabi
	$(OBJCOPY) -O binary ../../firmware/scsi_shellcode/target/thumbv6m-none-eabi/release/scsi_shellcode $@
$(BUILD)/recorder_publisher.bin: recorder_publisher.S recorder_publisher.ld $(BUILD)/scsi-stub.bin
	cd $(BUILD) && $(CC) -mcpu=cortex-a5 -mthumb -nostdlib -Wl,--build-id=none -Wl,-T,$(ROOT)/recorder_publisher.ld -o publisher.elf $(ROOT)/recorder_publisher.S
	$(OBJCOPY) -O binary $(BUILD)/publisher.elf $@
clean:
	rm -rf $(BUILD)
