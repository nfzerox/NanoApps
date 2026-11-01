#!/bin/bash
# dis.sh <hexVA> <bytes> [arm|thumb] — disassemble RetailOS 1.1.2 at a virtual address.
#
# The image is a flat load at 0x08000000, so file offset = VA - 0x08000000.
# The firmware is mixed ARM/Thumb: 0x0841xxxx is Thumb, 0x0862xxxx is ARM, but
# pass the mode explicitly rather than guessing.
IMG=/mnt/c/src/ipod/artifacts/firmware/decrypted-112/osos.live.bin
BASE=$((0x08000000))
OD=/mnt/c/src/ipod/artifacts/linux-n31/tinyalsa-build/arm-linux-musleabi-cross/bin/arm-linux-musleabi-objdump

VA=$1
N=${2:-128}
MODE=${3:-thumb}
OFF=$(( VA - BASE ))

FLAGS="-b binary -m arm --adjust-vma=$VA --start-address=$VA --stop-address=$((VA+N)) -D"
[ "$MODE" = thumb ] && FLAGS="$FLAGS -M force-thumb"

dd if="$IMG" of=/tmp/_dis.bin bs=1 skip=$OFF count=$N status=none
"$OD" $FLAGS /tmp/_dis.bin 2>/dev/null | sed -n '7,$p'
