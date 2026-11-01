#!/usr/bin/env python3
"""Find where a 16-bit constant is materialised in Thumb-2 code.

The FM tuner is reached by HCI vendor command 0xFC15, so if RetailOS builds
that command itself the opcode has to appear somewhere as an immediate. A
16-bit value that will not fit a MOV encoding comes from MOVW, and MOVW encodes
its operand in the instruction rather than in a literal pool - so searching for
the constant as bytes finds nothing and the encoding has to be searched for
instead.

    MOVW rd, #imm16   11110 i 100100 imm4 | 0 imm3 rd imm8
                      imm16 = imm4:i:imm3:imm8

Also checks literal pools, since a constant can equally be loaded with LDR, and
16-bit halfword stores of the value, since an HCI header is often assembled in
a buffer rather than held in a register.

Usage:  findimm.py 0xFC15 [more...]
"""
import struct
import sys

IMG = "/mnt/c/src/ipod/artifacts/firmware/decrypted-112/osos.live.bin"
BASE = 0x08000000
data = open(IMG, "rb").read()


def movw_encoding(imm16):
    """Every (hw1, hw2) MOVW pair that materialises imm16, over all 16 rd."""
    imm4 = (imm16 >> 12) & 0xF
    i = (imm16 >> 11) & 1
    imm3 = (imm16 >> 8) & 7
    imm8 = imm16 & 0xFF
    hw1 = 0xF240 | (i << 10) | imm4
    for rd in range(16):
        yield hw1, (imm3 << 12) | (rd << 8) | imm8, rd


for arg in sys.argv[1:]:
    val = int(arg, 16)
    print("=" * 66)
    print("0x%04X" % val)

    hits = 0
    for hw1, hw2, rd in movw_encoding(val):
        needle = struct.pack("<HH", hw1, hw2)
        i = 0
        while True:
            i = data.find(needle, i)
            if i < 0:
                break
            if i % 2 == 0:
                print("  MOVW r%-2d, #0x%04X   @ 0x%08x" % (rd, val, BASE + i))
                hits += 1
            i += 2

    # A 32-bit literal pool entry holding it, e.g. an opcode stored as a word.
    for width, fmt in ((4, "<I"), (2, "<H")):
        needle = struct.pack(fmt, val)
        i, shown = 0, 0
        while shown < 12:
            i = data.find(needle, i)
            if i < 0:
                break
            if i % width == 0:
                print("  %d-byte value        @ 0x%08x" % (width, BASE + i))
                shown += 1
                hits += 1
            i += width

    if not hits:
        print("  not materialised anywhere - this constant is not built here")
