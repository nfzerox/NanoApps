#!/usr/bin/env python3
"""Verify an automatic-Home firmware candidate without accessing a device."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import zipfile


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sections(mse: bytes):
    result = []
    for index in range(16):
        offset = 0x5000 + index * 40
        entry = mse[offset:offset + 40]
        if entry[:4] == bytes(4):
            continue
        dev_offset, length = struct.unpack_from("<II", entry, 12)
        result.append((index, entry[4:8], dev_offset, length, entry))
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--known-good", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--ipsw", type=Path, required=True)
    parser.add_argument("--cff", type=Path, required=True)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--ipod-sun", type=Path, required=True)
    args = parser.parse_args()

    sys.path.insert(0, str(args.ipod_sun / "tools"))
    import preflight_deferred_release as release

    known = args.known_good.read_bytes()
    candidate = args.candidate.read_bytes()
    if len(known) != len(candidate):
        raise SystemExit("candidate MSE size differs from known-good")
    known_sections = sections(known)
    candidate_sections = sections(candidate)
    if known_sections != candidate_sections:
        raise SystemExit("candidate MSE section table differs from known-good")
    resource = [item for item in known_sections if item[1] == b"crsr"]
    if len(resource) != 1:
        raise SystemExit("known-good MSE has no unique crsr section")
    _index, _name, dev_offset, length, _entry = resource[0]
    start, end = dev_offset + 0x1000, dev_offset + 0x1000 + length
    if known[:start] != candidate[:start] or known[end:] != candidate[end:]:
        raise SystemExit("candidate changes bytes outside the crsr resource section")

    with zipfile.ZipFile(args.ipsw) as archive:
        bad = archive.testzip()
        if bad is not None:
            raise SystemExit(f"IPSW CRC failure at {bad}")
        embedded_mse = archive.read("Firmware.MSE")
    if embedded_mse != candidate:
        raise SystemExit("IPSW does not contain the exact candidate MSE")

    raw_charstring = release.raw_cff_charstring(args.cff.read_bytes())
    stage = args.stage.read_bytes()
    release.verify_cff_endchar(raw_charstring, stage, "raw CFF charstring")
    with tempfile.TemporaryDirectory(prefix="nano7-auto-home-") as directory:
        fat, details = release.extract_fat_from_mse(candidate)
        font = release.extract_font(fat, Path(directory))
    embedded_charstring = release.font_charstring(font)
    if embedded_charstring != raw_charstring:
        raise SystemExit("packaged Helvetica charstring differs from raw CFF")
    before, after = release.roundtrip_font(font)
    if before != after or after != raw_charstring:
        raise SystemExit("packaged font does not survive a FontTools round trip")

    print(json.dumps({
        "result": "OFFLINE_CANDIDATE_VERIFIED",
        "known_good_mse_sha256": digest(known),
        "candidate_mse_sha256": digest(candidate),
        "candidate_ipsw_sha256": digest(args.ipsw.read_bytes()),
        "raw_cff_sha256": digest(args.cff.read_bytes()),
        "stage_sha256": digest(stage),
        "changes_confined_to": "crsr",
        "packaged_charstring_sha256": digest(embedded_charstring),
        "ipsw_crc_ok": True,
        **details,
    }, indent=2))


if __name__ == "__main__":
    main()
