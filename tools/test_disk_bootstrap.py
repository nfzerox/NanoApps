#!/usr/bin/env python3
"""Run the donor-derived disk bootstrap with late media and USB-return events."""
import struct
import unittest

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_SP)
from arm_elf import ROOT, symbols
from test_deferred_stage import hrl_info, relocated_hrl

SELECTOR = ROOT / 'system/disk_bootstrap/build/disk_bootstrap'
SELECTOR_BASE = 0x09700000
RESIDENT_BASE = 0x09880000
RESIDENT_ARENA = 0x0987FFF0
STACK = 0x09E00000
STOP = 0x09F00000
HOME_RAW_SLOT = 0x087CBB64
HOME_ACTION_SLOT = 0x087CBB6C
TEMPLATE_SLOT = 0x087AB8A4
STOCK_RAW = 0x081C4505
STOCK_ACTION = 0x082AE8DD
STOCK_TEMPLATE = 0x081E0A03
REPORT_BASE = 0x086C9B84
REPORT_MAGIC = 0x32505452
DISK = '/Library/resident.hbapp'


class BootstrapMachine:
    def __init__(self, ready=True, valid=True, resident_ok=True):
        self.ready, self.valid, self.resident_ok = ready, valid, resident_ok
        self.disabled = False
        self.ready_after_stock = False
        self.calls = []
        self.syms = symbols(SELECTOR.with_suffix('.reloc.elf'))
        packed = SELECTOR.with_suffix('.hbapp').read_bytes()
        self.entry_off, self.image, self.span, *_ = hrl_info(packed)
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.mem_map(0x08000000, 0x02000000)
        self.uc.mem_write(SELECTOR_BASE, relocated_hrl(packed, SELECTOR_BASE))
        for slot, stock in ((HOME_RAW_SLOT, STOCK_RAW), (HOME_ACTION_SLOT, STOCK_ACTION),
                            (TEMPLATE_SLOT, STOCK_TEMPLATE)):
            self.word(slot, stock)
        self.word(REPORT_BASE + 0x2c, REPORT_MAGIC)
        self.handlers = {SELECTOR_BASE + (self.syms[name] & ~1): name for name in (
            'hb_fs_is_dir', 'hb_fs_exists', 'hb_reloc_load_file_checked_at', 'hb_reloc_file_status',
            'hb_icache_invalidate', 'hb_silver_rawmsg')}
        self.handlers.update({RESIDENT_BASE: 'resident', STOCK_ACTION & ~1: 'stock',
                              STOCK_TEMPLATE & ~1: 'template', RESIDENT_BASE + 0x100: 'new_action'})
        self.uc.hook_add(UC_HOOK_CODE, self.on_call)
        self.call(SELECTOR_BASE + self.entry_off | 1)

    def word(self, address, value=None):
        if value is not None:
            self.uc.mem_write(address, struct.pack('<I', value))
        return struct.unpack('<I', self.uc.mem_read(address, 4))[0]

    def string(self, address):
        return bytes(self.uc.mem_read(address, 128)).split(b'\0', 1)[0].decode()

    def ret(self, value=0):
        self.uc.reg_write(UC_ARM_REG_R0, value)
        self.uc.reg_write(UC_ARM_REG_PC, self.uc.reg_read(UC_ARM_REG_LR))

    def on_call(self, uc, address, _size, _data):
        what = self.handlers.get(address)
        if not what:
            return
        args = tuple(uc.reg_read(reg) for reg in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3))
        if what == 'hb_fs_is_dir':
            assert self.string(args[0]) == '/iPod_Control'
            self.ret(int(self.ready))
        elif what == 'hb_fs_exists':
            assert self.string(args[0]) == '/Library/disabled'
            self.ret(int(self.disabled))
        elif what == 'hb_reloc_load_file_checked_at':
            assert self.string(args[0]) == DISK and args[1] == 0
            self.calls.append(('load', DISK))
            if not self.valid:
                self.ret()
                return
            sp = uc.reg_read(UC_ARM_REG_SP)
            for out, value in ((args[2], RESIDENT_ARENA), (args[3], 0x4000),
                    (self.word(sp), RESIDENT_BASE), (self.word(sp + 4), 0x1000),
                    (self.word(sp + 8), 0x800), (self.word(sp + 12), 123456)):
                self.word(out, value)
            self.ret(RESIDENT_BASE | 1)
        elif what == 'hb_reloc_file_status':
            self.ret(0 if self.valid else 1 << 24)
        elif what == 'resident':
            self.calls.append(('resident', *args[:3]))
            self.word(HOME_ACTION_SLOT, RESIDENT_BASE + 0x101)
            self.word(HOME_RAW_SLOT, RESIDENT_BASE + 0x201)
            self.word(TEMPLATE_SLOT, RESIDENT_BASE + 0x301 if self.resident_ok else STOCK_TEMPLATE)
            self.ret()
        elif what in ('stock', 'hb_silver_rawmsg'):
            self.calls.append((what,))
            if self.ready_after_stock:
                self.ready = True
            self.ret(0x510c)
        elif what == 'new_action':
            self.calls.append(('new_action', self.string(args[1]), args[2]))
            self.ret(0xa110)
        elif what == 'template':
            self.calls.append(('template', *args))
            self.ret()
        else:
            self.ret()

    def call(self, address, args=()):
        self.uc.reg_write(UC_ARM_REG_SP, STACK)
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            self.uc.reg_write(reg, value)
        self.uc.emu_start(address, STOP, count=5_000_000)
        return self.uc.reg_read(UC_ARM_REG_R0)

    def action(self):
        self.uc.mem_write(0x09d00000, b'HandleGotoMainScreen\0')
        return self.call(self.word(HOME_ACTION_SLOT), (0x123, 0x09d00000, 0x456))

    def usb_return(self, notice=0x6385):
        self.word(0x09d00104, 30)
        self.word(0x09d00108, 0x09d00200)
        self.word(0x09d00204, notice)
        return self.call(self.word(HOME_RAW_SLOT), (0x123, 0x09d00100))


