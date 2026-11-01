#!/usr/bin/env python3
"""Decode /Library/heapdiag.bin from the 39A10023 diagnostic resident."""
import argparse
from pathlib import Path
import struct

HEADER = struct.Struct("<9I")
REGION = struct.Struct("<2I")
SAMPLE = struct.Struct("<7I")
STATUS = {
    0: "walk completed",
    1: "invalid heap object pointer",
    2: "invalid region count",
    3: "invalid region bounds",
    4: "invalid block header",
    5: "block walk limit reached",
    6: "end marker before a target",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path)
    args = parser.parse_args()
    data = args.snapshot.read_bytes()
    expected = HEADER.size + 20 * REGION.size + 10 * SAMPLE.size
    if len(data) != expected:
        parser.error(f"expected {expected} bytes, got {len(data)}")
    magic, version, heap, count, status, bad, raw, walked, samples = HEADER.unpack_from(data)
    if magic != 0x31445048 or version != 1 or samples != 10:
        parser.error("unrecognized diagnostic snapshot")
    print(f"heap object {heap:#010x}; regions {count}; blocks walked {walked}")
    print(f"status: {STATUS.get(status, f'unknown {status}')}" +
          (f" at {bad:#010x}, word0={raw:#010x}" if status >= 3 else ""))
    for i in range(min(count, 20)):
        start, size = REGION.unpack_from(data, HEADER.size + i * REGION.size)
        print(f"region {i:2d}: {start:#010x}..{start + size:#010x} ({size:#x} bytes)")
    base = HEADER.size + 20 * REGION.size
    for i in range(samples):
        target, region, hdr, end, word0, word1, found = SAMPLE.unpack_from(
            data, base + i * SAMPLE.size)
        if found:
            print(f"{target:#010x}: region {region}, block {hdr:#010x}..{end:#010x}, "
                  f"raw=({word0:#010x}, {word1:#010x}), flags={word0 & 3:#x}, "
                  f"type={word1 & 0xffff:#06x}, owner={word1 >> 16:#06x}")
        else:
            print(f"{target:#010x}: no containing block found")


if __name__ == "__main__":
    main()
