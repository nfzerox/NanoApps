#!/usr/bin/env python3
"""Print decompiled functions by (substring of) name, tidied for reading.

Local declarations, decompiler warnings and the exception-frame bookkeeping are dropped,
and the position-independent address sums the compiler emits (DAT_x + 0xNNNN) are
resolved to the string, symbol or constant they point at.

usage: show.py <Class> <name substring>... [--raw]
"""
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
BIN = os.path.join(ROOT, 'build/TempleRun-armv7')
data = open(BIN, 'rb').read()
segs = []
sects = {}
pos = 28
for _ in range(struct.unpack_from('<I', data, 16)[0]):
    cmd, size = struct.unpack_from('<II', data, pos)
    if cmd == 1:
        va, vs, off, fs, _, _, nsects = struct.unpack_from('<7I', data, pos + 24)
        segs.append((va, off, fs))
        for s in range(nsects):
            b = pos + 56 + 68 * s
            sects[data[b:b + 16].rstrip(b'\0').decode()] = struct.unpack_from('<II', data, b + 32)
    pos += size


def off(va):
    for s, o, l in segs:
        if s <= va < s + l:
            return o + va - s
    return None


def u32(va):
    o = off(va)
    return struct.unpack_from('<I', data, o)[0] if o is not None else None


def f32(va):
    o = off(va)
    return struct.unpack_from('<f', data, o)[0] if o is not None else None


def cstr(va):
    o = off(va)
    if o is None:
        return None
    end = data.find(b'\0', o)
    s = data[o:end]
    if 0 < len(s) < 200 and all(32 <= c < 127 or c in (9, 10) for c in s):
        return s.decode()
    return None


def in_sect(name, va):
    if name not in sects:
        return False
    a, s = sects[name]
    return a <= va < a + s


_syms = None


def symbols():
    global _syms
    if _syms is None:
        _syms = {}
        out = subprocess.run(['nm', '-n', BIN], capture_output=True, text=True).stdout
        names = []
        for line in out.splitlines():
            p = line.split(None, 2)
            if len(p) == 3 and p[1] not in 'Uu':
                names.append((int(p[0], 16), p[2]))
        dem = subprocess.run(['c++filt'], input='\n'.join(n for _, n in names), capture_output=True, text=True).stdout.split('\n')
        for (a, _), d in zip(names, dem):
            _syms.setdefault(a, d)
    return _syms


def describe(va, depth=0):
    """What lives at this address."""
    if in_sect('__cstring', va):
        s = cstr(va)
        if s is not None:
            return '"%s"' % s.replace('\n', '\\n')
    if in_sect('__cfstring', va):
        s = cstr(u32(va + 8) or 0)
        return '@"%s"' % s if s is not None else 'cfstring'
    name = symbols().get(va)
    if name:
        return name
    if depth == 0 and (in_sect('__nl_symbol_ptr', va) or in_sect('__const', va) or in_sect('__data', va)):
        target = u32(va)
        if target:
            inner = describe(target, 1)
            if inner:
                return '&' + inner if not inner.startswith(('"', '@')) else 'ptr to ' + inner
    return None


PIC = re.compile(r'DAT_([0-9a-f]{8}) \+ (0x[0-9a-f]+)')
DAT = re.compile(r'\bDAT_([0-9a-f]{8})\b(?! \+ 0x)')
DECL = re.compile(r'^  (undefined\d?|int|uint|float|double|bool|byte|char|short|ushort|longlong|ulonglong|code|ID|SEL|'
                  r'size_t|long|ulong|void|[A-Za-z_]\w*)\s*\**\s*\w+(\s*\[\d+\])*;$')
NOISE = re.compile(r'^\s*(/\* WARNING|local_94 = (0x[0-9a-f]+|\d+);|local_(7c|80|74|78|70) = )')


def tidy(line):
    def pic(m):
        base = u32(int(m.group(1), 16))
        if base is None:
            return m.group(0)
        target = (base + int(m.group(2), 16)) & 0xffffffff
        d = describe(target)
        return '%s{%s}' % ('', d) if d else '0x%x' % target
    line = PIC.sub(pic, line)

    def dat(m):
        a = int(m.group(1), 16)
        v = u32(a)
        if v is None or not (in_sect('__text', a)):
            return m.group(0)
        f = f32(a)
        if v == 0 or (1e-6 < abs(f) < 1e7):
            return '%s(%g)' % (m.group(0), f)
        return m.group(0)
    return DAT.sub(dat, line)


STR_CTOR = re.compile(r'__ZNSsC1EPKcRKSaIcE\((\w+),(.+),(\w+)\);')
DROP = re.compile(r'__ZNSaIcE[CD]1Ev\(|__ZNSsD1Ev\(|__Unwind_SjLj_|__ZNSt8ios_base4Init')
MANGLED = re.compile(r'\b__Z\w+')
_dem = {}


def demangle(names):
    todo = [n for n in names if n not in _dem]
    if todo:
        out = subprocess.run(['c++filt'], input='\n'.join(todo), capture_output=True, text=True).stdout.split('\n')
        for n, d in zip(todo, out):
            d = re.sub(r'std::basic_string<char, std::char_traits<char>, std::allocator<char> >', 'string', d)
            _dem[n] = re.sub(r'\(.*\)( const)?$', '', d) if '(' in d else d


def simplify(lines):
    """Fold string temporaries into their uses, drop allocator noise, demangle call names."""
    strings, out = {}, []
    for line in lines:
        m = STR_CTOR.search(line)
        if m:
            strings[m.group(1)] = m.group(2).strip()
            continue
        if DROP.search(line):
            continue
        for tmp, lit in strings.items():
            if tmp in line:
                line = re.sub(r'\b%s\b' % tmp, lit, line)
        out.append(line)
    demangle(set(MANGLED.findall('\n'.join(out))))
    return [MANGLED.sub(lambda m: _dem.get(m.group(0), m.group(0)), l) for l in out]


if __name__ == '__main__':
    raw = '--raw' in sys.argv
    args = [a for a in sys.argv[1:] if a != '--raw']
    cls, pats = args[0], args[1:]
    text = open(os.path.join(ROOT, 'decomp/cls/%s.c' % cls)).read()
    for part in re.split(r'(?=/\* ==== [0-9a-f]{8} )', text):
        head = part.split('\n', 1)[0]
        if not any(p in head for p in pats):
            continue
        lines = []
        for line in part.split('\n'):
            if not raw and (DECL.match(line) or NOISE.match(line) or not line.strip()):
                continue
            lines.append(tidy(line))
        for line in (lines if raw else simplify(lines)):
            print(line[:220])
