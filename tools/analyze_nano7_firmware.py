#!/usr/bin/env python3
"""Offline checks of a decrypted 39A10023 IMG1 or raw OS body.

No device access. Address matches do not validate an untether boot trigger.
Requires capstone in the selected Python environment.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

BASE = 0x08000000


def inspect(path, raw=False):
    image = path.read_bytes()
    if raw:
        body = image
    else:
        if image[:7] != b"87402.0" or image[7] != 4:
            raise ValueError("expected an unsigned, decrypted Nano 7 IMG1 (8740 / 2.0 / format 4)")
        size = struct.unpack_from("<I", image, 12)[0]
        if len(image) < 0x400 + size:
            raise ValueError("truncated IMG1 body")
        body = image[0x400:0x400 + size]
    report = {"path": str(path), "body_size": len(body),
              "body_sha256": hashlib.sha256(body).hexdigest(),
              "address_checks": {}, "freetype_seed_candidates": []}
    expected = {
        "home_dispatch": (0x087CBB6C, struct.pack("<I", 0x082AE8DD)),
        "home_rawmsg": (0x087CBB64, struct.pack("<I", 0x081C4505)),
        "resource_getter_prologue": (0x08409E86, struct.pack("<H", 0xB5F8)),
        "touch_post_prologue": (0x084A27F6, struct.pack("<H", 0xE92D)),
        "datetime_formatter_prologue": (0x084A9ADC, struct.pack("<H", 0xE92D)),
    }
    for name, (address, value) in expected.items():
        actual = body[address - BASE:address - BASE + len(value)]
        report["address_checks"][name] = {
            "address": hex(address), "expected_hex": value.hex(),
            "actual_hex": actual.hex(), "match": actual == value,
        }
    # FreeType's old CFF decoder uses 0x7384 as the nonzero random-seed
    # fallback. Find Thumb-2 MOVW encodings rather than disassembling data
    # linearly and losing alignment. These are candidates, not symbols.
    cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    for offset in range(0, len(body) - 3, 2):
        h1, h2 = struct.unpack_from("<HH", body, offset)
        if h1 & 0xFBF0 != 0xF240 or h2 & 0x8000:
            continue
        imm = ((h1 & 15) << 12) | ((h1 & 0x400) << 1) | ((h2 & 0x7000) >> 4) | (h2 & 255)
        if imm != 0x7384:
            continue
        start = max(0, offset - 48)
        instructions = [f"{i.address:08x}: {i.mnemonic} {i.op_str}"
                        for i in cs.disasm(body[start:offset + 64], BASE + start)]
        report["freetype_seed_candidates"].append({
            "address": hex(BASE + offset), "context": instructions,
        })
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--raw", action="store_true", help="input is the raw decrypted OS body")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = inspect(args.image, args.raw)
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
