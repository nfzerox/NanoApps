#!/usr/bin/env python3
"""Find a recording's binaural layers by looking for a STABLE beat.

Three methods were tried before this one and the first two were wrong in ways
worth keeping written down.

  1. Fixed carrier bands, taken from one recording. Bands from Wave I miss the
     carrier Wave II is built on. A band list cannot be assumed.

  2. Discover carriers by amplitude, then pair peaks per channel. This finds
     loud tones, but loud is not the same as binaural: Wave II's loudest tone
     sits near 50 Hz and is the same in both ears, so pairing it produced a
     "beat" that flipped between 0.26 Hz and zero. Two tones closer together
     than an eight-second window can resolve cannot be measured by picking
     peaks at all.

  3. Phase difference at a given carrier. Correct, and resolves any beat - but
     it needs to be told where to look, and it returns a number whether or not
     there is a pair there. Run on that same 50 Hz tone it reports a beat
     wandering between -0.97 and +0.72 Hz, which is noise wearing an answer's
     clothes.

What actually separates a binaural pair from everything else is that its beat
is STEADY. So: sweep candidate carriers, measure the beat by phase at each,
and keep the ones whose beat holds still. Amplitude only decides which of the
survivors matters most - it never decides what is real.

    py find-layers.py <audio> [--json out.json]
"""

import argparse
import json
import subprocess
import sys

import numpy as np

SR = 4000
LO, HI = 35.0, 700.0
STEP = 2.0          # carrier sweep resolution; families are merged after
HALF = 12.0         # band-pass half-width around each candidate
WIN_S = 8.0
HOP_S = 4.0
MAX_BEAT = 40.0
MERGE_HZ = 8.0      # candidates this close describe the same family


def decode(path, sr=SR, start=None, dur=None):
    cmd = ["ffmpeg", "-v", "error"]
    if start is not None:
        cmd += ["-ss", str(start)]
    cmd += ["-i", path]
    if dur is not None:
        cmd += ["-t", str(dur)]
    cmd += ["-ac", "2", "-ar", str(sr), "-f", "f32le", "-"]
    out = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(out, dtype="<f4").reshape(-1, 2).astype(np.float64)


def analytic_band(X, f, lo, hi, n):
    keep = (f >= lo) & (f <= hi)
    Y = np.zeros(n, dtype=complex)
    Y[keep] = 2.0 * X[keep]
    return np.fft.ifft(Y)


def candidates(pcm, sr):
    """Carriers worth testing: every spectral peak with any energy at all.

    Deliberately over-inclusive. Amplitude only PROPOSES here - it decides
    nothing - because a loud tone is not necessarily a binaural pair and a
    quiet pair is still a pair. Testing every carrier from 35 to 700 Hz on a
    two-hertz grid was the first attempt and needed a full-length inverse FFT
    per step, which does not finish; proposing from the spectrum cuts that from
    hundreds of candidates to tens without deciding anything.
    """
    n = min(pcm.shape[0], int(600 * sr))     # ten minutes is plenty to propose
    seg = pcm[:n]
    nfft = 1 << int(np.ceil(np.log2(min(seg.shape[0], 1 << 19))))
    step = nfft // 2

    acc = None
    win = np.hanning(nfft)
    for s in range(0, seg.shape[0] - nfft + 1, step):
        m = np.abs(np.fft.rfft((seg[s:s + nfft, 0] + seg[s:s + nfft, 1]) * win))
        acc = m if acc is None else acc + m
    if acc is None:
        return []

    freqs = np.fft.rfftfreq(nfft, 1.0 / sr)
    sel = (freqs >= LO) & (freqs <= HI)
    mag, fs = acc[sel], freqs[sel]
    if mag.size < 3:
        return []

    loc = np.where((mag[1:-1] > mag[:-2]) & (mag[1:-1] >= mag[2:]))[0] + 1
    if loc.size == 0:
        return []
    # A low floor, so quiet families are still proposed.
    floor = np.median(mag) * 2.0
    loc = [k for k in loc if mag[k] > floor]
    loc.sort(key=lambda k: -mag[k])
    out, seen = [], []
    for k in loc[:120]:
        hz = float(fs[k])
        if any(abs(hz - s) < MERGE_HZ / 2 for s in seen):
            continue
        seen.append(hz)
        out.append(hz)
        if len(out) >= 40:
            break
    return sorted(out)


