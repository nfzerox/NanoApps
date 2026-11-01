"""Execute the resident's RAM fixes, including a dock hook beyond B.W range."""
from pathlib import Path
import struct
import unittest
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_INTR, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
    UC_ARM_REG_R12, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_SP)
from arm_elf import symbols
from test_untether import load_hrl

ROOT = Path(__file__).resolve().parents[1]
BT, DOCK, PORT = 0x082b2bd6, 0x084df4c4, 0x08918558
STOP, PACKET = 0x09f00000, 0x09e00000
BT_STOCK = bytes.fromhex('01ee10ca91ed010ab8ee400bb8ee411b08ee010bbceec00b81ed010a')
DOCK_STOCK = bytes.fromhex('10b54178022909d1811c05481a224ef706e9bde81040034848f7c0ba10bd0000a077ae08d4879108')


class Machine:
    def __init__(self, base=0x09a00000):
        prefix = ROOT / 'system/silver_resident/build/silver_resident'
        self.syms = symbols(prefix.with_suffix('.reloc.elf'))
        self.base = base
        self.m = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.m.mem_map(0x08000000, 0x02000000)
        load_hrl(self.m, prefix.with_suffix('.hbapp').read_bytes(), base)
        self.m.mem_write(BT, BT_STOCK)
        self.m.mem_write(DOCK, DOCK_STOCK)
        self.flushes, self.calls = [], []
        self.ready_after_sleep = False
        self.m.hook_add(UC_HOOK_INTR, lambda m, n, _: self.flushes.append(
            (m.reg_read(UC_ARM_REG_R0), m.reg_read(UC_ARM_REG_R1))))
        self.m.hook_add(UC_HOOK_CODE, self.service)

    def service(self, m, addr, size, _):
        r0, r1, r2 = [m.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)]
        if addr == 0x0842d6e0:
            m.mem_write(r0, bytes(m.mem_read(r1, r2)))
            self.calls.append(('copy', r2))
        elif addr == 0x08427a60:
            self.calls.append(('notify', r0))
            r0 = 0xace
        elif addr == 0x0842a994:
            r0 = 0x088ee000
        elif addr == 0x0802fbd8:
            self.calls.append(('post', r0, r1))
            r0 = 0
        elif addr == 0x080d0408:
            self.calls.append(('power', r0, r1))
            r0 = 0
        elif addr == 0x0842b1dc:
            self.calls.append(('sleep', r0, r1))
            if self.ready_after_sleep:
                m.mem_write(PORT, b'\x03')
            r0 = 0
        else:
            return
        m.reg_write(UC_ARM_REG_R0, r0)
        m.reg_write(UC_ARM_REG_PC, m.reg_read(UC_ARM_REG_LR))

    def address(self, name):
        return self.base + (self.syms[name] & ~1)

    def call(self, fn, arg=0):
        address = self.address(fn) if isinstance(fn, str) else fn
        self.m.reg_write(UC_ARM_REG_R0, arg)
        self.m.reg_write(UC_ARM_REG_SP, 0x09f80000)
        self.m.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.m.emu_start(address | 1, STOP, count=100000)
        assert self.m.reg_read(UC_ARM_REG_PC) == STOP
        return self.m.reg_read(UC_ARM_REG_R0)

    def packet(self, vendor=0x05ac, kind=5, packet_type=2):
        b = bytearray(28)
        b[1] = packet_type
        b[15:17] = vendor.to_bytes(2, 'big')
        b[18:20] = kind.to_bytes(2, 'big')
        self.m.mem_write(PACKET, bytes(b))
        return b


class CompatTests(unittest.TestCase):
    def test_bluetooth_guard_idempotence_and_timestamp_arithmetic(self):
        x = Machine()
        x.m.mem_write(BT, b'\0')
        self.assertEqual(x.call('hb_bluetooth_fix'), 0)
        self.assertFalse(x.flushes)
        x.m.mem_write(BT, BT_STOCK)
        self.assertEqual(x.call('hb_bluetooth_fix'), 1)
        self.assertEqual(x.flushes, [(BT, 28)])
        self.assertEqual(x.call('hb_bluetooth_fix'), 2)
        self.assertEqual(len(x.flushes), 1)
        for stamp, frames in ((123, 1), (0xfffffff0, 10), (0, 0x12345)):
            x.m.mem_write(PACKET + 4, struct.pack('<I', stamp))
            x.m.reg_write(UC_ARM_REG_R1, PACKET)
            x.m.reg_write(UC_ARM_REG_R12, frames)
            x.m.emu_start(BT | 1, BT + 28, count=100)
            self.assertEqual(struct.unpack('<I', x.m.mem_read(PACKET + 4, 4))[0],
                             (stamp + frames * 128) & 0xffffffff)

    def test_dock_mismatch_never_writes(self):
        x = Machine()
        x.m.mem_write(DOCK + 36, b'\0')  # literal pool is checked too
        before = bytes(x.m.mem_read(DOCK, 40))
        self.assertEqual(x.call('hb_dock_fix'), 0)
        self.assertEqual(bytes(x.m.mem_read(DOCK, 40)), before)
        self.assertFalse(x.flushes)

    def test_dock_hook_preserves_other_accessories_at_near_and_far_heap_addresses(self):
        for base in (0x08800000, 0x09a00000):
            with self.subTest(base=hex(base)):
                x = Machine(base)
                self.assertEqual(x.call('hb_dock_fix'), 1)
                self.assertEqual(x.call('hb_dock_fix'), 2)
                self.assertEqual(bytes(x.m.mem_read(DOCK + 10, 30)), DOCK_STOCK[10:])
                packet = x.packet(vendor=0x1234)
                self.assertEqual(x.call(DOCK, PACKET), 0xace)
                self.assertEqual(bytes(x.m.mem_read(0x08ae77a0, 26)), bytes(packet[2:]))
                self.assertFalse(any(c[0] == 'post' for c in x.calls))
                x.calls.clear()
                packet = x.packet(packet_type=1)
                self.assertEqual(x.call(DOCK, PACKET), PACKET)
                self.assertFalse(x.calls)

    def test_dock_only_powers_matching_ready_accessory(self):
        x = Machine()
        x.call('hb_dock_fix')
        packet = x.packet()
        x.call(DOCK, PACKET)
        packet[19] = 2
        self.assertEqual(bytes(x.m.mem_read(0x08ae77a0, 26)), bytes(packet[2:]))
        self.assertEqual(len([c for c in x.calls if c[0] == 'post']), 1)
        power_callback = [c[2] for c in x.calls if c[0] == 'post'][0]
        x.m.mem_write(PORT, b'\x01')
        x.ready_after_sleep = True
        x.call(power_callback)
        self.assertIn(('power', 4, 1), x.calls)
        for state in (0, 1, 4):
            x.calls.clear()
            x.ready_after_sleep = False
            x.m.mem_write(PORT, bytes([state]))
            x.call(power_callback)
            self.assertFalse(any(c[0] == 'power' for c in x.calls))
        x.packet(vendor=0x1234)
        x.call(DOCK, PACKET)
        x.calls.clear()
        x.m.mem_write(PORT, b'\x03')
        x.call(power_callback)
        self.assertFalse(x.calls)


if __name__ == '__main__':
    unittest.main(verbosity=2)
