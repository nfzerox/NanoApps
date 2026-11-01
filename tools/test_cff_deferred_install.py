#!/usr/bin/env python3
"""Emulate the cave publisher and its exact return-frame repair."""

import argparse
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_ARM, UC_HOOK_INTR, UC_MODE_THUMB
from unicorn.arm_const import (
    UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
    UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
    UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10,
    UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP,
)


STACK = 0x09E00000
STOP = 0x09F00000
HOME_RAW_SLOT = 0x087CBB64
HOME_ACTION_SLOT = 0x087CBB6C
STOCK_RAW = 0x081C4505
STOCK_ACTION = 0x082AE8DD
DEST = 0x086C97E4
STACK_SENTINELS = (0x53313532, 0x53313533)
R5_SENTINEL = 0x53415635
RETURN_SENTINEL = 0x5245544E
DESCRIPTOR_SENTINEL = 0x43534544
SAVED_REGS = (
    UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
    UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
)


def symbols(path):
    result = subprocess.run(
        ["arm-none-eabi-nm", "-n", str(path)],
        check=True, text=True, capture_output=True,
    )
    return {
        parts[2]: int(parts[0], 16)
        for line in result.stdout.splitlines()
        if len(parts := line.split()) == 3
    }


def patch_publisher(data, saved_stack, saved_r5):
    replacements = {
        **dict(zip(STACK_SENTINELS, saved_stack)),
        R5_SENTINEL: saved_r5,
        RETURN_SENTINEL: STOP | 1,
    }
    patched = bytearray(data)
    seen = set()
    for offset in range(0, len(patched), 4):
        value = struct.unpack_from("<I", patched, offset)[0]
        if value in replacements:
            struct.pack_into("<I", patched, offset, replacements[value])
            seen.add(value)
    if seen != set(replacements):
        raise ValueError("publisher capture sentinels are missing or ambiguous")
    return bytes(patched)


def word(machine, address, value=None):
    if value is not None:
        machine.mem_write(address, struct.pack("<I", value))
    return struct.unpack("<I", machine.mem_read(address, 4))[0]


