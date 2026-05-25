#!/usr/bin/env bash
# Upload a binary to RAM and call it as Thumb code.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

if [ $# -ne 2 ]; then
    echo "usage: $0 <addr_hex> <payload.bin>" >&2
    exit 1
fi

addr=$1
payload=$2
"$ROOT/tools/scsi_write.sh" "$addr" "$payload"
thumb=$(printf '0x%08x' $(( addr | 1 )))
"$ROOT/tools/scsi_exec.sh" "$thumb"

