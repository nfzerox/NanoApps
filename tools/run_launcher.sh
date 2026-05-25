#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

NANOAPPS_RELEASE=1 make -C "$ROOT/apps/launcher" >/dev/null
"$ROOT/tools/eject.sh" >/dev/null
sleep 3
"$ROOT/tools/run_payload.sh" 0x0867f8e4 "$ROOT/apps/launcher/build/launcher.bin"

