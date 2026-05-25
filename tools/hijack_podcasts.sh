#!/usr/bin/env bash
# Install the in-RAM Podcasts -> Homebrew launcher hook for this boot.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
: "${IPOD_PARTITION:=/dev/sda2}"
: "${IPOD_MOUNT:=/mnt/nanoapps-ipod}"

NANOAPPS_RELEASE=1 make -C "$ROOT/apps/_hijack_stub" all
NANOAPPS_RELEASE=1 make -C "$ROOT/apps/_hijack_install" all

sudo mkdir -p "$IPOD_MOUNT"
sudo umount "$IPOD_MOUNT" 2>/dev/null || true
sudo mount -o rw,flush "$IPOD_PARTITION" "$IPOD_MOUNT"
sudo mkdir -p "$IPOD_MOUNT/Apps"
sudo cp "$ROOT/apps/_hijack_stub/build/_hijack_stub.bin" "$IPOD_MOUNT/Apps/.hijack_stub"
sudo sync
sudo umount "$IPOD_MOUNT"

"$ROOT/tools/eject.sh" >/dev/null
sleep 3
"$ROOT/tools/run_payload.sh" 0x0867f8e4 "$ROOT/apps/_hijack_install/build/_hijack_install.bin"
echo "Tap Podcasts on the iPod home screen to open Homebrew."

