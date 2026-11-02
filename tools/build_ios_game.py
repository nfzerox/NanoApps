#!/usr/bin/env python3
"""Prepare an optional iOS game port from a local original IPA or .app folder.

    .venv/bin/python tools/build_ios_game.py doodlejump /path/to/game.ipa

Supported originals: Flappy Bird 1.2, Doodle Jump 1.0, Temple Run 1.0. These are
native ports, not an iOS emulator. The converters verify the inspected executable;
originals, converted artwork and generated Temple Run code stay in ignored folders.
Flappy Bird and Doodle Jump are verified on a 2015 nano with the current bundle
layout. Temple Run remains experimental: it built and passed host engine checks,
but ran out of memory at the loading screen on the fully populated test device.
macOS is currently required for the donor asset converters (sips/afconvert).
The prepared C sources and game data can subsequently be built on Linux.
Python needs Pillow and pyelftools; Temple Run also needs capstone, numpy and
texture2ddecoder. Original-code comparisons additionally use unicorn.

Use an ARM bare-metal toolchain including newlib (libc and libm), such as Arm's
GNU toolchain. --toolchain accepts its bin directory; it changes only this process.
After copying the prepared Data folder to the iPod, normal ./start
build/package/install GAME works, without including the ports in default builds
or production releases. Apps use the existing resident.

Outputs: build/packages/GAME.zip (the standalone .nanoapp) and
build/ios-games/GAME/GAME-disk.zip (Apps and Data, including converted game assets).
Copy both disk-ZIP folders to the iPod root, preserving unrelated apps and saves,
then eject. The disk ZIP is for local use; it contains the supplied game's assets.
No resident, firmware, original IPA or shared app catalog is packaged.
"""
import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
GAMES = {
    'flappybird': ('1.2', 'Flap.app', 'FlappyBird'),
    'doodlejump': ('1.0', 'DoodleJump.app', 'DoodleJump'),
    'templerun': ('1.0', 'TempleRun.app', 'TempleRun'),
}
TEMPLE_ARMV7 = '91d85b352333c95531dbe9b2a2edd3e772f82ee038e386953a4b0929afe01392'


def armv7(data):
    if data[:4] == b'\xca\xfe\xba\xbe':
        if len(data) < 8:
            raise ValueError('truncated universal executable')
        count = struct.unpack_from('>I', data, 4)[0]
        if count > (len(data) - 8) // 20:
            raise ValueError('truncated universal executable architecture table')
        for i in range(count):
            cpu, sub, off, size, _ = struct.unpack_from('>5I', data, 8 + i * 20)
            if cpu == 12 and sub == 9 and off <= len(data) and size <= len(data) - off:
                return data[off:off + size]
        raise ValueError('IPA has no ARMv7 executable')
    return data


