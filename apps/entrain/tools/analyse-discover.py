#!/usr/bin/env python3
"""Measure a recording's binaural layers without being told where they are.

This replaces the band-list approach, which was wrong in a way worth recording.
Bands derived from one recording do not generalise: the list taken from Wave I
clustered at 60/102/114/162/300 Hz, and Wave II turned out to be dominated by a
49 Hz carrier that none of those covered. Analysing with the wrong list does
not degrade gracefully - the layer is simply absent from the result, silently.
So nothing is chosen in advance.

One pass, three stages:

  1. Per window, take the strongest few peaks in each channel across the whole
     usable range, and pair them: a left peak with a right peak within
     PAIR_HZ. A binaural pair is two tones that are nearly the same, so that
     constraint is what makes a pair a pair rather than two unrelated tones
     whose difference happens to be a number.
  2. Cluster the pairs by carrier over the whole file. What comes out are the
     carrier families the recording actually uses - discovered, not assumed.
  3. Re-walk the windows and report each family's carrier, beat and amplitude
     over time. A family that is silent in a window is reported as silent,
     which is a measurement and not a gap.

    py analyse-discover.py <audio> --json out.json
"""

import argparse
import json
import subprocess
import sys

import numpy as np

SR = 4000
LO, HI = 30.0, 1000.0     # 4 kHz decode leaves 2 kHz; carriers live far below
PAIR_HZ = 25.0            # how far a pair's two tones may sit apart
MAX_BEAT_HZ = 40.0        # past this it is two tones, not a binaural pair
PEAKS_PER_CH = 8          # how many candidates to consider per channel
CLUSTER_HZ = 6.0          # carriers this close are the same family
MIN_SHARE = 0.02          # a family must appear in this fraction of windows
MIN_AMP = 0.02            # ... and actually be audible when it does


def decode(path, sr=SR):
    cmd = ["ffmpeg", "-v", "error", "-i", path,
           "-ac", "2", "-ar", str(sr), "-f", "f32le", "-"]
    out = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(out, dtype="<f4").reshape(-1, 2).astype(np.float64)


def refine(mag, k, freqs):
    """Parabolic interpolation on the log magnitude around bin k."""
    if k <= 0 or k >= mag.size - 1:
        return float(freqs[k]), float(mag[k])
    eps = 1e-20
    a, b, c = (np.log(mag[k - 1] + eps), np.log(mag[k] + eps),
               np.log(mag[k + 1] + eps))
    den = a - 2 * b + c
    d = 0.0 if den == 0 else float(np.clip(0.5 * (a - c) / den, -0.5, 0.5))
    return float(freqs[k] + d * (freqs[1] - freqs[0])), float(mag[k])


def top_peaks(mag, freqs, n):
    """The n strongest local maxima, refined."""
    if mag.size < 3:
        return []
    loc = np.where((mag[1:-1] > mag[:-2]) & (mag[1:-1] >= mag[2:]))[0] + 1
    if loc.size == 0:
        return []
    order = loc[np.argsort(mag[loc])[::-1][:n]]
    return [refine(mag, int(k), freqs) for k in order]


def window_pairs(fl, fr, freqs):
    """Every plausible binaural pair in one window."""
    ml, mr = np.abs(fl), np.abs(fr)
    pl = top_peaks(ml, freqs, PEAKS_PER_CH)
    pr = top_peaks(mr, freqs, PEAKS_PER_CH)

    out = []
    for hl, al in pl:
        best = None
        for hr, ar in pr:
            if abs(hr - hl) > PAIR_HZ:
                continue
            # Balanced between the ears, or it is not one tone heard twice.
            if max(al, ar) / max(min(al, ar), 1e-20) > 10.0:
                continue
            if best is None or abs(hr - hl) < abs(best[0] - hl):
                best = (hr, ar)
        if best is None:
            continue
        hr, ar = best
        if abs(hr - hl) > MAX_BEAT_HZ:
            continue
        out.append({
            "carrier": (hl + hr) / 2.0,
            "beat": abs(hr - hl),
            "amp": (al + ar) / 2.0,
        })
    return out


