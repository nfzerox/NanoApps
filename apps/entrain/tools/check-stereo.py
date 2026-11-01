#!/usr/bin/env python3
"""Is there any binaural information in this file at all?

Worth asking before measuring anything. A binaural beat exists only as a
difference between the ears; if the two channels are identical, or nearly so,
there is nothing to measure and any "beat" a tool reports is noise. Some rips
are mono-summed, and a folder of them looks exactly like a folder of good ones
until you check.

Reports the side/mid energy ratio: 0 means the channels are identical.

    py check-stereo.py <audio>...
"""

import argparse
import subprocess
import sys

import numpy as np

SR = 8000


def decode(path, sr=SR, dur=300.0):
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-t", str(dur),
           "-ac", "2", "-ar", str(sr), "-f", "f32le", "-"]
    out = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(out, dtype="<f4").reshape(-1, 2).astype(np.float64)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio", nargs="+")
    ap.add_argument("--dur", type=float, default=300.0)
    args = ap.parse_args()

    print(f"{'side/mid':>9} {'dB':>7}  file")
    for path in args.audio:
        try:
            pcm = decode(path, dur=args.dur)
        except subprocess.CalledProcessError as exc:
            print(f"{'-':>9} {'-':>7}  {path} (decode failed)", file=sys.stderr)
            continue
        if pcm.size == 0:
            continue
        mid = (pcm[:, 0] + pcm[:, 1]) * 0.5
        side = (pcm[:, 0] - pcm[:, 1]) * 0.5
        rm = float(np.sqrt((mid ** 2).mean()))
        rs = float(np.sqrt((side ** 2).mean()))
        ratio = rs / rm if rm > 0 else 0.0
        db = 20 * np.log10(ratio) if ratio > 0 else -999
        print(f"{ratio:9.5f} {db:7.1f}  {path.split('/')[-1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
