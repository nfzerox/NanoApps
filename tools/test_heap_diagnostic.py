#!/usr/bin/env python3
"""Exercise the compiled Thumb heap diagnostic against a synthetic OS heap."""
import struct
import unittest
import platform

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP

from arm_elf import ROOT, symbols

STEM = ROOT / "system/heap_diagnostic/build/heap_diagnostic"
SLOT = 0x087cbb6c
RAW_SLOT = 0x087cbb64
STOCK = 0x082ae8dd
RAW_STOCK = 0x081c4505
STOP = 0x09f00000
REGION = 0x090ff000
BLOCKS = (0x090ff000, 0x090ff800, 0x0911f800, 0x0912f800, 0x0916f800)
SIZES = (0x800, 0x20000, 0x10000, 0x40000, 0x10800)
SNAPSHOT_SIZE = 36 + 20 * 8 + 10 * 28


class Machine:
    def __init__(self):
        self.syms = symbols(STEM.with_suffix(".elf"))
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.mem_map(0x08000000, 0x02000000)
        self.uc.mem_write(0x09130000, STEM.with_suffix(".bin").read_bytes())
        self.word(SLOT, STOCK)
        self.word(RAW_SLOT, RAW_STOCK)
        self.word(0x08909134, 0x08950000)
        self.word(0x08950000 + 0xa4, 1)
        self.word(0x08950000 + 4, REGION)
        self.word(0x08950000 + 8, 0x81000)
        for hdr, size in zip(BLOCKS, SIZES):
            self.word(hdr, size | 2)
            self.word(hdr + 4, 0x00240003)
        self.snapshot = None
        self.writes = []
        self.uc.hook_add(UC_HOOK_CODE, self.step)
        self.uc.hook_add(UC_HOOK_INTR, self.interrupt)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write)

    def word(self, addr, value=None):
        if value is not None:
            self.uc.mem_write(addr, struct.pack("<I", value))
        return struct.unpack("<I", self.uc.mem_read(addr, 4))[0]

    def on_write(self, _uc, _access, address, size, _value, _data):
        self.writes.append((address, size))

    def interrupt(self, _uc, number, _data):
        if number != 2:  # SVC 70
            raise AssertionError(f"unexpected interrupt {number}")

    def ret(self, value=0):
        self.uc.reg_write(UC_ARM_REG_R0, value)
        self.uc.reg_write(UC_ARM_REG_PC, self.uc.reg_read(UC_ARM_REG_LR))

    def step(self, uc, address, _size, _data):
        if address == (self.syms["hb_fs_write"] & ~1):
            path = bytes(uc.mem_read(uc.reg_read(UC_ARM_REG_R0), 64)).split(b"\0", 1)[0]
            assert path == b"/Library/heapdiag.bin"
            length = uc.reg_read(UC_ARM_REG_R2)
            assert length == SNAPSHOT_SIZE
            self.snapshot = bytes(uc.mem_read(uc.reg_read(UC_ARM_REG_R1), length))
            self.ret(1)
        elif address == (self.syms["hb_icache_invalidate"] & ~1):
            self.ret()
        elif address == (self.syms["hb_silver_rawmsg"] & ~1):
            self.ret(2)
        elif address == (STOCK & ~1):
            self.ret(0x1234)

    def call(self, address, args=()):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_SP, 0x09e00000)
        uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        for reg, arg in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args):
            uc.reg_write(reg, arg)
        uc.emu_start(address | 1, STOP, count=1500000)
        assert uc.reg_read(UC_ARM_REG_PC) == STOP
        assert uc.reg_read(UC_ARM_REG_SP) == 0x09e00000
        return uc.reg_read(UC_ARM_REG_R0)


@unittest.skipIf(platform.system() == "Darwin" and platform.machine() == "arm64",
                 "local Unicorn 2.1.4 crashes on this Apple Silicon host; run on the mini PC")
class DiagnosticTests(unittest.TestCase):
    def test_snapshot_and_inert_icons(self):
        m = Machine()
        m.call(m.syms["payload_entry"])
        self.assertIsNotNone(m.snapshot)
        header = struct.unpack_from("<9I", m.snapshot)
        self.assertEqual(header, (0x31445048, 1, 0x08950000, 1, 0, 0, 0, 5, 10))
        samples = [struct.unpack_from("<7I", m.snapshot, 36 + 160 + 28 * i)
                   for i in range(10)]
        self.assertTrue(all(row[-1] == 1 for row in samples))
        self.assertEqual(samples[0][2:6], (0x090ff800, 0x0911f800, 0x20002, 0x00240003))
        self.assertEqual(samples[-1][2:4], (0x0916f800, 0x09180000))
        self.assertFalse(any(0x09100000 <= addr < 0x09180000 for addr, _ in m.writes))
        self.assertEqual(m.call(m.word(SLOT), (1, 2, 3)), 0x1234)
        self.assertEqual(m.call(m.word(RAW_SLOT), (1, 2)), 1)

    def test_corrupt_header_stops_walk(self):
        m = Machine()
        m.word(0x0911f800, 4)
        m.call(m.syms["payload_entry"])
        header = struct.unpack_from("<9I", m.snapshot)
        self.assertEqual(header[4:7], (4, 0x0911f800, 4))
        trace = struct.unpack_from("<7I", m.snapshot, 36 + 160 + 28 * 3)
        self.assertEqual(trace[-1], 0)


if __name__ == "__main__":
    unittest.main()
