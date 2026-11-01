"""Firmware reuse must exclude disk payloads and reject damaged cache entries."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import build_firmware as firmware


class FirmwareCacheTests(unittest.TestCase):
    def fixture(self, root):
        bundle = root / 'build/firmware'
        for path in ('resources/NanoApps/loader.hbapp', 'firmware/deferred_stage.bin'):
            target = bundle / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(path.encode())
        for path in ('system/firmware_publisher/build/recorder_publisher.bin',
                     'firmware/src/main.rs', 'tools/build_firmware.py',
                     'tools/requirements-release.txt'):
            target = root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(path.encode())
        return bundle

    def test_disk_updates_keep_key_and_bootstrap_updates_change_it(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            bundle = self.fixture(root)
            with patch.object(firmware, 'ROOT', root), patch.object(
                    firmware.subprocess, 'check_output', return_value=b'firmware/src/main.rs\0'):
                key = firmware.firmware_key(bundle)
                (bundle / 'Library').mkdir()
                (bundle / 'Library/resident.hbapp').write_bytes(b'new runtime')
                (root / 'apps').mkdir()
                (root / 'apps/new-app').write_bytes(b'new app')
                self.assertEqual(firmware.firmware_key(bundle), key)
                (bundle / 'resources/NanoApps/loader.hbapp').write_bytes(b'new loader')
                self.assertNotEqual(firmware.firmware_key(bundle), key)

    def test_corrupt_wrong_revision_or_runtime_is_a_cache_miss(self):
        with tempfile.TemporaryDirectory() as td:
            cache = Path(td)
            image = cache / 'NanoApps-2015-Model.ipsw'
            image.write_bytes(b'verified image')
            runtime = {'loader.hbapp': 'hash'}
            record = dict(firmware_key='key', revision='2015', firmware='1.1.2',
                          build='39A10023', scsi_group='98', runtime_files=runtime,
                          ipsw=image.name, ipsw_sha256=firmware.hashlib.sha256(image.read_bytes()).hexdigest())
            (cache / 'build.json').write_text(json.dumps(record))
            self.assertEqual(firmware.cached_record(cache, 'key', '2015', runtime), record)
            for key, revision, files in [('wrong', '2015', runtime), ('key', '2012', runtime),
                                         ('key', '2015', {'loader.hbapp': 'wrong'})]:
                self.assertIsNone(firmware.cached_record(cache, key, revision, files))
            image.write_bytes(b'corrupt')
            self.assertIsNone(firmware.cached_record(cache, 'key', '2015', runtime))


if __name__ == '__main__':
    unittest.main()
