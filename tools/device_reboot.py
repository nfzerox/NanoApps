"""Restart NanoApps firmware 1.1.2 through C6/98, then verify a fresh boot."""
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import time
from runtime_compat import ABI

REPORT = 0x086c9b84
SCRATCH = 0x180
HANDLER_SHA256 = '59300a5daa7d011b35482d39a0ca8da310e652947a7c56569c64f906ef42df3e'

# Thumb: cpsid if; mov.w r0,#0x3c800000; mov.w r1,#0x100000;
# str r1,[r0]; dsb sy; b .
# S5L watchdog reset (same register operation as Rockbox system_reboot).
# Verified on the connected 2015 nano running 1.1.2. No NAND writes.
# The unused tail of the 512-byte boot report is data, never executable before
# this one-shot call; the firmware reload clears it on reset. It ends before
# the action matcher at CAVE+0x5a0, and cannot overlap its code.
PAYLOAD = bytes.fromhex('73b64ff072504ff480110160bff34f8ffee7')


def restart(tr, timeout=90, expected_version=None):
    tr.prepare_restart()  # Flush and unmount before any device RAM writes.
    if hashlib.sha256(tr.peek(0x0819c1d4, 140)).hexdigest() != HANDLER_SHA256:
        raise RuntimeError('restart requires the verified 1.1.2 C6/98 handler')
    report = bytearray(tr.peek(REPORT, 512))
    if struct.unpack_from('<I', report, 0x2c)[0] != 0x32505452:
        raise RuntimeError('restart requires the NanoApps boot reporter')
    if any(report[SCRATCH:]):
        raise RuntimeError('restart scratch is occupied; restart manually once')
    report[SCRATCH:SCRATCH + len(PAYLOAD)] = PAYLOAD
    with tempfile.TemporaryDirectory(prefix='nano-reboot-') as td:
        path = Path(td) / 'report.bin'
        path.write_bytes(report)
        tr.scsi_write_file(str(path), REPORT)
    if tr.peek(REPORT + SCRATCH, len(PAYLOAD)) != PAYLOAD:
        raise RuntimeError('restart payload readback failed; reset was not sent')
    print('Restarting the nano…', flush=True)
    # Reset drops USB before a successful SCSI status can return. Send once;
    # only a cleared scratch and a valid new boot report establish success.
    try:
        tr.scsi_exec((REPORT + SCRATCH) | 1)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        pass
    deadline = time.monotonic() + timeout
    ejected = False
    while time.monotonic() < deadline:
        time.sleep(1)
        try:
            if not ejected:
                if not tr.reconnect_after_restart():
                    continue
                # The disk resident cannot load while USB owns the volume.
                # Eject once, then inspect without reloading media on macOS.
                tr.eject()
                ejected = True
            fresh = tr.peek_ejected(REPORT, 512)
        except (RuntimeError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
            continue
        if (len(fresh) == 512 and not any(fresh[SCRATCH:]) and
                struct.unpack_from('<I', fresh, 0x2c)[0] == 0x32505452 and
                struct.unpack_from('<2I', fresh, 0x50) == (1, 1) and
                struct.unpack_from('<I', fresh, 0x78)[0] == 3):
            if expected_version is not None and (
                    struct.unpack_from('<I', fresh, 0x15c)[0] != expected_version or
                    struct.unpack_from('<I', fresh, 0x160)[0] != ABI):
                raise RuntimeError('the iPod returned with a different resident version; '
                    'apply the matching NanoApps IPSW, or restart with Home + power')
            tr.finish_restart()
            count = struct.unpack_from('<I', fresh, 0x58)[0]
            print(f'Nano restarted; {count} apps registered automatically.')
            return fresh
    raise RuntimeError('files are installed, but a fresh NanoApps boot was not '
                       'verified; hold Home + power to restart manually')
