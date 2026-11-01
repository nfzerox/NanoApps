#!/usr/bin/env python3
"""Build the small disk bootstrap and its donor-derived exact-bound cave stage."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

from wrap_resident_slot import validate_hrl1


ROOT = Path(__file__).resolve().parents[1]


def run(*args: str) -> None:
    subprocess.run(args, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "system/disk_bootstrap/build/bundle")
    args = parser.parse_args()

    make_python = f"PYTHON={sys.executable}"
    run("make", "-s", "-C", str(ROOT / "system/disk_bootstrap"),
        make_python)

    bootstrap = ROOT / "system/disk_bootstrap/build/disk_bootstrap.hbapp"
    validate_hrl1(bootstrap.read_bytes())
    stage_dir = ROOT / "system/deferred_untether/build"
    run(sys.executable, str(ROOT / "tools/build_deferred_stage.py"),
        "--resident", str(bootstrap), "--output", str(stage_dir), "--limit", "0x3a0")
    stage_meta = json.loads((stage_dir / "deferred_stage.json").read_text())
    if (stage_meta["primary_size"] > 0x3A0 or stage_meta["report_offset"] != 0x3A0 or
            stage_meta["report_size"] != 0x200 or stage_meta["matcher_offset"] != 0x5A0 or
            stage_meta["matcher_size"] > 0x60 or stage_meta["published_size"] != 0x600):
        raise SystemExit("deferred split-stage layout violates cave boundaries")
    resources = args.output / "resources/NanoApps"
    if resources.exists():
        shutil.rmtree(resources)
    resources.mkdir(parents=True)
    shutil.copyfile(bootstrap, resources / "loader.hbapp")
    firmware = args.output / "firmware"
    firmware.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(stage_dir / "deferred_stage.bin", firmware / "deferred_stage.bin")
    # Remove obsolete generated output from previous build layouts.
    for obsolete in (args.output / "Apps/Startup", args.output / "Apps/resident.hbapp",
                     args.output / "manifest.json",
                     firmware / "deferred_stage.elf", firmware / "deferred_stage.json"):
        if obsolete.is_dir():
            shutil.rmtree(obsolete)
        else:
            obsolete.unlink(missing_ok=True)
    print(f"Disk-only firmware bootstrap: {args.output}")


if __name__ == "__main__":
    main()
