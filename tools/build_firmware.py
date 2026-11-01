#!/usr/bin/env python3
"""Build the 1.1.2 firmware and matching loader files from source; never flash."""
import argparse
import hashlib
import json
import os
import plistlib
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile
import urllib.request
from wrap_resident_slot import wrap_slot

ROOT = Path(__file__).resolve().parents[1]
STOCK_URL = ('http://appldnld.apple.com/ipod/sbml/osx/bundles/'
             '031-59796-20160525-8E6A5D46-21FF-11E6-89D1-C5D3662719FC/'
             'iPod_1.1.2_39A10023.ipsw')
STOCK_SHA256 = '960d570aa073f278f21b2c99d1aa3601ef136539353492b71cc71f462870a252'
BOOTLOADER_SHA256 = '7d921c6d97305e3ef944c1f6212cab2adcbb4a5d4c2ff45faeafc438aa3b1548'


def stock_ipsw(path=None):
    """Apple serves this discontinued image over HTTP; pin its complete hash."""
    if path is None:
        path = ROOT / 'build/downloads/iPod_1.1.2_39A10023.ipsw'
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            partial = path.with_suffix('.download')
            print('Downloading firmware 1.1.2 from Apple…', flush=True)
            try:
                urllib.request.urlretrieve(STOCK_URL, partial)
                if hashlib.sha256(partial.read_bytes()).hexdigest() != STOCK_SHA256:
                    raise ValueError('Apple firmware download checksum mismatch')
                partial.replace(path)
            finally:
                partial.unlink(missing_ok=True)
    if hashlib.sha256(path.read_bytes()).hexdigest() != STOCK_SHA256:
        raise ValueError('expected the unmodified Apple 1.1.2 / 39A10023 IPSW')
    return path



def run(*args, cwd=ROOT, env=None):
    subprocess.run([str(x) for x in args], cwd=cwd, env=env, check=True)


def prepare_bootstrap():
    bundle = ROOT / 'build/firmware'
    run(sys.executable, ROOT / 'tools/build_firmware_bootstrap.py', '--output', bundle)
    run('make', '-s', '-C', ROOT / 'system/firmware_publisher')
    return bundle


