# Stable, exact-bound stage-2 loader. The firmware cave loads this one small
# image; it waits for and verifies /Library/resident.hbapp.
APP_NAME := disk_bootstrap
SRCS     := disk_bootstrap.c
RELOC    := 1
# The firmware-embedded loader stays exactly as shipped (see hb_reloc_file.c).
EXTRA_CFLAGS += -DHB_RELOC_BOOT_ONLY
# Keep this firmware-loaded image below the deferred stage's 16-bit span limit.
# In particular, do not pass the whole SDK to the linker: some GNU ld versions
# retain hb_image.c's unused 64 KiB BMP scratch buffer despite --gc-sections.
# Same relative order as SDK_SRCS, so toolchains that already discarded the
# unused modules still produce a byte-identical image.
SDK_SRCS_ONLY := \
    ../../sdk/hb_fs.c \
    ../../sdk/hb_heap.c \
    ../../sdk/hb_trace.c \
    ../../sdk/hb_shared.c \
    ../../sdk/hb_silver_uitask.c \
    ../../sdk/hb_reloc.c \
    ../../sdk/hb_reloc_file.c
include ../../sdk/hb_app.mk
