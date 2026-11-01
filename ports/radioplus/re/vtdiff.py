#!/usr/bin/env python3
"""Diff the tuner vtables to find the slots that actually do tuner work.

TRFTuner is abstract - slots 4, 5, 11 and 12 are null, so it declares an
interface that the concrete tuners fill in. Laid side by side, the slots that
are IDENTICAL across all three are inherited plumbing (destructors, and
whatever TRFTuner implements once for everybody), and the slots that DIFFER are
the per-implementation overrides. Those are the tuner operations: the ones that
have to differ between talking to the chip over iAP and talking to it directly.

Nothing here needs the class hierarchy to be understood - the disagreement
between the tables is the signal.
"""
import struct

IMG = "/mnt/c/src/ipod/artifacts/firmware/decrypted-112/osos.live.bin"
BASE = 0x08000000
data = open(IMG, "rb").read()
END = BASE + len(data)

TABLES = [
    ("TRFTuner",       0x087F0050),
    ("TRFTuner_IAP",   0x087ECFD8),
    ("TRFTuner_RTXC",  0x087ED41C),
]
SLOTS = 40


def word(v):
    o = v - BASE
    if o < 0 or o + 4 > len(data):
        return None
    return struct.unpack_from("<I", data, o)[0]


def code(v):
    return v is not None and v != 0 and BASE <= (v & ~1) < END


# vtable layout: [offset_to_top][type_info*][vfn0]...
cols = []
for name, vt in TABLES:
    cols.append([word(vt + 8 + 4 * i) for i in range(SLOTS)])

print("slot  %-12s %-12s %-12s" % tuple(n for n, _ in TABLES))
print("-" * 58)

same, diff = [], []
for i in range(SLOTS):
    vals = [c[i] for c in cols]
    if not any(code(v) for v in vals):
        # Three nulls in a row means we have run off the end of every table.
        if all(v in (0, None) for v in vals) and i > 12:
            break

    def fmt(v):
        if v is None:
            return "-"
        if v == 0:
            return "PURE"
        return "0x%08x" % v

    mark = ""
    real = [v for v in vals if code(v)]
    if len(set(vals)) == 1 and code(vals[0]):
        same.append(i)
    elif len(real) >= 2 and len(set(real)) > 1:
        mark = "  <-- overridden"
        diff.append(i)

    print("[%2d]  %-12s %-12s %-12s%s" %
          (i, fmt(vals[0]), fmt(vals[1]), fmt(vals[2]), mark))

print()
print("identical in all three (inherited plumbing): %s" % same)
print("differ between implementations (the tuner API): %s" % diff)
