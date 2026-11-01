"""Check copy-to-disk layout, complete catalogs, and firmware/runtime pairing."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from app_install import pack
from test_app_install import bundle
import build_release as release
from wrap_resident_slot import wrap_slot
from package_app import manifest_for


class ReleaseTests(unittest.TestCase):
    def fixture(self, root):
        apps = root / 'disk/Apps'
        for folder in ('Demo.nanoapp',):
            (apps / folder).mkdir(parents=True)
        hrl = struct.pack('<6I', 0x314c5248, 0, 4, 4, 0, 4) + b'\x70\x47\0\0'
        entry = bundle('hb.demo', 'hb.demo', 0x10000001)
        icon = bytes(112 * 112 * 4 + 32)
        (apps / 'Demo.nanoapp/app.hbapp').write_bytes(hrl)
        (apps / 'Demo.nanoapp/icon.bin').write_bytes(icon)
        (apps / 'Demo.nanoapp/manifest.napp').write_bytes(manifest_for(entry, hrl, icon))
        # Minimal source catalog used by package_release's completeness check.
        for name in ('demo', 'benchmark', 'stress', 'widgets'):
            source = root / 'apps' / name
            source.mkdir(parents=True)
            (source / 'Info.plist').touch()
            (source / 'Makefile').touch()
        (root / 'data/Contacts').mkdir(parents=True)
        (root / 'data/Contacts/items.txt').write_text('Sample contact\n')
        firmware = root / 'firmware'
        (firmware / 'resources/NanoApps').mkdir(parents=True)
        (firmware / 'resources/NanoApps/loader.hbapp').write_bytes(hrl)
        (firmware / 'Library').mkdir()
        (firmware / 'Library/resident.hbapp').write_bytes(wrap_slot(hrl, 100))
        records = {}
        for revision in ('2012', '2015'):
            path = firmware / f'NanoApps-{revision}-Model.ipsw'
            path.write_bytes(b'fixture-' + revision.encode())
            records[revision] = {'firmware': '1.1.2', 'build': '39A10023',
                'revision': revision, 'scsi_group': '98', 'ipsw': path.name,
                'ipsw_sha256': release.sha256(path), 'runtime_files': {
                    name: release.sha256(firmware / 'resources/NanoApps' / name) for name in release.RUNTIME_FILES}}
        return apps, firmware, records

    def test_zip_has_apps_at_root_and_manifest_matches_all_assets(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            apps, firmware, records = self.fixture(root)
            with patch.object(release, 'ROOT', root):
                release.package_release(apps, firmware, records, root / 'out', 'a' * 40)
            with zipfile.ZipFile(root / 'out/NanoApps-disk.zip') as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual(set(archive.namelist()), {
                    'Apps/' + p.relative_to(apps).as_posix()
                    for p in apps.rglob('*') if p.is_file() and p.relative_to(apps).parts[0] != 'Startup'
                    and p.name != 'AllApps.pack'} | {'Library/resident.hbapp', 'Data/Contacts/items.txt'})
                self.assertEqual(archive.read('Library/resident.hbapp'), (firmware / 'Library/resident.hbapp').read_bytes())
                self.assertEqual(archive.read('Data/Contacts/items.txt'), b'Sample contact\n')
                self.assertFalse(any('/Startup/' in name for name in archive.namelist()))
                self.assertNotIn('Apps/AllApps.pack', archive.namelist())
            manifest = json.loads((root / 'out/build.json').read_text())
            self.assertEqual(manifest['source_commit'], 'a' * 40)
            self.assertEqual(manifest['app_count'], 1)
            self.assertEqual(len(manifest['files']), 3)
            for name, digest in manifest['files'].items():
                self.assertEqual(release.sha256(root / 'out' / name), digest)
            for line in (root / 'out/SHA256SUMS').read_text().splitlines():
                digest, name = line.split('  ')
                self.assertEqual(release.sha256(root / 'out' / name), digest)

    def test_invalid_app_tree_and_disk_resident_are_rejected(self):
        for failure in ('missing-app', 'unexpected-file', 'missing-executable', 'bad-resident'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                apps, firmware, records = self.fixture(root)
                if failure == 'missing-app':
                    (apps / 'Demo.nanoapp/manifest.napp').unlink()
                elif failure == 'unexpected-file':
                    (apps / 'AllApps.pack').write_bytes(pack([]))
                elif failure == 'missing-executable':
                    (apps / 'Demo.nanoapp/app.hbapp').unlink()
                else:
                    (firmware / 'Library/resident.hbapp').write_bytes(bytes(32))
                with patch.object(release, 'ROOT', root), self.assertRaises((ValueError, FileNotFoundError)):
                    release.package_release(apps, firmware, records, root / 'out', 'a' * 40)
                self.assertFalse((root / 'out').exists())

    def test_mismatched_firmware_or_disk_never_produces_release_assets(self):
        for failure in ('revision', 'handler', 'runtime', 'name', 'ipsw'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                apps, firmware, records = self.fixture(root)
                if failure == 'revision':
                    records['2012']['revision'] = '2015'
                elif failure == 'handler':
                    records['2012']['scsi_group'] = '96'
                elif failure == 'runtime':
                    records['2012']['runtime_files']['loader.hbapp'] = 'bad'
                elif failure == 'name':
                    records['2012']['ipsw'] = 'wrong-name.ipsw'
                else:
                    (firmware / records['2012']['ipsw']).write_bytes(b'bad')
                with patch.object(release, 'ROOT', root), self.assertRaises(ValueError):
                    release.package_release(apps, firmware, records, root / 'out', 'a' * 40)
                self.assertFalse((root / 'out').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
