#!/usr/bin/env python3
"""Generate BigLoad's payload: SIZE pseudo-random bytes plus a header with their
FNV-1a hash, so the app can prove the loader copied every byte intact."""
import sys
from pathlib import Path

SIZE = 3 * 1024 * 1024
out = Path(sys.argv[1] if len(sys.argv) > 1 else "build")
out.mkdir(parents=True, exist_ok=True)

data = bytearray(SIZE)
x = 0x2545F491
for i in range(0, SIZE, 4):
    x ^= (x << 13) & 0xFFFFFFFF
    x ^= x >> 17
    x ^= (x << 5) & 0xFFFFFFFF
    data[i:i + 4] = x.to_bytes(4, "little")

h = 0x811C9DC5
for b in data:
    h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF

(out / "blob.bin").write_bytes(data)
(out / "blob_expected.h").write_text(
    f"#define BLOB_SIZE {SIZE}u\n#define BLOB_FNV 0x{h:08x}u\n")
print(f"blob.bin {SIZE} bytes fnv1a32=0x{h:08x}")
