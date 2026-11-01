#!/usr/bin/env python3
"""Verify a retained GL app starts each launch with freshly zeroed BSS."""

from pathlib import Path
import struct
import subprocess
import unittest

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB
from unicorn.arm_const import (
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
    UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP,
)

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "apps/blocks/build/blocks"
BASE = 0x09500000
SHARED = 0x09000000
STACK = 0x09E00000
STOP = 0x09F00000
HB_SHARED_MAGIC = 0x48534248


def symbols(path):
    out = subprocess.run(["arm-none-eabi-nm", "-n", str(path)], check=True,
                         text=True, capture_output=True).stdout
    return {line.split()[2]: int(line.split()[0], 16)
            for line in out.splitlines() if len(line.split()) == 3}


def relocated(data, base):
    magic, entry, image_size, span, count, align = struct.unpack_from(
        "<6I", data)
    assert magic == 0x314C5248 and align == 16
    image = bytearray(span)
    image[:image_size] = data[24:24 + image_size]
    for (offset,) in struct.iter_unpack(
            "<I", data[24 + image_size:24 + image_size + count * 4]):
        value = struct.unpack_from("<I", image, offset)[0]
        struct.pack_into("<I", image, offset, (value + base) & 0xFFFFFFFF)
    return entry, bytes(image)


class GLRelaunchTests(unittest.TestCase):
    def setUp(self):
        self.syms = symbols(APP.with_suffix(".reloc.elf"))
        self.entry, image = relocated(APP.with_suffix(".hbapp").read_bytes(), BASE)
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.mem_map(0x08000000, 0x02000000)
        self.uc.mem_write(BASE, image)
        self.word(SHARED, HB_SHARED_MAGIC)
        self.word(SHARED + 4, 2)

    def word(self, address, value=None):
        if value is not None:
            self.uc.mem_write(address, struct.pack("<I", value))
        return struct.unpack("<I", self.uc.mem_read(address, 4))[0]

    def call(self, op=0):
        self.uc.reg_write(UC_ARM_REG_SP, STACK)
        self.word(STACK, SHARED)  # fifth AAPCS argument
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.uc.reg_write(UC_ARM_REG_R0, op)
        self.uc.reg_write(UC_ARM_REG_R1, 0)
        self.uc.reg_write(UC_ARM_REG_R2, 240)
        self.uc.reg_write(UC_ARM_REG_R3, 240)
        self.uc.emu_start(BASE + self.entry | 1, STOP, count=2_000_000)
        self.assertEqual(self.uc.reg_read(UC_ARM_REG_PC), STOP)
        return self.uc.reg_read(UC_ARM_REG_R0)

    def test_second_launch_zeros_stale_bss_and_restores_shared_pointer(self):
        g_inited = BASE + self.syms["g_inited"]
        shared_ptr = BASE + self.syms["hb_shared"]
        expected_frame = BASE + self.syms["gl_app_frame"] | 1

        self.word(g_inited, 0x11111111)
        self.word(shared_ptr, 0x22222222)
        self.assertEqual(self.call(), expected_frame)
        self.assertEqual(self.word(g_inited), 0)
        self.assertEqual(self.word(shared_ptr), SHARED)

        self.word(g_inited, 0x33333333)
        self.word(shared_ptr, 0x44444444)
        self.assertEqual(self.call(), expected_frame)
        self.assertEqual(self.word(g_inited), 0)
        self.assertEqual(self.word(shared_ptr), SHARED)

    def test_nonlaunch_operation_does_not_touch_bss(self):
        g_inited = BASE + self.syms["g_inited"]
        self.word(g_inited, 0x55555555)
        self.assertEqual(self.call(op=1), 0)
        self.assertEqual(self.word(g_inited), 0x55555555)


if __name__ == "__main__":
    unittest.main(verbosity=2)
