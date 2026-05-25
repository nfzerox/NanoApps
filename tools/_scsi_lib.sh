#!/usr/bin/env bash
# Common SCSI primitives for the ipod_sun_untethered c6 96 handler.
set -euo pipefail

: "${SCSI_DEV:=/dev/sda}"
SCSI_CHUNK=$((0x200))

addr_to_bytes() {
    local addr=$(( $1 ))
    printf '%02x %02x %02x %02x' \
        $(( (addr >> 24) & 0xff )) \
        $(( (addr >> 16) & 0xff )) \
        $(( (addr >>  8) & 0xff )) \
        $((  addr        & 0xff ))
}

scsi_read_chunk() {
    local addr=$1 out=$2 ab
    ab=$(addr_to_bytes "$addr")
    # shellcheck disable=SC2086
    sudo sg_raw -o "$out" -r "$SCSI_CHUNK" -v "$SCSI_DEV" \
        c6 96 02 $ab >/dev/null 2>&1
}

scsi_write_chunk() {
    local addr=$1 in=$2 ab sz
    sz=$(stat -c %s "$in")
    if [ "$sz" -ne "$SCSI_CHUNK" ]; then
        echo "scsi_write_chunk: $in is $sz bytes; expected $SCSI_CHUNK" >&2
        return 1
    fi
    ab=$(addr_to_bytes "$addr")
    # shellcheck disable=SC2086
    sudo sg_raw -s "$SCSI_CHUNK" -i "$in" -v "$SCSI_DEV" \
        c6 96 01 $ab >/dev/null 2>&1
}

scsi_exec() {
    local addr=$1 ab
    ab=$(addr_to_bytes "$addr")
    # shellcheck disable=SC2086
    sudo sg_raw -o /dev/null -r 512 -v "$SCSI_DEV" \
        c6 96 03 $ab >/dev/null 2>&1
}

