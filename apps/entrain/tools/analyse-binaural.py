#!/usr/bin/env python3
"""Measure the binaural beat schedule of a recording.

What this extracts is a measurement, not a copy: the carrier frequency and the
difference between the two channels, sampled over time. Numbers and timings.
Nothing of the recording itself is retained, and Entrain re-synthesises its own
tones from the schedule.

Method, per analysis window:

  * Decode to a low rate through ffmpeg. The carriers here live near 100-200 Hz,
    so 4 kHz is ten times more than is needed and makes an hour of audio cheap
    to work on.
  * Hann-window and FFT each channel, zero-padded, and take the strongest peak
    inside the carrier band. Parabolic interpolation on the log magnitude puts
    each peak well under a tenth of a hertz, which is what it takes to resolve a
    4 Hz beat between two carriers near 100 Hz.
  * beat = f_right - f_left; carrier = their mean.

Narration is the reason this is peak-picking in a band rather than anything
simpler. Voice is broadband and intermittent; the carriers are narrow and
continuous, so inside a band that excludes most speech energy the tone wins the
window. Windows where it does not are reported with a low confidence and can be
dropped rather than believed.

    py analyse-binaural.py <audio> [--band 60 400] [--window 8] [--hop 2]
"""

import argparse
import json
import subprocess
import sys

import numpy as np

SR = 4000          # analysis rate; carriers are two orders below Nyquist here
PAIR_HZ = 25.0     # how far from one carrier its partner may be
MAX_BEAT_HZ = 40.0 # past this it is two tones, not a binaural pair


def decode(path, sr=SR, start=None, dur=None):
    """Whole file (or a slice) as float32 stereo at `sr`, through ffmpeg."""
    cmd = ["ffmpeg", "-v", "error"]
    if start is not None:
        cmd += ["-ss", str(start)]
    cmd += ["-i", path]
    if dur is not None:
        cmd += ["-t", str(dur)]
    cmd += ["-ac", "2", "-ar", str(sr), "-f", "f32le", "-"]

    out = subprocess.run(cmd, capture_output=True, check=True).stdout
    a = np.frombuffer(out, dtype="<f4")
    return a.reshape(-1, 2).astype(np.float64)


def peak_hz(spec, freqs, lo, hi):
    """Strongest bin in [lo, hi], refined by a parabola on the log magnitude.

    Returns (hz, magnitude). Log rather than linear because the parabola is a
    much better fit to a Hann main lobe in dB, which is what buys the accuracy
    the beat needs.
    """
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
    delta = 0.0 if denom == 0 else 0.5 * (a - c) / denom
    delta = float(np.clip(delta, -0.5, 0.5))
    step = float(freqs[1] - freqs[0])
    return float(freqs[band[k]] + delta * step), float(mag[k])


def analyse(pcm, sr=SR, win_s=8.0, hop_s=2.0, band=(60.0, 400.0), pad=4):
    n = int(win_s * sr)
    hop = int(hop_s * sr)
    if pcm.shape[0] < n:
        return []

    window = np.hanning(n)
    nfft = 1 << int(np.ceil(np.log2(n * pad)))
    freqs = np.fft.rfftfreq(nfft, 1.0 / sr)

    rows = []
    for start in range(0, pcm.shape[0] - n + 1, hop):
        seg = pcm[start:start + n]
        fl = np.fft.rfft(seg[:, 0] * window, nfft)
        fr = np.fft.rfft(seg[:, 1] * window, nfft)

        # Find the strongest carrier in whichever channel is louder, then look
        # for its partner in a narrow window around it.
        #
        # Searching both channels over the whole band independently does not
        # work here: this material carries more than one carrier pair at once,
        # so the left channel would lock to the 300 Hz pair and the right to
        # the 118 Hz one, and the "beat" came out as 190 Hz. A beat is the
        # difference between two tones that are nearly the same, so the pairing
        # has to be constrained to say that.
        h0, m0 = peak_hz(fl, freqs, *band)
        h1, m1 = peak_hz(fr, freqs, *band)
        if h0 == 0.0 or h1 == 0.0:
            continue

        if m0 >= m1:
            hl, ml = h0, m0
            hr, mr = peak_hz(fr, freqs, hl - PAIR_HZ, hl + PAIR_HZ)
        else:
            hr, mr = h1, m1
            hl, ml = peak_hz(fl, freqs, hr - PAIR_HZ, hr + PAIR_HZ)
        if hl == 0.0 or hr == 0.0:
            continue

        # Beats used for entrainment are single digits. Anything past this is
        # two different tones, not a pair, and is dropped rather than averaged
        # into the schedule.
        if abs(hr - hl) > MAX_BEAT_HZ:
            continue

        # How far each peak stands above the median of its own band, and the
        # weaker of the two is what the pair is worth. Measuring only the left
        # channel reported high confidence for windows where the right channel
        # had no partner at all and the "beat" was whatever noise happened to
        # be nearest.
        sel = (freqs >= band[0]) & (freqs <= band[1])
        med_l = float(np.median(np.abs(fl[sel])))
        med_r = float(np.median(np.abs(fr[sel])))
        cl = ml / med_l if med_l > 0 else 0.0
        cr = mr / med_r if med_r > 0 else 0.0
        conf = float(min(cl, cr))

        # A binaural pair is roughly balanced between the ears by construction.
        # A ten-to-one imbalance means one side is the tone and the other is
        # whatever was closest, which is not a pair.
        if ml <= 0 or mr <= 0 or max(ml, mr) / min(ml, mr) > 10.0:
            continue

        rows.append({
            "t": round(start / sr, 2),
            "f_l": round(hl, 4),
            "f_r": round(hr, 4),
            "beat": round(hr - hl, 4),
            "carrier": round((hl + hr) / 2.0, 3),
            "conf": round(conf, 1),
        })
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("--band", nargs=2, type=float, default=[60.0, 400.0])
    ap.add_argument("--window", type=float, default=8.0)
    ap.add_argument("--hop", type=float, default=2.0)
    ap.add_argument("--start", type=float, default=None)
    ap.add_argument("--dur", type=float, default=None)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    pcm = decode(args.audio, start=args.start, dur=args.dur)
    print(f"{pcm.shape[0] / SR:.1f} s decoded at {SR} Hz", file=sys.stderr)

    rows = analyse(pcm, win_s=args.window, hop_s=args.hop,
                   band=(args.band[0], args.band[1]))
    if not rows:
        print("no usable windows", file=sys.stderr)
        return 1

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(rows, fh, indent=1)
        print(f"wrote {args.json} ({len(rows)} windows)", file=sys.stderr)
    else:
        print(f"{'t':>8} {'f_L':>9} {'f_R':>9} {'beat':>8} {'carrier':>8} {'conf':>6}")
        for r in rows:
            print(f"{r['t']:8.1f} {r['f_l']:9.3f} {r['f_r']:9.3f} "
                  f"{r['beat']:8.3f} {r['carrier']:8.2f} {r['conf']:6.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
