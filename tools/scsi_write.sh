#!/usr/bin/env bash
# Upload a file to iPod RAM through the SCSI command channel.
set -euo pipefail
source "$(dirname "$0")/_scsi_lib.sh"

if [ $# -ne 2 ]; then
    echo "usage: $0 <addr_hex> <file>" >&2
    exit 1
fi

addr=$(( $1 ))
in=$2
sz=$(stat -c %s "$in")
pad_sz=$(( (sz + SCSI_CHUNK - 1) / SCSI_CHUNK * SCSI_CHUNK ))
padded=$(mktemp)
chunk=$(mktemp)
trap 'rm -f "$padded" "$chunk"' EXIT

cp "$in" "$padded"
if [ "$pad_sz" -gt "$sz" ]; then
    dd if=/dev/zero bs=1 count=$(( pad_sz - sz )) >>"$padded" 2>/dev/null
fi

n=$(( pad_sz / SCSI_CHUNK ))
for i in $(seq 0 $((n - 1))); do
    dd if="$padded" of="$chunk" bs="$SCSI_CHUNK" count=1 skip="$i" 2>/dev/null
    cur=$(( addr + i * SCSI_CHUNK ))
    printf '[%d/%d] write 0x%08x\n' $((i + 1)) "$n" "$cur" >&2
    scsi_write_chunk "$cur" "$chunk"
done

echo "uploaded $sz bytes to $1"

