#!/usr/bin/env python3
"""Exercise the fixed deferred untether stage with mocked 39A10023 APIs.

The test loads the exact compiled stage and resident.hbapp artifacts.  It
models the firmware file-object calls closely enough to cover the exact
automatic-Home and Settings-fallback action gates, heap readiness, packed HRL1
read, relocation, three-hook handoff, failure cleanup and retry.
"""

import struct
import unittest

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE,
                     UC_HOOK_INTR)
from unicorn.arm_const import (
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
    UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5,
    UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
    UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP,
)

from arm_elf import ROOT, symbols


STAGE = ROOT / "system/deferred_untether/build/deferred_stage"
RESIDENT = ROOT / "system/disk_bootstrap/build/disk_bootstrap.hbapp"

HOME_RAW_SLOT = 0x087CBB64
HOME_ACTION_SLOT = 0x087CBB6C
TEMPLATE_SLOT = 0x087AB8A4
STOCK_RAW = 0x081C4505
STOCK_ACTION = 0x082AE8DD
STOCK_TEMPLATE = 0x081E0A03

HEAP_OBJECT_CELL = 0x08909134
HEAP_OBJECT = 0x08950000

OS_ALLOC = 0x0842B441
OS_FREE = 0x0842D86D
FOBJ_OPEN = 0x084137A9
FOBJ_CLOSE = 0x08423BE1
FOBJ_READY = 0x08417E19
FOBJ_READ = 0x0841CDDD
FS_EXISTS = 0x0841BB7D

HEAP_RAW = 0x09900013
# Deliberately different, non-firmware addresses: hardware passes equivalent
# strings through multiple pointers, so no test may rely on pointer identity.
SETTINGS_ACTION_PTR = 0x082F1040
MAIN_ACTION_PTR = 0x08317080
DYNAMIC_ACTION_PTR = 0x082AF000
REPORT_BASE = 0x086C9B84
REPORT_ACTION_HISTORY = 0x84
REPORT_ACTION_COUNT = 0x88
REPORT_LAST_ACTION = 0x8C
REPORT_READY_MASK = 0x90
REPORT_UNKNOWN_HASH = 0x94
REPORT_UNKNOWN_LENGTH = 0x98
REPORT_UNKNOWN_COUNT = 0x9C
REPORT_WRITE_END = 0xA0
PRIMARY_LIMIT = 0x3A0
REPORT_SIZE = 0x200
MATCHER_OFFSET = 0x5A0
MATCHER_SIZE = 0x60
PUBLISHED_SIZE = 0x600
STACK = 0x09E00000
STOP = 0x09F00000

SETTINGS_ACTION = b"HandleGotoSettings"
MAIN_ACTION = b"HandleGotoMainScreen"
DEFAULT_ACTION_ARGS = (0x1111, SETTINGS_ACTION_PTR, 0x3333, 0x4444)


def fnv1a32(data):
    value = 0x811C9DC5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value

ARG_REGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
CALLEE_REGS = (UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
               UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
               UC_ARM_REG_R10, UC_ARM_REG_R11)


def hrl_info(data):
    if len(data) < 24:
        raise ValueError("resident HRL1 is truncated")
    magic, entry, image, span, count, align = struct.unpack_from("<6I", data)
    if (magic != 0x314C5248 or not align or align > 32 or
            align & (align - 1) or entry & 1 or
            not entry < image <= span or len(data) > span or
            len(data) != 24 + image + 4 * count):
        raise ValueError("resident HRL1 header is inconsistent")
    relocs = struct.unpack_from(f"<{count}I", data, 24 + image)
    if (any(off & 3 or off + 4 > image for off in relocs) or
            any(right < left + 4 for left, right in zip(relocs, relocs[1:]))):
        raise ValueError("resident HRL1 relocation table is not canonical")
    return entry, image, span, align, relocs


def relocated_hrl(data, base):
    """Return the exact in-memory image expected from the stage's HRL1 loop."""
    _entry, image_size, span, _align, relocs = hrl_info(data)
    image = bytearray(span)
    image[:image_size] = data[24:24 + image_size]
    for off in relocs:
        value = struct.unpack_from("<I", image, off)[0]
        struct.pack_into("<I", image, off, (value + base) & 0xFFFFFFFF)
    return bytes(image)


