#!/usr/bin/env python3
"""Exercise the compiled heap bootstrap and resident with mocked firmware APIs.

These tests cover relocation, deferred loading, hook handoff and write bounds.
The source-built publisher and packaged CFF are checked separately; these tests
do not execute the stock firmware CFF decoder.
"""
from pathlib import Path
import struct
import unittest

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_INTR,
                     UC_HOOK_MEM_INVALID, UC_HOOK_MEM_WRITE)
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3,
                               UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
                               UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
                               UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
                               UC_ARM_REG_R10, UC_ARM_REG_R11)
from arm_elf import ROOT, symbols
from app_install import HEADER, pack


def catalog_bundle(sbid='hb.probe', name='Probe', app_id=0x0dad8100):
    """A bounded surface manifest; resource publication is mocked separately."""
    strings = sbid.encode() + b'\0' + name.encode() + b'\0'
    offset = HEADER.size + 24
    section = (offset + len(strings) + 3) & ~3
    from mkapp import make_label_db
    label = make_label_db(app_id, name)
    padded = label + bytes(-len(label) % 4)
    screen = struct.pack('<3I', 3, 12, 0)
    return (HEADER.pack(0x50414248, 1, 1, app_id, app_id,
                        0x0dad8002, 0x0dad8001, 0, 0x48426170, 0x48425344,
                        offset, offset + len(sbid) + 1, section + len(padded) + len(screen), 2)
            + struct.pack('<6I', 2, section, len(label), 3, section + len(padded), len(screen))
            + strings + bytes(section - offset - len(strings)) + padded + screen)

BOOT = ROOT / "tools/fixtures/untether_bootstrap/build/untether_bootstrap"
RESIDENT = ROOT / "system/silver_resident/build/silver_resident"
SLOT = 0x087CBB6C
STOCK = 0x082AE8DD
RAW_SLOT = 0x087CBB64
RAW_STOCK = 0x081C4505
TEMPLATE_SLOT = 0x087AB73C + 0x168
STOP = 0x09F00000
BOOT_BASE = 0x09800000
HEAP_BASE = 0x09A00000
REPORT_BASE = 0x086C9B84
RARGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)
CALLEE = (UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
          UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)


def hrl_info(data):
    magic, entry, image, span, count, align = struct.unpack_from("<6I", data)
    assert magic == 0x314C5248 and align == 16
    return entry, image, span, count


def load_hrl(uc, data, base):
    """Relocate a checked HRL1 build artifact at a chosen emulator address."""
    entry, image_size, span, count = hrl_info(data)
    image = bytearray(span)
    image[:image_size] = data[24:24 + image_size]
    for off in struct.unpack_from(f"<{count}I", data, 24 + image_size):
        value = struct.unpack_from("<I", image, off)[0]
        struct.pack_into("<I", image, off, (value + base) & 0xFFFFFFFF)
    uc.mem_write(base, bytes(image))
    return base + entry, span, bytes(image)


def mock_resident():
    # bx lr; nop, plus enough span for two mock hooks validated by the bootstrap.
    return struct.pack("<6I", 0x314C5248, 0, 4, 0x200, 0, 16) + b"\x70\x47\x00\xbf"


