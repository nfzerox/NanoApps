"""Exercise compiled publisher code and C6/96 preservation without Apple code."""
from pathlib import Path
import struct
import unittest
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_INTR
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R12

ROOT = Path(__file__).resolve().parents[1]
CAVE, STACK, STOP = 0x086c97e4, 0x09e00000, 0x09f00000
SITE96, SITE98, ACTION = 0x0819c458, 0x0819c1d4, 0x087cbb6c


class PublisherTests(unittest.TestCase):
    def test_complete_handler_is_source_built_and_original_handler_is_preserved(self):
        publisher = (ROOT / 'system/firmware_publisher/build/recorder_publisher.bin').read_bytes()
        stub = (ROOT / 'system/firmware_publisher/build/scsi-stub.bin').read_bytes()
        self.assertEqual(len(publisher), 500)
        self.assertEqual(len(stub), 140)
        self.assertEqual(publisher[0x168:], stub)
        m = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        m.mem_map(0x08000000, 0x02000000)
        m.mem_write(CAVE, bytes(0x600) + publisher)
        stock = bytes(range(140))
        m.mem_write(SITE96, stock)
        m.mem_write(ACTION, struct.pack('<I', 0x082ae8dd))
        m.mem_write(0x081994f8, struct.pack('<I', 0xffaef002))
        flushes = []

        def svc(uc, number, _data):
            flushes.append((uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)))
            for reg in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R12):
                uc.reg_write(reg, 0xbad00000)

        m.hook_add(UC_HOOK_INTR, svc)
        for attempt in range(2):
            m.mem_write(STACK - 0x268, struct.pack('<4I', 0x1111, 0x2222, 0x5555, STOP | 1))
            m.reg_write(UC_ARM_REG_SP, STACK)
            m.emu_start((CAVE + 0x600) | 1, STOP, count=10000)
            self.assertEqual(m.reg_read(UC_ARM_REG_PC), STOP)
            self.assertEqual(bytes(m.mem_read(SITE96, 140)), stock)
            self.assertEqual(bytes(m.mem_read(SITE98, 140)), stub)
            self.assertEqual(bytes(m.mem_read(0x081994f8, 4)), struct.pack('<I', 0xffaef002))
            self.assertEqual(len(flushes), 2)
        self.assertEqual(flushes[0], (SITE98, 140))
        self.assertEqual(struct.unpack('<I', m.mem_read(ACTION, 4))[0], CAVE | 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
