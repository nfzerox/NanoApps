#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/_scsi_lib.sh"

if [ $# -ne 2 ]; then
    echo "usage: $0 <addr_hex> <out_file>" >&2
    exit 1
fi

scsi_read_chunk "$1" "$2"
echo "read 0x200 bytes from $1 -> $2"