class ChainMachine:
    def __init__(self, real_resident=False, heap_base=HEAP_BASE, prev_rings=()):
        self.boot_syms = symbols(BOOT.with_suffix(".reloc.elf"))
        self.boot_blob = BOOT.with_suffix(".hbapp").read_bytes()
        self.res_syms = symbols(RESIDENT.with_suffix(".reloc.elf"))
        self.res_blob = RESIDENT.with_suffix(".hbapp").read_bytes() if real_resident else mock_resident()
        self.real_resident = real_resident
        self.files = {"/Library/resident.hbapp": self.res_blob,
                      "/Library/AllApps.pack": b"HBPK",
                      "/iPod_Control": b'',
                      "/Apps/Executables/Probe.hbapp": mock_resident(),
                      "/Apps/Icons/Probe.bin": bytes(112 * 112 * 4 + 32)}
        self.read_limit = None
        self.replace_slot_on_read = None
        self.alloc_fail = False
        self.install_hooks = True
        self.install_raw_hook = True
        self.resident_hook = heap_base + 0x101
        self.resident_raw_hook = heap_base + 0x181
        self.heap_base = self.heap_next = heap_base
        self.live_allocs = {}
        self.calls = []
        self.flushes = []
        self.writes = []
        self.written_files = {}
        self.invalid_access = None
        self.rawmsg_result = 0
        self.heap_free_value = 0x00180000
        self.heap_largest_value = 0x00100000
        self.register_records = []
        self.registered_catalog = None
        self.auto_register = False
        self.hidden_apps = []
        self.lr_at_action = None

        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.mem_map(0x08000000, 0x02000000)
        self.uc.mem_write(0x08d00000, b"TestAction\0")
        self.boot_payload_entry, self.boot_span, self.boot_expected = load_hrl(
            self.uc, self.boot_blob, BOOT_BASE)
        # payload_entry is the CFF stack-stage handoff: it restores the frame
        # saved by cff_heap_stage and branches to the captured firmware PC.
        # These unit tests exercise the C bootstrap body directly; the adjacent
        # real-decoder integration test owns the stage/entry ABI.
        self.boot_entry = BOOT_BASE + (self.boot_syms["untether_bootstrap_main"] & ~1)
        self.word(SLOT, STOCK)
        self.word(RAW_SLOT, RAW_STOCK)
        for addr, ring in prev_rings:
            self.uc.mem_write(addr, ring)

        self.handlers = {}
        self.extra_handlers = {}
        self.uc.hook_add(UC_HOOK_INTR, self.interrupt)
        self.uc.hook_add(UC_HOOK_CODE, self.on_call)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self.on_invalid)
        boot_fns = {"hb_fs_exists": "exists", "hb_fs_size_at": "size",
                    "hb_fs_read_at": "read", "hb_os_alloc": "alloc",
                    "hb_os_free": "free", "hb_icache_invalidate": "flush",
                    "hb_os_heap_largest": "zero", "hb_os_heap_free": "zero",
                    "hb_silver_rawmsg": "rawmsg_helper"}
        for sym, what in boot_fns.items():
            self.intercept(BOOT_BASE + (self.boot_syms[sym] & ~1), what)
        for address in (0x0841bdd0, 0x0841cd7a, 0x0842484c):
            self.intercept(address, "zero")
        self.intercept(STOCK & ~1, "stock")
        self.intercept(RAW_STOCK & ~1, "raw_stock")

        self.res_base = heap_base
        if real_resident:
            res_fns = {"hb_fs_size": "size", "hb_fs_read": "read",
                       "hb_fs_is_dir": "exists",
                       "hb_fs_size_at": "size", "hb_fs_read_at": "read",
                       "hb_fs_dir_open": "dir_open", "hb_fs_dir_next": "dir_next",
                       "hb_fs_dir_close": "zero",
                       "hb_fs_write": "write", "hb_os_alloc": "alloc",
                       "hb_os_free": "free", "hb_icache_invalidate": "flush",
                       "hb_silver_icon_live": "zero", "hb_silver_icon_rearm": "zero",
                       "hb_silver_icon_reset_resolver": "icon_reset",
                       "hb_app_register_pack": "register",
                       "hb_silver_home_hide_item": "hide_app",
                       "hb_silver_home_reload": "zero",
                       "hb_silver_rawmsg": "rawmsg_helper"}
            for sym, what in res_fns.items():
                self.intercept(self.res_addr(sym), what)
            # The heap wrappers are tail calls. Intercept the firmware entries
            # too so Unicorn cannot continue through a translated wrapper block
            # after the entry hook changes PC.
            self.intercept(0x08417824, "heap_free")
            self.intercept(0x0800CBB8, "heap_largest")
            self.intercept(0x0842B440, "alloc")
            self.intercept(0x0842D86C, "free")
            self.intercept(self.res_addr("action_hook"), "action_hook")
        else:
            self.intercept(heap_base, "mock_entry")
            self.intercept(self.resident_hook & ~1, "next")
            self.intercept(self.resident_raw_hook & ~1, "raw_next")

    def intercept(self, addr, what):
        self.handlers[addr] = what

    def on_write(self, _uc, _access, address, size, _value, _data):
        self.writes.append((address, size))

    def on_invalid(self, _uc, access, address, size, value, _data):
        self.invalid_access = (access, address, size, value)
        return False

    def word(self, addr, value=None):
        if value is not None:
            self.uc.mem_write(addr, struct.pack("<I", value))
        return struct.unpack("<I", self.uc.mem_read(addr, 4))[0]

    def string(self, addr):
        return bytes(self.uc.mem_read(addr, 256)).split(b"\0", 1)[0].decode()

    def ret(self, value=0):
        self.uc.reg_write(UC_ARM_REG_R0, value)
        self.uc.reg_write(UC_ARM_REG_PC, self.uc.reg_read(UC_ARM_REG_LR))

    def interrupt(self, uc, number, _):
        if number != 2:
            raise AssertionError(f"unexpected interrupt {number}")
        self.flushes.append((uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)))

    def on_call(self, uc, address, _size, _):
        if address in self.extra_handlers:
            self.extra_handlers[address](uc)
            return
        what = self.handlers.get(address)
        if not what:
            return
        a = tuple(uc.reg_read(r) for r in RARGS)
        if what == "action_hook":
            self.lr_at_action = uc.reg_read(UC_ARM_REG_LR)
            self.calls.append(("action_hook", a))
            return
        if what == "dir_open":
            prefix = self.string(a[1]) + '/'
            entries = {}
            for path in self.files:
                if path.startswith(prefix):
                    child, sep, _ = path[len(prefix):].partition('/')
                    if child:
                        entries[child] = entries.get(child, False) or bool(sep)
            self.dir_names = iter(entries.items())
            self.ret(1)
        elif what == "dir_next":
            entry = next(self.dir_names, None)
            if entry is not None:
                name, is_dir = entry
                uc.mem_write(a[1], name.encode()[:a[2] - 1] + b'\0')
                uc.mem_write(uc.reg_read(UC_ARM_REG_R3), bytes([int(is_dir)]))
            self.ret(int(entry is not None))
        elif what in ("exists", "size", "read", "write"):
            path = self.string(a[0])
            self.calls.append((what, path))
            if what == "read" and path.startswith("/Apps/Executables/"):
                self.calls.append(("exec_read_args", a[1], a[2]))
            data = self.files.get(path, b"")
            if what == "exists":
                self.ret(int(path in self.files))
            elif what == "size":
                self.ret(len(data))
            elif what == "read":
                data = data[:a[2]]
                if self.read_limit is not None:
                    data = data[:self.read_limit]
                if data:
                    uc.mem_write(a[1], data)
                if self.replace_slot_on_read is not None:
                    self.word(SLOT, self.replace_slot_on_read)
                self.ret(len(data))
            else:
                self.written_files[path] = bytes(uc.mem_read(a[1], a[2]))
                self.ret(1)
        elif what == "alloc":
            self.calls.append(("alloc", a[0]))
            if self.alloc_fail:
                self.ret(0)
            else:
                block = self.heap_next
                self.heap_next += (a[0] + 0xFFF) & ~0xFFF
                self.live_allocs[block] = a[0]
                self.ret(block)
        elif what == "free":
            if a[0]:
                self.calls.append(("free", a[0]))
                assert self.live_allocs.pop(a[0], None) is not None, "bad free"
            self.ret()
        elif what == "flush":
            self.flushes.append(a[:2])
            self.ret()
        elif what == "mock_entry":
            self.calls.append(("entry",))
            if self.install_hooks:
                self.word(SLOT, self.resident_hook)
                self.word(RAW_SLOT, self.resident_raw_hook if self.install_raw_hook else RAW_STOCK)
            self.ret()
        elif what == "rawmsg_helper":
            self.calls.append(("rawmsg_helper", a[:2]))
            self.ret(self.rawmsg_result)
        elif what == "next":
            self.calls.append(("next", a))
            self.ret(0x5678)
        elif what == "raw_next":
            self.calls.append(("raw_next", a[:2]))
            self.ret(0x8765)
        elif what == "stock":
            self.calls.append(("stock", a))
            self.ret(0x1234)
        elif what == "raw_stock":
            self.calls.append(("raw_stock", a[:2]))
            self.ret(0x4321)
        elif what == "zero":
            self.ret(0)
        elif what == "heap_free":
            self.ret(self.heap_free_value)
        elif what == "heap_largest":
            self.ret(self.heap_largest_value)
        elif what == "icon_reset":
            self.calls.append(("icon_reset",))
            self.ret()
        elif what == "register":
            count = self.word(a[0] + 8)
            end = 12 + 4 * count
            for index in range(count):
                off = self.word(a[0] + 12 + 4 * index)
                end = max(end, off + self.word(a[0] + off + 44))
            self.registered_catalog = bytes(uc.mem_read(a[0], end))
            out_apps = uc.reg_read(UC_ARM_REG_R3)
            self.calls.append(("register", out_apps))
            records = self.register_records
            if self.auto_register:
                from app_install import bundles
                records = []
                for b in bundles(self.registered_catalog):
                    h = HEADER.unpack_from(b)
                    sbid = b[h[10]:].split(b'\0', 1)[0]
                    name = b[h[11]:].split(b'\0', 1)[0]
                    records.append(struct.pack('<IHH4I', h[3], h[2], 0, h[5], h[6], h[7], 0)
                                   + sbid.ljust(24, b'\0') + name.ljust(24, b'\0') + bytes(8))
            for index, record in enumerate(records):
                uc.mem_write(out_apps + 80 * index, record)
            self.ret(len(records))
        elif what == "hide_app":
            self.hidden_apps.append(self.string(a[0]))
            self.ret()
        elif what == "app_entry":
            self.calls.append(("app_entry", address))
            self.ret(0x08800001)
        elif what == "push":
            self.calls.append(("push", a[:2]))
            self.ret()

    def call(self, address, args=(0x1111, 0x08d00000, 0x3333), count=20000000):
        self.uc.reg_write(UC_ARM_REG_SP, 0x09E00000)
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        for reg, value in zip((*RARGS, UC_ARM_REG_R3), args):
            self.uc.reg_write(reg, value)
        for index, value in enumerate(args[4:]):
            self.word(0x09E00000 + 4 * index, value)
        for i, reg in enumerate(CALLEE):
            self.uc.reg_write(reg, 0xCA110000 + i)
        try:
            self.uc.emu_start(address | 1, STOP, count=count)
        except UcError as error:
            regs = tuple(self.uc.reg_read(r) for r in RARGS)
            raise AssertionError(
                f"emulation failed at {self.uc.reg_read(UC_ARM_REG_PC):#x}, "
                f"lr={self.uc.reg_read(UC_ARM_REG_LR):#x}, "
                f"args={tuple(hex(v) for v in regs)}, "
                f"invalid={self.invalid_access}, allocs={self.live_allocs!r}, "
                f"calls={self.calls[-50:]!r}: {error}") from error
        assert self.uc.reg_read(UC_ARM_REG_PC) == STOP, "did not return"
        assert self.uc.reg_read(UC_ARM_REG_SP) == 0x09E00000, "stack unbalanced"
        for i, reg in enumerate(CALLEE):
            actual = self.uc.reg_read(reg)
            expected = 0xCA110000 + i
            assert actual == expected, \
                f"callee r{4 + i} clobbered: {actual:#x} != {expected:#x}"
        return self.uc.reg_read(UC_ARM_REG_R0)

    def arm(self):
        self.call(self.boot_entry)

    def dispatch(self):
        return self.call(self.word(SLOT))

    def rawmsg(self):
        return self.call(self.word(RAW_SLOT), (0x1111, 0x08d00000))

    def res_addr(self, sym):
        return self.res_base + (self.res_syms[sym] & ~1)


