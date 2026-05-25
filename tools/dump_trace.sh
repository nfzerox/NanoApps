#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-/tmp/nanoapps-trace.bin}

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

: >"$out"
for i in $(seq 0 8); do
    addr=$(printf '0x%08x' $((0x09120000 + i * 0x200)))
    "$ROOT/tools/scsi_read.sh" "$addr" "$tmp" >/dev/null
    cat "$tmp" >>"$out"
done

python3 - "$out" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
magic, idx, count, _ = struct.unpack_from('<IIII', data, 0)
if magic != 0x48425452:
    print('no valid trace buffer at 0x09120000')
    raise SystemExit(1)
print(f'HBTR idx={idx} count={count}')
n = min(count, 256)
for i in range(n):
    off = 16 + i * 16
    tag = data[off:off+8].rstrip(b' \0').decode('ascii', 'replace')
    v1, v2 = struct.unpack_from('<II', data, off + 8)
    print(f'{i:03d} {tag:<8} 0x{v1:08x} 0x{v2:08x}')
PY