def main():
    root = Path(__file__).resolve().parents[1]
    build = root / "system/deferred_untether/build"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--installer", type=Path,
                        default=build / "cff_deferred_install.bin")
    parser.add_argument("--deferred-bin", type=Path,
                        default=build / "deferred_stage.bin")
    parser.add_argument("--deferred-elf", type=Path,
                        default=build / "deferred_stage.elf")
    args = parser.parse_args()

    publisher_template = args.installer.read_bytes()
    deferred = args.deferred_bin.read_bytes()
    action = symbols(args.deferred_elf)["deferred_action"]
    publisher = DEST + len(deferred)
    if action != DEST:
        raise ValueError("deferred_action must be exactly the fixed cave base")
    if publisher & 3:
        raise ValueError("publisher entry is not word aligned")

    descriptor_off = publisher_template.find(
        struct.pack("<I", DESCRIPTOR_SENTINEL))
    if descriptor_off < 0 or descriptor_off & 3:
        raise ValueError("publisher descriptor sentinel is missing or misaligned")
    if publisher_template.count(struct.pack("<I", DESCRIPTOR_SENTINEL)) != 1:
        raise ValueError("publisher descriptor sentinel is ambiguous")
    image_size = len(deferred) + len(publisher_template)
    descriptor_size = struct.unpack_from(
        "<I", publisher_template, descriptor_off + 0x2C)[0]
    if descriptor_size != image_size:
        raise ValueError("publisher descriptor has the wrong image size")
    if struct.unpack_from(
            "<I", publisher_template, descriptor_off + 0x34)[0] != DEST:
        raise ValueError("publisher descriptor has the wrong cave address")

    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    uc.mem_map(0x08000000, 0x02000000)
    word(uc, HOME_RAW_SLOT, STOCK_RAW)
    word(uc, HOME_ACTION_SLOT, STOCK_ACTION)

    interrupts = []

    def interrupt(machine, number, _):
        interrupts.append((
            number,
            machine.reg_read(UC_ARM_REG_R0),
            machine.reg_read(UC_ARM_REG_R1),
            machine.reg_read(UC_ARM_REG_R12),
        ))
        # Model the observed ABI: every volatile register may be destroyed.
        for register, value in (
            (UC_ARM_REG_R0, 0x5C5C0000), (UC_ARM_REG_R1, 0x5C5C0001),
            (UC_ARM_REG_R2, 0x5C5C0002), (UC_ARM_REG_R3, 0x5C5C0003),
            (UC_ARM_REG_R12, 0x5C5C000C),
        ):
            machine.reg_write(register, value)

    uc.hook_add(UC_HOOK_INTR, interrupt)

    def run(seed):
        before = tuple(seed + index for index in range(len(SAVED_REGS)))
        original_stack = tuple(seed + 0x100 + index for index in range(2))
        cave_publisher = patch_publisher(
            publisher_template, original_stack, before[1])
        uc.mem_write(DEST, deferred)
        uc.mem_write(publisher, cave_publisher)

        # State immediately after the stock cache wrapper has published the
        # complete cave and popped original r4 plus the publisher entry.
        uc.mem_write(STACK, struct.pack("<2I", before[0], publisher | 1))
        for register, value in zip(SAVED_REGS, before):
            uc.reg_write(register, value)
        uc.reg_write(UC_ARM_REG_R5, 0x084117DF)
        uc.reg_write(UC_ARM_REG_R0, 0x5C5C1000)
        uc.reg_write(UC_ARM_REG_SP, STACK + 8)
        uc.emu_start(publisher | 1, STOP, count=100000)
        after = tuple(uc.reg_read(register) for register in SAVED_REGS)
        if after != before:
            raise AssertionError(f"saved register mismatch: {after!r} != {before!r}")
        if uc.reg_read(UC_ARM_REG_R0) != 0:
            raise AssertionError("publisher did not reconstruct the decoder result")
        if uc.reg_read(UC_ARM_REG_SP) != STACK:
            raise AssertionError("publisher left the stack unbalanced")
        if uc.reg_read(UC_ARM_REG_PC) != STOP:
            raise AssertionError("publisher did not resume the captured continuation")
        repaired = struct.unpack("<2I", uc.mem_read(STACK, 8))
        if repaired != original_stack:
            raise AssertionError(
                f"caller stack repair mismatch: {repaired!r} != {original_stack!r}")
        if bytes(uc.mem_read(DEST, len(deferred))) != deferred:
            raise AssertionError("publisher changed the direct-written stage")
        return cave_publisher

    cave_publisher = run(0xCA110000)
    if word(uc, HOME_ACTION_SLOT) != action | 1:
        raise AssertionError("publisher did not publish the deferred action")
    if word(uc, HOME_RAW_SLOT) != STOCK_RAW:
        raise AssertionError("publisher changed the raw-message slot")
    expected_interrupts = [(2, HOME_ACTION_SLOT, 4, 7)]
    if interrupts != expected_interrupts:
        raise AssertionError(f"unexpected cache SVCs: {interrupts!r}")

    # A repeat with foreign ownership repairs and returns without publishing.
    custom_action = 0x09905555
    word(uc, HOME_ACTION_SLOT, custom_action)
    run(0xCA220000)
    if word(uc, HOME_ACTION_SLOT) != custom_action:
        raise AssertionError("repeat path overwrote a non-stock action hook")
    if interrupts != expected_interrupts:
        raise AssertionError("repeat path unexpectedly issued a cache SVC")

    print(
        f"pass: {len(cave_publisher)}-byte publisher after "
        f"{len(deferred)}-byte direct stage, action_svc={len(interrupts)}, "
        "no stream-pointer dependency"
    )


if __name__ == "__main__":
    main()
