#!/usr/bin/env python3
"""Find non-ASCII characters inside C string literals.

LVGL's bundled Montserrat carries ASCII plus a small symbol set. A character
outside that renders as an empty box on the device, which is worse than an
ugly ASCII substitute — so anything this reports needs a decision.
"""

import re
import sys

# Codepoints LVGL's Montserrat builds do include.
KNOWN_OK = {0x2022}   # bullet

PAT = re.compile(r'"((?:[^"\\]|\\.)*)"')

bad_found = False
for path in sys.argv[1:]:
    src = open(path, encoding="utf-8").read()
    for m in PAT.finditer(src):
        text = m.group(1)
        odd = {c for c in text if ord(c) > 127 and ord(c) not in KNOWN_OK}
        if odd:
            line = src[: m.start()].count("\n") + 1
            names = ", ".join("U+%04X %r" % (ord(c), c) for c in sorted(odd))
            print("%s:%d  %s" % (path, line, names))
            print("    %s" % text[:70])
            bad_found = True

if bad_found:
    print("^ these render as empty boxes on the device font")
    sys.exit(1)
print("glyphs: clean")
