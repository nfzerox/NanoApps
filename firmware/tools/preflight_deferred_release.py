#!/usr/bin/env python3
"""Verify a built 1.1.2 IPSW against Apple's stock image and the source-built CFF.

No device access. Check the complete ZIP, device-family metadata, unchanged OS
and bootloader, every resource file, and the exact packaged exploit charstring.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import plistlib
import struct
import tempfile
import zipfile
from fontTools.ttLib import TTFont
from pyfatfs.PyFatFS import PyFatFS


def fail(message):
    raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read_cff_index(data: bytes, offset: int) -> tuple[int, list[bytes]]:
    if offset + 2 > len(data):
        fail("raw CFF has a truncated INDEX count")
    count = int.from_bytes(data[offset:offset + 2], "big")
    offset += 2
    if count == 0:
        return offset, []
    if offset >= len(data):
        fail("raw CFF has a truncated INDEX offset size")
    off_size = data[offset]
    offset += 1
    if off_size not in (1, 2, 3, 4):
        fail(f"raw CFF has invalid INDEX offset size {off_size}")
    table_size = (count + 1) * off_size
    if offset + table_size > len(data):
        fail("raw CFF has a truncated INDEX offset table")
    offsets = [
        int.from_bytes(data[offset + i * off_size:offset + (i + 1) * off_size],
                       "big")
        for i in range(count + 1)
    ]
    offset += table_size
    if offsets[0] != 1 or any(b < a for a, b in zip(offsets, offsets[1:])):
        fail("raw CFF has malformed INDEX offsets")
    end = offset + offsets[-1] - 1
    if end > len(data):
        fail("raw CFF INDEX data exceeds the file")
    entries = [
        data[offset + offsets[i] - 1:offset + offsets[i + 1] - 1]
        for i in range(count)
    ]
    return end, entries

def raw_cff_charstring(cff: bytes) -> bytes:
    if len(cff) < 4 or cff[2] != 4:
        fail("raw CFF header is not the expected CFF1 header")
    offset = cff[2]
    entries: list[bytes] = []
    for _ in range(5):
        offset, entries = read_cff_index(cff, offset)
    if len(entries) < 2:
        fail("raw CFF has no exploit charstring at index 1")
    return entries[1]

def extract_fat_from_mse(mse: bytes) -> tuple[bytes, dict[str, object]]:
    if len(mse) < 0x5000 + 16 * 40:
        fail("Firmware.MSE is too short for its section table")
    sections = []
    for index in range(16):
        entry = mse[0x5000 + index * 40:0x5000 + (index + 1) * 40]
        tag, name = entry[:4], entry[4:8]
        if tag == b"\0\0\0\0":
            continue
        dev_offset = struct.unpack_from("<I", entry, 12)[0]
        length = struct.unpack_from("<I", entry, 16)[0]
        if dev_offset + 0x1000 + length > len(mse):
            fail(f"MSE section {name!r} exceeds Firmware.MSE")
        sections.append((name, dev_offset, length))
    resource = [section for section in sections if section[0] == b"crsr"]
    if len(resource) != 1:
        fail(f"Firmware.MSE has {len(resource)} crsr sections")
    _, dev_offset, length = resource[0]
    img1 = mse[dev_offset + 0x1000:dev_offset + 0x1000 + length]
    if len(img1) < 0x400 or img1[:8] != b"87402.0\x03":
        fail("resource IMG1 is missing the Nano 7 patched header")
    body_length, data_length, cert_offset, cert_length = struct.unpack_from(
        "<4I", img1, 12
    )
    if 0x400 + body_length > len(img1):
        fail("resource IMG1 body exceeds the crsr section")
    fat = img1[0x400:0x400 + body_length]
    return fat, {
        "mse_sections": [name.decode("ascii") for name, _, _ in sections],
        "crsr_dev_offset": dev_offset,
        "crsr_length": length,
        "img1_body_length": body_length,
        "img1_data_length": data_length,
        "img1_cert_offset": cert_offset,
        "img1_cert_length": cert_length,
    }

def extract_font(fat_image: bytes, temp: Path) -> bytes:
    fat_path = temp / "resource-fat.img"
    fat_path.write_bytes(fat_image)
    fat = PyFatFS(str(fat_path), read_only=True)
    try:
        with fat.openbin("/Resources/Fonts/Helvetica.ttf", mode="rb") as handle:
            return handle.read()
    finally:
        fat.close()

def font_charstring(font: bytes) -> bytes:
    tt = TTFont(io.BytesIO(font), lazy=False)
    try:
        charstring = tt["CFF "].cff[0].CharStrings["space"]
        if charstring.bytecode is None:
            charstring.decompile()
        return bytes(charstring.bytecode)
    finally:
        tt.close()

def roundtrip_font(font: bytes) -> tuple[bytes, bytes]:
    tt = TTFont(io.BytesIO(font), lazy=False)
    charstring = tt["CFF "].cff[0].CharStrings["space"]
    if charstring.bytecode is None:
        charstring.decompile()
    before = bytes(charstring.bytecode)
    charstring.calcBounds = lambda _bounds: None
    output = io.BytesIO()
    tt.save(output, reorderTables=False)
    tt.close()
    return before, font_charstring(output.getvalue())


def sections(mse):
    result = {}
    for i in range(16):
        entry = mse[0x5000 + i * 40:0x5000 + (i + 1) * 40]
        if entry[:4] == bytes(4):
            continue
        offset, size = struct.unpack_from('<II', entry, 12)
        result[entry[4:8]] = mse[offset:offset + 0x1000 + size]
    return result


def resource_files(data, path):
    path.write_bytes(data)
    fat = PyFatFS(str(path), read_only=True)
    try:
        return {name: fat.readbytes(name) for name in fat.walk.files('/')}
    finally:
        fat.close()


def verify(ipsw, stock, cff, revision, resource_dir):
    with zipfile.ZipFile(ipsw) as z, zipfile.ZipFile(stock) as original:
        assert len(z.namelist()) == 3 and set(z.namelist()) == {
            'Firmware.MSE', 'N31.bootloader.rb3', 'manifest.plist'}, 'unexpected IPSW members'
        assert z.testzip() is None, 'IPSW CRC failure'
        manifest = plistlib.loads(z.read('manifest.plist'))['FirmwarePayload']
        assert manifest == {
            'FirmwareName': 'Firmware.MSE', 'BootloaderName': 'N31.bootloader.rb3',
            'BuildVersion': '39A10023', 'ProductVersion': '1.1.2',
            'UpdaterFamilyID': {'2012': 37, '2015': 39}[revision], 'FamilyID': 18,
        }, 'firmware metadata mismatch'
        assert z.read('N31.bootloader.rb3') == original.read('N31.bootloader.rb3'), 'bootloader changed'
        mse, before = z.read('Firmware.MSE'), original.read('Firmware.MSE')
    assert mse[:0x5280] == before[:0x5280], 'MSE header or section table changed'
    patched, stock_sections = sections(mse), sections(before)
    assert patched.keys() == stock_sections.keys(), 'MSE sections changed'
    for name in patched:
        if name != b'crsr':
            assert patched[name] == stock_sections[name], f'non-resource section {name!r} changed'
    stock_patched = before.replace(b'87402.0\x04', b'87402.0\x03', 1)
    fat, _ = extract_fat_from_mse(mse)
    stock_fat, _ = extract_fat_from_mse(stock_patched)
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        after = resource_files(fat, td / 'after.img')
        previous = resource_files(stock_fat, td / 'before.img')
    additions = {'/NanoApps/loader.hbapp'}
    assert set(after) == set(previous) | additions, 'unexpected resource filenames'
    for name in additions:
        assert after[name] == (resource_dir / name.lstrip('/')).read_bytes(), 'bundled runtime mismatch'
    changed = [name for name in previous if after[name] != previous[name]]
    assert changed == ['/Resources/Fonts/Helvetica.ttf'], f'unexpected resource changes: {changed}'
    program = raw_cff_charstring(cff.read_bytes())
    font = after[changed[0]]
    assert program[-1] == 0x0e and font_charstring(font) == program, 'packaged exploit differs'
    a, b = roundtrip_font(font)
    assert a == b == program, 'FontTools round trip changed the exploit'
    return {'ipsw_sha256': sha256(ipsw.read_bytes()), 'firmware': '1.1.2',
            'revision': revision, 'unchanged_os_and_bootloader': True,
            'changed_resources': changed, 'added_resources': sorted(additions), 'scsi_group': '98',
            'charstring_sha256': sha256(program), 'result': 'pass'}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ipsw', type=Path, required=True)
    p.add_argument('--stock', type=Path, required=True)
    p.add_argument('--cff', type=Path, required=True)
    p.add_argument('--revision', choices=('2012', '2015'), required=True)
    p.add_argument('--resource-dir', type=Path, required=True)
    a = p.parse_args()
    print(json.dumps(verify(a.ipsw, a.stock, a.cff, a.revision, a.resource_dir), indent=2))
