#!/usr/bin/env python3
"""Wrap an HRL1 resident with versioned compatibility metadata and FNV-1a.

Append four little-endian uint32 words (HBVR magic, ABI, firmware, positive
resident version), then a uint32 FNV-1a covering both HRL1 and metadata. This
detects accidental corruption, not publisher authenticity. Legacy HRL1 plus
checksum slots have version zero and belong to the known ABI-2 contract.

Release versions use the source commit's timestamp, recorded as resident_version
in build.json. Uncommitted runtime edits use at least commit time + 1 and the
current time.
SOURCE_DATE_EPOCH overrides either for controlled builds. Do not backdate runtime
updates: app manifests can require a minimum resident version.
"""

import argparse
import os
from pathlib import Path
import struct
import subprocess
import time
from runtime_compat import ABI, FIRMWARE

VERSION_MAGIC = 0x52564248  # HBVR


def source_version():
    """Reproducible release ordering, also used for local resident updates."""
    root = Path(__file__).resolve().parents[1]
    explicit = os.environ.get('SOURCE_DATE_EPOCH')
    value = int(explicit or subprocess.check_output(
        ['git', 'show', '-s', '--format=%ct', 'HEAD'], cwd=root, text=True).strip())
    if not explicit and subprocess.check_output([
            'git', 'status', '--porcelain', '--', 'sdk', 'system/silver_resident',
            'system/disk_bootstrap', 'tools/wrap_resident_slot.py'], cwd=root):
        # Identify local runtime edits without requiring a commit.
        # Clean CI builds remain reproducible from commit time.
        value = max(value + 1, int(time.time()))
    if not 0 < value <= 0xffffffff:
        raise ValueError('resident version must be a positive 32-bit source timestamp')
    return value


def wrap_slot(data, version=None):
    validate_hrl1(data)
    version = source_version() if version is None else version
    if not 0 < version <= 0xffffffff:
        raise ValueError('invalid resident version')
    body = data + struct.pack('<4I', VERSION_MAGIC, ABI, FIRMWARE, version)
    return body + struct.pack('<I', fnv1a32(body))


def validate_slot(data):
    """Return (canonical HRL1, version); accept old checksummed slots as v0."""
    if len(data) < 28 or fnv1a32(data[:-4]) != int.from_bytes(data[-4:], 'little'):
        raise ValueError('resident slot checksum mismatch')
    h = struct.unpack_from('<6I', data)
    length = 24 + h[2] + h[4] * 4
    if length + 20 == len(data):
        magic, abi, firmware, version = struct.unpack_from('<4I', data, length)
        if (magic, abi, firmware) != (VERSION_MAGIC, ABI, FIRMWARE) or not version:
            raise ValueError('incompatible resident version metadata')
    elif length + 4 == len(data):
        version = 0
    else:
        raise ValueError('invalid resident slot size')
    validate_hrl1(data[:length])
    return data[:length], version


def fnv1a32(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def validate_hrl1(data: bytes) -> None:
    if len(data) < 24:
        raise ValueError("resident is shorter than an HRL1 header")
    magic, entry, image, span, count, align = struct.unpack_from("<6I", data)
    if (magic != 0x314C5248 or not 0 < align <= 32 or align & (align - 1)
            or image < 4 or image > span or entry >= image or entry & 1
            or len(data) != 24 + image + 4 * count):
        raise ValueError("resident is not a canonical HRL1 image")
    prior = None
    for (offset,) in struct.iter_unpack("<I", data[24 + image:]):
        if offset & 3 or offset + 4 > image or (
                prior is not None and offset < prior + 4):
            raise ValueError("resident relocation table is not canonical")
        prior = offset


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--version", type=int, help="source timestamp (default: SOURCE_DATE_EPOCH or commit time)")
    args = parser.parse_args()
    data = args.input.read_bytes()
    slot = wrap_slot(data, args.version)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(slot)
    print(f"wrote {args.output}: {len(slot)} bytes, version={validate_slot(slot)[1]}")


if __name__ == "__main__":
    main()
