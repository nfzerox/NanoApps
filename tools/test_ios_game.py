"""Reject incompatible/unsafe original packages before replacing local inputs."""
from pathlib import Path
import plistlib
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import build_ios_game as ios

# Keep the real executable validator available while redirecting originals to
# a temporary repository root; never replace the user's inspected game files.
sys.path.insert(0, str(ios.ROOT / 'ports/flappybird/tools'))


class OriginalPackageTests(unittest.TestCase):
    def test_universal_executable_selects_armv7(self):
        data = (b'\xca\xfe\xba\xbe' + struct.pack('>I', 2) +
                struct.pack('>5I', 12, 6, 48, 4, 0) +
                struct.pack('>5I', 12, 9, 52, 4, 0) + b'v6__v7__')
        self.assertEqual(ios.armv7(data), b'v7__')
        with self.assertRaises(ValueError):
            ios.armv7(data[:28])

    def test_unsafe_archive_paths_are_rejected(self):
        for member in ('../escaped', '/absolute'):
            with self.subTest(member=member), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                ipa = root / 'source.ipa'
                with zipfile.ZipFile(ipa, 'w') as archive:
                    archive.writestr(member, b'bad')
                with patch.object(ios, 'ROOT', root), self.assertRaises(ValueError):
                    ios.original(ipa, 'flappybird')
                self.assertFalse((root / 'ports').exists())

    def test_wrong_build_preserves_existing_local_original(self):
        for version in ('1.3', '1.2'):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                previous = root / 'ports/flappybird/original/v1.2/Payload/Flap.app'
                previous.mkdir(parents=True)
                (previous / 'keep').write_bytes(b'previous inspected original')
                source = root / 'other.app'
                source.mkdir()
                (source / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleVersion': version}))
                (source / 'flap').write_bytes(b'unsupported executable')
                with patch.object(ios, 'ROOT', root), self.assertRaises(ValueError):
                    ios.original(source, 'flappybird')
                self.assertEqual((previous / 'keep').read_bytes(), b'previous inspected original')


if __name__ == '__main__':
    unittest.main(verbosity=2)
