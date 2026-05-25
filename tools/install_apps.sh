#!/usr/bin/env bash
# Build all release apps and install them to /Apps on the iPod volume.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
: "${IPOD_PARTITION:=/dev/sda2}"
: "${IPOD_MOUNT:=/mnt/nanoapps-ipod}"

NANOAPPS_RELEASE=1 make -C "$ROOT/apps" bundles

sudo mkdir -p "$IPOD_MOUNT"
sudo umount "$IPOD_MOUNT" 2>/dev/null || true
sudo mount -o rw,flush "$IPOD_PARTITION" "$IPOD_MOUNT"
sudo mkdir -p "$IPOD_MOUNT/Apps"
sudo cp -R "$ROOT/apps/out/Apps/." "$IPOD_MOUNT/Apps/"
sudo sync
sudo umount "$IPOD_MOUNT"

echo "Installed apps to $IPOD_PARTITION:/Apps."
echo "Run ./tools/eject.sh before launching apps or using filesystem APIs."

