"""Run package discovery in the compiled ARM resident, with a mock filesystem."""
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from app_install import HEADER, bundles, identity, pack
from package_app import MANIFEST, MAGIC, VERSION, ABI, FIRMWARE, write_package
from test_untether import ChainMachine, catalog_bundle, load_hrl
from wrap_resident_slot import fnv1a32


EXE = struct.pack('<6I', 0x314c5248, 0, 4, 4, 0, 4) + b'\x70\x47\0\0'
ICON = bytes(112 * 112 * 4 + 32)


def app_files(sbid='hb.extra', app_id=0x10001234, folder=None):
    folder = folder or sbid[3:].title() + '.nanoapp'
    b = catalog_bundle(sbid, sbid, app_id)
    return {f'/Apps/{folder}/manifest.napp': MANIFEST.pack(
                MAGIC, VERSION, ABI, FIRMWARE, 0, len(b), len(EXE),
                fnv1a32(EXE), fnv1a32(ICON)) + b,
            f'/Apps/{folder}/app.hbapp': EXE,
            f'/Apps/{folder}/icon.bin': ICON}


class AppPackageTests(unittest.TestCase):
    def run_catalog(self, files, legacy=None, resident_version=100):
        m = ChainMachine(real_resident=True)
        m.files.pop('/Library/AllApps.pack', None)
        if legacy is not None:
            m.files['/Library/AllApps.pack'] = pack(legacy)
            for b in legacy:
                h = HEADER.unpack_from(b)
                name = b[h[11]:].split(b'\0', 1)[0].decode()
                m.files[f'/Apps/Executables/{name}.hbapp'] = EXE
                m.files[f'/Apps/Icons/{name}.bin'] = ICON
        m.files.update(files)
        _, span, _ = load_hrl(m.uc, m.res_blob, m.res_base)
        m.heap_next = (m.res_base + span + 0xfff) & ~0xfff
        address = 0x09d00000
        m.uc.mem_write(address, b'/Library/AllApps.pack\0')
        result_address = m.call(m.res_addr('hb_app_catalog_load'), (address, resident_version, 0))
        result = []
        if result_address:
            count = m.word(result_address + 8)
            end = 12 + count * 4
            for i in range(count):
                off = m.word(result_address + 12 + 4 * i)
                end = max(end, off + m.word(result_address + off + 44))
            result = bundles(bytes(m.uc.mem_read(result_address, end)))
        # The resident and final catalog are retained; source buffers are freed.
        retained = [size for size in m.live_allocs.values()]
        self.assertLessEqual(len(retained), 1)
        return result, m

    def test_independent_app_needs_no_shared_catalog(self):
        result, _ = self.run_catalog(app_files())
        self.assertEqual([identity(b) for b in result], ['hb.extra'])

    def test_update_replaces_only_matching_legacy_identity(self):
        old = catalog_bundle('hb.extra', 'Extra', 0x0dad8101)
        other = catalog_bundle('hb.other', 'Other', 0x0dad8102)
        result, _ = self.run_catalog(app_files(), [old, other])
        self.assertEqual([identity(b) for b in result], ['hb.extra', 'hb.other'])
        self.assertEqual(HEADER.unpack_from(result[0])[3], 0x10001234)
        self.assertEqual(result[1], other)

    def test_directory_order_does_not_change_package_order(self):
        files = app_files('hb.zed', 0x10000001)
        files.update(app_files('hb.alpha', 0x10000002))
        result, _ = self.run_catalog(files)
        self.assertEqual([identity(b) for b in result], ['hb.alpha', 'hb.zed'])

    def test_folder_names_allow_spaces_unicode_and_renaming_without_identity_change(self):
        for folder in ('Countdown Days.nanoapp', 'My Renamed App.NANOAPP', 'Café.nanoapp',
                       'A' * 55 + '.nanoapp'):
            with self.subTest(folder=folder):
                result, _ = self.run_catalog(app_files(folder=folder))
                self.assertEqual([identity(b) for b in result], ['hb.extra'])
        result, _ = self.run_catalog(app_files(folder='A' * 56 + '.nanoapp'))
        self.assertEqual(result, [])

    def test_duplicate_identity_in_two_folders_keeps_first_deterministically(self):
        files = app_files(folder='Zed.nanoapp')
        files.update(app_files(folder='Alpha.nanoapp', app_id=0x10004321))
        result, _ = self.run_catalog(files)
        self.assertEqual(len(result), 1)
        self.assertEqual(HEADER.unpack_from(result[0])[3], 0x10004321)

    def test_newer_compatible_resident_accepts_older_app_minimum(self):
        files = app_files()
        key = '/Apps/Extra.nanoapp/manifest.napp'
        data = bytearray(files[key])
        struct.pack_into('<I', data, 16, 90)
        files[key] = bytes(data)
        result, _ = self.run_catalog(files, resident_version=100)
        self.assertEqual([identity(b) for b in result], ['hb.extra'])
        result, _ = self.run_catalog(files, resident_version=89)
        self.assertEqual(result, [])

    def test_invalid_or_incomplete_packages_leave_existing_apps(self):
        for failure in ('abi', 'firmware', 'version', 'minimum', 'length', 'offset', 'sections', 'resource', 'namespace',
                        'missing-exe', 'short-exe', 'missing-icon', 'bad-folder', 'truncated'):
            with self.subTest(failure=failure):
                files = app_files()
                key = '/Apps/Extra.nanoapp/manifest.napp'
                data = bytearray(files[key])
                if failure in ('version', 'abi', 'firmware', 'minimum', 'length'):
                    field = {'version': 1, 'abi': 2, 'firmware': 3, 'minimum': 4, 'length': 5}[failure]
                    struct.pack_into('<I', data, field * 4, 999)
                elif failure == 'offset':
                    struct.pack_into('<I', data, MANIFEST.size + 36, 0xfffffff0)
                elif failure == 'sections':
                    struct.pack_into('<I', data, MANIFEST.size + 48, 0xffffffff)
                elif failure == 'resource':
                    label_offset = struct.unpack_from('<I', data, MANIFEST.size + HEADER.size + 4)[0]
                    struct.pack_into('<I', data, MANIFEST.size + label_offset + 8, 0xffffffff)
                elif failure == 'truncated':
                    data = data[:20]
                elif failure == 'missing-exe':
                    files.pop('/Apps/Extra.nanoapp/app.hbapp')
                elif failure == 'short-exe':
                    files['/Apps/Extra.nanoapp/app.hbapp'] = EXE[:-1]
                elif failure == 'missing-icon':
                    files.pop('/Apps/Extra.nanoapp/icon.bin')
                files[key] = bytes(data)
                if failure == 'namespace':
                    files = {p.replace('hb.extra', 'xx.extra'):
                             value.replace(b'hb.extra', b'xx.extra') if p.endswith('.napp') else value
                             for p, value in files.items()}
                if failure == 'bad-folder':
                    files = {p.replace('Extra.nanoapp', '../Extra.nanoapp'): v for p, v in files.items()}
                old = catalog_bundle()
                result, _ = self.run_catalog(files, [old])
                self.assertEqual(result, [old])

    def test_id_collision_and_capacity_preserve_existing_apps(self):
        old = catalog_bundle(app_id=0x10001234)
        result, _ = self.run_catalog(app_files(), [old])
        self.assertEqual(result, [old])
        full = [catalog_bundle(f'hb.app{i}', f'App{i}', 0x0dad8100 + i) for i in range(64)]
        result, _ = self.run_catalog(app_files(), full)
        self.assertEqual(result, full)
        result, _ = self.run_catalog(app_files('hb.app0'), full)
        self.assertEqual(len(result), 64)
        self.assertEqual(HEADER.unpack_from(result[0])[3], 0x10001234)

    def test_zip_is_copy_only_and_never_replaces_catalog_startup_or_data(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            files = app_files()
            for name, data in files.items():
                path = root / name.removeprefix('/Apps/')
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            output = write_package(root, root / 'out.zip')
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(set(archive.namelist()), {p.removeprefix('/Apps/') for p in files})
                for name, data in files.items():
                    self.assertEqual(archive.read(name.removeprefix('/Apps/')), data)
            (root / 'Extra.nanoapp/app.hbapp').write_bytes(b'corrupt')
            previous = output.read_bytes()
            with self.assertRaises(ValueError):
                write_package(root, output)
            self.assertEqual(output.read_bytes(), previous)


if __name__ == '__main__':
    unittest.main(verbosity=2)
