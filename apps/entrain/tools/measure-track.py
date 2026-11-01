#!/usr/bin/env python3
"""Measure one recording's binaural layers over its whole length.

Two stages, because doing it in one does not finish. Testing every candidate
carrier at every band width over a full thirty-five minute track needs about
two hundred full-length inverse FFTs and runs for five minutes a track; across
thirty-six tracks that is three hours.

  Stage one, on excerpts: which carriers hold a steady beat, and at what band
  width. Cheap, and the answer does not depend on hearing the whole track -
  a layer either is a binaural pair or is not.

  Stage two, on the whole track: those carriers only, at the width stage one
  chose, reporting beat and amplitude every four seconds. This is the data the
  port is built from.

Excerpts are spread across the track rather than taken from the front, so a
layer that only appears late is still proposed.

See find-layers.py for why steadiness is the test and why the width cannot be
fixed in advance.

    py measure-track.py <audio> --json out.json
"""

import argparse
import json
import subprocess
import sys

import numpy as np

SR = 4000
LO, HI = 35.0, 700.0
WIN_S = 8.0
HOP_S = 4.0
MAX_BEAT = 40.0
MERGE_HZ = 8.0
HALVES = (2.5, 4.0, 6.0, 9.0, 14.0)

# Real layers measure at a spread under 0.01; everything else - mono tones,
# noise beds, narration - sits above 0.1. The threshold goes in the gap, which
# is two orders of magnitude wide and so is not a judgement call.
MAX_MAD = 0.05
MIN_BEAT = 0.3


def duration(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=nw=1:nk=1", path],
        capture_output=True, text=True, check=True).stdout.strip()
    return float(out)


def decode(path, start=None, dur=None):
    cmd = ["ffmpeg", "-v", "error"]
    if start is not None:
        cmd += ["-ss", str(start)]
    cmd += ["-i", path]
    if dur is not None:
        cmd += ["-t", str(dur)]
    cmd += ["-ac", "2", "-ar", str(SR), "-f", "f32le", "-"]
    out = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(out, dtype="<f4").reshape(-1, 2).astype(np.float64)


def analytic_band(X, f, lo, hi, n):
    keep = (f >= lo) & (f <= hi)
    Y = np.zeros(n, dtype=complex)
    Y[keep] = 2.0 * X[keep]
    return np.fft.ifft(Y)


