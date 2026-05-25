# Parametric load stub — linked at 0x09100000 (NOT the standard
# app VA). Reads target path from the global at 0x09100400, loads
# the file to 0x0867f8e4, BLX's.
#
# Used by hb_app_load_and_exec as the trampoline that performs the
# self-overwriting load without crashing.

APP_NAME := _load_stub
SRCS     := load_stub.c
LINK_VA  := 0x09100000

include ../../sdk/hb_app.mk