class DeferredMachine:
    def __init__(self):
        self.stage_syms = symbols(STAGE.with_suffix(".elf"))
        self.stage_data = STAGE.with_suffix(".bin").read_bytes()
        self.matcher_before = self.stage_data[
            MATCHER_OFFSET:MATCHER_OFFSET + MATCHER_SIZE]
        self.resident_data = RESIDENT.read_bytes()
        (self.res_entry_off, self.res_image_size, self.res_span,
         self.res_align, self.res_relocs) = hrl_info(self.resident_data)

        self.stage_base = self.stage_syms["deferred_action"] & ~1
        self.stage_action = self.stage_syms["deferred_action"] | 1
        self.busy = self.stage_syms["stage_busy"]

        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.mem_map(0x08000000, 0x02000000)
        self.uc.mem_write(self.stage_base, self.stage_data)
        self.word(HEAP_OBJECT_CELL, HEAP_OBJECT)
        self.word(HOME_RAW_SLOT, STOCK_RAW)
        self.word(HOME_ACTION_SLOT, self.stage_action)
        self.word(TEMPLATE_SLOT, STOCK_TEMPLATE)
        self.set_action_string(SETTINGS_ACTION, SETTINGS_ACTION_PTR)
        self.set_action_string(MAIN_ACTION, MAIN_ACTION_PTR)

        self.alloc_fail = False
        self.open_ok = True
        self.short_read = None
        self.read_status = 0
        self.disabled = False
        self.live_allocs = {}
        self.calls = []
        self.flushes = []
        self.loaded_image = None
        self.entry_args = None
        self.entry_lr = None
        self.entry_hook_overrides = {}

        self.res_base = None
        self.res_action = None
        self.res_raw = None
        self.res_template = None
        self.handlers = {
            OS_ALLOC & ~1: "alloc",
            OS_FREE & ~1: "free",
            FOBJ_OPEN & ~1: "open",
            FOBJ_CLOSE & ~1: "close",
            FOBJ_READY & ~1: "ready",
            FOBJ_READ & ~1: "read",
            FS_EXISTS & ~1: "exists",
            STOCK_ACTION & ~1: "stock_action",
            STOCK_RAW & ~1: "stock_raw",
        }
        self.uc.hook_add(UC_HOOK_CODE, self.on_call)
        self.uc.hook_add(UC_HOOK_INTR, self.on_interrupt)

    def word(self, address, value=None):
        if value is not None:
            self.uc.mem_write(address, struct.pack("<I", value))
        return struct.unpack("<I", self.uc.mem_read(address, 4))[0]

    def string(self, address, limit=256):
        return bytes(self.uc.mem_read(address, limit)).split(b"\0", 1)[0].decode()

    def set_action_string(self, value, address=DYNAMIC_ACTION_PTR):
        if isinstance(value, str):
            value = value.encode("ascii")
        # Poison the tail so a missing terminator or prefix-only comparison is
        # visible to the emulated stage rather than masked by zero-filled RAM.
        self.uc.mem_write(address, value + b"\0" + b"\xA5" * 64)

    def args(self):
        return tuple(self.uc.reg_read(reg) for reg in ARG_REGS)

    def ret(self, value=0):
        self.uc.reg_write(UC_ARM_REG_R0, value)
        self.uc.reg_write(UC_ARM_REG_PC, self.uc.reg_read(UC_ARM_REG_LR))

    def on_interrupt(self, uc, number, _data):
        if number != 2:
            raise AssertionError(f"unexpected interrupt {number}")
        self.flushes.append((uc.reg_read(UC_ARM_REG_R0),
                             uc.reg_read(UC_ARM_REG_R1),
                             uc.reg_read(UC_ARM_REG_R12)))
        # Match the hardware-observed SVC ABI. Callers must not keep live
        # state in volatile registers across cache maintenance.
        for register, value in (
                (UC_ARM_REG_R0, 0x5C5C0000),
                (UC_ARM_REG_R1, 0x5C5C0001),
                (UC_ARM_REG_R2, 0x5C5C0002),
                (UC_ARM_REG_R3, 0x5C5C0003),
                (UC_ARM_REG_R12, 0x5C5C000C)):
            uc.reg_write(register, value)

    def on_call(self, uc, address, _size, _data):
        what = self.handlers.get(address)
        if not what:
            return
        a = self.args()

        if what == "alloc":
            self.calls.append(("alloc", a[0], a[1]))
            if self.alloc_fail:
                self.ret(0)
                return
            self.live_allocs[HEAP_RAW] = a[0]
            self.res_base = (HEAP_RAW + 31) & ~31
            # The bootstrap keeps waiting hooks in its retained executable image.
            self.res_action = self.res_base + 1
            self.res_raw = self.res_base + 5
            self.res_template = self.res_base + 9
            entry = self.res_base + self.res_entry_off
            self.handlers[entry] = "resident_entry"
            self.handlers[self.res_action & ~1] = "resident_action"
            self.handlers[self.res_raw & ~1] = "resident_raw"
            self.ret(HEAP_RAW)
        elif what == "exists":
            path = self.string(a[0])
            self.calls.append(("exists", path, a[1]))
            self.ret(int(self.disabled))
        elif what == "free":
            self.calls.append(("free", a[0]))
            if self.live_allocs.pop(a[0], None) is None:
                raise AssertionError(f"free of unknown allocation {a[0]:#x}")
            self.ret()
        elif what == "open":
            sp = uc.reg_read(UC_ARM_REG_SP)
            extras = struct.unpack("<4I", bytes(uc.mem_read(sp, 16)))
            path = self.string(a[1])
            self.calls.append(("open", a, path, extras))
            self.ret(0)
        elif what == "ready":
            self.calls.append(("ready", a[0]))
            self.ret(int(self.open_ok))
        elif what == "read":
            requested = a[1]
            amount = requested if self.short_read is None else self.short_read
            amount = min(amount, len(self.resident_data), requested)
            if amount:
                uc.mem_write(a[2], self.resident_data[:amount])
            self.word(a[3], amount)
            self.calls.append(("read", a[0], requested, a[2], a[3], amount))
            self.ret(self.read_status)
        elif what == "close":
            self.calls.append(("close", a[0]))
            self.ret()
        elif what == "resident_entry":
            self.entry_args = a
            self.entry_lr = uc.reg_read(UC_ARM_REG_LR)
            self.loaded_image = bytes(uc.mem_read(self.res_base, self.res_span))
            self.calls.append(("resident_entry", a))
            self.word(HOME_RAW_SLOT,
                      self.entry_hook_overrides.get("raw", self.res_raw))
            self.word(HOME_ACTION_SLOT,
                      self.entry_hook_overrides.get("action", self.res_action))
            self.word(TEMPLATE_SLOT,
                      self.entry_hook_overrides.get("template", self.res_template))
            self.ret()
        elif what == "resident_action":
            self.calls.append(("resident_action", a))
            self.ret(0xA110)
        elif what == "resident_raw":
            self.calls.append(("resident_raw", a[:2]))
            self.ret(0xA120)
        elif what == "stock_action":
            self.calls.append(("stock_action", a))
            self.ret(0x510C)
        elif what == "stock_raw":
            self.calls.append(("stock_raw", a[:2]))
            self.ret(0x520C)

    def call(self, address, args, count=20000000):
        self.uc.reg_write(UC_ARM_REG_SP, STACK)
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        for reg, value in zip(ARG_REGS, args):
            self.uc.reg_write(reg, value)
        for i, reg in enumerate(CALLEE_REGS):
            self.uc.reg_write(reg, 0xCA110000 + i)
        try:
            self.uc.emu_start(address | 1, STOP, count=count)
        except UcError as error:
            raise AssertionError(
                f"emulation failed at {self.uc.reg_read(UC_ARM_REG_PC):#x}, "
                f"lr={self.uc.reg_read(UC_ARM_REG_LR):#x}, args={self.args()}: "
                f"{error}") from error
        self.assert_machine_state()
        return self.uc.reg_read(UC_ARM_REG_R0)

    def assert_machine_state(self):
        if self.uc.reg_read(UC_ARM_REG_PC) != STOP:
            raise AssertionError("stage did not return")
        if self.uc.reg_read(UC_ARM_REG_SP) != STACK:
            raise AssertionError("stage left the stack unbalanced")
        for i, reg in enumerate(CALLEE_REGS):
            actual = self.uc.reg_read(reg)
            expected = 0xCA110000 + i
            if actual != expected:
                raise AssertionError(
                    f"callee r{4 + i} clobbered: {actual:#x} != {expected:#x}")

    def action(self, args=DEFAULT_ACTION_ARGS):
        return self.call(self.word(HOME_ACTION_SLOT), args)

    def main_action(self, args=None):
        if args is None:
            args = (0x1111, MAIN_ACTION_PTR, 0x3333, 0x4444)
        return self.action(args)

    def assert_matcher_unchanged(self):
        actual = bytes(self.uc.mem_read(
            self.stage_base + MATCHER_OFFSET, MATCHER_SIZE))
        if actual != self.matcher_before:
            raise AssertionError("published action matcher was rewritten")

