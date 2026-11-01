#!/usr/bin/env python3
"""Measure several concurrent binaural layers, one per carrier band.

analyse-binaural.py tracks whichever pair is loudest in a window, which is the
right answer when there is one. This material has two carrier families sounding
at once - one near 102 Hz and one near 300 Hz - and the loudest alternates, so
a single-pair measurement reads as a carrier that leaps back and forth when
what is really happening is a crossfade.

So each band is measured independently and reported with its own amplitude.
That amplitude is the point: it is what says which layer is actually sounding
at any moment, and it becomes the layer gain envelope in the ported program.

Amplitudes are reported relative to the loudest single layer measurement across
the whole file, so a layer that is present throughout reads near 1.0 and one
that fades in and out reads as the curve it is.

    py analyse-layers.py <audio> --json out.json
"""

import argparse
import json
import subprocess
import sys

import numpy as np

SR = 4000
PAIR_HZ = 25.0
MAX_BEAT_HZ = 40.0

# The carrier families this material actually uses, found by looking at where
# analyse-binaural.py's measured carriers cluster across all six parts:
# 102/114/119 sit together low, 162 alone in the middle, 300 on its own above.
DEFAULT_BANDS = [
    ("sub", 55.0, 72.0),      # ~61, part one only
    ("main", 96.0, 108.0),    # ~102, the signal that carries most of the run
    ("ref", 108.0, 126.0),    # ~112-119, the steady reference tones
    ("mid", 150.0, 180.0),    # ~162, brief
    ("high", 280.0, 320.0),   # ~300, the upper family
]

# 96-108 and 108-126 were one band to begin with, and that was wrong: they hold
# different tones at different moments, so a single band reported a carrier
# that jumped between 102 and 119 and a layer gain that made no sense. A band
# is only useful here if its carrier is stable inside it.


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


def peak_hz(spec, freqs, lo, hi):
    band = np.where((freqs >= lo) & (freqs <= hi))[0]
    if band.size < 3:
        return 0.0, 0.0
    mag = np.abs(spec[band])
    k = int(np.argmax(mag))
    if k == 0 or k == band.size - 1:
        return float(freqs[band[k]]), float(mag[k])
    eps = 1e-20
    a, b, c = (np.log(mag[k - 1] + eps), np.log(mag[k] + eps),
               np.log(mag[k + 1] + eps))
    denom = a - 2 * b + c
    delta = 0.0 if denom == 0 else float(np.clip(0.5 * (a - c) / denom, -0.5, 0.5))
    step = float(freqs[1] - freqs[0])
    return float(freqs[band[k]] + delta * step), float(mag[k])


def measure_band(fl, fr, freqs, lo, hi):
    """One band's pair: (carrier, beat, amplitude, confidence)."""
    h0, m0 = peak_hz(fl, freqs, lo, hi)
    h1, m1 = peak_hz(fr, freqs, lo, hi)
    if h0 == 0.0 or h1 == 0.0:
        return None

    if m0 >= m1:
        hl, ml = h0, m0
        hr, mr = peak_hz(fr, freqs, max(lo, hl - PAIR_HZ), min(hi, hl + PAIR_HZ))
    else:
        hr, mr = h1, m1
        hl, ml = peak_hz(fl, freqs, max(lo, hr - PAIR_HZ), min(hi, hr + PAIR_HZ))
    if hl == 0.0 or hr == 0.0 or ml <= 0 or mr <= 0:
        return None
    if abs(hr - hl) > MAX_BEAT_HZ:
        return None
    if max(ml, mr) / min(ml, mr) > 10.0:
        return None

    sel = (freqs >= lo) & (freqs <= hi)
    med_l = float(np.median(np.abs(fl[sel])))
    med_r = float(np.median(np.abs(fr[sel])))
    cl = ml / med_l if med_l > 0 else 0.0
    cr = mr / med_r if med_r > 0 else 0.0

    return {
        "carrier": round((hl + hr) / 2.0, 3),
        "beat": round(abs(hr - hl), 4),   # the ear does not hear which side leads
        "amp": (ml + mr) / 2.0,
        "conf": round(float(min(cl, cr)), 1),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("--window", type=float, default=8.0)
    ap.add_argument("--hop", type=float, default=4.0)
    ap.add_argument("--json", required=True)
    args = ap.parse_args()

    pcm = decode(args.audio)
    n = int(args.window * SR)
    hop = int(args.hop * SR)
    win = np.hanning(n)
    nfft = 1 << int(np.ceil(np.log2(n * 4)))
    freqs = np.fft.rfftfreq(nfft, 1.0 / SR)

    rows = []
    for start in range(0, pcm.shape[0] - n + 1, hop):
        seg = pcm[start:start + n]
        fl = np.fft.rfft(seg[:, 0] * win, nfft)
        fr = np.fft.rfft(seg[:, 1] * win, nfft)

        entry = {"t": round(start / SR, 2)}
        for name, lo, hi in DEFAULT_BANDS:
            entry[name] = measure_band(fl, fr, freqs, lo, hi)
        rows.append(entry)

    # Normalise amplitudes against the loudest single measurement in the file,
    # so a gain of 1.0 means "as present as this recording ever gets".
    peak = 0.0
    for r in rows:
        for name, _, _ in DEFAULT_BANDS:
            if r[name]:
                peak = max(peak, r[name]["amp"])
    if peak > 0:
        for r in rows:
            for name, _, _ in DEFAULT_BANDS:
                if r[name]:
                    r[name]["amp"] = round(r[name]["amp"] / peak, 4)

    with open(args.json, "w", encoding="utf-8") as fh:
        json.dump({"seconds": round(pcm.shape[0] / SR, 1),
                   "hop_s": args.hop,
                   "bands": [{"name": b[0], "lo": b[1], "hi": b[2]}
                             for b in DEFAULT_BANDS],
                   "rows": rows}, fh)
    print(f"wrote {args.json}: {len(rows)} windows, "
          f"{pcm.shape[0] / SR:.0f}s", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
