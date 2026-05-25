#!/usr/bin/env bash
# Leave USB Connected mode so the iPod mounts its filesystem.
set -euo pipefail
: "${SCSI_DEV:=/dev/sda}"

sudo eject "$SCSI_DEV" 2>/dev/null || true
echo "Requested eject for $SCSI_DEV. Wait for the iPod home screen before using filesystem APIs."