class DeferredStageTests(unittest.TestCase):
    def assert_retry_armed(self, machine):
        self.assertEqual(machine.word(HOME_RAW_SLOT), STOCK_RAW)
        self.assertEqual(machine.word(HOME_ACTION_SLOT), machine.stage_action)
        self.assertEqual(machine.word(TEMPLATE_SLOT), STOCK_TEMPLATE)
        machine.assert_matcher_unchanged()
        self.assertEqual(machine.word(machine.busy), 0)

    def assert_success(self, machine, action_args):
        self.assertEqual(machine.word(HOME_RAW_SLOT), machine.res_raw)
        self.assertEqual(machine.word(HOME_ACTION_SLOT), machine.res_action)
        self.assertEqual(machine.word(TEMPLATE_SLOT), machine.res_template)
        self.assertEqual(machine.word(machine.busy), 0)
        self.assertEqual(machine.loaded_image,
                         relocated_hrl(machine.resident_data, machine.res_base))
        self.assertEqual(machine.entry_args[:2],
                         (machine.res_base, machine.res_span))
        self.assertTrue(machine.stage_base <= (machine.entry_lr & ~1) <
                        machine.stage_base + len(machine.stage_data))
        self.assertEqual(machine.calls[-1], ("resident_action", action_args))
        self.assertIn(HEAP_RAW, machine.live_allocs)

    def assert_failed_stock(self, machine):
        self.assertEqual(machine.word(HOME_RAW_SLOT), STOCK_RAW)
        self.assertEqual(machine.word(HOME_ACTION_SLOT), STOCK_ACTION)
        self.assertEqual(machine.word(TEMPLATE_SLOT), STOCK_TEMPLATE)
        self.assertEqual(machine.word(machine.busy), 0)
        self.assertEqual(machine.live_allocs, {})

    def test_stage_is_action_only_at_the_verified_free_atom(self):
        m = DeferredMachine()
        self.assertNotIn("deferred_rawmsg", m.stage_syms)
        self.assertEqual(m.stage_base, 0x086C97E4)
        self.assertEqual(m.stage_action, 0x086C97E5)
        self.assertEqual(m.word(HOME_RAW_SLOT), STOCK_RAW)
        self.assertEqual(m.word(TEMPLATE_SLOT), STOCK_TEMPLATE)
        self.assertEqual(len(m.stage_data), PUBLISHED_SIZE)
        self.assertLessEqual(
            m.stage_syms["deferred_primary_end"] - m.stage_base,
            PRIMARY_LIMIT)
        self.assertEqual(
            m.stage_syms["action_match_helper"] - m.stage_base,
            MATCHER_OFFSET)
        self.assertLessEqual(
            m.stage_syms["deferred_stage_end"] -
            m.stage_syms["action_match_helper"], MATCHER_SIZE)
        self.assertLessEqual(REPORT_WRITE_END, MATCHER_OFFSET)
        self.assertEqual(PRIMARY_LIMIT + REPORT_SIZE, MATCHER_OFFSET)
        self.assertEqual(m.stage_syms["deferred_stage_end"] - m.stage_base,
                         len(m.stage_data))

    def test_unrecognised_dynamic_names_are_stock_only_and_reported(self):
        actions = (
            b"HandleBeginRestoringNowPlayingState",
            b"HandleFinishRestoringNowPlayingState",
            b"HandleHomeButtonUp",
            b"",
            b"HandleGotoSetting",
            b"HandleGotoSettingsExtra",
        )
        for index, action in enumerate(actions):
            with self.subTest(action=action):
                m = DeferredMachine()
                m.set_action_string(action)
                args = (0xA000 + index, DYNAMIC_ACTION_PTR,
                        0xB000 + index, 0xC000 + index)
                hooks_before = (
                    m.word(HOME_RAW_SLOT), m.word(HOME_ACTION_SLOT),
                    m.word(TEMPLATE_SLOT),
                )
                primary_before = bytes(m.uc.mem_read(
                    m.stage_base, PRIMARY_LIMIT))
                heap_before = m.word(HEAP_OBJECT_CELL)

                self.assertEqual(m.action(args), 0x510C)

                self.assertEqual(m.calls, [("stock_action", args)])
                self.assertEqual(m.flushes, [])
                self.assertEqual(m.live_allocs, {})
                self.assertIsNone(m.loaded_image)
                self.assertEqual(m.word(HEAP_OBJECT_CELL), heap_before)
                self.assertEqual((m.word(HOME_RAW_SLOT),
                                  m.word(HOME_ACTION_SLOT),
                                  m.word(TEMPLATE_SLOT)), hooks_before)
                self.assertEqual(bytes(m.uc.mem_read(
                    m.stage_base, PRIMARY_LIMIT)), primary_before)
                self.assertEqual(m.word(REPORT_BASE + REPORT_UNKNOWN_HASH),
                                 fnv1a32(action[:64]))
                self.assertEqual(m.word(REPORT_BASE + REPORT_UNKNOWN_LENGTH),
                                 min(len(action), 64))
                self.assertEqual(m.word(REPORT_BASE + REPORT_UNKNOWN_COUNT), 1)
                m.assert_matcher_unchanged()

    def test_content_identity_ignores_action_pointer(self):
        cases = (
            (SETTINGS_ACTION, 2, 0x08123040),
            (SETTINGS_ACTION, 2, 0x08F10400),
            (SETTINGS_ACTION, 2, 0x09D00080),
            (MAIN_ACTION, 1, 0x08124080),
            (MAIN_ACTION, 1, 0x08F20440),
            (MAIN_ACTION, 1, 0x09D100C0),
        )
        for name, kind, address in cases:
            with self.subTest(name=name, address=hex(address)):
                m = DeferredMachine()
                m.set_action_string(name, address)
                args = (0x101, address, 0x303, 0x404)
                self.assertEqual(m.action(args), 0xA110)
                self.assert_success(m, args)
                self.assertEqual(
                    m.word(REPORT_BASE + REPORT_LAST_ACTION), kind)
                m.assert_matcher_unchanged()

    def test_automatic_main_action_loads_and_records_readiness(self):
        m = DeferredMachine()
        args = (0x10203040, MAIN_ACTION_PTR, 0x30405060, 0x40506070)
        self.assertEqual(m.main_action(args), 0xA110)
        self.assert_success(m, args)
        self.assertEqual(m.word(REPORT_BASE + REPORT_ACTION_HISTORY), 1)
        self.assertEqual(m.word(REPORT_BASE + REPORT_ACTION_COUNT), 1)
        self.assertEqual(m.word(REPORT_BASE + REPORT_LAST_ACTION), 1)
        self.assertEqual(m.word(REPORT_BASE + REPORT_READY_MASK), 31)
        m.assert_matcher_unchanged()

    def test_early_automatic_action_passes_through_then_settings_fallback_loads(self):
        m = DeferredMachine()
        m.word(HEAP_OBJECT_CELL, 0)
        main_args = (0xA1, MAIN_ACTION_PTR, 0xA3, 0xA4)
        self.assertEqual(m.main_action(main_args), 0x510C)
        self.assert_retry_armed(m)
        self.assertEqual(m.calls, [("stock_action", main_args)])
        self.assertEqual(m.word(REPORT_BASE + REPORT_ACTION_HISTORY), 1)
        self.assertEqual(m.word(REPORT_BASE + REPORT_ACTION_COUNT), 1)
        self.assertEqual(m.word(REPORT_BASE + REPORT_LAST_ACTION), 1)
        self.assertEqual(m.word(REPORT_BASE + REPORT_READY_MASK), 1)

        m.word(HEAP_OBJECT_CELL, HEAP_OBJECT)
        settings_args = (0xB1, SETTINGS_ACTION_PTR, 0xB3, 0xB4)
        self.assertEqual(m.action(settings_args), 0xA110)
        self.assert_success(m, settings_args)
        self.assertEqual(m.word(REPORT_BASE + REPORT_ACTION_HISTORY), 6)
        self.assertEqual(m.word(REPORT_BASE + REPORT_ACTION_COUNT), 2)
        self.assertEqual(m.word(REPORT_BASE + REPORT_LAST_ACTION), 2)
        self.assertEqual(m.word(REPORT_BASE + REPORT_READY_MASK), 31)
        m.assert_matcher_unchanged()

    def test_exact_hrl_load_relocation_handoff_and_argument_forwarding(self):
        m = DeferredMachine()
        args = (0x10203040, SETTINGS_ACTION_PTR, 0x30405060, 0x40506070)
        self.assertEqual(m.action(args), 0xA110)
        self.assert_success(m, args)

        read = next(call for call in m.calls if call[0] == "read")
        file_obj_off = (m.res_span + 7) & ~7
        file_cache_off = (file_obj_off + 0x60 + 31) & ~31
        arena_size = file_cache_off + 0x1010 + 31
        self.assertIn(("alloc", arena_size, 2), m.calls)
        self.assertEqual(read[2], len(m.resident_data))
        self.assertEqual(read[3], m.res_base + m.res_span - len(m.resident_data))
        self.assertEqual(read[5], len(m.resident_data))
        opened = next(call for call in m.calls if call[0] == "open")
        self.assertEqual(opened[2], "/NanoApps/loader.hbapp")
        self.assertEqual(opened[1][0], m.res_base + file_obj_off)
        self.assertEqual(opened[1][0] & 7, 0)
        self.assertEqual(opened[1][2:], (1, 4))
        self.assertEqual(opened[3][0:2], (0x1000, 1))
        self.assertEqual(opened[3][2], m.res_base + file_cache_off)
        self.assertEqual(m.flushes[0], (m.res_base, m.res_span, 7))
        self.assertEqual(m.flushes[1], (HOME_RAW_SLOT, 12, 7))
        self.assertEqual(m.flushes[2], (TEMPLATE_SLOT, 4, 7))

    def test_heap_must_be_live_before_allocator_is_called(self):
        for heap_word in (0, 0x07FFFFFC, 0x0A000000, HEAP_OBJECT | 1):
            with self.subTest(heap_word=hex(heap_word)):
                m = DeferredMachine()
                m.word(HEAP_OBJECT_CELL, heap_word)
                self.assertEqual(m.action(), 0x510C)
                self.assertEqual(m.calls, [
                    ("stock_action", DEFAULT_ACTION_ARGS)
                ])
                self.assertEqual(m.live_allocs, {})
                self.assert_retry_armed(m)

        m = DeferredMachine()
        m.word(HEAP_OBJECT_CELL, 0)
        self.assertEqual(m.action(), 0x510C)
        m.word(HEAP_OBJECT_CELL, HEAP_OBJECT)
        args = (0x51, SETTINGS_ACTION_PTR, 0x53, 0x54)
        self.assertEqual(m.action(args), 0xA110)
        self.assert_success(m, args)

    def test_allocator_failure_keeps_retry_hooks_then_succeeds(self):
        m = DeferredMachine()
        m.alloc_fail = True
        self.assertEqual(m.action(), 0x510C)
        self.assertEqual(m.live_allocs, {})
        self.assert_retry_armed(m)

        m.alloc_fail = False
        args = (0x11, SETTINGS_ACTION_PTR, 0x33, 0x44)
        self.assertEqual(m.action(args), 0xA110)
        self.assert_success(m, args)

    def test_disabled_marker_restores_stock_action_without_loading(self):
        m = DeferredMachine()
        m.disabled = True
        args = (0xD1, SETTINGS_ACTION_PTR, 0xD3, 0xD4)
        self.assertEqual(m.action(args), 0x510C)
        self.assertEqual(m.calls, [
            ("exists", "/Library/disabled", 0),
            ("stock_action", args),
        ])
        self.assertEqual(m.word(HOME_ACTION_SLOT), STOCK_ACTION)
        self.assertEqual(m.word(HOME_RAW_SLOT), STOCK_RAW)
        self.assertEqual(m.word(TEMPLATE_SLOT), STOCK_TEMPLATE)
        self.assertEqual(m.word(m.busy), 0)
        self.assertEqual(m.live_allocs, {})
        self.assertEqual(m.flushes, [(HOME_ACTION_SLOT, 4, 7)])

    def test_open_failure_closes_frees_and_retries(self):
        m = DeferredMachine()
        m.open_ok = False
        self.assertEqual(m.action(), 0x510C)
        self.assertEqual(m.live_allocs, {})
        self.assertEqual([call[0] for call in m.calls],
                         ["exists", "alloc", "open", "ready", "close", "free",
                          "stock_action"])
        self.assert_retry_armed(m)

        m.open_ok = True
        args = (0x101, SETTINGS_ACTION_PTR, 0x303, 0x404)
        self.assertEqual(m.action(args), 0xA110)
        self.assert_success(m, args)

    def test_short_read_closes_frees_and_retries(self):
        m = DeferredMachine()
        m.short_read = len(m.resident_data) - 1
        self.assertEqual(m.action(), 0x510C)
        self.assertEqual(m.live_allocs, {})
        self.assertIn(("free", HEAP_RAW), m.calls)
        self.assertEqual(sum(call[0] == "close" for call in m.calls), 1)
        self.assert_retry_armed(m)

        m.short_read = None
        args = (0x1001, SETTINGS_ACTION_PTR, 0x3003, 0x4004)
        self.assertEqual(m.action(args), 0xA110)
        self.assert_success(m, args)

    def test_every_hrl_header_field_must_match_the_built_resident(self):
        fields = ("magic", "entry", "image", "span", "relocs", "align")
        for index, name in enumerate(fields):
            with self.subTest(field=name):
                m = DeferredMachine()
                good = m.resident_data
                bad = bytearray(good)
                value = struct.unpack_from("<I", bad, index * 4)[0]
                struct.pack_into("<I", bad, index * 4, value ^ 4)
                m.resident_data = bytes(bad)
                self.assertEqual(m.action(), 0x510C)
                self.assertNotIn("resident_entry", [call[0] for call in m.calls])
                self.assertEqual(m.live_allocs, {})
                self.assert_retry_armed(m)

                m.resident_data = good
                args = (index + 1, SETTINGS_ACTION_PTR, 3, 4)
                self.assertEqual(m.action(args), 0xA110)
                self.assert_success(m, args)

    def test_same_layout_resident_corruption_is_rejected(self):
        m = DeferredMachine()
        good = m.resident_data
        bad = bytearray(good)
        bad[24 + 32] ^= 0x40
        m.resident_data = bytes(bad)
        self.assertEqual(m.action(), 0x510C)
        self.assertNotIn("resident_entry", [call[0] for call in m.calls])
        self.assertEqual(m.live_allocs, {})
        self.assert_retry_armed(m)

        m.resident_data = good
        args = (0x901, SETTINGS_ACTION_PTR, 0x903, 0x904)
        self.assertEqual(m.action(args), 0xA110)
        self.assert_success(m, args)

    def test_relocation_table_is_bounded_aligned_and_canonical(self):
        for name, replacement in (
                ("past_image", lambda m, _first: m.res_image_size),
                ("unaligned", lambda _m, first: first + 1),
                ("duplicate", lambda _m, first: first)):
            with self.subTest(case=name):
                m = DeferredMachine()
                bad = bytearray(m.resident_data)
                reloc_table = 24 + m.res_image_size
                first = struct.unpack_from("<I", bad, reloc_table)[0]
                target_index = 1 if name == "duplicate" else 0
                struct.pack_into("<I", bad, reloc_table + target_index * 4,
                                 replacement(m, first))
                m.resident_data = bytes(bad)
                self.assertEqual(m.action(), 0x510C)
                self.assertNotIn("resident_entry", [call[0] for call in m.calls])
                self.assertEqual(m.live_allocs, {})
                self.assert_retry_armed(m)

    def test_partial_or_invalid_resident_hooks_are_rolled_back(self):
        invalid = {
            "action": STOCK_ACTION,
            "raw": STOCK_RAW,
            "template": STOCK_TEMPLATE,
        }
        for hook, target in invalid.items():
            with self.subTest(hook=hook, reason="outside"):
                m = DeferredMachine()
                m.entry_hook_overrides[hook] = target
                self.assertEqual(m.action(), 0x510C)
                self.assertIn(("free", HEAP_RAW), m.calls)
                self.assertNotIn("resident_action", [call[0] for call in m.calls])
                self.assert_failed_stock(m)

        m = DeferredMachine()
        # A pointer into the right image is still invalid without Thumb state.
        m.entry_hook_overrides["action"] = (HEAP_RAW + 31) & ~31
        self.assertEqual(m.action(), 0x510C)
        self.assert_failed_stock(m)

        m = DeferredMachine()
        # BSS belongs to the arena but is not executable resident PROGBITS.
        resident_base = (HEAP_RAW + 31) & ~31
        m.entry_hook_overrides["template"] = (
            resident_base + m.res_image_size + 1
        )
        self.assertLess(m.entry_hook_overrides["template"],
                        resident_base + m.res_span)
        self.assertEqual(m.action(), 0x510C)
        self.assert_failed_stock(m)


if __name__ == "__main__":
    unittest.main(verbosity=2)
