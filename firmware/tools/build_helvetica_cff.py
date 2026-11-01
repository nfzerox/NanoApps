#!/usr/bin/env python3
"""Build local CFF Helvetica carriers from Apple's stock Nano firmware.

The output is intentionally kept under the ignored build/ directory.  This
tool does not inject or alter the exploit CharString; the existing firmware
builder performs that one substitution after the selected carrier is final.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import io
import json
import logging
import math
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zipfile

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.ttLib import TTFont
from pyfatfs.PyFatFS import PyFatFS


FONT_PATH = "/Resources/Fonts/Helvetica.ttf"
EXCLUDED_SOURCE_TABLES = {"GlyphOrder", "glyf", "loca", "maxp", "hdmx"}
# AFDKO writes the current time into head.modified. Pin it to the exact value
# in the hardware-tested carrier so identical inputs reproduce identical bytes.
HARDWARE_TESTED_HEAD_MODIFIED = 3873562078
logging.getLogger("fontTools").setLevel(logging.ERROR)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def patch_img1_header(mse: bytes) -> bytes:
    old = b"87402.0\x04"
    new = b"87402.0\x03"
    count = mse.count(old)
    if count == 0:
        return mse
    if count != 1:
        raise ValueError("stock MSE does not contain one Nano 7 IMG1 header")
    return mse.replace(old, new, 1)


def extract_resource_fat(mse: bytes) -> bytes:
    resource = []
    for index in range(16):
        entry = mse[0x5000 + index * 40 : 0x5000 + (index + 1) * 40]
        if len(entry) != 40 or entry[:4] == b"\0\0\0\0":
            continue
        name = entry[4:8]
        offset, length = struct.unpack_from("<II", entry, 12)
        if name == b"crsr":
            resource.append((offset, length))
    if len(resource) != 1:
        raise ValueError(f"expected one crsr section, found {len(resource)}")
    offset, length = resource[0]
    img1 = mse[offset + 0x1000 : offset + 0x1000 + length]
    if img1[:8] != b"87402.0\x03":
        raise ValueError("resource IMG1 header is not in parsed form")
    body_length = struct.unpack_from("<I", img1, 12)[0]
    fat = img1[0x400 : 0x400 + body_length]
    if len(fat) != body_length:
        raise ValueError("resource FAT is truncated")
    return fat


def read_mse(path: Path) -> bytes:
    if path.suffix.lower() == ".ipsw":
        with zipfile.ZipFile(path) as archive:
            return archive.read("Firmware.MSE")
    return path.read_bytes()


def extract_stock_font(firmware: Path) -> tuple[bytes, dict[str, int]]:
    fat_bytes = extract_resource_fat(patch_img1_header(read_mse(firmware)))
    with tempfile.TemporaryDirectory(prefix="nano7-font-") as directory:
        fat_path = Path(directory) / "resource-fat.img"
        fat_path.write_bytes(fat_bytes)
        fat = PyFatFS(str(fat_path), read_only=True)
        try:
            font = fat.openbin(FONT_PATH, "rb").read()
            raw_fat = fat.fs
            free_clusters = sum(1 for value in raw_fat.fat[2:] if value == 0)
            cluster_bytes = raw_fat.bytes_per_cluster
        finally:
            fat.close()
    reclaimed = math.ceil(len(font) / cluster_bytes) * cluster_bytes
    return font, {
        "resource_fat_bytes": len(fat_bytes),
        "cluster_bytes": cluster_bytes,
        "free_clusters": free_clusters,
        "free_bytes_before_replacement": free_clusters * cluster_bytes,
        "maximum_replacement_bytes": free_clusters * cluster_bytes + reclaimed,
    }


def name_value(font: TTFont, name_id: int, fallback: str) -> str:
    value = font["name"].getDebugName(name_id)
    return value or fallback


def glyph_bounds(font: TTFont, name: str):
    glyph_set = font.getGlyphSet()
    pen = BoundsPen(glyph_set)
    glyph_set[name].draw(pen)
    return pen.bounds


def make_cff_font(source_bytes: bytes) -> bytes:
    source = TTFont(io.BytesIO(source_bytes), recalcTimestamp=False, lazy=False)
    source.ensureDecompiled()
    glyph_order = source.getGlyphOrder()
    glyph_set = source.getGlyphSet()
    metrics = source["hmtx"].metrics

    charstrings = {}
    for name in glyph_order:
        pen = T2CharStringPen(metrics[name][0], glyph_set, roundTolerance=0.0)
        glyph_set[name].draw(pen)
        charstrings[name] = pen.getCharString(optimize=True)

    builder = FontBuilder(source["head"].unitsPerEm, isTTF=False)
    builder.setupGlyphOrder(glyph_order)
    builder.setupCFF(
        name_value(source, 6, "Helvetica"),
        {
            "version": name_value(source, 5, "1.0"),
            "Notice": name_value(source, 0, ""),
            "FullName": name_value(source, 4, "Helvetica"),
            "FamilyName": name_value(source, 1, "Helvetica"),
            "Weight": name_value(source, 2, "Regular"),
            "FontBBox": [
                source["head"].xMin,
                source["head"].yMin,
                source["head"].xMax,
                source["head"].yMax,
            ],
            "ItalicAngle": source["post"].italicAngle,
            "UnderlinePosition": source["post"].underlinePosition,
            "UnderlineThickness": source["post"].underlineThickness,
            "isFixedPitch": source["post"].isFixedPitch,
        },
        charstrings,
        {
            # Zones and representative stems are measured from Apple's own
            # outlines (baseline/round overshoot, x-height and cap height).
            # They give AFDKO enough global information to add CFF hints
            # without altering the outlines.
            "BlueValues": [-43, 0, 1071, 1102, 1469, 1509],
            "OtherBlues": [-453, -427],
            "StdHW": 142,
            "StdVW": 143,
        },
    )
    converted = builder.font
    converted.sfntVersion = "OTTO"

    # Keep Apple's mappings, metrics, names, kerning and Apple layout tables.
    # Only TrueType-outline-specific tables are replaced or removed.
    for tag in source.keys():
        if tag not in EXCLUDED_SOURCE_TABLES and tag != "CFF ":
            converted[tag] = copy.deepcopy(source[tag])
    converted["head"].indexToLocFormat = 0
    converted["head"].glyphDataFormat = 0
    converted.setGlyphOrder(glyph_order)

    output = io.BytesIO()
    converted.save(output, reorderTables=False)
    source.close()
    converted.close()
    return output.getvalue()


def validate_conversion(stock_bytes: bytes, converted_bytes: bytes) -> dict[str, object]:
    stock = TTFont(io.BytesIO(stock_bytes), recalcTimestamp=False, lazy=False)
    converted = TTFont(io.BytesIO(converted_bytes), recalcTimestamp=False, lazy=False)
    stock.ensureDecompiled()
    converted.ensureDecompiled()
    if converted.sfntVersion != "OTTO" or "CFF " not in converted:
        raise ValueError("converted font is not an OpenType/CFF font")
    if converted.getGlyphOrder() != stock.getGlyphOrder():
        raise ValueError("glyph order changed during conversion")
    if converted["hmtx"].metrics != stock["hmtx"].metrics:
        raise ValueError("horizontal metrics changed during conversion")
    if converted.getBestCmap() != stock.getBestCmap():
        raise ValueError("Unicode mapping changed during conversion")
    if converted.getBestCmap().get(0x20) != "space":
        raise ValueError("U+0020 no longer maps to the space glyph")

    maximum_bound_delta = 0.0
    for name in stock.getGlyphOrder():
        before = glyph_bounds(stock, name)
        after = glyph_bounds(converted, name)
        if before is None or after is None:
            if before != after:
                raise ValueError(f"empty/non-empty outline mismatch for {name}")
            continue
        maximum_bound_delta = max(
            maximum_bound_delta,
            *(abs(float(a) - float(b)) for a, b in zip(before, after)),
        )
    # A handful of Apple's composite glyphs contain move-only phantom points
    # outside the painted contour.  CFF correctly drops those when another
    # component follows; AFDKO also removes a 31-unit move-only point from
    # dotlessi.  Neither point paints pixels.
    # Anything beyond that is an actual conversion regression.
    if maximum_bound_delta > 31.001:
        raise ValueError(f"outline bounds changed by {maximum_bound_delta}")

    result = {
        "glyphs": len(stock.getGlyphOrder()),
        "space_glyph_index": stock.getGlyphOrder().index("space"),
        "space_advance": stock["hmtx"]["space"][0],
        "maximum_outline_bound_delta": maximum_bound_delta,
        "tables": sorted(converted.keys()),
    }
    stock.close()
    converted.close()
    return result


def run_autohint(program: str, source: Path, output: Path) -> None:
    subprocess.run(
        [program, "-p", "1", "-o", str(output), str(source)],
        check=True,
        text=True,
    )
    hinted = TTFont(output, recalcTimestamp=False, lazy=False)
    hinted["head"].modified = HARDWARE_TESTED_HEAD_MODIFIED
    hinted.save(output, reorderTables=False)
    hinted.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--patched-mse-output", type=Path)
    parser.add_argument("--otfautohint", default=shutil.which("otfautohint"))
    args = parser.parse_args()
    if not args.otfautohint:
        raise SystemExit("otfautohint is required")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    if args.patched_mse_output:
        args.patched_mse_output.parent.mkdir(parents=True, exist_ok=True)
        args.patched_mse_output.write_bytes(
            patch_img1_header(read_mse(args.firmware))
        )
    stock_bytes, fat = extract_stock_font(args.firmware)
    stock_path = args.out_dir / "Helvetica-stock.ttf"
    unhinted_path = args.out_dir / "Helvetica-CFF-unhinted.otf"
    hinted_path = args.out_dir / "Helvetica-CFF-hinted.otf"
    stock_path.write_bytes(stock_bytes)
    unhinted_path.write_bytes(make_cff_font(stock_bytes))
    unhinted = validate_conversion(stock_bytes, unhinted_path.read_bytes())
    run_autohint(args.otfautohint, unhinted_path, hinted_path)
    hinted = validate_conversion(stock_bytes, hinted_path.read_bytes())

    if hinted_path.stat().st_size > fat["maximum_replacement_bytes"]:
        raise SystemExit("hinted CFF font exceeds the resource FAT replacement budget")

    manifest = {
        "source_firmware": str(args.firmware.resolve()),
        "stock": {
            "path": str(stock_path),
            "bytes": stock_path.stat().st_size,
            "sha256": sha256(stock_path.read_bytes()),
        },
        "unhinted": {
            "path": str(unhinted_path),
            "bytes": unhinted_path.stat().st_size,
            "sha256": sha256(unhinted_path.read_bytes()),
            **unhinted,
        },
        "hinted": {
            "path": str(hinted_path),
            "bytes": hinted_path.stat().st_size,
            "sha256": sha256(hinted_path.read_bytes()),
            **hinted,
        },
        "fat": fat,
    }
    manifest_path = args.out_dir / "font-build.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(json.dumps(manifest, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
