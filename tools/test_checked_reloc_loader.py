#!/usr/bin/env python3
"""Run the real checked HRL1 file loader against valid/corrupt slot bytes."""

import struct
import unittest

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_SP)

from arm_elf import ROOT, symbols
from test_deferred_stage import hrl_info, relocated_hrl
from wrap_resident_slot import fnv1a32, wrap_slot, validate_slot


SELECTOR = ROOT / "system/disk_bootstrap/build/disk_bootstrap"
SELECTOR_BASE = 0x09700000
ARENA = 0x09800013
STACK = 0x09E00000
STOP = 0x09F00000
PATH = 0x09D00000
OUT = 0x09D00100


def checked_selector_slot():
    """Use the freshly built selector HRL1 as a small checked-loader fixture."""
    packed = SELECTOR.with_suffix(".hbapp").read_bytes()
    return packed + struct.pack("<I", fnv1a32(packed))


class CheckedLoaderMachine:
    def __init__(self, file_data):
        self.file_data = file_data
        self.frees = []
        self.flushes = []
        self.syms = symbols(SELECTOR.with_suffix(".reloc.elf"))
        selector = SELECTOR.with_suffix(".hbapp").read_bytes()
        self.selector_entry, _image, self.selector_span, _align, _relocs = hrl_info(selector)
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.mem_map(0x08000000, 0x02000000)
        self.uc.mem_write(SELECTOR_BASE, relocated_hrl(selector, SELECTOR_BASE))
        self.uc.mem_write(PATH, b"/slot\0")
        self.handlers = {}
        for name in ("hb_fs_size_at", "hb_fs_read_at", "hb_os_heap_free",
                     "hb_os_heap_largest", "hb_os_free", "hb_os_alloc",
                     "hb_trace_log", "hb_icache_invalidate"):
            self.handlers[SELECTOR_BASE + (self.syms[name] & ~1)] = name
        self.uc.hook_add(UC_HOOK_CODE, self.on_call)

    def word(self, address, value=None):
        if value is not None:
            self.uc.mem_write(address, struct.pack("<I", value))
        return struct.unpack("<I", self.uc.mem_read(address, 4))[0]

    def ret(self, value=0):
        self.uc.reg_write(UC_ARM_REG_R0, value)
        self.uc.reg_write(UC_ARM_REG_PC, self.uc.reg_read(UC_ARM_REG_LR))

    def on_call(self, uc, address, _size, _data):
        name = self.handlers.get(address)
        if not name:
            return
        if name == "hb_fs_size_at":
            self.ret(len(self.file_data))
        elif name == "hb_fs_read_at":
            dst = uc.reg_read(UC_ARM_REG_R1)
            maximum = uc.reg_read(UC_ARM_REG_R2)
            amount = min(maximum, len(self.file_data))
            uc.mem_write(dst, self.file_data[:amount])
            self.ret(amount)
        elif name in ("hb_os_heap_free", "hb_os_heap_largest"):
            self.ret(0x100000)
        elif name == "hb_os_alloc":
            self.ret(ARENA)
        elif name == "hb_os_free":
            self.frees.append(uc.reg_read(UC_ARM_REG_R0))
            self.ret()
        elif name == "hb_trace_log":
            self.ret()
        elif name == "hb_icache_invalidate":
            self.flushes.append((uc.reg_read(UC_ARM_REG_R0),
                                 uc.reg_read(UC_ARM_REG_R1)))
            self.ret()

    def run(self):
        sp = STACK - 0x20
        self.word(sp, OUT + 8)       # fifth argument: out_base
        self.word(sp + 4, OUT + 12)  # sixth argument: out_span
        self.word(sp + 8, OUT + 16)  # seventh argument: out_image_size
        self.word(sp + 12, OUT + 20) # eighth argument: out_version
        self.uc.reg_write(UC_ARM_REG_SP, sp)
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.uc.reg_write(UC_ARM_REG_R0, PATH)
        self.uc.reg_write(UC_ARM_REG_R1, 0)
        self.uc.reg_write(UC_ARM_REG_R2, OUT)
        self.uc.reg_write(UC_ARM_REG_R3, OUT + 4)
        entry = SELECTOR_BASE + (self.syms["hb_reloc_load_file_checked_at"] & ~1)
        try:
            self.uc.emu_start(entry | 1, STOP, count=5_000_000)
        except UcError as error:
            raise AssertionError(
                f"checked loader failed at {self.uc.reg_read(UC_ARM_REG_PC):#x}: {error}")
        return self.uc.reg_read(UC_ARM_REG_R0)


class CheckedRelocLoaderTests(unittest.TestCase):
    def test_versioned_slot_and_metadata_are_validated_before_relocation(self):
        packed = SELECTOR.with_suffix('.hbapp').read_bytes()
        slot = wrap_slot(packed, 123456)
        self.assertEqual(validate_slot(slot), (packed, 123456))
        m = CheckedLoaderMachine(slot)
        self.assertNotEqual(m.run(), 0)
        self.assertEqual(m.word(OUT + 20), 123456)
        for field in (0, 1, 2, 3):
            with self.subTest(field=field):
                bad = bytearray(slot)
                struct.pack_into('<I', bad, len(packed) + field * 4, 0)
                struct.pack_into('<I', bad, len(bad) - 4, fnv1a32(bad[:-4]))
                m = CheckedLoaderMachine(bytes(bad))
                self.assertEqual(m.run(), 0)
                self.assertEqual(m.frees, [ARENA])
                self.assertEqual(m.flushes, [])

    def test_valid_slot_relocates_and_publishes(self):
        slot = checked_selector_slot()
        packed = slot[:-4]
        entry_off, _image, span, align, _relocs = hrl_info(packed)
        m = CheckedLoaderMachine(slot)
        result = m.run()
        base = (ARENA + align - 1) & ~(align - 1)
        self.assertEqual(result, base + entry_off | 1)
        self.assertEqual(m.word(OUT), ARENA)
        self.assertEqual(m.word(OUT + 4), len(slot))
        self.assertEqual(m.word(OUT + 8), base)
        self.assertEqual(m.word(OUT + 12), span)
        self.assertEqual(m.word(OUT + 16), hrl_info(packed)[1])
        self.assertEqual(bytes(m.uc.mem_read(base, span)), relocated_hrl(packed, base))
        self.assertEqual(m.flushes, [(base, span)])
        self.assertEqual(m.frees, [])

    def test_checksum_corruption_is_rejected_and_freed(self):
        bad = bytearray(checked_selector_slot())
        bad[100] ^= 0x40
        m = CheckedLoaderMachine(bytes(bad))
        self.assertEqual(m.run(), 0)
        self.assertEqual(m.frees, [ARENA])
        self.assertEqual(m.flushes, [])

    def test_canonical_relocation_check_precedes_code_changes(self):
        bad = bytearray(checked_selector_slot())
        _entry, image, _span, _align, relocs = hrl_info(bad[:-4])
        self.assertGreaterEqual(len(relocs), 2)
        reloc_table = 24 + image
        first = struct.unpack_from("<I", bad, reloc_table)[0]
        struct.pack_into("<I", bad, reloc_table + 4, first)
        struct.pack_into("<I", bad, len(bad) - 4, fnv1a32(bad[:-4]))
        m = CheckedLoaderMachine(bytes(bad))
        self.assertEqual(m.run(), 0)
        self.assertEqual(m.frees, [ARENA])
        self.assertEqual(m.flushes, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
