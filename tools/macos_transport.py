"""Direct macOS iPod file and SCSI transport. No SSH or Linux host required."""
import glob
import plistlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from app_install import install


def plist(*args):
    return plistlib.loads(subprocess.check_output(['diskutil', args[0], '-plist', *args[1:]]))


def devices():
    result = []
    for disk in plist('list', 'external', 'physical').get('AllDisksAndPartitions', []):
        info = plist('info', disk['DeviceIdentifier'])
        if 'ipod' not in str(info.get('MediaName', '')).lower():
            continue
        parts = []
        for part in disk.get('Partitions', []):
            p = plist('info', part['DeviceIdentifier'])
            if p.get('FilesystemType') in ('msdos', 'hfs'):
                parts.append(p)
        if len(parts) == 1:
            result.append((info, parts[0]))
    return result


class MacDevice:
    def __init__(self, root, disk=None):
        choices = devices()
        if not choices and not disk:
            # Like sg_start --loej on Linux: load the still-connected iPod's
            # media after eject. No custom firmware command or cable reset.
            subprocess.run([str(Path(root) / 'tools/macos/ipod_sg_raw'),
                            'auto', '1b', '00', '00', '00', '03', '00'], check=True)
            for _ in range(80):
                choices = devices()
                if choices:
                    break
                time.sleep(0.5)
        if disk:
            choices = [(d, p) for d, p in choices if '/dev/' + d['DeviceIdentifier'] == disk]
        if len(choices) != 1:
            raise RuntimeError(f'expected one iPod, found {len(choices)}; set IPOD_DEV explicitly')
        self.info, self.volume = choices[0]
        self.disk = '/dev/' + self.info['DeviceIdentifier']
        self.part = '/dev/' + self.volume['DeviceIdentifier']
        self.uuid = self.volume.get('VolumeUUID')
        if not self.uuid:
            raise RuntimeError('iPod volume has no stable UUID')
        self.root = Path(root)

    def mount(self):
        current = plist('info', self.part)
        if current.get('VolumeUUID') != self.uuid:
            raise RuntimeError('iPod volume identity changed')
        if not current.get('MountPoint'):
            subprocess.run(['diskutil', 'mount', self.part], check=True)
            current = plist('info', self.part)
        return Path(current['MountPoint'])

    def _disk_command(self, command):
        # Finder's device agent can briefly dissent while processing new media.
        # Retry ordinary unmount/eject; never force it or stop the agent.
        for attempt in range(5):
            result = subprocess.run(['diskutil', command, self.disk],
                                    capture_output=True, text=True)
            if result.returncode == 0:
                return
            if 'could not be unmounted' not in result.stdout + result.stderr or attempt == 4:
                raise subprocess.CalledProcessError(result.returncode, result.args,
                                                    result.stdout, result.stderr)
            time.sleep(1)

    def unmount(self):
        self._disk_command('unmountDisk')

    def eject(self):
        self._disk_command('eject')

    def scsi(self, sub, address, data=None, length=0):
        binary = self.root / 'tools/macos/ipod_sg_raw'
        with tempfile.TemporaryDirectory(prefix='nano-scsi-') as td:
            f = Path(td) / 'transfer'
            args = [str(binary)]
            if data is not None:
                f.write_bytes(data)
                args += ['-s', str(len(data)), '-i', str(f)]
            elif length:
                args += ['-r', str(length), '-o', str(f)]
            self.unmount()
            args += [self.disk, 'c6', '98', f'{sub:02x}',
                     *(f'{b:02x}' for b in address.to_bytes(4, 'big'))]
            subprocess.run(args, check=True)
            return f.read_bytes() if length else b''

    def peek(self, address, size):
        data = bytearray()
        while len(data) < size:
            addr = address + len(data)
            base = addr & ~511
            block = self.scsi(2, base, length=512)
            data.extend(block[addr - base:addr - base + size - len(data)])
        return bytes(data)

    def peek_ejected(self, address, size):
        """Read without loading media: the nano must retain filesystem ownership."""
        data = bytearray()
        with tempfile.TemporaryDirectory(prefix='nano-ejected-peek-') as td:
            path = Path(td) / 'block'
            while len(data) < size:
                addr = address + len(data)
                base = addr & ~511
                subprocess.run([str(self.root / 'tools/macos/ipod_sg_raw'),
                    '-r', '512', '-o', str(path), 'auto', 'c6', '98', '02',
                    *(f'{b:02x}' for b in base.to_bytes(4, 'big'))], check=True)
                block = path.read_bytes()
                if len(block) != 512:
                    raise RuntimeError('short ejected SCSI read')
                data.extend(block[addr - base:addr - base + size - len(data)])
        return bytes(data)

    def write(self, file, address):
        data = Path(file).read_bytes()
        for i in range(0, len(data), 512):
            block = data[i:i + 512]
            if len(block) < 512:
                block += self.peek(address + i + len(block), 512 - len(block))
            self.scsi(1, address + i, data=block)

    def install(self, staged, resident=None):
        mount = self.mount()
        result = install(staged, mount, self.root / 'build/install-backups', resident)
        subprocess.run(['sync'], check=True)
        return result

    def pull(self, patterns, destination):
        mount = self.mount()
        out = Path(destination)
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        for pattern in patterns:
            for source in mount.glob(pattern):
                if source.is_file():
                    shutil.copy2(source, out / source.name)
        return len(list(out.iterdir()))

    def clear(self, patterns):
        mount = self.mount()
        for pattern in patterns:
            for file in mount.glob(pattern):
                if file.is_file() and not file.is_symlink():
                    file.unlink()
        subprocess.run(['sync'], check=True)
        return True
