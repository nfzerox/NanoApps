#!/usr/bin/env bash
# Build, run, wait briefly, then dump the crash trace buffer.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

if [ $# -ne 1 ]; then
    echo "usage: $0 <app_dir_name>" >&2
    exit 1
fi

"$ROOT/tools/run_app.sh" "$1" || true
echo "Waiting for reboot/SCSI recovery..."
sleep 12
"$ROOT/tools/dump_trace.sh"

