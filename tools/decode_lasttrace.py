#!/usr/bin/env python3
"""Decode /Library/lasttrace.bin (the previous boot's hb_trace RAM ring,
saved by the resident at entry) into chronological order.

Usage: decode_lasttrace.py lasttrace.bin
"""
import struct
import sys

MAGIC, MAX = 0x48425452, 256

data = open(sys.argv[1], "rb").read()
magic, widx, count, seq = struct.unpack_from("<4I", data, 0)
if magic != MAGIC:
    sys.exit(f"bad magic {magic:#010x}")
n = min(count, MAX)
first = (widx - n) % MAX
print(f"boot seq {seq}: {count} entries logged, showing last {n}")
for i in range(n):
    idx = (first + i) % MAX
    tag, v1, v2 = struct.unpack_from("<8sII", data, 16 + 16 * idx)
    seq = count - n + i
    print(f"{seq:6d}  {tag.decode('latin-1')}  {v1:#010x}  {v2:#010x}")
