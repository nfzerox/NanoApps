#!/usr/bin/env python3
"""Carry an address from the 1.0.2 image to the 1.1.2 one by code signature.

The Bluetooth HCI notes in docs-internal were written against osos.dec.bin,
which is 1.0.2. The device runs 1.1.2, and the two are different builds, so an
address from those notes disassembles to nonsense in the image that matters -
which is exactly what happened on the first attempt at the send function.

Both are builds of the same source, so a function body is usually identical
apart from PC-relative operands. This takes the bytes at an address in 1.0.2,
blanks the fields that are allowed to move, and looks for the same code in
1.1.2.

Masked, because they encode distances that shift when anything before them
grows:
  * BL / B.W / BLX      F0xx-F7xx followed by D0xx-Ffxx
  * PC-relative loads   LDR rt,[pc,#imm]  (0x48xx-0x4Fxx)

Usage:  port.py 0x08422794 [more...]
"""
import struct
import sys

OLD = "/mnt/c/src/ipod/artifacts/firmware/decrypted-102/osos.dec.bin"
NEW = "/mnt/c/src/ipod/artifacts/firmware/decrypted-112/osos.live.bin"
BASE = 0x08000000

old = open(OLD, "rb").read()
new = open(NEW, "rb").read()


def signature(buf, off, n):
    """n bytes from off, with movable operands replaced by wildcards."""
    sig = bytearray(buf[off:off + n])
    mask = bytearray(b"\xff" * len(sig))

    i = 0
    while i + 1 < len(sig):
        hw = struct.unpack_from("<H", sig, i)[0]
        if 0xF000 <= hw <= 0xF7FF and i + 3 < len(sig):
            hw2 = struct.unpack_from("<H", sig, i + 2)[0]
            if 0xD000 <= hw2 <= 0xFFFF:          # BL / B.W / BLX pair
                mask[i:i + 4] = b"\x00\x00\x00\x00"
                i += 4
                continue
        if 0x4800 <= hw <= 0x4FFF:               # LDR rt,[pc,#imm]
            mask[i:i + 2] = b"\x00\x00"
        i += 2
    return bytes(sig), bytes(mask)


def search(hay, sig, mask):
    out = []
    n = len(sig)
    # Anchor on the first unmasked byte pair to keep this fast.
    anchor = next((i for i in range(0, n - 1, 2) if mask[i] == 0xFF), 0)
    needle = sig[anchor:anchor + 2]
    i = anchor
    while True:
        j = hay.find(needle, i)
        if j < 0:
            break
        start = j - anchor
        if start >= 0 and start % 2 == 0 and start + n <= len(hay):
            if all(mask[k] == 0 or hay[start + k] == sig[k] for k in range(n)):
                out.append(start)
        i = j + 2
    return out


for arg in sys.argv[1:]:
    va = int(arg, 16)
    print("=" * 66)
    print("1.0.2  0x%08x" % va)

    for n in (40, 32, 24, 16, 12):
        sig, mask = signature(old, va - BASE, n)
        hits = search(new, sig, mask)
        kept = sum(1 for m in mask if m)
        if len(hits) == 1:
            print("  1.1.2  0x%08x   (unique on %d bytes, %d significant)"
                  % (BASE + hits[0], n, kept))
            break
        if not hits:
            continue
        print("  %d bytes -> %d candidates, narrowing" % (n, len(hits)))
    else:
        print("  no confident match - the function changed between builds")
