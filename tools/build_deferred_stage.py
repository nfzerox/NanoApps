#!/usr/bin/env python3
"""Build the fixed firmware preloader from an exact resident.hbapp artifact."""

import argparse
import json
from pathlib import Path
import struct
import subprocess


def fnv1a32(data):
    value = 0x811C9DC5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resident", type=Path, required=True)
    parser.add_argument("--output", type=Path,
                        default=Path("system/deferred_untether/build"))
    parser.add_argument("--link-va", type=lambda s: int(s, 0), default=0x086C97E4)
    parser.add_argument("--limit", type=lambda s: int(s, 0), default=0x7F4)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    src = root / "system/deferred_untether/deferred_stage.S"
    ld = root / "system/deferred_untether/deferred_stage.ld"
    data = args.resident.read_bytes()
    if len(data) < 24:
        raise SystemExit("resident is shorter than an HRL1 header")
    magic, entry, image, span, relocs, align = struct.unpack_from("<6I", data)
    if magic != 0x314C5248 or len(data) != 24 + image + 4 * relocs:
        raise SystemExit("resident is not a canonical HRL1 image")
    if not (image >= 4 and entry < image <= span and align and
            not align & (align - 1) and align <= 32 and
            not entry & 1 and len(data) <= span):
        raise SystemExit("resident HRL1 bounds are invalid")
    reloc_values = struct.unpack_from(f"<{relocs}I", data, 24 + image)
    if any(off & 3 or off + 4 > image for off in reloc_values):
        raise SystemExit("resident HRL1 has an unaligned or out-of-bounds relocation")
    if any(right < left + 4
           for left, right in zip(reloc_values, reloc_values[1:])):
        raise SystemExit("resident HRL1 relocations are not canonical and sorted")

    src_off = span - len(data)
    file_obj_off = (span + 7) & ~7
    file_cache_off = (file_obj_off + 0x60 + 31) & ~31
    arena_size = file_cache_off + 0x1010 + 31
    immediates = {
        "file size": len(data), "image size": image, "image span": span,
        "entry offset": entry, "relocation count": relocs,
        "packed source offset": src_off,
        "relocation-table offset": src_off + 24 + image,
        "file-object offset": file_obj_off,
        "file-cache offset": file_cache_off, "arena size": arena_size,
    }
    too_large = [(name, value) for name, value in immediates.items()
                 if not 0 <= value <= 0xFFFF]
    if too_large:
        detail = ", ".join(f"{name}={value:#x}" for name, value in too_large)
        raise SystemExit(f"resident exceeds deferred-stage 16-bit immediates: {detail}")

    args.output.mkdir(parents=True, exist_ok=True)
    elf = args.output / "deferred_stage.elf"
    binary = args.output / "deferred_stage.bin"
    defs = {
        "LINK_VA": args.link_va,
        "RES_FILE_SIZE": len(data),
        "RES_IMAGE_SIZE": image,
        "RES_SPAN": span,
        "RES_ENTRY_OFF": entry,
        "RES_RELOC_COUNT": relocs,
        "RES_ALIGN": align,
        "RES_FNV1A": fnv1a32(data),
    }
    cmd = ["arm-none-eabi-gcc", "-mcpu=cortex-a8", "-mthumb", "-nostdlib",
           "-Wl,--build-id=none", f"-Wl,-T,{ld}",
           f"-Wl,--defsym,LINK_VA={args.link_va:#x}",
           f"-Wl,--defsym,STAGE_LIMIT={args.limit:#x}", "-o", str(elf), str(src)]
    for key, value in defs.items():
        cmd.insert(-3, f"-D{key}={value}")
    subprocess.run(cmd, check=True)
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", str(elf), str(binary)],
                   check=True)
    size = binary.stat().st_size
    nm = subprocess.run(["arm-none-eabi-nm", "-n", str(elf)], check=True,
                        text=True, capture_output=True).stdout.splitlines()
    symbol_values = {line.split()[-1]: int(line.split()[0], 16)
                     for line in nm if len(line.split()) >= 3}
    try:
        action_va = symbol_values["deferred_action"]
        primary_end = symbol_values["deferred_primary_end"]
        matcher = symbol_values["action_match_helper"]
        stage_end = symbol_values["deferred_stage_end"]
    except KeyError as error:
        raise SystemExit(f"linked stage lacks layout symbol: {error.args[0]}") from error
    action_offset = action_va - args.link_va
    if not 0 <= action_offset < size:
        raise SystemExit("deferred_action is outside the emitted stage")
    metadata = {
        "link_va": args.link_va,
        "size": size,
        "limit": args.limit,
        "action_va": action_va,
        "action_thumb": action_va | 1,
        "action_offset": action_offset,
        "primary_size": primary_end - args.link_va,
        "report_offset": 0x3A0,
        "report_size": 0x200,
        "matcher_offset": matcher - args.link_va,
        "matcher_size": stage_end - matcher,
        "published_size": 0x600,
        "resident_file_size": len(data),
        "resident_image_size": image,
        "resident_span": span,
        "resident_entry_offset": entry,
        "resident_relocation_count": relocs,
        "resident_alignment": align,
        "resident_fnv1a32": fnv1a32(data),
        "file_object_offset": file_obj_off,
        "file_cache_offset": file_cache_off,
    }
    meta_path = args.output / "deferred_stage.json"
    meta_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    print(f"wrote {binary}: {size} bytes at {args.link_va:#010x}; "
          f"action={action_va | 1:#010x} offset={action_offset:#x}; "
          f"primary={primary_end - args.link_va}/928 "
          f"matcher={stage_end - matcher}/96; "
          f"resident file={len(data)} image={image} span={span} relocs={relocs}")
    print(f"wrote {meta_path}")


if __name__ == "__main__":
    main()
