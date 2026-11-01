"""Merge and install independent app bundles without rebuilding other apps."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
import time

HEADER = struct.Struct('<IHHIIIIIIIIIII')


def bundles(data):
    if len(data) < 12:
        raise ValueError('truncated app catalog')
    magic, version, count = struct.unpack_from('<III', data)
    if magic != 0x4b504248 or version != 1 or not 0 <= count <= 64 or len(data) < 12 + 4 * count:
        raise ValueError('unsupported app catalog')
    result = []
    for i in range(count):
        off = struct.unpack_from('<I', data, 12 + 4 * i)[0]
        if off < 12 + 4 * count or off + HEADER.size > len(data):
            raise ValueError('invalid bundle offset')
        h = HEADER.unpack_from(data, off)
        if h[0] != 0x50414248 or h[1] != 1 or h[12] < HEADER.size or off + h[12] > len(data):
            raise ValueError('invalid app bundle')
        b = data[off:off + h[12]]
        if HEADER.size + h[13] * 12 > len(b):
            raise ValueError('invalid section table')
        for k in (10, 11):
            if not HEADER.size <= h[k] < len(b) or b.find(b'\0', h[k]) < 0:
                raise ValueError('invalid app identity')
        for j in range(h[13]):
            _, start, size = struct.unpack_from('<III', b, HEADER.size + 12 * j)
            if start > len(b) or size > len(b) - start:
                raise ValueError('invalid app section')
        result.append(b)
    return result


def identity(bundle):
    h = HEADER.unpack_from(bundle)
    return bundle[h[10]:].split(b'\0', 1)[0].decode('ascii')


def pack(items):
    if len(items) > 64:
        raise ValueError('catalog exceeds 64 apps')
    data = bytearray(struct.pack('<III', 0x4b504248, 1, len(items)) + bytes(4 * len(items)))
    for i, b in enumerate(items):
        struct.pack_into('<I', data, 12 + 4 * i, len(data))
        data.extend(b)
        data.extend(bytes(-len(data) % 4))
    bundles(data)
    return bytes(data)


def merge(existing, incoming):
    new = bundles(incoming)
    replacements = {identity(b): b for b in new}
    if len(replacements) != len(new):
        raise ValueError('duplicate incoming identity')
    result = []
    for b in bundles(existing) if existing else []:
        result.append(replacements.pop(identity(b), b))
    result.extend(replacements.values())
    for field in (3, 11):
        seen = set()
        for b in result:
            h = HEADER.unpack_from(b)
            value = h[field] if field == 3 else b[h[field]:].split(b'\0', 1)[0]
            if value in seen:
                raise ValueError('app id or filename collision')
            seen.add(value)
    return pack(result)


def atomic_write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.nanoapps-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(name, path)
        if path.read_bytes() != data:
            raise OSError('readback failed: ' + str(path))
    finally:
        Path(name).unlink(missing_ok=True)


def folder_apps(root, strict=False):
    """Read validated .nanoapp folders; ignore incomplete copies on the device.

    Folder names are presentation, SBIDs are identity. Installation can therefore
    replace an older folder with a changed display name without leaving two apps.
    Extra asset files are preserved; the three managed files are backed up by
    install(), with the manifest published after code/icon bytes.
    """
    from package_app import MANIFEST, manifest_for, validate_folder
    found = []
    if not root.exists():
        return found
    for folder in sorted(root.iterdir()):
        if not folder.is_dir() or not folder.name.lower().endswith('.nanoapp'):
            continue
        if folder.is_symlink():
            raise ValueError('symlink app folder: ' + str(folder))
        try:
            validate_folder(folder.name)
            manifest = (folder / 'manifest.napp').read_bytes()
            if len(manifest) < MANIFEST.size + HEADER.size:
                raise ValueError('truncated app manifest')
            entry = manifest[MANIFEST.size:]
            if bundles(pack([entry])) != [entry]:
                raise ValueError('invalid app bundle length')
            exe = (folder / 'app.hbapp').read_bytes()
            icon = (folder / 'icon.bin').read_bytes()
            if manifest != manifest_for(entry, exe, icon, MANIFEST.unpack_from(manifest)[4]):
                raise ValueError('invalid app manifest')
            found.append((folder.name, entry, exe, icon, manifest))
        except (ValueError, OSError, struct.error, UnicodeError):
            if strict:
                raise
    return found


def install(staged, target, backup_root, resident=None, retired=()):
    """Install into an iPod volume: Apps bundles and optional Library resident.

    Back up touched files, copy app folders, then publish their manifests.

    A resident is updated only when explicitly supplied. App-only updates leave
    unrelated files untouched. This is recoverable file replacement, not a claim
    of multi-file atomicity across power loss.
    """
    staged, volume = Path(staged), Path(target)
    target = volume / "Apps"
    catalog = volume / 'Library/AllApps.pack'
    existing = catalog.read_bytes() if catalog.exists() else b''
    foldered = not (staged / 'Executables').exists()
    incoming_apps = folder_apps(staged, strict=True) if foldered else []
    incoming = pack([b for _, b, *_ in incoming_apps]) if foldered else (staged / 'AllApps.pack').read_bytes()
    if foldered and (len(incoming_apps) != len(bundles(incoming)) or
            len({name.casefold() for name, *_ in incoming_apps}) != len(incoming_apps)):
        raise ValueError('duplicate app identity or folder')
    replaced = set(retired) | ({identity(b) for b in bundles(incoming)} if foldered else set())
    removed = [b for b in bundles(existing) if identity(b) in replaced] if existing else []
    if removed:
        existing = pack([b for b in bundles(existing) if identity(b) not in replaced])
    merged = merge(existing, incoming)
    writes = {} if foldered else {p.relative_to(staged): p.read_bytes() for folder in ('Executables', 'Icons')
                                  for p in (staged / folder).glob('*') if p.is_file()}
    installed = folder_apps(target)
    incoming_ids = {identity(b) for b in bundles(incoming)}
    destinations = {name.casefold(): identity(b) for name, b, *_ in incoming_apps}
    valid_names = {name.casefold() for name, *_ in installed}
    # A broken or incompatible folder may still belong to another author. Don't
    # overwrite it merely because its title matches a newly built app.
    for child in target.iterdir() if target.exists() else ():
        if child.name.casefold() in destinations and child.name.casefold() not in valid_names:
            raise ValueError('existing app folder is incomplete or incompatible: ' + child.name)
    kept = []
    existing_case = {}
    for name, b, *_ in installed:
        sbid = identity(b)
        if name.casefold() in destinations and destinations[name.casefold()] != sbid:
            raise ValueError('app folder already belongs to another identity: ' + name)
        if name.casefold() in destinations:
            # FAT is case-insensitive. Reuse the existing spelling so rollback
            # cannot mistake two differently cased paths for different files.
            existing_case[name.casefold()] = name
        if sbid in incoming_ids or sbid in retired:
            for file in ('manifest.napp', 'app.hbapp', 'icon.bin'):
                writes[Path(name) / file] = None
        else:
            kept.append(b)
    # Check IDs and the total app limit across independent folders and legacy
    # entries, excluding the legacy entries those folders override.
    combined = merge(existing, pack(kept)) if kept else existing
    total = len(bundles(merge(combined, incoming)))
    for name, b, exe, icon, manifest in incoming_apps:
        name = existing_case.get(name.casefold(), name)
        for file, data in (('app.hbapp', exe), ('icon.bin', icon), ('manifest.napp', manifest)):
            rel = Path(name) / file
            writes.pop(rel, None)  # publish each manifest last, including updates
            writes[rel] = data
    # An explicit source install replaces a standalone override of the same
    # app, while preserving every unrelated independently installed package.
    for b in bundles(incoming):
        sbid = identity(b)
        if not sbid.startswith('hb.') or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-' for c in sbid):
            raise ValueError('invalid installed app identity')
        manifest = Path('Manifests') / (sbid + '.napp')
        if (target / manifest).exists():
            writes[manifest] = None
            for folder, suffix in (('Executables', '.hbapp'), ('Icons', '.bin')):
                old = Path(folder) / (sbid + suffix)
                if old not in writes:
                    writes[old] = None
    if not foldered or removed:
        writes[Path('AllApps.pack')] = existing if foldered else merged
    for b in removed:
        offset = HEADER.unpack_from(b)[11]
        name = b[offset:].split(b'\0', 1)[0].decode('utf-8')
        if not name or name in ('.', '..') or Path(name).name != name:
            raise ValueError('invalid retired app filename')
        for folder, suffix in (('Executables', '.hbapp'), ('Icons', '.bin')):
            rel = Path(folder) / (name + suffix)
            if rel in writes:
                raise ValueError('retired app filename reused by incoming app')
            writes[rel] = None
    writes = {(Path("Library") if rel == Path("AllApps.pack") else Path("Apps")) / rel: data
              for rel, data in writes.items()}
    target = volume
    if resident:
        # One copyable, checksummed update. Publish it last, covered by the same
        # backup/readback/rollback receipt as app files.
        writes[Path('Library/resident.hbapp')] = Path(resident).read_bytes()
    for rel in writes:
        current = target
        for part in (None, *rel.parts):
            if part is not None:
                current = current / part
            if current.is_symlink():
                raise ValueError('symlink in install target: ' + str(current))
    backup = Path(backup_root) / (time.strftime('%Y%m%d-%H%M%S') + '-' + str(time.time_ns()))
    backup.mkdir(parents=True, exist_ok=False)
    receipt = {}
    for rel in writes:
        old = target / rel
        receipt[str(rel)] = old.exists()
        if old.exists():
            if not old.is_file():
                raise ValueError('install target is not a file')
            dest = backup / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(old, dest)
    (backup / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    try:
        for rel, data in writes.items():
            if data is None:
                (target / rel).unlink(missing_ok=True)
            else:
                atomic_write(target / rel, data)
    except BaseException:
        for rel, existed in receipt.items():
            if existed:
                atomic_write(target / rel, (backup / rel).read_bytes())
            else:
                (target / rel).unlink(missing_ok=True)
        raise
    # Remove only empty retired/renamed folders; unrelated assets/data survive.
    for rel, data in writes.items():
        if data is None and rel.parent.suffix.lower() == '.nanoapp':
            try:
                (target / rel.parent).rmdir()
            except OSError:
                pass
    return total, backup
