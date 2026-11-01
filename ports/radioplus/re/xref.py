#!/usr/bin/env python3
"""Find what references a virtual address, and show the code around it.

Naming a vtable slot needs a caller, and the cheapest callers to find are the
ones that log. RetailOS formats its tuner activity with strings that name the
operation outright - "[Tune] ... New frequency is %d kHz", "Our RSSI is %d" -
so whatever loads those literals is the function doing that operation, and the
virtual calls it makes are the slots worth naming.

ARM reaches a literal through a PC-relative load from a nearby pool, so the
pool entry holds the string address verbatim and a plain word search finds it.
From there the referencing function is a short walk backwards.

Usage:  xref.py 0x082c5dd0 [more VAs...]
"""
import struct
import subprocess
import sys
import os

IMG = "/mnt/c/src/ipod/artifacts/firmware/decrypted-112/osos.live.bin"
BASE = 0x08000000
HERE = os.path.dirname(os.path.abspath(__file__))

data = open(IMG, "rb").read()


def find_words(value):
    needle = struct.pack("<I", value)
    out, i = [], 0
    while True:
        i = data.find(needle, i)
        if i < 0:
            break
        if i % 4 == 0:
            out.append(BASE + i)
        i += 4 - (i % 4) if i % 4 else 1
    return out


def cstr(va, limit=60):
    o = va - BASE
    end = data.find(b"\0", o)
    if end < 0 or end - o > limit:
        end = o + limit
    return data[o:end].decode("latin-1")


def dis(va, n, mode="thumb"):
    return subprocess.run([os.path.join(HERE, "dis.sh"), str(va), str(n), mode],
                          capture_output=True, text=True).stdout


for arg in sys.argv[1:]:
    va = int(arg, 16)
    print("=" * 74)
    print("0x%08x  %r" % (va, cstr(va)))

    pools = find_words(va)
    if not pools:
        print("  no literal pool entry references this")
        continue

    for p in pools:
        print("  -- pool entry @ 0x%08x; code before it:" % p)
        # The pool sits after the function that uses it, so walk back a little
        # and disassemble up to the pool itself.
        start = p - 0x60
        out = dis(start, 0x60)
        for line in out.splitlines()[1:]:
            print("     " + line.rstrip())
        print()