"""Band half-widths to try at each candidate.

There is no single right value, which took two wrong answers to establish.
The band must be wide enough to contain BOTH tones of a pair and narrow enough
to contain nothing else, and those pull in opposite directions:

  * Wave I's pair sits 3.86 Hz apart. At half=3 the band around the candidate
    holds only one of the two tones, so there is no difference to measure and
    its steadiness collapses from 0.008 to 0.63.
  * Wave II's pair sits 1.5 Hz apart with other carriers close by. At half=12
    those neighbours fall inside the band, the analytic signal is a mixture,
    and the beat reads -0.2 with a spread of 0.34. At half=3 it is -1.499 with
    a spread of 0.0037.

So each candidate is tried at several widths and keeps the steadiest, because
"isolated exactly one pair" is precisely what a steady beat means.
"""
HALVES = (2.5, 4.0, 6.0, 9.0, 14.0)


def sweep(pcm, sr, cands, halves=HALVES):
    """Beat series at each proposed carrier, at its best band width."""
    n = pcm.shape[0]
    XL = np.fft.fft(pcm[:, 0])
    XR = np.fft.fft(pcm[:, 1])
    f = np.fft.fftfreq(n, 1.0 / sr)

    wn = int(WIN_S * sr)
    hop = int(HOP_S * sr)
    t = np.arange(wn) / sr

    out = []
    for c in cands:
        best = None
        for half in halves:
            al = analytic_band(XL, f, c - half, c + half, n)
            ar = analytic_band(XR, f, c - half, c + half, n)
            d = al * np.conj(ar)
            phase = np.unwrap(np.angle(d))
            env = np.abs(al) * np.abs(ar)

            beats, amps = [], []
            for s in range(0, n - wn + 1, hop):
                w = env[s:s + wn]
                tot = w.sum()
                if tot <= 0:
                    continue
                ww = w / tot
                tm = (ww * t).sum()
                seg = phase[s:s + wn]
                pm = (ww * seg).sum()
                var = (ww * (t - tm) ** 2).sum()
                if var <= 0:
                    continue
                slope = (ww * (t - tm) * (seg - pm)).sum() / var
                b = slope / (2 * np.pi)
                if abs(b) > MAX_BEAT:
                    continue
                beats.append(b)
                amps.append(np.sqrt(w.mean()))

            if len(beats) < 8:
                continue
            b = np.array(beats)
            a = np.array(amps)
            # Steadiness, measured where the tone is actually present: the
            # spread of the beat over the loudest half of the windows.
            strong = a >= np.median(a)
            bb = b[strong]
            if bb.size < 4:
                continue
            med = float(np.median(bb))
            mad = float(np.median(np.abs(bb - med)))
            row = {"carrier": float(c), "beat": med, "mad": mad,
                   "half": half, "amp": float(np.median(a)),
                   "amp_max": float(a.max())}
            if best is None or mad < best["mad"]:
                best = row
        if best:
            out.append(best)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("--json", default=None)
    ap.add_argument("--start", type=float, default=None)
    ap.add_argument("--dur", type=float, default=None)
    ap.add_argument("--max-mad", type=float, default=0.15)
    ap.add_argument("--min-beat", type=float, default=0.3)

    args = ap.parse_args()

    pcm = decode(args.audio, start=args.start, dur=args.dur)
    cands = candidates(pcm, SR)
    if not cands:
        print("no candidate carriers", file=sys.stderr)
        return 1
    print(f"{len(cands)} candidates proposed", file=sys.stderr)
    rows = sweep(pcm, SR, cands)
    if not rows:
        print("nothing", file=sys.stderr)
        return 1

    peak = max(r["amp_max"] for r in rows) or 1.0
    for r in rows:
        r["amp"] /= peak
        r["amp_max"] /= peak

    # A layer is a candidate whose beat holds still and is not zero. Zero means
    # the same tone in both ears - real, but not a binaural pair.
    live = [r for r in rows
            if r["mad"] <= args.max_mad and abs(r["beat"]) >= args.min_beat]

    # Neighbouring candidates describe one family; keep the steadiest.
    live.sort(key=lambda r: r["carrier"])
    fams = []
    for r in live:
        if fams and r["carrier"] - fams[-1]["carrier"] <= MERGE_HZ:
            if r["mad"] < fams[-1]["mad"]:
                fams[-1] = r
        else:
            fams.append(r)
    fams.sort(key=lambda r: -r["amp_max"])

    print(f"{'carrier':>8} {'beat':>8} {'steady':>8} {'half':>5} {'amp':>7}")
    for r in fams:
        print(f"{r['carrier']:8.1f} {r['beat']:8.3f} {r['mad']:8.4f} "
              f"{r['half']:5.1f} {r['amp_max']:7.3f}")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"families": fams, "all": rows}, fh)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
