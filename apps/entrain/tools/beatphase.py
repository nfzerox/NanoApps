#!/usr/bin/env python3
"""Measure a binaural beat from the phase difference between the ears.

Picking a spectral peak in each channel and subtracting works when the two
tones are far enough apart to resolve. It stops working when they are not: an
eight-second window has a 0.125 Hz bin, so a pair a quarter of a hertz apart
lands in one peak and the measured "beat" flips between that peak's width and
zero depending on noise. Wave II does exactly this - its carrier pair sits
around 50 Hz with a sub-hertz difference - and the peak method reported 0.26 Hz
one window and 0.00 the next.

Phase does not have that limit. Band-pass both channels around the carrier,
take the analytic signal of each, and the difference of their instantaneous
phases advances at exactly the beat rate however small it is. Fitting a slope
to the unwrapped difference over a window gives the beat directly, with sign.

    py beatphase.py <audio> --carrier 50 --start 600 --dur 120
"""

import argparse
import subprocess
import sys

import numpy as np

SR = 4000


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


def analytic_band(x, sr, lo, hi):
    """Analytic signal of x band-limited to [lo, hi], by zeroing the rest of
    the spectrum. One FFT does the filtering and the Hilbert transform at
    once: keep only positive frequencies in the band and double them."""
    n = x.size
    X = np.fft.fft(x)
    f = np.fft.fftfreq(n, 1.0 / sr)
    keep = (f >= lo) & (f <= hi)
    Y = np.zeros_like(X)
    Y[keep] = 2.0 * X[keep]
    return np.fft.ifft(Y)


def beat_over_time(pcm, sr, carrier, half=20.0, win_s=8.0, hop_s=4.0):
    lo, hi = max(1.0, carrier - half), carrier + half
    al = analytic_band(pcm[:, 0], sr, lo, hi)
    ar = analytic_band(pcm[:, 1], sr, lo, hi)

    # The phase of L times the conjugate of R advances at the beat rate.
    d = al * np.conj(ar)
    phase = np.unwrap(np.angle(d))
    env = np.abs(al) * np.abs(ar)

    n = int(win_s * sr)
    hop = int(hop_s * sr)
    t = np.arange(n) / sr

    rows = []
    for s in range(0, pcm.shape[0] - n + 1, hop):
        seg = phase[s:s + n]
        w = env[s:s + n]
        if w.sum() <= 0:
            continue
        # Weighted least squares: windows where the tone is loud dominate the
        # fit, so narration between phrases does not drag the slope around.
        ww = w / w.sum()
        tm = (ww * t).sum()
        pm = (ww * seg).sum()
        var = (ww * (t - tm) ** 2).sum()
        if var <= 0:
            continue
        slope = (ww * (t - tm) * (seg - pm)).sum() / var
        rows.append({
            "t": round(s / sr, 2),
            "beat": round(slope / (2 * np.pi), 4),
            "amp": float(np.sqrt(w.mean())),
        })
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("--carrier", type=float, required=True)
    ap.add_argument("--half", type=float, default=20.0)
    ap.add_argument("--start", type=float, default=None)
    ap.add_argument("--dur", type=float, default=None)
    args = ap.parse_args()

    pcm = decode(args.audio, start=args.start, dur=args.dur)
    rows = beat_over_time(pcm, SR, args.carrier, args.half)
    if not rows:
        print("nothing", file=sys.stderr)
        return 1
    peak = max(r["amp"] for r in rows) or 1.0
    print(f"{'t':>8} {'beat':>9} {'amp':>7}")
    for r in rows:
        print(f"{r['t']:8.1f} {r['beat']:9.4f} {r['amp']/peak:7.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
