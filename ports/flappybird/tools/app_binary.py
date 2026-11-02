"""Inspected Flappy Bird 1.2 executable and a verified archive.org packaging.

The archive copy is accepted both as a universal ARMv7/ARMv7s file and as its
thin ARMv7 slice. Check gameplay against the original with compare_original.py
when adding a build; a version label alone does not establish compatibility.
"""
import hashlib
import struct

UNIVERSAL_SHA256 = 'b323b26eb21d89a28907db33933dbe4b0f758a769ebd4bf2958f15fd7b18f378'
ARCHIVE_SHA256 = '3f06221928906958dcba87f4099f22405916f0b41d8d365f1671a2650a770817'
THIN_SHA256 = 'cb9aa46a7db12312a1d995c2e21317a682655bee167b5db035ef51312da856ac'


def thin_armv7(path):
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest == THIN_SHA256:
        return data
    if digest not in (UNIVERSAL_SHA256, ARCHIVE_SHA256):
        raise ValueError('Use a supported Flappy Bird 1.2 executable; other builds need analysis')
    for i in range(struct.unpack_from('>I', data, 4)[0]):
        cpu, sub, off, size, _ = struct.unpack_from('>5I', data, 8 + 20 * i)
        if cpu == 12 and sub == 9:
            return data[off:off + size]
    raise ValueError('no ARMv7 slice')