class LoaderTests(unittest.TestCase):
    def setUp(self):
        self.m = ChainMachine()

    def test_entry_only_arms_hooks(self):
        self.m.arm()
        self.assertEqual(self.m.word(SLOT), BOOT_BASE + self.m.boot_syms["untether_dispatch"] | 1)
        self.assertEqual(self.m.word(RAW_SLOT), BOOT_BASE + self.m.boot_syms["untether_rawmsg"] | 1)
        self.assertEqual(self.m.calls, [])

    def test_preload_gate_claims_homebrew_tap(self):
        self.m.arm()
        self.m.rawmsg_result = 2
        self.assertEqual(self.m.rawmsg(), 1)
        self.m.rawmsg_result = 0x4321
        self.assertEqual(self.m.rawmsg(), 0x4321)

    def test_repeated_entry_heals_partial_install(self):
        self.m.arm()
        self.m.word(RAW_SLOT, RAW_STOCK)
        self.m.arm()
        self.assertEqual(self.m.word(RAW_SLOT), BOOT_BASE + self.m.boot_syms["untether_rawmsg"] | 1)

    def test_existing_hook_is_preserved(self):
        self.m.word(SLOT, 0x08800001)
        self.m.arm()
        self.assertEqual((self.m.word(SLOT), self.m.word(RAW_SLOT)),
                         (0x08800001, RAW_STOCK))

    def test_valid_resident_loads_and_forwards_same_event(self):
        self.m.arm()
        self.assertEqual(self.m.dispatch(), 0x5678)
        self.assertIn(("entry",), self.m.calls)
        self.assertEqual(self.m.calls[-1], ("next", (0x1111, 0x08d00000, 0x3333)))
        self.assertEqual(self.m.word(RAW_SLOT), self.m.resident_raw_hook)
        self.assertEqual(list(self.m.live_allocs), [self.m.heap_base])

    def test_loader_never_writes_fixed_heap_window(self):
        self.m.arm()
        self.m.writes.clear()
        self.m.dispatch()
        forbidden = [(a, n) for a, n in self.m.writes
                     if 0x09100000 <= a and a + n <= 0x09500000]
        self.assertEqual(forbidden, [])

    def test_allocation_failure_retries(self):
        self.m.alloc_fail = True
        self.m.arm()
        self.assertEqual(self.m.dispatch(), 0x1234)
        self.assertEqual(self.m.live_allocs, {})
        self.m.alloc_fail = False
        self.assertEqual(self.m.dispatch(), 0x5678)

    def test_absent_pack_retries(self):
        self.m.files.pop("/Library/AllApps.pack")
        self.m.arm()
        self.assertEqual(self.m.dispatch(), 0x1234)
        self.m.files["/Library/AllApps.pack"] = b"HBPK"
        self.assertEqual(self.m.dispatch(), 0x5678)

    def test_missing_short_and_corrupt_resident(self):
        cases = [None, self.m.res_blob[:-1], b"bad!" + self.m.res_blob[4:]]
        for data in cases:
            with self.subTest(size=None if data is None else len(data)):
                self.m = ChainMachine()
                if data is None:
                    self.m.files.pop("/Library/resident.hbapp")
                else:
                    self.m.files["/Library/resident.hbapp"] = data
                self.m.arm()
                self.assertEqual(self.m.dispatch(), 0x1234)
                self.assertEqual(self.m.live_allocs, {})

    def test_disabled_restores_stock_and_keeps_raw_gate(self):
        self.m.files["/Library/disabled"] = b""
        self.m.arm()
        self.assertEqual(self.m.dispatch(), 0x1234)
        self.assertEqual(self.m.word(SLOT), STOCK)
        self.assertEqual(self.m.word(RAW_SLOT), BOOT_BASE + self.m.boot_syms["untether_rawmsg"] | 1)

    def test_another_installer_wins_during_read(self):
        self.m.replace_slot_on_read = 0x08800001
        self.m.arm()
        self.assertEqual(self.m.dispatch(), 0x1234)
        self.assertEqual(self.m.word(SLOT), 0x08800001)
        self.assertEqual(self.m.live_allocs, {})

    def test_invalid_or_partial_resident_hooks_fall_back(self):
        for install, raw in ((False, True), (True, False)):
            self.m = ChainMachine()
            self.m.install_hooks = install or raw
            self.m.install_raw_hook = raw
            if not install:
                self.m.resident_hook = STOCK
            self.m.arm()
            self.assertEqual(self.m.dispatch(), 0x1234)
            self.assertEqual(self.m.word(SLOT), STOCK)
            self.assertEqual(self.m.word(RAW_SLOT),
                             BOOT_BASE + self.m.boot_syms["untether_rawmsg"] | 1)
            self.assertEqual(self.m.live_allocs, {})

    def test_reentrant_dispatch_uses_stock(self):
        self.m.arm()
        self.m.word(BOOT_BASE + self.m.boot_syms["busy"], 1)
        self.assertEqual(self.m.dispatch(), 0x1234)
        self.assertEqual(self.m.calls, [("stock", (0x1111, 0x08d00000, 0x3333))])


