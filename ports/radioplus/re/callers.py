#!/usr/bin/env python3
"""Find every BL/BLX that targets an address.

Thumb-2 encodes a call as a distance rather than a destination, so a caller
cannot be found by searching for the callee's address. Every branch pair in the
image has to be decoded and its target computed. At ten megabytes that is a few
seconds, which is cheap for the answer.

    BL   11110 S imm10 | 11 J1 1 J2 imm11
    BLX  11110 S imm10 | 11 J1 0 J2 imm11     (to ARM; target word-aligned)

J1 sits inside the top nibble of the second halfword, so that nibble is not a
constant - it is 0xD or 0xF for BL and 0xC or 0xE for BLX. Match on bits 15, 14
and 12 only.

Usage:  callers.py 0x08410820 [more...]
"""
import struct
import sys

IMG = "/mnt/c/src/ipod/artifacts/firmware/decrypted-112/osos.live.bin"
BASE = 0x08000000
data = open(IMG, "rb").read()

wanted = {int(a, 16) & ~1 for a in sys.argv[1:]}
found = {w: [] for w in wanted}

for off in range(0, len(data) - 4, 2):
    hw1 = struct.unpack_from("<H", data, off)[0]
    if (hw1 & 0xF800) != 0xF000:
        continue
    hw2 = struct.unpack_from("<H", data, off + 2)[0]
    # Bits 15..12 of hw2 are 1, 1, J1, 1 - so the nibble is 0xD or 0xF for BL
    # depending on J1, and 0xC or 0xE for BLX. Testing the nibble against 0xD
    # and 0xC alone silently misses every branch with J1 set, which is about
    # half of them, and reports zero callers for functions that plainly have
    # hundreds. Mask off J1 instead.
    if (hw2 & 0xC000) != 0xC000:
        continue
    kind = 0xD if (hw2 & 0x1000) else 0xC        # bit 12: 1 = BL, 0 = BLX

    s = (hw1 >> 10) & 1
    imm10 = hw1 & 0x3FF
    j1 = (hw2 >> 13) & 1
    j2 = (hw2 >> 11) & 1
    imm11 = hw2 & 0x7FF

    i1 = 1 - (j1 ^ s)
    i2 = 1 - (j2 ^ s)
    imm = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1)
    if s:
        imm -= 1 << 25

    pc = BASE + off + 4
    target = pc + imm
    if kind == 0xC:                     # BLX rounds the target down to a word
        target &= ~3

    if target in found:
        found[target].append((BASE + off, "BLX" if kind == 0xC else "BL"))

for w in sorted(found):
    print("=" * 62)
    print("callers of 0x%08x: %d" % (w, len(found[w])))
    for va, kind in found[w][:40]:
        print("  %-3s from 0x%08x" % (kind, va))
    if len(found[w]) > 40:
        print("  ... and %d more" % (len(found[w]) - 40))
