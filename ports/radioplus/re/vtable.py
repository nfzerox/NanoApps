#!/usr/bin/env python3
"""Find C++ vtables in RetailOS by walking the Itanium ABI backwards from RTTI.

The tuner classes are C++ with RTTI, which is a gift: the ABI lays the pieces out
in a fixed shape, so a vtable can be reached from nothing but its class name.

    type_info      [ vptr ][ __name -> "N3ISL8TRFTunerE" ][ ...base info... ]
    vtable         [ offset_to_top ][ type_info* ][ vfn0 ][ vfn1 ] ...

So: find the name string, find the word pointing at it (that is type_info+4),
find the word pointing at type_info (that is vtable+4), and the virtual
functions start immediately after. No guessing, and every step is checkable -
a real vtable is a run of plausible code pointers, and anything else means the
chain went wrong.
"""
import struct
import sys

IMG = "/mnt/c/src/ipod/artifacts/firmware/decrypted-112/osos.live.bin"
BASE = 0x08000000

data = open(IMG, "rb").read()
END = BASE + len(data)


def va(off):
    return BASE + off


def off(v):
    return v - BASE


def word(o):
    if o < 0 or o + 4 > len(data):
        return None
    return struct.unpack_from("<I", data, o)[0]


def find_all(value, aligned=True):
    """Every offset holding this 32-bit little-endian value."""
    needle = struct.pack("<I", value)
    out, i = [], 0
    while True:
        i = data.find(needle, i)
        if i < 0:
            break
        if not aligned or i % 4 == 0:
            out.append(i)
        i += 1
    return out


def find_string(s):
    """Offset of a NUL-terminated string."""
    needle = s.encode() + b"\0"
    i = data.find(needle)
    return i if i >= 0 else None


def looks_like_code(v):
    """A plausible ARM/Thumb function pointer into this image."""
    return v is not None and BASE <= (v & ~1) < END


TARGETS = [
    "N3ISL8TRFTunerE",
    "N3ISL11IPodRFTunerE",
    "N3ISL12TRFTuner_IAPE",
    "N3ISL13TRFTuner_RTXCE",
    "N3ISL21IPodRFTunerPresetListE",
    "18TRFTunerPresetList",
    "16CIapLingoRFTuner",
    "24TSilverRadioTunerBarView",
]

SLOTS = int(sys.argv[1]) if len(sys.argv) > 1 else 20

for name in TARGETS:
    print("=" * 74)
    print(name)

    so = find_string(name)
    if so is None:
        print("  name string not found")
        continue
    print("  name string      @ 0x%08x" % va(so))

    name_refs = find_all(va(so))
    if not name_refs:
        print("  nothing points at the name - not RTTI, or a different ABI")
        continue

    for nr in name_refs:
        ti = nr - 4                       # __name sits at type_info+4
        print("  type_info        @ 0x%08x  (vptr 0x%08x)" % (va(ti), word(ti) or 0))

        ti_refs = find_all(va(ti))
        vt_refs = [r for r in ti_refs if r != nr]
        if not vt_refs:
            print("    no vtable references this type_info")
            continue

        for vr in vt_refs:
            vtable = vr - 4               # type_info* sits at vtable+4
            top = word(vtable)
            first = vr + 4

            # A vtable is a run of code pointers. Anything else and the chain
            # landed on a base-class descriptor or unrelated data.
            # Dump raw rather than stopping at the first odd word. A zero is a
            # pure virtual, which is worth seeing rather than mistaking for the
            # end of the table.
            raw = [word(first + 4 * i) for i in range(SLOTS)]
            fns = []
            for w in raw:
                if not looks_like_code(w):
                    break
                fns.append(w)

            kind = "vtable" if len(fns) >= 3 else "ref (not a vtable)"
            print("    %-18s @ 0x%08x  offset_to_top=0x%08x  %d slots" %
                  (kind, va(vtable), top if top is not None else 0, len(fns)))
            if len(fns) >= 3:
                for i, w in enumerate(raw):
                    if w is None:
                        break
                    if looks_like_code(w):
                        tag = "thumb" if w & 1 else "arm"
                    elif w == 0:
                        tag = "PURE VIRTUAL / end"
                    else:
                        tag = "not code"
                    print("      [%2d] 0x%08x  %s" % (i, w, tag))