def firmware_key(bundle):
    """Hash actual bootstrap bytes and firmware inputs, excluding disk payloads.

    The cache is disposable: each hit must pass key, revision, runtime-file and
    complete IPSW SHA checks. A miss rebuilds and runs full resource preflight.
    Resident/app/data changes do not enter this key or the firmware image.
    """
    paths = [bundle / 'resources/NanoApps/loader.hbapp',
             bundle / 'firmware/deferred_stage.bin',
             ROOT / 'system/firmware_publisher/build/recorder_publisher.bin']
    tracked = subprocess.check_output(['git', 'ls-files', '-z', 'firmware'], cwd=ROOT).split(b'\0')
    prefixes = ('firmware/src/', 'firmware/parse/', 'firmware/scsi_shellcode/',
                'firmware/helpers/')
    files = {'firmware/Cargo.toml', 'firmware/Cargo.lock', 'firmware/build.rs',
             'firmware/rust-toolchain.toml', 'firmware/Base.ttf',
             'firmware/tools/build_helvetica_cff.py',
             'firmware/tools/preflight_deferred_release.py',
             'firmware/iPod_1.1.2_39A10023_2012/manifest.plist',
             'firmware/iPod_1.1.2_39A10023_2015/manifest.plist'}
    paths += [ROOT / name for p in tracked if p
              and ((name := p.decode()) in files or name.startswith(prefixes))]
    paths += [ROOT / 'tools/build_firmware.py', ROOT / 'tools/requirements-release.txt']
    digest = hashlib.sha256(STOCK_SHA256.encode())
    for path in sorted(paths):
        digest.update(str(path.relative_to(ROOT)).encode() + b'\0')
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def cached_record(cache, key, revision, runtime):
    try:
        record = json.loads((cache / 'build.json').read_text())
        image = cache / f'NanoApps-{revision}-Model.ipsw'
        if (record['firmware_key'] != key or record['revision'] != revision or
                record['ipsw'] != image.name or record['firmware'] != '1.1.2' or
                record['build'] != '39A10023' or record['scsi_group'] != '98' or
                record['runtime_files'] != runtime or
                record['ipsw_sha256'] != hashlib.sha256(image.read_bytes()).hexdigest()):
            return None
        return record
    except (OSError, KeyError, ValueError):
        return None


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ipsw', type=Path, help='optional cached stock IPSW; otherwise download from Apple')
    p.add_argument('--revision', choices=('2012', '2015'))
    p.add_argument('--fingerprint-file', type=Path, help='prepare the bootstrap and write its firmware cache key, then exit')
    args = p.parse_args()
    if not args.revision and not args.fingerprint_file:
        p.error('--revision is required when building firmware')
    os.environ["PATH"] = str(Path(sys.executable).parent) + os.pathsep + os.environ.get("PATH", "")
    bundle = prepare_bootstrap()
    key = firmware_key(bundle)
    if args.fingerprint_file:
        args.fingerprint_file.parent.mkdir(parents=True, exist_ok=True)
        args.fingerprint_file.write_text(key + '\n')
        return
    # Build the independently replaceable disk resident, even on an IPSW cache hit.
    run('make', '-s', '-C', ROOT / 'system/silver_resident', f'PYTHON={sys.executable}')
    resident = bundle / 'Library/resident.hbapp'
    resident.parent.mkdir(parents=True, exist_ok=True)
    resident.write_bytes(wrap_slot((ROOT / 'system/silver_resident/build/silver_resident.hbapp').read_bytes()))
    runtime = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
               for p in (bundle / 'resources/NanoApps').iterdir()}
    cache = ROOT / 'build/firmware-cache' / key / args.revision
    record = cached_record(cache, key, args.revision, runtime)
    if record:
        shutil.copy2(cache / record['ipsw'], bundle / record['ipsw'])
        (bundle / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
        print(f"Reused verified firmware: {bundle / record['ipsw']}")
        return
    args.ipsw = stock_ipsw(args.ipsw)
    builder = ROOT / 'firmware'
    with zipfile.ZipFile(args.ipsw) as archive:
        manifest = archive.read('manifest.plist')
        if b'39A10023' not in manifest:
            raise SystemExit('expected stock firmware 1.1.2 / 39A10023')
        payload = plistlib.loads(manifest).get('FirmwarePayload', {})
        if payload.get('ProductVersion') != '1.1.2':
            raise SystemExit('only stock firmware 1.1.2 is supported')
        bootloader = archive.read('N31.bootloader.rb3')
        if hashlib.sha256(bootloader).hexdigest() != BOOTLOADER_SHA256:
            raise SystemExit('unexpected stock bootloader')
        for folder in builder.glob('iPod_1.1.2_39A10023_*'):
            if folder.is_dir():
                (folder / 'N31.bootloader.rb3').write_bytes(bootloader)
        stock = archive.read('Firmware.MSE')
    if stock.count(b'87402.0\x04') != 1:
        raise SystemExit('firmware has no unique original resource header')
    (builder / 'Firmware-39A10023.MSE').write_bytes(stock.replace(b'87402.0\x04', b'87402.0\x03', 1))
    if not shutil.which('otfautohint'):
        raise SystemExit('font preservation needs AFDKO: python3 -m pip install afdko==4.0.2')
    fonts = ROOT / 'build/firmware-font'
    run(sys.executable, builder / 'tools/build_helvetica_cff.py',
        '--firmware', args.ipsw, '--out-dir', fonts)
    stage = bundle / 'firmware/deferred_stage.bin'
    publisher = ROOT / 'system/firmware_publisher/build/recorder_publisher.bin'
    env = dict(os.environ, PYTHON=sys.executable,
               NANO7_RESOURCE_DIR=str(bundle / 'resources'),
               NANO7_CFF_CARRIER=str(fonts / 'Helvetica-CFF-hinted.otf'))
    run('cargo', 'run', '--locked', '--release', '--', '--device', 'nano7-refresh',
        '--deferred-stage', stage, '--deferred-installer', publisher,
        '--cff-output', bundle / 'exploit.cff', cwd=builder, env=env)
    run('cargo', 'run', '--locked', '--release', '--', '--device', 'nano7-refresh',
        '--deferred-stage', stage, '--deferred-installer', publisher, cwd=builder, env=env)
    # The donor builder emits both device-family manifests in one invocation.
    # Verify/cache both here so the second release revision is a cache hit.
    for revision in ('2012', '2015'):
        built = builder / f'iPod_1.1.2_39A10023_{revision}/Firmware.MSE'
        ipsw = bundle / f'NanoApps-{revision}-Model.ipsw'
        shutil.copy2(builder / f'iPod_1.1.2_39A10023_{revision}_nanoapps-deferred.ipsw', ipsw)
        run(sys.executable, builder / 'tools/preflight_deferred_release.py',
            '--ipsw', ipsw, '--stock', args.ipsw, '--cff', bundle / 'exploit.cff',
            '--resource-dir', bundle / 'resources', '--revision', revision)
        record = {'firmware': '1.1.2', 'build': '39A10023', 'revision': revision,
                  'scsi_group': '98', 'stock_sha256': hashlib.sha256(stock).hexdigest(),
                  'output_sha256': hashlib.sha256(built.read_bytes()).hexdigest(),
                  'ipsw': ipsw.name, 'ipsw_sha256': hashlib.sha256(ipsw.read_bytes()).hexdigest(),
                  'runtime_files': runtime, 'firmware_key': key, 'hardware_validated': False}
        cache = ROOT / 'build/firmware-cache' / key / revision
        cache.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ipsw, cache / ipsw.name)
        (cache / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
        if revision == args.revision:
            (bundle / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'Built both model IPSWs in {bundle}. No device was written.')


if __name__ == '__main__':
    main()