def make_ring(seq, count, tag=b"CRASHED "):
    ring = bytearray(16 + 16 * 256)
    struct.pack_into("<4I", ring, 0, 0x48425452, count % 256, count, seq)
    for i in range(256):
        struct.pack_into("<8sII", ring, 16 + 16 * i, tag, seq, i)
    return bytes(ring)


class ResidentTests(unittest.TestCase):
    def test_bootstrap_loads_real_resident_and_direct_hooks(self):
        m = ChainMachine(real_resident=True)
        m.arm()
        self.assertEqual(m.dispatch(), 0x1234)
        self.assertEqual(m.word(SLOT), m.res_addr("action_hook") | 1)
        self.assertEqual(m.word(RAW_SLOT), m.res_addr("rawmsg_hook") | 1)
        self.assertEqual(m.word(TEMPLATE_SLOT), m.res_addr("templ_hook") | 1)
        self.assertTrue(BOOT_BASE <= (m.lr_at_action & ~1) < BOOT_BASE + m.boot_span)
        self.assertEqual(m.live_allocs[m.heap_base], hrl_info(m.res_blob)[2] + 16)
        self.assertEqual(sorted(m.live_allocs.values())[0], 12)  # empty live catalog
        self.assertEqual(len(m.live_allocs), 2)

    def test_real_chain_never_writes_fixed_heap_window(self):
        m = ChainMachine(real_resident=True)
        m.arm()
        m.writes.clear()
        m.dispatch()
        forbidden = sorted({(hex(a), n) for a, n in m.writes
                            if 0x09100000 <= a and a + n <= 0x09500000})
        self.assertEqual(forbidden, [])

    def test_demo_patches_apply_at_near_and_far_heap_addresses(self):
        for base in (0x09A00000, 0x08E00000):
            m = ChainMachine(real_resident=True, heap_base=base)
            m.arm(); m.dispatch()
            for site in (0x084A9ADC, 0x08215684, 0x08215600):
                self.assertEqual(m.word(site), 0xf000f8df)
                self.assertTrue(base <= (m.word(site + 4) & ~1) < base + hrl_info(m.res_blob)[2])

    def test_previous_ring_is_found_saved_and_retired(self):
        newest = make_ring(seq=7, count=300)
        older = make_ring(seq=3, count=20, tag=b"OLDBOOT ")
        bogus = bytearray(make_ring(seq=99, count=300))
        struct.pack_into("<I", bogus, 4, 7)
        m = ChainMachine(real_resident=True,
                         prev_rings=((0x09120000, newest), (0x08F00010, older),
                                     (0x09300000, bytes(bogus))))
        m.arm(); m.dispatch()
        self.assertEqual(m.written_files["/Library/lasttrace.bin"], newest)
        self.assertEqual(m.word(m.res_addr("s_prev_ring")), 0)

    def test_pack_is_registered_with_heap_state(self):
        m = ChainMachine(real_resident=True)
        m.files["/Library/AllApps.pack"] = pack([catalog_bundle()])
        m.arm(); m.dispatch()
        self.assertTrue(any(call[0] == "register" for call in m.calls))

    def test_surface_executables_load_lazily_and_release_other_apps(self):
        def record(app_id, kind, name):
            value = bytearray(80)
            struct.pack_into("<IHH", value, 0, app_id, kind, 0)
            encoded = name.encode()
            value[48:48 + len(encoded)] = encoded
            return bytes(value)

        m = ChainMachine(real_resident=True)
        app_specs = (
            (0x1001, 2, "Blocks"),
            (0x1002, 1, "Calculator"),
            (0x1003, 1, "Probe"),
        )
        m.files["/Library/AllApps.pack"] = pack([
            catalog_bundle(f'hb.test{i}', name, 0x0dad8100 + i)
            for i, (_, _, name) in enumerate(app_specs)])
        for app_id, kind, name in app_specs:
            m.register_records.append(record(app_id, kind, name))
            m.files[f"/Apps/Executables/{name}.hbapp"] = mock_resident()
            m.files[f"/Apps/Icons/{name}.bin"] = bytes(112 * 112 * 4 + 32)

        m.intercept(m.res_addr("hb_silver_push_screen_id"), "push")
        m.word(REPORT_BASE + 0x2C, 0x32505452)
        m.arm(); m.dispatch()
        executable_reads = [call for call in m.calls
                            if call[0] == "read" and call[1].startswith("/Apps/Executables/")]
        self.assertEqual(executable_reads, [])
        self.assertEqual(m.word(REPORT_BASE + 0x5C), 0)
        self.assertEqual(m.word(REPORT_BASE + 0xA0), m.heap_free_value)
        self.assertEqual(m.word(REPORT_BASE + 0xA4), m.heap_largest_value)

        launch_base = m.res_addr("g_launch")
        entries_base = m.res_addr("s_app_entry")
        frees_before = len([call for call in m.calls if call[0] == "free"])
        m.call(m.res_addr("hb_app_launch"), (0x1111, launch_base))
        first_reads = [call for call in m.calls
                       if call[0] == "read" and call[1].startswith("/Apps/Executables/")]
        self.assertEqual(len(first_reads), 2)  # header + full image
        self.assertNotEqual(m.word(entries_base), 0)
        self.assertEqual(m.word(REPORT_BASE + 0xA8), 1)
        self.assertEqual(tuple(m.word(REPORT_BASE + 0xAC + 4 * i)
                               for i in range(5)),
                         (1, 0x1001, 4, m.heap_free_value,
                          m.heap_largest_value))

        calls_before = len(m.calls)
        m.call(m.res_addr("hb_app_launch"), (0x1111, launch_base))
        self.assertFalse(any(call[0] == "read" for call in m.calls[calls_before:]))
        self.assertEqual(tuple(m.word(REPORT_BASE + 0xC0 + 4 * i)
                               for i in range(5)),
                         (2, 0x1001, 5, m.heap_free_value,
                          m.heap_largest_value))

        m.call(m.res_addr("hb_app_launch"), (0x1111, launch_base + 80))
        all_reads = [call for call in m.calls
                     if call[0] == "read" and call[1].startswith("/Apps/Executables/")]
        self.assertEqual(len(all_reads), 4)
        self.assertNotEqual(m.word(entries_base + 4), 0)
        self.assertEqual(m.word(entries_base + 8), 0)
        self.assertEqual(len([call for call in m.calls if call[0] == "free"]), frees_before + 1)
        self.assertEqual(m.word(entries_base), 0)
        self.assertEqual(m.word(REPORT_BASE + 0xA8), 2)

    def test_lazy_load_failure_is_reported_without_opening_app(self):
        def record(app_id, kind, name):
            value = bytearray(80)
            struct.pack_into("<IHH", value, 0, app_id, kind, 0)
            value[48:48 + len(name)] = name.encode()
            return bytes(value)

        m = ChainMachine(real_resident=True)
        m.files["/Library/AllApps.pack"] = pack([catalog_bundle()])
        m.register_records.append(record(0x2001, 2, "Missing"))
        m.intercept(m.res_addr("hb_silver_push_screen_id"), "push")
        m.word(REPORT_BASE + 0x2C, 0x32505452)
        m.arm(); m.dispatch()
        m.call(m.res_addr("hb_app_launch"),
               (0x1111, m.res_addr("g_launch")))
        self.assertFalse(any(call[0] == "push" for call in m.calls))
        self.assertEqual(m.word(REPORT_BASE + 0x60), 1)
        self.assertEqual(tuple(m.word(REPORT_BASE + 0xAC + 4 * i)
                               for i in range(5)),
                         (1, 0x2001, 1, m.heap_free_value,
                          m.heap_largest_value))


if __name__ == "__main__":
    unittest.main(verbosity=2)
