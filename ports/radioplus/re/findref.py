#!/usr/bin/env python3
"""Find every instruction that touches a given struct offset.

voice::setCallback stashes the caller's callback at voice+0xA4, its cbdata at
voice+0xA0 and the descriptor at voice+0x9C. Whoever READS those is the code
that fires the callback - and finding it tells us both the signature and, more
importantly, which task it runs on.

Scans for ARM ldr/str with a 12-bit immediate offset and for Thumb ldr/str with
the 12-bit immediate form (T3/T4), which is what gcc emits for offsets past 124.
"""
import struct
import sys

IMG = "/mnt/c/src/ipod/artifacts/firmware/decrypted-112/osos.live.bin"
BASE = 0x08000000

want = [int(a, 16) for a in sys.argv[1:]] or [0x9C, 0xA0, 0xA4]
data = open(IMG, "rb").read()

hits = []

# ---- ARM: cond 01 I P U B W L Rn Rd imm12, I=0 (immediate), P=1, B=0 -------
for off in range(0, len(data) - 4, 4):
    w = struct.unpack_from("<I", data, off)[0]
    if (w >> 28) == 0xE and ((w >> 25) & 7) == 0b010:
        imm = w & 0xFFF
        if imm in want:
            rn = (w >> 16) & 0xF
            rd = (w >> 12) & 0xF
            ld = (w >> 20) & 1
            up = (w >> 23) & 1
            if up and rn != 15:
                hits.append((BASE + off, "ARM",
                             "%s r%d, [r%d, #0x%X]" %
                             ("ldr" if ld else "str", rd, rn, imm)))

# ---- Thumb T3: 1111 1000 1101 Rn / Rt imm12 (ldr.w), 1100 for str.w --------
for off in range(0, len(data) - 4, 2):
    hw = struct.unpack_from("<H", data, off)[0]
    # Word, halfword and byte forms all matter: a state field is a byte, so
    # ldrb.w / strb.w (F890 / F880) are exactly what gets missed by only
    # looking for ldr.w / str.w - which is why the state field appeared to
    # have no accessors at all.
    OPS = {0xF8D0: "ldr", 0xF8C0: "str",
           0xF8B0: "ldrh", 0xF8A0: "strh",
           0xF890: "ldrb", 0xF880: "strb"}
    op = OPS.get(hw & 0xFFF0)
    if op:
        hw2 = struct.unpack_from("<H", data, off + 2)[0]
        imm = hw2 & 0xFFF
        if imm in want:
            rn = hw & 0xF
            rt = (hw2 >> 12) & 0xF
            if rn != 15:
                hits.append((BASE + off, "THM",
                             "%s.w r%d, [r%d, #0x%X]" % (op, rt, rn, imm)))

for va, mode, txt in sorted(hits):
    print("%08x  %s  %s" % (va, mode, txt))
print("--- %d hits for %s ---" % (hits.__len__(),
                                  ", ".join("0x%X" % w for w in want)))
