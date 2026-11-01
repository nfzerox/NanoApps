# A small replacement resident for the existing 39A10023 bootstrap. It records
# heap metadata and leaves homebrew icons inert; it does not register apps.
APP_NAME := heap_diagnostic
SRCS     := heap_diagnostic.c
LINK_VA  := 0x09130000
BSS_VA  := 0x09132000
include ../../sdk/hb_app.mk
