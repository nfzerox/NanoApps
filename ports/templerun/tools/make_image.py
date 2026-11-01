#!/usr/bin/env python3
"""Write the engine's data image and tunables for the translated engine to load.

engine.img is the original executable's string, constant and data sections (no code), at the
addresses port/src/rt.h expects; config.txt is ImangiConfigDefaults.plist as "key value"
lines. Both come from the original app and stay untracked.

usage: make_image.py <out directory>
"""
import plistlib
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LO, HI = 0xba000, 0xea000

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
data = (ROOT / 'build/TempleRun-armv7').read_bytes()
image = bytearray(HI - LO)
pos = 28
for _ in range(struct.unpack_from('<I', data, 16)[0]):
    cmd, size = struct.unpack_from('<II', data, pos)
    if cmd == 1 and data[pos + 8:pos + 18] != b'__LINKEDIT':
        va, vs, off, length = struct.unpack_from('<4I', data, pos + 24)
        lo, hi = max(va, LO), min(va + length, HI)
        if lo < hi:
            image[lo - LO:hi - LO] = data[off + lo - va:off + hi - va]
    pos += size
(out / 'engine.img').write_bytes(image)
config = plistlib.loads((ROOT / 'original/v1.0/Payload/TempleRun.app/ImangiConfigDefaults.plist').read_bytes())
lines = []
for key, value in sorted(config.items()):
    if isinstance(value, bool):
        lines.append('%s %d' % (key, value))
    elif isinstance(value, (int, float)):
        lines.append('%s %r' % (key, value))
(out / 'config.txt').write_text('\n'.join(lines) + '\n')
print('engine.img %d bytes, config.txt %d values' % (len(image), len(lines)))
