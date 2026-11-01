"""Build an independently distributable app with ``./start package my_app``.

The output is build/packages/my_app.zip; authors can host it on any repository
or download page. The ZIP contains My App.nanoapp/ directly. Users copy that
folder into Apps on the iPod, then eject. The resulting device layout is:

    Apps/My App.nanoapp/manifest.napp
    Apps/My App.nanoapp/app.hbapp
    Apps/My App.nanoapp/icon.bin

Library/resident.hbapp supplies the shared runtime. Packages contain these three files,
never startup files, a shared catalog, other apps or saved data. Home refreshes
without rebooting; resident updates still require a restart.

Choose a unique, stable source folder name (up to 20 ASCII letters, digits,
underscores or hyphens) and CFBundleIdentifier; keep both for updates. The SBID
is hb.<source-folder>. The distributable folder uses CFBundleName + .nanoapp and
may contain capitals, spaces and UTF-8 (up to 63 bytes including the extension).
It can be renamed without changing Home identity; replace the existing folder
when updating rather than keeping two copies of the same app. Executables
are canonical HRL1 images; icons are the existing 112x112 bitmap format.

Delete the app folder to remove it, retaining its saved data. A package
overrides an optional legacy AllApps.pack entry with the same identity; removing
the override exposes that legacy entry again. Source installs replace matching
overrides while preserving unrelated packages (see app_install.py).

HBMinimumResidentVersion in Info.plist defaults to zero. Set it to a release's
build.json resident_version only when an app needs a compatible feature/fix.
The wire format and compatibility policy live in sdk/hb_app_catalog.h and
sdk/hb_runtime_compat.h. Checksums detect corruption, not publisher authenticity.
"""
from pathlib import Path
import re
import struct
import tempfile
import zipfile

from app_install import HEADER, identity, folder_apps
from wrap_resident_slot import validate_hrl1, fnv1a32
from runtime_compat import ABI, FIRMWARE

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = struct.Struct('<9I')
MAGIC, VERSION = 0x504e4248, 1


def validate_folder(name):
    """A single FAT/Windows-safe .nanoapp folder, also bounded by the resident."""
    if (not name.lower().endswith('.nanoapp') or len(name) <= 8 or name.startswith('.')
            or len(name.encode('utf-8')) > 63
            or any(ord(c) < 32 or ord(c) == 127 or c in '<>:"/\\|?*' for c in name)):
        raise ValueError('use a .nanoapp folder of at most 63 UTF-8 bytes, without reserved filename characters')
    return name


def manifest_for(bundle, executable, icon, minimum_resident=0):
    """The same compatibility/integrity manifest for one app or a collection."""
    sbid = identity(bundle)
    if not re.fullmatch(r'hb\.[A-Za-z0-9_-]{1,20}', sbid):
        raise ValueError('use a unique app folder of at most 20 letters, digits, underscores or hyphens')
    h = HEADER.unpack_from(bundle)
    key = bundle[h[11]:].split(b'\0', 1)[0].decode('ascii')
    if key != sbid:
        raise ValueError('manifest file key must use the stable app identity')
    validate_hrl1(executable)
    if len(icon) != 112 * 112 * 4 + 32:
        raise ValueError('invalid home icon size')
    if not 0 <= minimum_resident <= 0xffffffff:
        raise ValueError('minimum resident version must be a 32-bit source timestamp')
    manifest = MANIFEST.pack(MAGIC, VERSION, ABI, FIRMWARE, minimum_resident,
                             len(bundle), len(executable), fnv1a32(executable), fnv1a32(icon)) + bundle
    if len(manifest) > 64 * 1024:
        raise ValueError('app manifest exceeds the resident limit')
    return manifest


def write_package(staged, output, minimum_resident=0):
    """Accept one identity-keyed app, validate it, and publish exactly 3 files."""
    staged, output = Path(staged), Path(output)
    entries = folder_apps(staged, strict=True)
    if len(entries) != 1:
        raise ValueError('a standalone package must contain exactly one app folder')
    name, bundle, executable, icon, _ = entries[0]
    folder = staged / name
    manifest = manifest_for(bundle, executable, icon, minimum_resident)
    files = {
        f'{folder.name}/manifest.napp': manifest,
        f'{folder.name}/app.hbapp': executable,
        f'{folder.name}/icon.bin': icon,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    # Build privately then replace, so a failed build cannot leave a partial ZIP.
    with tempfile.TemporaryDirectory(dir=output.parent) as td:
        pending = Path(td) / 'app.zip'
        with zipfile.ZipFile(pending, 'w', zipfile.ZIP_DEFLATED) as archive:
            for name, data in files.items():
                archive.writestr(name, data)
        pending.replace(output)
    return output


def package_app(name, output=None):
    import plistlib
    from build_apps import main as stage_app, app_path
    output = output or ROOT / 'build/packages' / (name + '.zip')
    with tempfile.TemporaryDirectory(prefix='nanoapp-package-') as td:
        stage_app([name], out=Path(td) / 'stage')
        meta = plistlib.loads((app_path(name) / 'Info.plist').read_bytes())
        result = write_package(Path(td) / 'stage/Apps', output,
                               int(meta.get('HBMinimumResidentVersion', 0)))
    print(f'Package: {result}\nUnzip, copy the app folder into Apps on the iPod, then eject. Home refreshes automatically.')
    return result
