#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/_scsi_lib.sh"

if [ $# -ne 1 ]; then
    echo "usage: $0 <addr_hex>" >&2
    echo "for Thumb code, pass addr|1, e.g. 0x0867f8e5" >&2
    exit 1
fi

scsi_exec "$1"
echo "exec returned for $1"