def propose(pcm):
    """Spectral peaks worth testing. Over-inclusive on purpose."""
    nfft = 1 << int(np.ceil(np.log2(min(pcm.shape[0], 1 << 18))))
    if pcm.shape[0] < nfft:
        return []
    win = np.hanning(nfft)
    acc = None
    for s in range(0, pcm.shape[0] - nfft + 1, nfft // 2):
        m = np.abs(np.fft.rfft((pcm[s:s + nfft, 0] + pcm[s:s + nfft, 1]) * win))
        acc = m if acc is None else acc + m
    if acc is None:
        return []
    freqs = np.fft.rfftfreq(nfft, 1.0 / SR)
    sel = (freqs >= LO) & (freqs <= HI)
    mag, fs = acc[sel], freqs[sel]
    loc = np.where((mag[1:-1] > mag[:-2]) & (mag[1:-1] >= mag[2:]))[0] + 1
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
        if len(out) >= 30:
            break
    return sorted(out)


def beat_series(XL, XR, f, n, carrier, half):
    """(times, beats, amps) at one carrier and width."""
    al = analytic_band(XL, f, carrier - half, carrier + half, n)
    ar = analytic_band(XR, f, carrier - half, carrier + half, n)
    d = al * np.conj(ar)
    phase = np.unwrap(np.angle(d))
    env = np.abs(al) * np.abs(ar)

    wn = int(WIN_S * SR)
    hop = int(HOP_S * SR)
    t = np.arange(wn) / SR

    ts, bs, amps = [], [], []
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
        ts.append(s / SR)
        bs.append(b if abs(b) <= MAX_BEAT else 0.0)
        amps.append(float(np.sqrt(w.mean())))
    return np.array(ts), np.array(bs), np.array(amps)


def steadiness(bs, amps):
    if bs.size < 6:
        return None
    strong = amps >= np.median(amps)
    bb = bs[strong]
    if bb.size < 4:
        return None
    med = float(np.median(bb))
    return med, float(np.median(np.abs(bb - med)))


def screen(path, total):
    """Stage one: which carriers, at what width."""
    spans = [(total * fr, min(200.0, total * 0.15))
             for fr in (0.10, 0.40, 0.70)]
    chunks = [decode(path, start=s, dur=d) for s, d in spans]
    pcm = np.concatenate([c for c in chunks if c.size], axis=0)
    if pcm.size == 0:
        return []

    cands = propose(pcm)
    if not cands:
        return []

    n = pcm.shape[0]
    XL = np.fft.fft(pcm[:, 0])
    XR = np.fft.fft(pcm[:, 1])
    f = np.fft.fftfreq(n, 1.0 / SR)

    found = []
    for c in cands:
        best = None
        for half in HALVES:
            _, bs, amps = beat_series(XL, XR, f, n, c, half)
            st = steadiness(bs, amps)
            if not st:
                continue
            med, mad = st
            if best is None or mad < best[1]:
                best = (med, mad, half, float(amps.max()))
        if not best:
            continue
        med, mad, half, amax = best
        if mad <= MAX_MAD and abs(med) >= MIN_BEAT:
            found.append({"carrier": c, "beat": round(med, 4),
                          "mad": round(mad, 5), "half": half,
                          "amp_max": amax})

    found.sort(key=lambda r: r["carrier"])
    merged = []
    for r in found:
        if merged and r["carrier"] - merged[-1]["carrier"] <= MERGE_HZ:
            if r["mad"] < merged[-1]["mad"]:
                merged[-1] = r
        else:
            merged.append(r)
    merged.sort(key=lambda r: -r["amp_max"])
    return merged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("--json", required=True)
    args = ap.parse_args()

    total = duration(args.audio)
    fams = screen(args.audio, total)
    if not fams:
        print(f"{args.json}: no steady layer found", file=sys.stderr, flush=True)
        return 1

    pcm = decode(args.audio)
    n = pcm.shape[0]
    XL = np.fft.fft(pcm[:, 0])
    XR = np.fft.fft(pcm[:, 1])
    f = np.fft.fftfreq(n, 1.0 / SR)

    series, peak = [], 0.0
    for fam in fams:
        ts, bs, amps = beat_series(XL, XR, f, n, fam["carrier"], fam["half"])
        peak = max(peak, float(amps.max()) if amps.size else 0.0)
        series.append((ts, bs, amps))
    if peak <= 0:
        return 1

    rows = []
    ts0 = series[0][0]
    for i in range(len(ts0)):
        entry = {"t": round(float(ts0[i]), 2), "L": []}
        for (ts, bs, amps) in series:
            if i < ts.size:
                entry["L"].append({"b": round(float(bs[i]), 4),
                                   "a": round(float(amps[i]) / peak, 4)})
            else:
                entry["L"].append({"b": 0.0, "a": 0.0})
        rows.append(entry)

    with open(args.json, "w", encoding="utf-8") as fh:
        json.dump({"seconds": round(n / SR, 1), "hop_s": HOP_S,
                   "families": [{"hz": round(fm["carrier"], 2),
                                 "beat_typ": fm["beat"],
                                 "mad": fm["mad"], "half": fm["half"]}
                                for fm in fams],
                   "rows": rows}, fh)

    txt = ", ".join(f"{fm['carrier']:.1f}Hz@{fm['beat']:+.2f}" for fm in fams)
    print(f"{args.json}: {n/SR:.0f}s, {len(fams)} layers: {txt}",
          file=sys.stderr, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
