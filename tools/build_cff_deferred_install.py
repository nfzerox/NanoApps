#!/usr/bin/env python3
"""Build the cave-resident CFF publisher for a fixed deferred-stage artifact."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD = ROOT / "system/deferred_untether/build"
DEFERRED_CAVE_CAPACITY = 0x7F4
SENTINELS = {
    "cff_saved_stack152": 0x53313532,
    "cff_saved_stack153": 0x53313533,
    "cff_saved_r5": 0x53415635,
    "cff_install_continuation": 0x5245544E,
    "cff_install_descriptor": 0x43534544,
}
FORBIDDEN_MNEMONICS = {"bl", "blx"}


def symbols(path):
    result = subprocess.run(
        ["arm-none-eabi-nm", "-n", str(path)],
        check=True, text=True, capture_output=True,
    )
    found = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            found[parts[2]] = int(parts[0], 16)
    return found


def disassembly(path):
    return subprocess.run(
        ["arm-none-eabi-objdump", "-d", str(path)],
        check=True, text=True, capture_output=True,
    ).stdout


def validate_installer(data, syms, listing, limit, deferred_size, dest):
    if not data or len(data) & 3 or len(data) > limit:
        raise ValueError("publisher is empty, misaligned, or exceeds its cave window")
    if syms.get("cff_deferred_install") != 0:
        raise ValueError("publisher entry is not linked at offset zero")

    words = struct.unpack(f"<{len(data) // 4}I", data)
    for name, sentinel in SENTINELS.items():
        off = syms.get(name)
        if off is None or off & 3 or off + 4 > len(data):
            raise ValueError(f"{name} is missing, misaligned, or outside the image")
        if struct.unpack_from("<I", data, off)[0] != sentinel:
            raise ValueError(f"{name} does not contain its expected sentinel")
        if words.count(sentinel) != 1:
            raise ValueError(f"{name} sentinel is missing or ambiguous")
    captured_names = ("cff_saved_stack152", "cff_saved_stack153",
                      "cff_saved_r5", "cff_install_continuation")
    if [syms[name] for name in captured_names] != [0x60, 0x64, 0x68, 0x6C]:
        raise ValueError("captured return state moved from publisher offsets 0x60-0x6c")
    descriptor = syms.get("cff_install_descriptor")
    if descriptor != 0x70:
        raise ValueError("cache descriptor moved from publisher offset 0x70")
    if syms.get("cff_install_flush_size") != descriptor + 0x2C:
        raise ValueError("cache size is not descriptor+0x2c")
    if syms.get("cff_install_cave") != descriptor + 0x34:
        raise ValueError("cache address is not descriptor+0x34")
    expected_size = deferred_size + len(data)
    if struct.unpack_from("<I", data, descriptor + 0x2C)[0] != expected_size:
        raise ValueError("cache size is not the complete stage+publisher image")
    if struct.unpack_from("<I", data, descriptor + 0x34)[0] != dest:
        raise ValueError("cache address is not the fixed cave base")
    code_end = syms.get("cff_deferred_install_end")
    if code_end is None:
        raise ValueError("publisher code-end symbol is missing")
    if code_end > 0x60:
        raise ValueError("publisher code extends into captured state")

    mnemonics = []
    for line in listing.splitlines():
        fields = line.split("\t")
        if len(fields) >= 3 and fields[0].strip().endswith(":"):
            mnemonics.append(fields[2].strip().split()[0].split(".")[0])
    forbidden = sorted(FORBIDDEN_MNEMONICS.intersection(mnemonics))
    if forbidden:
        raise ValueError(f"installer contains external-call instruction(s): {forbidden}")
    svc_count = mnemonics.count("svc")
    if svc_count != 1:
        raise ValueError(
            "expected one ACTION cache SVC instruction, "
            f"found {svc_count}"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deferred-bin", type=Path,
                        default=DEFAULT_BUILD / "deferred_stage.bin")
    parser.add_argument("--deferred-elf", type=Path,
                        default=DEFAULT_BUILD / "deferred_stage.elf")
    parser.add_argument("--output", type=Path, default=DEFAULT_BUILD)
    parser.add_argument("--dest", type=lambda value: int(value, 0),
                        default=0x086C97E4)
    parser.add_argument("--limit", type=lambda value: int(value, 0),
                        help="maximum publisher bytes inside the cave")
    # Kept for release-script compatibility. Cache publication is mandatory;
    # there is deliberately no corresponding --no-flush mode.
    parser.add_argument("--flush", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()

    deferred = args.deferred_bin.read_bytes()
    deferred_syms = symbols(args.deferred_elf)
    with tempfile.TemporaryDirectory(prefix="cff-deferred-check-") as temp:
        extracted = Path(temp) / "deferred_stage.bin"
        subprocess.run(
            ["arm-none-eabi-objcopy", "-O", "binary",
             str(args.deferred_elf), str(extracted)],
            check=True,
        )
        if extracted.read_bytes() != deferred:
            raise SystemExit("deferred ELF and binary do not describe the same image")
    action = deferred_syms.get("deferred_action")
    if (not deferred or len(deferred) & 3 or
            len(deferred) > DEFERRED_CAVE_CAPACITY):
        raise SystemExit(
            "deferred stage must be nonempty, word aligned, and fit the "
            f"{DEFERRED_CAVE_CAPACITY:#x}-byte verified cave"
        )
    if action != args.dest:
        raise SystemExit("deferred_action must be exactly the fixed cave base")
    action_off = action - args.dest
    if action_off & 1:
        raise SystemExit("deferred_action symbol must be an even Thumb code address")
    publisher_limit = (args.limit if args.limit is not None else
                       DEFERRED_CAVE_CAPACITY - len(deferred))
    if publisher_limit <= 0:
        raise SystemExit("deferred stage leaves no room for its cave publisher")

    args.output.mkdir(parents=True, exist_ok=True)
    elf = args.output / "cff_deferred_install.elf"
    binary = args.output / "cff_deferred_install.bin"
    src = ROOT / "system/deferred_untether/cff_deferred_install.S"
    linker = ROOT / "system/deferred_untether/cff_deferred_install.ld"
    command = [
        "arm-none-eabi-gcc", "-mcpu=cortex-a8", "-mthumb", "-nostdlib",
        "-Wl,--build-id=none", f"-Wl,-T,{linker}",
        f"-Wl,--defsym,CFF_INSTALL_LIMIT={publisher_limit:#x}",
        f"-DDEFERRED_SIZE={len(deferred)}",
        "-o", str(elf), str(src),
    ]
    subprocess.run(command, check=True)
    subprocess.run(
        ["arm-none-eabi-objcopy", "-O", "binary", str(elf), str(binary)],
        check=True,
    )

    data = binary.read_bytes()
    syms = symbols(elf)
    listing = disassembly(elf)
    validate_installer(data, syms, listing, publisher_limit,
                       len(deferred), args.dest)
    print(
        f"wrote {binary}: {len(data)}-byte cave publisher (one SVC); "
        f"CFF image={len(deferred) + len(data)} bytes at {args.dest:#010x}; "
        f"action {(args.dest + action_off) | 1:#010x}"
    )


if __name__ == "__main__":
    main()
