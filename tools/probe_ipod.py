#!/usr/bin/env python3
"""Read-only Linux identity and SCSI-patch probe; never mount or write the iPod.

Uses libgpod's documented VPD XML page enumeration (page C0).
https://github.com/gtkpod/libgpod/blob/main/tools/ipod-scsi.c
"""
import argparse
import json
from pathlib import Path
import plistlib
import struct
import subprocess


def read_scsi(device, cdb, length):
    result = subprocess.run(
        ["sg_raw", "--cmdset=1", "--readonly", "--timeout=10",
         f"--request={length}", "--outfile=-", device,
         *(f"{b:02x}" for b in cdb)], capture_output=True, check=True)
    return result.stdout


def inquiry_page(device, page):
    data = read_scsi(device, [0x12, 1, page, 0, 252, 0], 252)
    if len(data) < 4 or data[1] != page:
        raise ValueError(f"invalid inquiry page {page:#x}")
    length = int.from_bytes(data[2:4], "big")
    if len(data) < 4 + length:
        raise ValueError(f"short inquiry page {page:#x}")
    return data[4:4 + length]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device")
    args = parser.parse_args()
    device = str(Path(args.device).resolve())
    sysdev = Path("/sys/class/block") / Path(device).name / "device"
    vendor = (sysdev / "vendor").read_text().strip()
    model = (sysdev / "model").read_text().strip()
    if vendor != "Apple" or model != "iPod":
        raise SystemExit("Refusing to probe a device other than an Apple iPod")
    report = {"device": device, "vendor": vendor, "model": model,
              "scsi_revision": (sysdev / "rev").read_text().strip()}
    try:
        pages = inquiry_page(device, 0xC0).split(b"\0", 1)[0]
        if not pages or len(pages) > 63 or any(p < 0xC1 for p in pages):
            raise ValueError("unexpected SysInfo page list")
        xml = b"".join(inquiry_page(device, page) for page in pages).rstrip(b"\0")
        info = plistlib.loads(xml)
        report["sysinfo"] = {
            k: v for k, v in info.items()
            if any(term in k.lower() for term in ("version", "model", "family", "build"))
            and isinstance(v, (str, int, bool, float))
        }
    except (OSError, ValueError, plistlib.InvalidFileException,
            subprocess.CalledProcessError) as error:
        report["sysinfo_error"] = str(error)
    report["memory_probes"] = []
    for address in (0x087CBA00, 0x0819C1D4):
        try:
            data = read_scsi(device, [0xC6, 0x98, 2, *address.to_bytes(4, "big")], 512)
            probe = {"address": hex(address), "received": len(data),
                     "full_length": len(data) == 512}
            if address == 0x087CBA00 and len(data) == 512:
                dispatch = struct.unpack_from("<I", data, 0x16C)[0]
                probe["home_dispatch"] = hex(dispatch)
                probe["matches_39A10023_stock_dispatch"] = dispatch == 0x082AE8DD
            elif len(data) < 32:
                probe["response_hex"] = data.hex()
        except subprocess.CalledProcessError as error:
            probe = {"address": hex(address), "error": error.stderr.decode(errors="replace")}
        report["memory_probes"].append(probe)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
