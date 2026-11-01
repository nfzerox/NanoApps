#!/usr/bin/env python3
"""Where does a recording put its carriers?

Run before trusting a fixed band list. analyse-layers.py measures bands that
are given to it, so a band list derived from one recording will quietly miss a
carrier family that only appears in another - and missing it is not a small
error, it is a whole layer absent from the port.

This takes the strongest few peaks per window across a wide range and
histograms them by frequency, weighted by amplitude, so the carrier families
show up as spikes and can be read off rather than guessed.

    py survey-carriers.py <audio>... [--lo 40] [--hi 520]
"""

import argparse
import subprocess
import sys

import numpy as np

SR = 4000


def decode(path, sr=SR):
    cmd = ["ffmpeg", "-v", "error", "-i", path,
           "-ac", "2", "-ar", str(sr), "-f", "f32le", "-"]
    out = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(out, dtype="<f4").reshape(-1, 2).astype(np.float64)


def survey(pcm, lo, hi, win_s=8.0, hop_s=8.0, top=6, bin_hz=1.0):
    n = int(win_s * SR)
    hop = int(hop_s * SR)
    if pcm.shape[0] < n:
        return None
    window = np.hanning(n)
    nfft = 1 << int(np.ceil(np.log2(n * 2)))
    freqs = np.fft.rfftfreq(nfft, 1.0 / SR)
    sel = (freqs >= lo) & (freqs <= hi)
    fsel = freqs[sel]

    nbins = int((hi - lo) / bin_hz) + 1
    hist = np.zeros(nbins)

    for start in range(0, pcm.shape[0] - n + 1, hop):
        seg = pcm[start:start + n]
        # Mono sum: a binaural pair is present in both ears, and summing keeps
        # the tones while averaging down anything uncorrelated.
        spec = np.abs(np.fft.rfft((seg[:, 0] + seg[:, 1]) * window, nfft))[sel]
        if spec.max() <= 0:
            continue
        idx = np.argpartition(spec, -top)[-top:]
        for i in idx:
            b = int((fsel[i] - lo) / bin_hz)
            if 0 <= b < nbins:
                hist[b] += spec[i] / spec.max()
    return hist


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio", nargs="+")
    ap.add_argument("--lo", type=float, default=40.0)
    ap.add_argument("--hi", type=float, default=520.0)
    ap.add_argument("--peaks", type=int, default=14)
    args = ap.parse_args()

    total = None
    for path in args.audio:
        pcm = decode(path)
        h = survey(pcm, args.lo, args.hi)
        if h is None:
            continue
        total = h if total is None else total + h
        print(f"  surveyed {path.split('/')[-1]}", file=sys.stderr)

    if total is None:
        return 1

    # Report the strongest bins, merging neighbours so one carrier is one line.
    order = np.argsort(total)[::-1]
    seen, rows = [], []
    for i in order:
        hz = args.lo + i
        if any(abs(hz - s) < 4.0 for s in seen):
            continue
        seen.append(hz)
        rows.append((hz, total[i]))
        if len(rows) >= args.peaks:
            break

    peak = max(r[1] for r in rows)
    print(f"{'Hz':>7} {'weight':>9}")
    for hz, w in sorted(rows):
        bar = "#" * int(40 * w / peak)
        print(f"{hz:7.0f} {w:9.1f}  {bar}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
