#!/usr/bin/env python3
"""Reject VFP/NEON/coprocessor-10/11 instructions in Thumb ELF regions."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys


MAP_RE = re.compile(
    r"^\s*\d+:\s+([0-9a-fA-F]+)\s+\d+\s+NOTYPE\s+LOCAL\s+DEFAULT\s+"
    r"(\d+)\s+\$([atd])(?:\.\d+)?\s*$"
)
INSN_RE = re.compile(
    r"^\s*([0-9a-fA-F]+):\s+(?:[0-9a-fA-F]{4,8}\s+)+"
    r"([A-Za-z][A-Za-z0-9.]*)\s*(.*)$"
)
SECTION_RE = re.compile(r"^Disassembly of section (\S+):$")
SECTION_INDEX_RE = re.compile(r"^\s*\[\s*(\d+)\]\s+(\S+)\s+")
FUNC_RE = re.compile(
    r"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+FUNC\s+\S+\s+\S+\s+"
    r"(\d+)\s+(\S+)\s*$"
)


def output(*args: str) -> str:
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="reject FP/SIMD instructions in Thumb ELF regions"
    )
    parser.add_argument("elf")
    parser.add_argument(
        "--allow-symbol", action="append", default=[], metavar="NAME",
        help="allow Thumb FP only inside this exact function (diagnostics only)",
    )
    args = parser.parse_args()
    elf = args.elf
    allowed_names = set(args.allow_symbol)
    section_index: dict[str, int] = {}
    for line in output("arm-none-eabi-readelf", "-SW", elf).splitlines():
        match = SECTION_INDEX_RE.match(line)
        if match:
            section_index[match.group(2)] = int(match.group(1))

    symbol_lines = output("arm-none-eabi-readelf", "-sW", elf).splitlines()
    mappings: dict[int, list[tuple[int, str]]] = {}
    allowed_ranges: dict[int, list[tuple[int, int, str]]] = {}
    found_allowed: set[str] = set()
    for line in symbol_lines:
        match = MAP_RE.match(line)
        if match:
            address, index, kind = match.groups()
            mappings.setdefault(int(index), []).append((int(address, 16), kind))
        match = FUNC_RE.match(line)
        if match:
            address, size, index, name = match.groups()
            if name in allowed_names:
                # ELF Thumb function values carry the state bit in bit 0;
                # disassembly/mapping-symbol addresses do not.
                start = int(address, 16) & ~1
                end = start + int(size)
                if end <= start:
                    print(f"ERROR: allowlisted symbol has no size: {name}", file=sys.stderr)
                    return 1
                allowed_ranges.setdefault(int(index), []).append((start, end, name))
                found_allowed.add(name)
    missing = allowed_names - found_allowed
    if missing:
        print("ERROR: allowlisted symbols not found: " + ", ".join(sorted(missing)),
              file=sys.stderr)
        return 1
    for entries in mappings.values():
        entries.sort()

    violations: list[str] = []
    allowed_hits: set[str] = set()
    section = ""
    mode = ""
    entries: list[tuple[int, str]] = []
    map_pos = 0
    for line in output("arm-none-eabi-objdump", "-d", elf).splitlines():
        match = SECTION_RE.match(line)
        if match:
            section = match.group(1)
            entries = mappings.get(section_index.get(section, -1), [])
            map_pos = 0
            mode = ""
            continue
        match = INSN_RE.match(line)
        if not match or not entries:
            continue
        address = int(match.group(1), 16)
        while map_pos < len(entries) and entries[map_pos][0] <= address:
            mode = entries[map_pos][1]
            map_pos += 1
        if mode != "t":
            continue
        mnemonic = match.group(2).lower()
        operands = match.group(3).lower()
        is_vector = mnemonic.startswith("v")
        is_raw_cp = mnemonic.split(".", 1)[0] in {
            "cdp", "cdp2", "ldc", "ldc2", "mcr", "mcr2", "mcrr",
            "mcrr2", "mrc", "mrc2", "mrrc", "mrrc2", "stc", "stc2",
        } and re.search(r"\bp1[01]\b", operands)
        if is_vector or is_raw_cp:
            index = section_index.get(section, -1)
            allowed = next(
                (name for start, end, name in allowed_ranges.get(index, [])
                 if start <= address < end),
                None,
            )
            if allowed:
                allowed_hits.add(allowed)
            else:
                violations.append(
                    f"{section}:0x{address:08x}: {mnemonic} {operands}".rstrip()
                )

    if violations:
        print("ERROR: Thumb VFP/NEON instructions found:", file=sys.stderr)
        for item in violations:
            print(f"  {item}", file=sys.stderr)
        return 1
    unused = allowed_names - allowed_hits
    if unused:
        print("ERROR: allowlisted symbols contained no Thumb FP: " +
              ", ".join(sorted(unused)), file=sys.stderr)
        return 1
    suffix = ""
    if allowed_hits:
        suffix = " (diagnostic allowlist: " + ", ".join(sorted(allowed_hits)) + ")"
    print(f"Thumb-FP gate: PASS ({elf}){suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
