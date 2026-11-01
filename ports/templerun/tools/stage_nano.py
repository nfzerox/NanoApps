#!/usr/bin/env python3
"""Stage Temple Run beside the apps in an existing NanoApps pack. No device writes."""
import argparse
import hashlib
import io
import shutil
import struct
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace


def read_pack(path):
    data = Path(path).read_bytes()
    magic, version, count = struct.unpack_from('<III', data)
    if magic != 0x4b504248 or version != 1 or not 0 < count <= 64:
        raise ValueError('unsupported app pack')
    bundles = []
    for i in range(count):
        off = struct.unpack_from('<I', data, 12 + 4 * i)[0]
        hdr = struct.unpack_from('<IHHIIIIIIIIIII', data, off)
        size = hdr[12]
        if hdr[0] != 0x50414248 or hdr[1] != 1 or off + size > len(data):
            raise ValueError('invalid bundle')
        bundles.append(data[off:off + size])
    return bundles


def bundle_name(b):
    off = struct.unpack_from('<I', b, 40)[0]
    return b[off:].split(b'\0', 1)[0].decode('ascii')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk', type=Path, required=True, help='NanoApps checkout')
    ap.add_argument('--app', type=Path, required=True, help='built templerun.hbapp')
    ap.add_argument('--data', type=Path, required=True, help='converted data with files.lst')
    ap.add_argument('--icon', type=Path, required=True, help='112 by 112 RGBA icon from build_icon.py')
    ap.add_argument('--base-pack', type=Path, required=True, help='current AllApps-B.pack')
    ap.add_argument('--out', type=Path, required=True, help='new staging directory')
    args = ap.parse_args()
    if args.out.exists():
        ap.error('--out must be a new directory')
    sys.path.insert(0, str(args.sdk.resolve() / 'tools'))
    import build_apps
    import mkapp
    from PIL import Image

    with Image.open(args.icon) as source_icon:
        icon = source_icon.convert('RGBA')
    if icon.size != (build_apps.SIZE, build_apps.SIZE):
        raise ValueError('icon must match the NanoApps tile size')

    app = args.app.read_bytes()
    magic, entry, image, span, relocs, align = struct.unpack_from('<6I', app)
    if magic != 0x314c5248 or len(app) != 24 + image + relocs * 4 or not entry < image <= span:
        raise ValueError('invalid .hbapp')
    originals = read_pack(args.base_pack)
    retained = [b for b in originals if bundle_name(b) != 'Temple Run']
    ids = {struct.unpack_from('<I', b, 8)[0] for b in retained}
    new_id = build_apps.assign_app_ids(['org.nanoapps.templerun'])['org.nanoapps.templerun']
    if new_id in ids:
        raise ValueError('Temple Run app ID collides with an existing app')
    template = next(b for b in originals if struct.unpack_from('<H', b, 6)[0] == 2)
    hdr = struct.unpack_from('<IHHIIIIIIIIIII', template)
    screen = None
    for i in range(hdr[13]):
        kind, off, size = struct.unpack_from('<III', template, 52 + 12 * i)
        if kind == 3:
            screen = template[off:off + size]
    if not screen:
        raise ValueError('base pack has no GL surface screen resource')

    # Copy only the files named by the converter, never local saves or stale pages.
    files = []
    for line in (args.data / 'files.lst').read_text(encoding='ascii').splitlines():
        name, size = line.split()
        if Path(name).name != name or len(name) >= 48:
            raise ValueError('invalid manifest name')
        source = args.data / name
        if source.stat().st_size != int(size):
            raise ValueError('manifest size mismatch: ' + name)
        files.append(source)
    if len(files) > 255:
        raise ValueError('too many data files')
    apps = args.out / 'Apps'
    data_out = apps / 'Data' / 'TempleRun'
    data_out.mkdir(parents=True)
    for source in files:
        shutil.copyfile(source, data_out / source.name)
    (data_out / 'files.lst').write_text(''.join('%s %d\n' % (f.name, f.stat().st_size) for f in files))
    (apps / 'Executables').mkdir()
    (apps / 'Executables' / 'Temple Run.hbapp').write_bytes(app)
    (apps / 'Icons').mkdir()
    with tempfile.TemporaryDirectory(prefix='tr-stage-') as temp:
        scratch = Path(temp)
        screen_file = scratch / 'screen.bin'
        screen_file.write_bytes(screen)
        new_bundle = scratch / 'templerun.app'
        mkapp.build_bundle(SimpleNamespace(
            out=str(new_bundle), id=new_id, kind=2, sbid='hb.templerun', name='Temple Run',
            label_id=new_id, screen_id=hdr[5], layout_id=hdr[6], os_handler=0,
            label_tag=hdr[8], screen_tag=hdr[9], label_text='Temple Run',
            screen=str(screen_file), icon=None, label=None, code=None))
        bundle_files = []
        for i, b in enumerate(retained):
            path = scratch / ('existing%d.app' % i)
            path.write_bytes(b)
            bundle_files.append(str(path))
        mkapp.build_pack(str(apps / 'AllApps-B.pack'), bundle_files + [str(new_bundle)])
        icons = scratch / 'icons'
        icons.mkdir()
        icon.save(icons / '229441873_1888.png')
        output = io.BytesIO()
        build_apps.pack_silverdb(output, icons)
        db = output.getvalue()
        start = struct.unpack_from('<I', db, 4)[0]
        off, size = struct.unpack_from('<II', db, 32)
        (apps / 'Icons' / 'Temple Run.bin').write_bytes(db[start + off:start + off + size])
    packed = read_pack(apps / 'AllApps-B.pack')
    assert packed[:-1] == retained, 'existing app metadata changed'
    (args.out / 'base-pack.sha256').write_text(hashlib.sha256(args.base_pack.read_bytes()).hexdigest() + '\n')
    checksums = []
    for path in sorted(apps.rglob('*')):
        if path.is_file():
            checksums.append('%s  %s\n' % (hashlib.sha256(path.read_bytes()).hexdigest(), path.relative_to(args.out)))
    (args.out / 'SHA256SUMS').write_text(''.join(checksums))
    print('Ready:', ', '.join(bundle_name(b) for b in packed))
    print('%d files staged; existing bundles preserved byte-for-byte' % len(checksums))


if __name__ == '__main__':
    main()
