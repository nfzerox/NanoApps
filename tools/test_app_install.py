import tempfile
import shutil
from pathlib import Path
import unittest
from unittest.mock import patch

import app_install as app


def bundle(identifier, name, app_id):
    strings = identifier.encode() + b'\0' + name.encode() + b'\0'
    return app.HEADER.pack(0x50414248, 1, 1, app_id, app_id, 1, 1, 0, 0, 0,
                           52, 53 + len(identifier), 52 + len(strings), 0) + strings


class InstallTests(unittest.TestCase):
    def folder_fixture(self, root, name='Extra.nanoapp'):
        from test_app_packages import app_files
        from test_untether import catalog_bundle
        stage, target = self.fixture(root)
        shutil.rmtree(stage / 'Executables')
        shutil.rmtree(stage / 'Icons')
        (stage / 'AllApps.pack').write_bytes(app.pack([
            catalog_bundle('hb.extra', 'hb.extra', 0x10001234)]))
        for path, data in app_files(folder=name).items():
            out = stage / path.removeprefix('/Apps/')
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(data)
        return stage, target

    def test_folder_install_and_renamed_update_preserve_other_apps_and_data(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            stage, target = self.folder_fixture(root)
            count, _ = app.install(stage, target.parent, root / 'backups')
            self.assertEqual(count, 2)
            self.assertTrue((target / 'Extra.nanoapp/app.hbapp').is_file())
            self.assertEqual(len(app.bundles((target.parent / 'Library/AllApps.pack').read_bytes())), 1)
            (stage / 'Extra.nanoapp').rename(stage / 'My Extra App.nanoapp')
            count, backup = app.install(stage, target.parent, root / 'backups')
            self.assertEqual(count, 2)
            self.assertFalse((target / 'Extra.nanoapp').exists())
            self.assertTrue((target / 'My Extra App.nanoapp/manifest.napp').is_file())
            self.assertTrue((backup / 'Apps/Extra.nanoapp/manifest.napp').is_file())
            self.assertEqual((target.parent / 'Data/Notes/personal.txt').read_text(), 'keep')

    def test_folder_install_rejects_overwriting_another_identity(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            stage, target = self.folder_fixture(root)
            from test_app_packages import app_files
            for path, data in app_files('hb.other', 0x10000111, 'Extra.nanoapp').items():
                out = target / path.removeprefix('/Apps/')
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(data)
            with self.assertRaises(ValueError):
                app.install(stage, target.parent, root / 'backups')
            (target / 'Extra.nanoapp/icon.bin').unlink()
            with self.assertRaises(ValueError):
                app.install(stage, target.parent, root / 'backups')

    def test_folder_manifest_failure_restores_the_previous_app(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            stage, target = self.folder_fixture(root)
            app.install(stage, target.parent, root / 'backups')
            before = {p.relative_to(target): p.read_bytes() for p in target.rglob('*') if p.is_file()}
            (stage / 'Extra.nanoapp').rename(stage / 'Renamed.nanoapp')
            original = app.atomic_write
            def fail_manifest(path, data):
                if Path(path) == target / 'Renamed.nanoapp/manifest.napp':
                    raise OSError('injected publication failure')
                original(path, data)
            with patch.object(app, 'atomic_write', fail_manifest), self.assertRaises(OSError):
                app.install(stage, target.parent, root / 'backups')
            self.assertEqual({p.relative_to(target): p.read_bytes() for p in target.rglob('*') if p.is_file()}, before)

    def test_merge_preserves_more_than_32_existing_apps_and_replaces_by_identity(self):
        old = [bundle(f'hb.app{i}', f'App{i}', 0x100 + i) for i in range(50)]
        updated = bundle('hb.app1', 'App1', 0x101)
        new = bundle('hb.new', 'New', 0x500)
        merged = app.bundles(app.merge(app.pack(old), app.pack([updated, new])))
        self.assertEqual(merged[:50], old)
        self.assertEqual(merged[50], new)

    def test_collision_and_truncated_pack_are_rejected(self):
        a = app.pack([bundle('hb.a', 'A', 1)])
        b = app.pack([bundle('hb.b', 'B', 1)])
        with self.assertRaises(ValueError):
            app.merge(a, b)
        with self.assertRaises(ValueError):
            app.bundles(a[:-5])

    def fixture(self, root):
        stage, target = root / 'stage', root / 'device/Apps'
        (stage / 'Executables').mkdir(parents=True)
        (stage / 'Icons').mkdir()
        (target / 'Executables').mkdir(parents=True)
        (target.parent / 'Data/Notes').mkdir(parents=True)
        (target.parent / 'Library').mkdir(exist_ok=True)
        (target.parent / 'Data/Notes/personal.txt').write_text('keep')
        (target.parent / 'Library/AllApps.pack').write_bytes(app.pack([bundle('hb.a', 'A', 1)]))
        (target / 'Executables/A.hbapp').write_bytes(b'old')
        (stage / 'AllApps.pack').write_bytes(app.pack([bundle('hb.b', 'B', 2)]))
        (stage / 'Executables/B.hbapp').write_bytes(b'new')
        return stage, target

    def test_install_preserves_existing_apps_and_user_data(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            stage, target = self.fixture(root)
            count, backup = app.install(stage, target.parent, root / 'backups')
            self.assertEqual(count, 2)
            self.assertEqual((target / 'Executables/A.hbapp').read_bytes(), b'old')
            self.assertEqual((target.parent / 'Data/Notes/personal.txt').read_text(), 'keep')
            self.assertEqual(len(app.bundles((backup / 'Library/AllApps.pack').read_bytes())), 1)

    def test_source_install_replaces_only_matching_standalone_override(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            stage, target = self.fixture(root)
            (target / 'Manifests').mkdir()
            for sbid in ('hb.b', 'hb.unrelated'):
                (target / 'Manifests' / (sbid + '.napp')).write_bytes(b'manifest')
                (target / 'Executables' / (sbid + '.hbapp')).write_bytes(b'old package')
            app.install(stage, target.parent, root / 'backups')
            self.assertFalse((target / 'Manifests/hb.b.napp').exists())
            self.assertFalse((target / 'Executables/hb.b.hbapp').exists())
            self.assertEqual((target / 'Executables/B.hbapp').read_bytes(), b'new')
            self.assertEqual((target / 'Manifests/hb.unrelated.napp').read_bytes(), b'manifest')

    def test_failed_catalog_write_restores_touched_files(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            stage, target = self.fixture(root)
            original = app.atomic_write
            failed = False

            def fail_once(path, data):
                nonlocal failed
                if Path(path).name == 'AllApps.pack' and not failed:
                    failed = True
                    raise OSError('injected write failure')
                original(path, data)

            with patch.object(app, 'atomic_write', fail_once):
                with self.assertRaises(OSError):
                    app.install(stage, target.parent, root / 'backups')
            self.assertFalse((target / 'Executables/B.hbapp').exists())
            self.assertEqual(len(app.bundles((target.parent / 'Library/AllApps.pack').read_bytes())), 1)

    def test_retired_app_is_removed_with_backup_and_other_files_are_preserved(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            stage, target = self.fixture(root)
            (target / 'Icons').mkdir()
            (target / 'Icons/A.bin').write_bytes(b'icon')
            count, backup = app.install(stage, target.parent, root / 'backups', retired=('hb.a',))
            self.assertEqual(count, 1)
            self.assertEqual(app.identity(app.bundles((target.parent / 'Library/AllApps.pack').read_bytes())[0]), 'hb.b')
            self.assertFalse((target / 'Executables/A.hbapp').exists())
            self.assertFalse((target / 'Icons/A.bin').exists())
            self.assertEqual((backup / 'Apps/Executables/A.hbapp').read_bytes(), b'old')
            self.assertEqual((backup / 'Apps/Icons/A.bin').read_bytes(), b'icon')
            self.assertEqual((target.parent / 'Data/Notes/personal.txt').read_text(), 'keep')

    def test_failed_resident_publication_restores_disk_resident_and_catalog(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            stage, target = self.fixture(root)
            resident = root / 'resident.hbapp'
            resident.write_bytes(b'resident')
            original = app.atomic_write

            def fail_readback(path, data):
                original(path, data)
                if Path(path).name == 'resident.hbapp':
                    raise OSError('injected resident readback failure')

            with patch.object(app, 'atomic_write', fail_readback):
                with self.assertRaises(OSError):
                    app.install(stage, target.parent, root / 'backups', resident=resident)
            self.assertFalse((target.parent / 'Library/resident.hbapp').exists())
            self.assertEqual(len(app.bundles((target.parent / 'Library/AllApps.pack').read_bytes())), 1)

    def test_library_symlink_is_rejected_before_any_app_or_resident_writes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            stage, target = self.fixture(root)
            library = target.parent / 'Library'
            library.rename(root / 'saved-library')
            outside = root / 'outside'
            outside.mkdir()
            library.symlink_to(outside, target_is_directory=True)
            resident = root / 'resident.hbapp'
            resident.write_bytes(b'candidate')
            with self.assertRaisesRegex(ValueError, 'symlink'):
                app.install(stage, target.parent, root / 'backups', resident=resident)
            self.assertEqual(list(outside.iterdir()), [])
            self.assertEqual((target / 'Executables/A.hbapp').read_bytes(), b'old')
            self.assertFalse((target / 'Executables/B.hbapp').exists())

    def test_symlink_target_is_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            stage, target = self.fixture(root)
            outside = root / 'outside'
            outside.mkdir()
            (target / 'Executables/A.hbapp').unlink()
            (target / 'Executables').rmdir()
            (target / 'Executables').symlink_to(outside)
            with self.assertRaises(ValueError):
                app.install(stage, target.parent, root / 'backups')
            self.assertEqual(list(outside.iterdir()), [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