class DiskBootstrapTests(unittest.TestCase):
    def assert_loaded(self, m):
        self.assertEqual(m.word(HOME_ACTION_SLOT), RESIDENT_BASE + 0x101)
        self.assertEqual(m.word(HOME_RAW_SLOT), RESIDENT_BASE + 0x201)
        self.assertEqual(m.word(TEMPLATE_SLOT), RESIDENT_BASE + 0x301)
        self.assertEqual(m.word(REPORT_BASE + 0x78), 3)
        self.assertIn(('resident', RESIDENT_BASE, 0x1000, 123456), m.calls)
        self.assertEqual(m.calls.count(('load', DISK)), 1)

    def test_entry_arms_waiters_without_disk_io(self):
        m = BootstrapMachine()
        self.assertEqual(m.calls, [])
        for slot in (HOME_ACTION_SLOT, HOME_RAW_SLOT, TEMPLATE_SLOT):
            self.assertTrue(SELECTOR_BASE <= m.word(slot) & ~1 < SELECTOR_BASE + m.image)
        self.assertEqual(m.word(REPORT_BASE + 0x78), 4)
        self.assertEqual(m.word(REPORT_BASE + 0x164), 4)

    def test_existing_resident_loads_and_forwards_original_action(self):
        m = BootstrapMachine()
        self.assertEqual(m.action(), 0xa110)
        self.assert_loaded(m)
        self.assertIn(('new_action', 'HandleGotoMainScreen', 0x456), m.calls)
        self.assertNotIn(('stock',), m.calls)

    def test_disk_becoming_ready_in_stock_action_initializes_without_replay(self):
        m = BootstrapMachine(ready=False)
        m.ready_after_stock = True
        self.assertEqual(m.action(), 0x510c)
        self.assert_loaded(m)
        self.assertEqual(m.calls.count(('stock',)), 1)
        self.assertIn(('new_action', 'NanoAppsInitialize', 0), m.calls)

    def test_fresh_restore_copy_and_eject_needs_no_action(self):
        m = BootstrapMachine(ready=False, valid=False)
        m.action()
        m.ready, m.valid = True, True
        self.assertEqual(m.usb_return(), 0x510c)
        self.assert_loaded(m)
        self.assertIn(('new_action', 'NanoAppsInitialize', 0), m.calls)

    def test_bad_resident_can_be_replaced_then_loaded_on_eject(self):
        m = BootstrapMachine(valid=False)
        m.action()
        self.assertFalse(any(c[0] == 'resident' for c in m.calls))
        m.valid = True
        m.usb_return()
        self.assertEqual(m.word(REPORT_BASE + 0x78), 3)
        self.assertEqual(sum(c[0] == 'resident' for c in m.calls), 1)

    def test_other_raw_messages_do_not_try_disk_and_template_forwards_all_args(self):
        m = BootstrapMachine()
        m.usb_return(0x1234)
        self.assertEqual(m.calls, [('hb_silver_rawmsg',)])
        m.call(m.word(TEMPLATE_SLOT), (1, 2, 3, 4))
        self.assertIn(('template', 1, 2, 3, 4), m.calls)

    def test_disable_marker_blocks_late_load_and_removal_allows_eject_retry(self):
        m = BootstrapMachine()
        m.disabled = True
        m.action()
        self.assertFalse(any(c[0] == 'load' for c in m.calls))
        m.disabled = False
        m.usb_return()
        self.assert_loaded(m)

    def test_partial_resident_install_restores_stock_without_retry(self):
        m = BootstrapMachine(resident_ok=False)
        m.action()
        self.assertEqual(m.word(REPORT_BASE + 0x78), 2)
        for slot, stock in ((HOME_RAW_SLOT, STOCK_RAW), (HOME_ACTION_SLOT, STOCK_ACTION),
                            (TEMPLATE_SLOT, STOCK_TEMPLATE)):
            self.assertEqual(m.word(slot), stock)
        m.action()
        self.assertEqual(m.calls.count(('load', DISK)), 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
