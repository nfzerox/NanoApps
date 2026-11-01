#!/usr/bin/env python3
"""Resolve Mach-O literal pools, Objective-C selectors and ivar offsets in local output."""
import argparse
import json
import re
import struct
import subprocess
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('binary', type=Path)
ap.add_argument('input', type=Path)
ap.add_argument('output', type=Path)
args = ap.parse_args()
data = args.binary.read_bytes()
assert data[:4] == b'\xce\xfa\xed\xfe'
ncmds = struct.unpack_from('<I', data, 16)[0]
segments, sections = [], []
pos = 28
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', data, pos)
    if cmd == 1:
        va, vs, fo, fs = struct.unpack_from('<4I', data, pos + 24)
        segments.append((va, fo, fs))
        ns = struct.unpack_from('<I', data, pos + 48)[0]
        for i in range(ns):
            sec = pos + 56 + i * 68
            name = data[sec:sec+16].split(b'\0')[0].decode()
            address, length = struct.unpack_from('<II', data, sec + 32)
            sections.append((name, address, length))
    pos += size

def offset(address):
    for va, fo, fs in segments:
        if va <= address < va + fs:
            return address - va + fo
    return None

def word(address):
    p = offset(address)
    return struct.unpack_from('<I', data, p)[0] if p is not None and p + 4 <= len(data) else None

def string(address):
    p = offset(address)
    if p is None:
        return None
    end = data.find(b'\0', p, p + 400)
    raw = data[p:end] if end >= 0 else b''
    return raw.decode('ascii') if raw and all(32 <= b < 127 for b in raw) else None

symbols = {}
for line in subprocess.check_output(['nm', str(args.binary)], text=True).splitlines():
    m = re.match(r'([0-9a-f]+) [^Uu] (.+)', line)
    if m:
        symbols[int(m[1], 16)] = m[2]

def pointer(address):
    if address in symbols:
        name = symbols[address]
        if name.startswith('_OBJC_IVAR_$_'):
            return '%s/*%s*/' % (hex(word(address)), name.split('$_')[1])
        return '&' + name.replace('*/', '')
    for name, va, size in sections:
        if va <= address < va + size:
            if name == '__objc_selrefs':
                return 'SEL_' + (string(word(address)) or hex(address)).replace(':', '_')
            if name == '__cfstring':
                return '@' + json.dumps(string(word(address + 8)))
    return None

def replace(m):
    deref, address = m[1], int(m[2], 16)
    value = word(address)
    if value is None:
        return m[0]
    desc = pointer(value)
    if deref and desc:
        return desc
    text = desc or (json.dumps(string(value)) if string(value) else None)
    if not text:
        f = struct.unpack('<f', struct.pack('<I', value))[0]
        text = '%.9gf' % f if 1e-6 <= abs(f) <= 1e7 else hex(value)
    return m[0] + '/*' + text.replace('*/', '* /') + '*/'

args.output.mkdir(parents=True, exist_ok=True)
for p in args.input.glob('*.c'):
    src = p.read_text()
    def double_pair(m):
        high, low = word(int(m[1], 16)), word(int(m[2], 16))
        return repr(struct.unpack('<d', struct.pack('<II', low, high))[0])
    src = re.sub(r'\(double\)CONCAT44\(DAT_([0-9a-f]{8}),\s*DAT_([0-9a-f]{8})\)', double_pair, src)
    def double_high(m):
        return repr(struct.unpack('<d', struct.pack('<II', 0, word(int(m[1], 16))))[0])
    src = re.sub(r'\(double\)\(\(ulonglong\)DAT_([0-9a-f]{8}) << 0x20\)', double_high, src)
    (args.output / p.name).write_text(re.sub(r'(\*?)DAT_([0-9a-f]{8})', replace, src))
