#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

if [ $# -ne 1 ]; then
    echo "usage: $0 <app_dir_name>" >&2
    echo "example: $0 paint" >&2
    exit 1
fi

app=$1
NANOAPPS_RELEASE=1 make -C "$ROOT/apps/$app" clean all
"$ROOT/tools/eject.sh" >/dev/null
sleep 3
"$ROOT/tools/run_payload.sh" 0x0867f8e4 "$ROOT/apps/$app/build/$app.bin"