def original(source, game):
    version, app_name, _ = GAMES[game]
    target = ROOT / 'ports' / game / 'original' / ('v' + version) / 'Payload' / app_name
    with tempfile.TemporaryDirectory(prefix='nano-ipa-') as td:
        unpacked = Path(td)
        if source.is_dir():
            app = source
        else:
            with zipfile.ZipFile(source) as archive:
                if sum(i.file_size for i in archive.infolist()) > 128 * 1024 * 1024:
                    raise ValueError('unexpectedly large IPA')
                for info in archive.infolist():
                    path = Path(info.filename)
                    if path.is_absolute() or '..' in path.parts:
                        raise ValueError('unsafe IPA member: ' + info.filename)
                    # iOS signing metadata is unused by the native port; some
                    # originals have CodeResources as a relative symlink.
                    if stat.S_ISLNK(info.external_attr >> 16):
                        if path.name == 'CodeResources':
                            continue
                        raise ValueError('symlink in IPA: ' + info.filename)
                    archive.extract(info, unpacked)
            apps = list((unpacked / 'Payload').glob('*.app'))
            if len(apps) != 1:
                raise ValueError('expected one original app in IPA/Payload')
            app = apps[0]
        meta = plistlib.loads((app / 'Info.plist').read_bytes())
        if meta.get('CFBundleVersion') != version:
            raise ValueError(f'{game} requires version {version}')
        if any(p.is_symlink() and p.name != 'CodeResources' for p in app.rglob('*')):
            raise ValueError('symlink in original app')
        # Validate before changing the canonical local original used by the
        # converters and reference tests. Version strings alone are insufficient.
        if game == 'templerun':
            binary = armv7((app / 'TempleRun').read_bytes())
            if hashlib.sha256(binary).hexdigest() != TEMPLE_ARMV7:
                raise ValueError('use the inspected Temple Run 1.0 ARMv7 build')
        elif game == 'doodlejump':
            sys.path.insert(0, str(ROOT / 'ports/doodlejump/tools'))
            from convert_assets import scenes
            scenes(app / 'DoodleJump')
        else:
            sys.path.insert(0, str(ROOT / 'ports/flappybird/tools'))
            from app_binary import thin_armv7
            thin_armv7(app / 'flap')
        if app.resolve() != target.resolve():
            shutil.rmtree(target, ignore_errors=True)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(app, target, ignore=shutil.ignore_patterns('CodeResources'))
    return target


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', choices=GAMES)
    p.add_argument('source', type=Path, help='original IPA or extracted .app')
    p.add_argument('--toolchain', type=Path, help='ARM GNU toolchain bin directory (with newlib)')
    args = p.parse_args()
    if sys.platform != 'darwin':
        p.error('asset preparation currently needs macOS sips/afconvert; prepared ports build on Linux')
    if args.toolchain:
        os.environ['PATH'] = str(args.toolchain.resolve()) + os.pathsep + os.environ['PATH']
    os.environ['PATH'] = str(Path(sys.executable).parent) + os.pathsep + os.environ['PATH']
    libc = subprocess.check_output(['arm-none-eabi-gcc', '-marm', '-march=armv5te+fp',
                                   '-mfloat-abi=softfp', '-print-file-name=libc.a'], text=True).strip()
    if not Path(libc).is_file():
        p.error('ARM compiler needs newlib; pass --toolchain /path/to/arm-gnu-toolchain/bin')
    game = args.game
    folder = ROOT / 'ports' / game
    build = folder / 'build'
    build.mkdir(parents=True, exist_ok=True)
    app = original(args.source.resolve(), game)

    def run(*cmd):
        subprocess.run([str(c) for c in cmd], check=True, cwd=ROOT)

    script = folder / 'tools'
    if game == 'templerun':
        (build / 'TempleRun-armv7').write_bytes(armv7((app / 'TempleRun').read_bytes()))
        run(sys.executable, script / 'recomp.py', build / 'recomp')
        run(sys.executable, script / 'convert_assets.py', build / 'assets')
        data = build / 'assets/nano'
        icon_args = ['--source', app]
    else:
        data = build / 'data'
        run(sys.executable, script / 'convert_assets.py', app, data)
        icon_args = (['--atlas', build / 'png/atlas.png', '--atlas-text', app / 'atlas.txt']
                     if game == 'flappybird' else ['--source', app])
    run(sys.executable, script / 'build_icon.py', '--sdk', ROOT,
        *icon_args, '--out', build / 'icon.png')
    run('make', '-C', folder)
    from build_apps import main as stage
    from package_app import package_app
    package_app(game)
    output = ROOT / 'build/ios-games' / game
    disk = output / 'disk'
    stage([game], out=disk)
    shutil.copytree(data, disk / 'Data' / GAMES[game][2])
    zip_path = output / (game + '-disk.zip')
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(disk.rglob('*')):
            if path.is_file():
                archive.write(path, path.relative_to(disk))
    print(f'Prepared {zip_path}\nCopy Apps and Data to the iPod root, then eject.')


if __name__ == '__main__':
    main()
