"""Exercise demo hooks and original forwarding at near/far resident addresses."""
import struct
import unittest
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
    UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
    UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
    UC_ARM_REG_R12, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_SP)
from test_untether import ChainMachine

CLOCK, LEVEL, STATUS = 0x084a9adc, 0x08215684, 0x08215600
LITERAL = 0x084a9c5c
SELF, OUT, DATE = 0x09ed0000, 0x09ed1000, 0x09ed2000
REGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
        UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12, UC_ARM_REG_LR)


def machine(base):
    m = ChainMachine(real_resident=True, heap_base=base)
    m.word(LITERAL, 0x88776655)
    m.arm(); m.dispatch()
    m.word(SELF + 0x19c, 0x12345678)
    return m


class DemoStatusbarTests(unittest.TestCase):
    def test_clock_preserves_literal_arguments_stack_and_locale_forwarding(self):
        for base in (0x08e00000, 0x09a00000):
            m = machine(base)
            calls = []

            def original(u, address, size, _):
                if address != CLOCK + 8:
                    return
                sp = u.reg_read(UC_ARM_REG_SP)
                dt = struct.unpack('<I', u.mem_read(sp + 56, 4))[0]
                calls.append((u.reg_read(UC_ARM_REG_R0), u.reg_read(UC_ARM_REG_R4),
                              u.reg_read(UC_ARM_REG_R1), u.reg_read(UC_ARM_REG_R2),
                              u.reg_read(UC_ARM_REG_R3), dt))
                # Model the original formatter's epilogue and return value.
                saved = struct.unpack('<14I', u.mem_read(sp, 56))
                for reg, value in zip(REGS, saved):
                    u.reg_write(reg, value)
                u.reg_write(UC_ARM_REG_SP, sp + 56)
                u.reg_write(UC_ARM_REG_R0, 0x941)
                u.reg_write(UC_ARM_REG_PC, saved[13])

            m.uc.hook_add(UC_HOOK_CODE, original)
            for enabled in (0, 1):
                m.uc.mem_write(m.res_addr('s_shared') + 29, bytes([enabled]))
                for fmt in (19, 20, 1):
                    with self.subTest(base=hex(base), enabled=enabled, fmt=fmt):
                        m.word(0x09e00000, DATE)
                        m.uc.reg_write(UC_ARM_REG_R3, fmt)
                        self.assertEqual(m.call(CLOCK, (SELF, OUT, 40)), 0x941)
                        literal, self_arg, out, size, form, date = calls[-1]
                        self.assertEqual((literal, self_arg, out, size, form),
                                         (0x88776655, SELF, OUT, 40, fmt))
                        if enabled and fmt in (19, 20):
                            self.assertEqual(bytes(m.uc.mem_read(date, 3)), b'\0\x29\x09')
                        else:
                            self.assertEqual(date, DATE)

    def test_battery_demo_values_and_original_behavior_at_any_distance(self):
        for base in (0x08e00000, 0x09a00000):
            m = machine(base)
            calls = []

            def original(u, address, size, _):
                if address not in (LEVEL + 8, STATUS + 8):
                    return
                calls.append((address, u.reg_read(UC_ARM_REG_R0),
                              u.reg_read(UC_ARM_REG_R4), u.reg_read(UC_ARM_REG_R5)))
                sp = u.reg_read(UC_ARM_REG_SP)
                r4, r5, r6, lr = struct.unpack('<4I', u.mem_read(sp, 16))
                for reg, value in ((UC_ARM_REG_R4, r4), (UC_ARM_REG_R5, r5),
                                   (UC_ARM_REG_R6, r6)):
                    u.reg_write(reg, value)
                u.reg_write(UC_ARM_REG_SP, sp + 16)
                u.reg_write(UC_ARM_REG_R0, 7)
                u.reg_write(UC_ARM_REG_PC, lr)

            m.uc.hook_add(UC_HOOK_CODE, original)
            for enabled in (0, 1, 0):
                m.uc.mem_write(m.res_addr('s_shared') + 29, bytes([enabled]))
                for site, demo_value in ((LEVEL, 22), (STATUS, 30)):
                    with self.subTest(base=hex(base), enabled=enabled, site=hex(site)):
                        before = len(calls)
                        self.assertEqual(m.call(site, (SELF,)), demo_value if enabled else 7)
                        if enabled:
                            self.assertEqual(len(calls), before)
                        elif site == LEVEL:
                            self.assertEqual(calls[-1][:3], (LEVEL + 8, 0x12345678, 30))
                        else:
                            self.assertEqual(calls[-1], (STATUS + 8, 149, 30, SELF))


if __name__ == '__main__':
    unittest.main(verbosity=2)
