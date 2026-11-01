APP_NAME     := bigload
SRCS         := bigload.c blob.S
RAW_SURFACE  := 1
EXTRA_CFLAGS += -Ibuild
include ../../sdk/hb_app.mk

build/blob.bin build/blob_expected.h: gen_blob.py
	$(PYTHON) gen_blob.py build

$(BUILD)/$(APP_NAME).reloc.elf: build/blob.bin build/blob_expected.h
