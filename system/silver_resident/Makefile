# Resident Silver hook module — a relocatable .hbapp. The boot stub
# (system/disk_bootstrap) loads it into its own OS-heap block once per boot, so
# it owns no fixed DRAM (0x0910xxxx-0x094xxxxx is all live OS heap). Its .bss
# (trampolines, vtable copies, trace ring, shared app state) is part of that
# block; the stub zeroes it before calling payload_entry.
APP_NAME := silver_resident
SRCS     := silver_resident.c bluetooth_fix.c dock_fix.c ../../sdk/hb_app_catalog.c
RELOC    := 1
EXTRA_CFLAGS += -DHB_RAWMSG_TRACE
include ../../sdk/hb_app.mk
