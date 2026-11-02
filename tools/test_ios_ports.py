"""Optional, asset-local checks of the three real ARM app entries on relaunch.

Skip unprepared ports. Run after build_ios_game.py; emulate the actual linked
Thumb entry and filesystem calls, including a renamed .nanoapp folder. No device.
"""
from pathlib import Path
import struct
import unittest

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
    UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP,
)
from test_gl_relaunch import relocated, symbols
from test_app_install import bundle
from package_app import manifest_for

ROOT = Path(__file__).resolve().parents[1]
BASE, SHARED, BUFFER, STACK, STOP = 0x09500000, 0x09000000, 0x09A00000, 0x09E00000, 0x09F00000


class IOSPortRelaunchTests(unittest.TestCase):
    def exercise(self, game):
        app = ROOT / 'ports' / game / 'build' / game
        if not app.with_suffix('.hbapp').exists():
            self.skipTest('prepare this optional port with tools/build_ios_game.py')
        raw = app.with_suffix('.hbapp').read_bytes()
        entry, image = relocated(raw, BASE)
        syms = symbols(app.with_suffix('.reloc.elf'))
        identity = 'hb.' + game
        manifest = manifest_for(bundle(identity, identity, 0x10000001), raw,
                                bytes(112 * 112 * 4 + 32))
        fs = {'/Apps/Renamed Port.nanoapp/manifest.napp': manifest,
              '/Apps/Renamed Port.nanoapp/app.hbapp': raw}
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        uc.mem_map(0x08000000, 0x02000000)
        uc.mem_write(BASE, bytes(image))
        uc.mem_write(SHARED, struct.pack('<2I', 0x48534248, 2))
        current = [0]
        reads = []

        def string(address):
            return bytes(uc.mem_read(address, 256)).split(b'\0', 1)[0].decode()

        def hook(name):
            def callback(machine, address, size, user):
                r0, r1, r2, r3 = [uc.reg_read(reg) for reg in
                    (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
                result = 0
                if name == 'hb_fs_dir_open':
                    self.assertEqual(string(r1), '/Apps')
                    current[0] = 0
                    result = 1
                elif name == 'hb_fs_dir_next':
                    if not current[0]:
                        uc.mem_write(r1, b'Renamed Port.nanoapp\0')
                        uc.mem_write(r3, b'\1')
                        current[0] = result = 1
                elif name == 'hb_fs_size':
                    result = len(fs.get(string(r0), b''))
                elif name == 'hb_fs_read':
                    path = string(r0)
                    data = fs.get(path, b'')
                    self.assertLessEqual(len(data), r2)
                    uc.mem_write(r1, data)
                    reads.append(path)
                    result = len(data)
                elif name == 'hb_os_alloc':
                    self.assertLess(r0, 1024 * 1024)
                    result = BUFFER
                uc.reg_write(UC_ARM_REG_R0, result)
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
            address = BASE + (syms[name] & ~1)
            uc.hook_add(UC_HOOK_CODE, callback, begin=address, end=address)

        for name in ('hb_fs_dir_open', 'hb_fs_dir_next', 'hb_fs_dir_close',
                     'hb_fs_size', 'hb_fs_read', 'hb_os_alloc', 'hb_os_free'):
            hook(name)

        def launch():
            uc.reg_write(UC_ARM_REG_SP, STACK)
            uc.mem_write(STACK, struct.pack('<I', SHARED))
            for reg, value in ((UC_ARM_REG_LR, STOP | 1), (UC_ARM_REG_R0, 0),
                               (UC_ARM_REG_R1, 0), (UC_ARM_REG_R2, 240), (UC_ARM_REG_R3, 432)):
                uc.reg_write(reg, value)
            uc.emu_start(BASE + entry | 1, STOP, count=10_000_000)
            self.assertEqual(uc.reg_read(UC_ARM_REG_PC), STOP)
            return uc.reg_read(UC_ARM_REG_R0)

        first = launch()
        self.assertNotEqual(first, 0)
        # App-owned initialized renderer state must be restored on a cached
        # launch, alongside fresh BSS. File and folder names are independent.
        dirty = BASE + syms.get('gap_estimate', syms['__data_start'])
        before = bytes(uc.mem_read(dirty, 4))
        uc.mem_write(dirty, b'\xAB' * 4)
        second = launch()
        self.assertEqual(first, second)
        self.assertEqual(bytes(uc.mem_read(dirty, 4)), before)
        self.assertEqual(reads, list(fs))
        self.assertEqual(struct.unpack('<I', uc.mem_read(BASE + syms['hb_shared'], 4))[0], SHARED)

    def test_flappybird(self):
        self.exercise('flappybird')

    def test_doodlejump(self):
        self.exercise('doodlejump')

    def test_templerun(self):
        self.exercise('templerun')


if __name__ == '__main__':
    unittest.main(verbosity=2)
