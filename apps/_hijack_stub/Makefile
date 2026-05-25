# Hijack stub — runs at FIXED address 0x09130000.
#
# This binary lives in unused heap RAM. The hijack installer copies
# its bytes into 0x09130000 once, then patches the OS to `BL 0x09130000+1`
# (Thumb-bit set). When the user taps the hijacked icon (Podcasts) on the 
# home screen, control transfers here, we load /Apps/Homebrew.app/Homebrew,
# and BLX to it.
#
# Important: linked at 0x09130000 (NOT the standard app VA), so this
# binary CANNOT be exec'd via the normal SCSI run path.

APP_NAME := _hijack_stub
SRCS     := stub.c
LINK_VA  := 0x09130000

include ../../sdk/hb_app.mk
