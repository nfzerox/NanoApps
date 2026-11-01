#!/bin/sh
# Build the game logic for the host and for the nano's ARM/VFP target, run both (the ARM
# one under qemu-arm), and compare the digests. Usage: tests/cross_check.sh <app>/atlas.txt
set -e
cd "$(dirname "$0")/.."
mkdir -p build
awk 'BEGIN{print "static const struct { const char *name; int w, h; } kAtlas[] = {"}
     NF==7{printf "    { \"%s\", %d, %d },\n", $1, $2, $3} END{print "};"}' "$1" > build/atlas_table.h
cc -O2 -ffp-contract=off -Wall -Iport/src -Ibuild tests/cross_platform.c tests/probe.c port/src/game.c -o build/cross-host
host=$(./build/cross-host)
echo "host digest: $host"
if command -v arm-none-eabi-gcc >/dev/null && command -v qemu-arm >/dev/null; then
    arm-none-eabi-gcc -marm -mcpu=cortex-a5 -mfloat-abi=softfp -mfpu=vfpv3-d16 -O2 -ffp-contract=off -std=gnu99 \
        -fno-builtin -DFB_ARM_CHECK -nostdlib -static -Iport/src -Ibuild \
        tests/cross_platform.c tests/probe.c port/src/game.c -lgcc -o build/cross-arm
    arm=$(qemu-arm -cpu cortex-a7 build/cross-arm)
    echo "ARM digest:  $arm"
    [ "$host" = "$arm" ] && echo "PASS: identical" || { echo "FAIL: digests differ"; exit 1; }
else
    echo "(no arm-none-eabi-gcc/qemu-arm here: host digest only)"
fi