def cluster(carriers, weights):
    """Group carriers that sit within CLUSTER_HZ into families."""
    if not carriers:
        return []
    order = np.argsort(carriers)
    fams = []
    for i in order:
        hz, w = carriers[i], weights[i]
        if fams and hz - fams[-1]["hz_max"] <= CLUSTER_HZ:
            f = fams[-1]
            f["sum"] += hz * w
            f["w"] += w
            f["hz_max"] = hz
            f["n"] += 1
        else:
            fams.append({"sum": hz * w, "w": w, "hz_max": hz, "n": 1})
    for f in fams:
        f["hz"] = f["sum"] / f["w"] if f["w"] else 0.0
    return fams


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("--json", required=True)
    ap.add_argument("--window", type=float, default=8.0)
    ap.add_argument("--hop", type=float, default=4.0)
    args = ap.parse_args()

    pcm = decode(args.audio)
    n = int(args.window * SR)
    hop = int(args.hop * SR)
    if pcm.shape[0] < n:
        print("too short", file=sys.stderr)
        return 1

    win = np.hanning(n)
    nfft = 1 << int(np.ceil(np.log2(n * 4)))
    freqs = np.fft.rfftfreq(nfft, 1.0 / SR)
    sel = (freqs >= LO) & (freqs <= HI)
    fsel = freqs[sel]

    # --- pass one: every pair in every window ---------------------------
    per_window = []
    for start in range(0, pcm.shape[0] - n + 1, hop):
        seg = pcm[start:start + n]
        fl = np.fft.rfft(seg[:, 0] * win, nfft)[sel]
        fr = np.fft.rfft(seg[:, 1] * win, nfft)[sel]
        per_window.append((start / SR, window_pairs(fl, fr, fsel)))

    peak_amp = max((p["amp"] for _, ps in per_window for p in ps), default=0.0)
    if peak_amp <= 0:
        print("no pairs found", file=sys.stderr)
        return 1
    for _, ps in per_window:
        for p in ps:
            p["amp"] /= peak_amp

    # --- pass two: what families are there ------------------------------
    cs = [p["carrier"] for _, ps in per_window for p in ps]
    ws = [p["amp"] for _, ps in per_window for p in ps]
    fams = cluster(cs, ws)

    nwin = len(per_window)
    keep = []
    for f in fams:
        share = f["n"] / max(nwin, 1)
        if share < MIN_SHARE:
            continue
        best = 0.0
        for _, ps in per_window:
            for p in ps:
                if abs(p["carrier"] - f["hz"]) <= CLUSTER_HZ:
                    best = max(best, p["amp"])
        if best < MIN_AMP:
            continue
        keep.append({"hz": round(f["hz"], 3), "share": round(share, 4),
                     "amp_max": round(best, 4), "energy": round(f["w"], 2)})
    keep.sort(key=lambda f: -f["energy"])

    if not keep:
        print("no family survived", file=sys.stderr)
        return 1

    # --- pass three: each family over time -------------------------------
    rows = []
    for t, ps in per_window:
        entry = {"t": round(t, 2), "L": []}
        for f in keep:
            hit = None
            for p in ps:
                if abs(p["carrier"] - f["hz"]) <= CLUSTER_HZ:
                    if hit is None or p["amp"] > hit["amp"]:
                        hit = p
            entry["L"].append(
                {"b": round(hit["beat"], 4), "a": round(hit["amp"], 4)}
                if hit else {"b": None, "a": 0.0})
        rows.append(entry)

    with open(args.json, "w", encoding="utf-8") as fh:
        json.dump({"seconds": round(pcm.shape[0] / SR, 1),
                   "hop_s": args.hop,
                   "families": keep,
                   "rows": rows}, fh)

    fam_txt = ", ".join(f"{f['hz']:.1f}Hz({f['share']*100:.0f}%)" for f in keep)
    print(f"{args.json}: {pcm.shape[0]/SR:.0f}s, {len(keep)} families: {fam_txt}",
          file=sys.stderr, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
