#!/usr/bin/env python3
"""Build matched 1.1.2 IPSWs and a copy-to-disk ZIP. No device or GitHub access.

The IPSWs carry only the disk bootstrap. The disk ZIP puts everything
in three disk-root folders: Apps for .nanoapp bundles, Data for app data,
and Library for the versioned resident.hbapp. Validate the app manifests and firmware runtime directly;
no shared catalog or duplicate startup tree is produced.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

from app_install import identity, merge, pack, folder_apps
from wrap_resident_slot import validate_hrl1, validate_slot, source_version

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_FILES = {'loader.hbapp'}
# Keep LVGL diagnostics and developer tools available to developers, outside the
# production disk ZIP.
EXCLUDED_APPS = {'benchmark', 'stress', 'widgets', 'freeram', 'lap'}


def release_apps():
    return sorted(d.name for d in (ROOT / 'apps').iterdir()
                  if d.name not in EXCLUDED_APPS and
                  (d / 'Info.plist').is_file() and (d / 'Makefile').is_file())


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate_disk_tree(apps, expected):
    entries = folder_apps(apps, strict=True)
    if {identity(b) for _, b, *_ in entries} != {'hb.' + name for name in expected}:
        raise ValueError('app folders do not contain every requested app')
    if len(entries) != len(expected) or len({n.casefold() for n, *_ in entries}) != len(entries):
        raise ValueError('duplicate app identity or folder')
    merge(b'', pack([b for _, b, *_ in entries]))  # resource-ID collision check
    files = {Path(name) / file for name, *_ in entries
             for file in ('manifest.napp', 'app.hbapp', 'icon.bin')}
    actual = {p.relative_to(apps) for p in apps.rglob('*') if p.is_file()}
    if actual != files or any(p.is_symlink() for p in apps.rglob('*')):
        raise ValueError('app tree has missing or unexpected files')
    return len(entries)


def package_release(apps, firmware, records, output, commit):
    """Publish only a complete, verified set into a local output directory."""
    expected = release_apps()
    count = validate_disk_tree(apps, expected)
    runtime = firmware / 'resources/NanoApps'
    if {p.name for p in runtime.iterdir()} != RUNTIME_FILES or any(p.is_symlink() for p in runtime.iterdir()):
        raise ValueError('runtime tree has missing or unexpected files')
    validate_hrl1((runtime / 'loader.hbapp').read_bytes())
    resident = firmware / 'Library/resident.hbapp'
    resident_version = validate_slot(resident.read_bytes())[1]
    if not resident_version:
        raise ValueError('release resident must be versioned')
    runtime_hashes = {name: sha256(runtime / name) for name in RUNTIME_FILES}
    for revision in ('2012', '2015'):
        record = records[revision]
        if (record['revision'] != revision or record['firmware'] != '1.1.2' or
                record['build'] != '39A10023' or record['scsi_group'] != '98' or
                record['ipsw'] != f'NanoApps-{revision}-Model.ipsw' or
                record['runtime_files'] != runtime_hashes or
                record['ipsw_sha256'] != sha256(firmware / record['ipsw'])):
            raise ValueError('firmware and disk payload do not match: ' + revision)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='nano-release-', dir=output.parent) as td:
        stage = Path(td)
        disk = stage / 'NanoApps-disk.zip'
        with zipfile.ZipFile(disk, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for path in sorted(p for p in apps.rglob('*') if p.is_file()):
                member = zipfile.ZipInfo((Path('Apps') / path.relative_to(apps)).as_posix())
                member.compress_type = zipfile.ZIP_DEFLATED
                member.external_attr = 0o100644 << 16
                archive.writestr(member, path.read_bytes())
            archive.writestr('Library/resident.hbapp', resident.read_bytes())
            for path in sorted((ROOT / 'data').rglob('*')):
                if path.is_symlink():
                    raise ValueError('symlink in bundled app data')
                if path.is_file():
                    member = zipfile.ZipInfo((Path('Data') / path.relative_to(ROOT / 'data')).as_posix())
                    member.compress_type = zipfile.ZIP_DEFLATED
                    member.external_attr = 0o100644 << 16
                    archive.writestr(member, path.read_bytes())
        with zipfile.ZipFile(disk) as archive:
            if archive.testzip():
                raise ValueError('disk ZIP failed CRC verification')
        for revision in ('2012', '2015'):
            source = firmware / records[revision]['ipsw']
            shutil.copy2(source, stage / source.name)
        manifest = {'source_commit': commit, 'firmware': '1.1.2', 'build': '39A10023',
                    'resident_version': resident_version,
                    'app_count': count, 'scsi_group': '98', 'runtime_files': runtime_hashes,
                    'files': {p.name: sha256(p) for p in sorted(stage.iterdir())}}
        (stage / 'build.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
        (stage / 'SHA256SUMS').write_text(''.join(
            f'{sha256(p)}  {p.name}\n' for p in sorted(stage.iterdir())))
        for path in stage.iterdir():
            path.replace(output / path.name)
    print(f'Release: {output} ({count} apps, two 1.1.2 IPSWs, matching disk ZIP)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/release')
    args = parser.parse_args()
    # Makefiles and the firmware helper invoke python3 by name. Keep them in
    # this interpreter's environment even when called as .venv/bin/python.
    os.environ['PATH'] = str(Path(sys.executable).parent) + os.pathsep + os.environ.get('PATH', '')
    # Both hardware revisions must embed exactly the same runtime, including
    # its version, even when this is a build of local uncommitted edits.
    os.environ.setdefault('SOURCE_DATE_EPOCH', str(source_version()))
    names = release_apps()
    subprocess.run([sys.executable, str(ROOT / 'start'), 'build', *names], check=True)
    firmware = ROOT / 'build/firmware'
    records = {}
    for revision in ('2012', '2015'):
        subprocess.run([sys.executable, str(ROOT / 'start'), 'firmware', 'build',
                        '--revision', revision], check=True)
        records[revision] = json.loads((firmware / 'build.json').read_text())
    subprocess.run([sys.executable, str(ROOT / 'tools/build_apps.py'), *names], check=True)
    apps = Path('/tmp/hbapps/Apps')
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    package_release(apps, firmware, records, args.output.resolve(), commit)


if __name__ == '__main__':
    main()
