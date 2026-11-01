#!/usr/bin/env python3
"""Measure one recording into a multi-layer program.

This supersedes measure-track.py, which screened on three excerpts
CONCATENATED into one array and then unwrapped phase across the joins. Phase
unwrapping across a discontinuity invents phase, so the beat it computed was
partly a fiction of the splice, and the fiction was not steady - which is why
that script found no layer at all on a track find-layers.py handles fine.
Here every excerpt is decoded, analysed and scored ON ITS OWN, and only the
resulting numbers are merged.

The method is find-layers.py's, unchanged, because it is the one that works:
sweep candidate carriers, measure the beat by phase difference, and keep the
ones whose beat HOLDS STILL. Amplitude proposes; steadiness decides. See
MEASUREMENT_NOTES.md for the three methods that came before it and what each
one got wrong.

Three things this adds.

  * Two stages, so it finishes. Screening runs on a few short excerpts, where
    an inverse FFT per candidate per width is cheap. Only the survivors - two
    to five of them - are measured over the whole track.

  * Per-hemisphere carriers. The earlier work reported one carrier and a beat,
    which is not what these recordings are: the two ears get DIFFERENT
    frequencies and the beat is the difference between them. Each channel's
    own instantaneous frequency is measured separately, so the left carrier
    and the right carrier are both reported, and the beat derived from the
    phase difference is checked against fR - fL. Two independent routes to the
    same number is what makes the sign trustworthy, and the sign is the whole
    point: it says which ear is high.

  * A schedule rather than one number. The beat and level series are segmented
    where they actually change, so a long hold is one segment and a ramp is
    not flattened into one.

    py measure-program.py <audio> --name "Practice 1" [--json out.json]
"""

import argparse
import json
import os
import subprocess
import sys

import numpy as np

SR = 4000
LO, HI = 35.0, 700.0
WIN_S = 8.0
HOP_S = 4.0
MAX_BEAT = 40.0
MERGE_HZ = 8.0

# Band half-widths to try at each candidate. There is no single right value:
# a pair 3.86 Hz apart needs a band wide enough to hold both tones, and a pair
# 1.5 Hz apart with neighbours nearby needs one narrow enough to exclude them.
# Each is invisible at the other's width, so every candidate is tried at all of
# them and keeps the steadiest - "isolated exactly one pair" is precisely what
# a steady beat means.
HALVES = (2.5, 4.0, 6.0, 9.0, 14.0)


# ---- decoding --------------------------------------------------------------

def probe_seconds(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=nw=1:nk=1", path],
        capture_output=True, text=True, check=True).stdout.strip()
    return float(out)


def decode(path, start=None, dur=None, sr=SR):
    cmd = ["ffmpeg", "-v", "error"]
    if start is not None:
        cmd += ["-ss", str(start)]
    cmd += ["-i", path]
    if dur is not None:
        cmd += ["-t", str(dur)]
    cmd += ["-ac", "2", "-ar", str(sr), "-f", "f32le", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype="<f4").reshape(-1, 2).astype(np.float64)


# ---- the sweep -------------------------------------------------------------

def analytic_band(X, f, lo, hi, n):
    keep = (f >= lo) & (f <= hi)
    Y = np.zeros(n, dtype=complex)
    Y[keep] = 2.0 * X[keep]
    return np.fft.ifft(Y)


def candidates(pcm, sr=SR, cap=40):
    """Carriers worth testing: spectral peaks, deliberately over-inclusive.

    Amplitude only PROPOSES here. A loud tone need not be a binaural pair and
    a quiet pair is still a pair; what decides is steadiness, later.
    """
    n = pcm.shape[0]
    nfft = 1 << int(np.floor(np.log2(min(n, 1 << 18))))
    if nfft < 4096:
        return []
    step = nfft // 2
    win = np.hanning(nfft)

    acc = None
    for s in range(0, n - nfft + 1, step):
        m = np.abs(np.fft.rfft((pcm[s:s + nfft, 0] + pcm[s:s + nfft, 1]) * win))
        acc = m if acc is None else acc + m
    if acc is None:
        return []

    freqs = np.fft.rfftfreq(nfft, 1.0 / sr)
    sel = (freqs >= LO) & (freqs <= HI)
    mag, fs = acc[sel], freqs[sel]
    if mag.size < 3:
        return []

    loc = np.where((mag[1:-1] > mag[:-2]) & (mag[1:-1] >= mag[2:]))[0] + 1
    floor = np.median(mag) * 2.0
    loc = [k for k in loc if mag[k] > floor]
    loc.sort(key=lambda k: -mag[k])

    out, seen = [], []
    for k in loc[:160]:
        hz = float(fs[k])
        if any(abs(hz - s) < MERGE_HZ / 2 for s in seen):
            continue
        seen.append(hz)
        out.append(hz)
        if len(out) >= cap:
            break
    return sorted(out)


def _slopes(phase, env, wn, hop, t, n):
    """Amplitude-weighted least-squares slope of `phase` per window.

    Weighted by the envelope so a window where the tone is barely present
    contributes where it is present and not where it is not. Returns
    (times, slopes, amps) with slopes in radians per second.
    """
    times, slopes, amps = [], [], []
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
        times.append((s + wn / 2.0) / SR)
        slopes.append((ww * (t - tm) * (seg - pm)).sum() / var)
        amps.append(float(np.sqrt(max(w.mean(), 0.0))))
    return np.array(times), np.array(slopes), np.array(amps)


def local_mad(x, keep, win=5):
    """Spread of `x` within a short sliding window, median over the track.

    The point of measuring locally is that a programme's beat is meant to
    change over half an hour and not meant to change over twenty seconds. A
    layer that holds still from moment to moment is real whatever it does over
    the whole run; one that does not is noise at every scale.

    `keep` marks the windows where the tone is actually sounding, and windows
    are only taken from RUNS of it. Selecting the strong samples first and
    then sliding over what is left would put samples from either side of a
    two-minute passage of narration into the same window and call the jump
    between them instability - which is the same mistake as splicing two
    excerpts together and unwrapping across the join, one level up.
    """
    out = []
    n = x.size
    i = 0
    while i < n:
        if not keep[i]:
            i += 1
            continue
        j = i
        while j < n and keep[j]:
            j += 1
        run = x[i:j]
        if run.size >= win:
            for k in range(0, run.size - win + 1):
                w = run[k:k + win]
                out.append(float(np.median(np.abs(w - np.median(w)))))
        i = j
    if not out:
        return float("inf")
    return float(np.median(out))


def measure_carrier(XL, XR, f, n, carrier, half, want_series=False):
    """Beat, per-ear carriers and steadiness at one candidate and width."""
    al = analytic_band(XL, f, carrier - half, carrier + half, n)
    ar = analytic_band(XR, f, carrier - half, carrier + half, n)
    env = np.abs(al) * np.abs(ar)

    wn = int(WIN_S * SR)
    hop = int(HOP_S * SR)
    if n < wn:
        return None
    t = np.arange(wn) / SR

    # The beat, from the phase of one channel against the other. d's phase
    # advances at 2*pi*(fL - fR), so the beat as the renderer wants it - right
    # minus left - is the negation.
    tt, sl, amp = _slopes(np.unwrap(np.angle(al * np.conj(ar))), env,
                          wn, hop, t, n)
    if sl.size < 8:
        return None
    beat = -sl / (2 * np.pi)

    ok = np.abs(beat) <= MAX_BEAT
    if ok.sum() < 8:
        return None
    tt, beat, amp = tt[ok], beat[ok], amp[ok]

    # Steadiness, measured where the tone is actually present.
    strong = amp >= np.median(amp)
    if strong.sum() < 4:
        return None
    med = float(np.median(beat[strong]))
    mad = float(np.median(np.abs(beat[strong] - med)))

    row = {"carrier": float(carrier), "half": float(half),
           "beat": med, "mad": mad,
           "amp": float(np.median(amp)), "amp_max": float(amp.max())}

    if want_series:
        # Each ear's own instantaneous frequency, measured the same way. This
        # is the per-hemisphere part: the two ears carry different tones, and
        # fR - fL is an independent route to the beat above.
        _, sll, _ = _slopes(np.unwrap(np.angle(al)), env, wn, hop, t, n)
        _, slr, _ = _slopes(np.unwrap(np.angle(ar)), env, wn, hop, t, n)
        fl = sll[ok] / (2 * np.pi)
        fr = slr[ok] / (2 * np.pi)
        row["t"] = tt
        row["beat_series"] = beat
        row["amp_series"] = amp
        row["fl"] = fl
        row["fr"] = fr
        row["fl_med"] = float(np.median(fl[strong]))
        row["fr_med"] = float(np.median(fr[strong]))
        # The cross-check. If these disagree, one of the two is measuring
        # something other than a pair and the layer should not be trusted.
        row["beat_from_freqs"] = float(np.median((fr - fl)[strong]))
        row["local_mad"] = local_mad(beat, strong)
    return row


# ---- stage 1: screen -------------------------------------------------------

def screen(path, total_s, n_excerpts, excerpt_s, max_mad, min_beat,
           mono_max_mad, mono_votes, verbose):
    """Find which carriers carry a steady beat, one excerpt at a time.

    Excerpts are never joined. Concatenating audio from different times and
    unwrapping phase across the join is what broke the previous attempt: the
    unwrap invents phase at the discontinuity and the invented part is not
    steady, so real layers score as noise.
    """
    fracs = np.linspace(0.12, 0.88, n_excerpts)
    hits = []
    mono = {}

    for i, fr in enumerate(fracs):
        start = max(0.0, total_s * float(fr))
        if start + excerpt_s > total_s:
            start = max(0.0, total_s - excerpt_s)
        pcm = decode(path, start=start, dur=excerpt_s)
        n = pcm.shape[0]
        if n < int(WIN_S * SR) * 2:
            continue

        cands = candidates(pcm)
        if not cands:
            continue

        XL = np.fft.fft(pcm[:, 0])
        XR = np.fft.fft(pcm[:, 1])
        f = np.fft.fftfreq(n, 1.0 / SR)

        found = 0
        for c in cands:
            best = None
            for half in HALVES:
                r = measure_carrier(XL, XR, f, n, c, half)
                if r and (best is None or r["mad"] < best["mad"]):
                    best = r
            if not best:
                continue
            if verbose and best["mad"] < 0.5:
                mark = "  " if (best["mad"] <= max_mad and
                                abs(best["beat"]) >= min_beat) else "x "
                print(f"      {mark}{best['carrier']:7.1f} Hz  "
                      f"beat {best['beat']:+7.3f}  mad {best['mad']:.4f}  "
                      f"half {best['half']:4.1f}", file=sys.stderr)
            if abs(best["beat"]) < min_beat:
                # Same tone in both ears. Real, but not a pair - pooled by
                # frequency so that a tone which keeps turning up in the same
                # place can be told from a voice that does not.
                if best["mad"] <= mono_max_mad:
                    mkey = round(best["carrier"])
                    mono.setdefault(mkey, []).append(best)
                continue
            if best["mad"] > max_mad:
                continue
            hits.append(best)
            found += 1

        if verbose:
            print(f"    excerpt {i + 1}/{n_excerpts} at {start:6.0f}s: "
                  f"{len(cands):3d} candidates, {found} steady",
                  file=sys.stderr)

    # Cluster by proximity rather than by a fixed grid. A pair reported at
    # 298.3 Hz in one excerpt and 303.0 Hz in another is one layer; keying on
    # round(carrier / 8) put those either side of a line and made it two.
    hits.sort(key=lambda r: r["carrier"])
    groups = []
    for r in hits:
        if groups and r["carrier"] - groups[-1][-1]["carrier"] <= MERGE_HZ:
            groups[-1].append(r)
        else:
            groups.append([r])

    # A layer has to show up in more than one excerpt. One excerpt agreeing
    # with itself is not evidence: a passage of narration or a swell can hold a
    # steady phase slope for ninety seconds and mean nothing.
    need = 2 if n_excerpts > 1 else 1
    layers = []
    for rows in groups:
        if len(rows) < need:
            continue
        rows.sort(key=lambda r: r["mad"])
        layers.append({
            "carrier": float(np.median([r["carrier"] for r in rows])),
            "half": rows[0]["half"],
            "beat_hint": float(np.median([r["beat"] for r in rows])),
            "mad_hint": rows[0]["mad"],
            "votes": len(rows),
        })
    for key, rows in sorted(mono.items()):
        # Neighbouring 1 Hz bins are the same tone seen through FFT noise.
        near = [r for k2, rs in mono.items() if abs(k2 - key) <= 1 for r in rs]
        if len(near) < mono_votes or key - 1 in mono:
            continue
        layers.append({
            "carrier": float(np.median([r["carrier"] for r in near])),
            "half": near[0]["half"],
            "beat_hint": 0.0,
            "mad_hint": float(np.median([r["mad"] for r in near])),
            "votes": len(near),
            "mono": True,
        })

    layers.sort(key=lambda r: r["carrier"])
    return layers


# ---- stage 3: segmentation -------------------------------------------------

def _fit_ok(ts, ys, tol):
    """Does a straight line through (ts, ys) stay within `tol` everywhere?"""
    if ts.size < 2:
        return True
    a, b = np.polyfit(ts, ys, 1)
    return bool(np.max(np.abs(ys - (a * ts + b))) <= tol)


def _endpoints(ts, ys):
    """The fitted line's value at each end of the span."""
    if ts.size == 1:
        return float(ys[0]), float(ys[0])
    a, b = np.polyfit(ts, ys, 1)
    return float(a * ts[0] + b), float(a * ts[-1] + b)


def segment(t, beats, levels, beat_tol, level_tol, max_segs, min_seg_s):
    """Cut the timeline where the numbers actually change.

    One boundary set shared by every layer, because the file format has one
    segment list and a group per layer inside it. A segment is extended while
    a straight line through EVERY layer's beat and level stays within
    tolerance, so a forty-minute hold is one segment and a ramp keeps its
    shape instead of being averaged into the hold beside it.
    """
    n = t.size
    if n == 0:
        return []

    def cut(tol_b, tol_l):
        bounds = []
        i = 0
        while i < n:
            j = i + 1
            while j < n:
                span = slice(i, j + 1)
                ts = t[span]
                if ts[-1] - ts[0] >= min_seg_s and not all(
                        _fit_ok(ts, beats[k][span], tol_b) and
                        _fit_ok(ts, levels[k][span], tol_l)
                        for k in range(len(beats))):
                    break
                j += 1
            bounds.append((i, min(j, n) - 1))
            i = j
        return bounds

    tol_b, tol_l = beat_tol, level_tol
    bounds = cut(tol_b, tol_l)
    # Loosen rather than truncate: a program that will not fit is one whose
    # tolerance is too tight for the material, and dropping its tail would be
    # dropping data. Doubling converges in a handful of rounds.
    while len(bounds) > max_segs and tol_b < 8.0:
        tol_b *= 1.6
        tol_l *= 1.6
        bounds = cut(tol_b, tol_l)

    segs = []
    for (a, b) in bounds:
        span = slice(a, b + 1)
        ts = t[span]
        dur = float(ts[-1] - ts[0]) + HOP_S
        groups = []
        for k in range(len(beats)):
            b0, b1 = _endpoints(ts, beats[k][span])
            g0, g1 = _endpoints(ts, levels[k][span])
            groups.append((b0, b1, max(0.0, g0), max(0.0, g1)))
        segs.append({"seconds": max(1, int(round(dur))), "layers": groups})
    return segs, tol_b, tol_l


# ---- driver ----------------------------------------------------------------

def measure(path, name, args):
    total_s = probe_seconds(path)
    print(f"  {os.path.basename(path)}  ({total_s / 60.0:.1f} min)",
          file=sys.stderr)

    layers = screen(path, total_s, args.excerpts, args.excerpt_seconds,
                    args.max_mad, args.min_beat, args.mono_max_mad,
                    args.mono_votes, args.verbose)
    if not layers:
        print("    no steady layer found", file=sys.stderr)
        return None
    print(f"    {len(layers)} layer(s) survive screening: "
          + ", ".join("%.1f Hz%s" % (l["carrier"], " mono" if l.get("mono")
                                     else "")
                      for l in layers),
          file=sys.stderr)

    pcm = decode(path)
    n = pcm.shape[0]
    XL = np.fft.fft(pcm[:, 0])
    XR = np.fft.fft(pcm[:, 1])
    f = np.fft.fftfreq(n, 1.0 / SR)

    series = []
    judged = []          # every candidate that got as far as being measured

    def note(r, verdict, why=""):
        judged.append({
            "carrier": r["carrier"],
            "half": r["half"],
            "beat": r["beat"],
            "beat_from_freqs": r.get("beat_from_freqs"),
            "carrier_left": r.get("fl_med"),
            "carrier_right": r.get("fr_med"),
            "mad": r["mad"],
            "local_mad": r["local_mad"],
            "mono": bool(r.get("mono")),
            "verdict": verdict,
            "why": why,
        })

    for l in layers:
        r = measure_carrier(XL, XR, f, n, l["carrier"], l["half"],
                            want_series=True)
        if not r:
            print(f"    {l['carrier']:.1f} Hz vanished over the full track",
                  file=sys.stderr)
            continue
        # The cross-check: the beat from the phase difference against the beat
        # from each ear's own frequency. They measure the same thing two ways,
        # and a layer where they disagree is not a pair.
        disagree = abs(r["beat"] - r["beat_from_freqs"])
        r["disagree"] = disagree
        r["mono"] = bool(l.get("mono"))
        if r["mono"]:
            # Nothing to cross-check: both routes read zero, and agreeing
            # about nothing proves nothing. What has to hold is that it is
            # still mono over the whole track rather than only in the
            # excerpts that were looked at.
            if abs(r["beat"]) >= args.min_beat:
                print(f"    {l['carrier']:.1f} Hz REJECTED: screened as mono, "
                      f"reads {r['beat']:+.3f} over the full track",
                      file=sys.stderr)
                note(r, "rejected", "mono candidate with a beat")
                continue
            # An oscillator holds still to about a thousandth. A voice does
            # not, and matching frequencies across excerpts was not enough to
            # tell them apart - formants cluster. The full track is what
            # settles it.
            if r["local_mad"] > args.mono_full_mad:
                print(f"    {l['carrier']:.1f} Hz REJECTED: mono but not "
                      f"steady moment to moment (local spread "
                      f"{r['local_mad']:.4f} > {args.mono_full_mad})",
                      file=sys.stderr)
                note(r, "rejected", "mono, unsteady")
                continue
            note(r, "kept", "mono")
            series.append(r)
            continue
        if disagree > args.max_disagree:
            print(f"    {l['carrier']:.1f} Hz REJECTED: beat {r['beat']:+.3f} "
                  f"vs fR-fL {r['beat_from_freqs']:+.3f} "
                  f"(differ by {disagree:.3f})", file=sys.stderr)
            note(r, "rejected", "the two routes to the beat disagree")
            continue
        # Admitted on the strength of a non-zero beat in the excerpts, so it
        # has to still have one over the whole track. Two candidates on Wave I
        # part 1 screened as pairs and then read exactly zero, which is a tone
        # that happened to look like a pair in the windows that were sampled.
        if abs(r["beat"]) < args.min_beat:
            print(f"    {l['carrier']:.1f} Hz REJECTED: screened as a pair, "
                  f"reads {r['beat']:+.3f} over the full track",
                  file=sys.stderr)
            note(r, "rejected", "pair candidate with no beat")
            continue
        if r["local_mad"] > args.max_mad:
            print(f"    {l['carrier']:.1f} Hz REJECTED: beat not steady "
                  f"moment to moment (local spread {r['local_mad']:.4f} "
                  f"> {args.max_mad})", file=sys.stderr)
            note(r, "rejected", "pair, unsteady")
            continue
        note(r, "kept", "pair")
        series.append(r)

    if not series:
        print("    nothing survived the full-track pass", file=sys.stderr)
        return {"name": name, "source": os.path.basename(path),
                "seconds": int(round(total_s)), "layers": [], "segments": [],
                "judged": judged, "empty": True}

    # The cap is applied HERE, after the full-track gates, and not to the
    # screening candidates. Cutting first would mean discarding a layer
    # without ever measuring it, and on Wave I part 1 the screen proposes
    # eleven of which two survive - so a cut at eight would have been three
    # coin flips. Ranked binaural first, then by steadiness, and whatever goes
    # is said out loud.
    if len(series) > args.max_layers:
        series.sort(key=lambda r: (bool(r.get("mono")), r["local_mad"]))
        cut = series[args.max_layers:]
        series = series[:args.max_layers]
        print("    DROPPED (over --max-layers): "
              + ", ".join(f"{r['carrier']:.1f} Hz" for r in cut),
              file=sys.stderr)
    series.sort(key=lambda r: r["carrier"])

    # A common time base. Every layer was measured with the same window and
    # hop over the same array, so they already share one; this only guards
    # against a short tail.
    m = min(r["t"].size for r in series)
    t = series[0]["t"][:m]

    peak = max(float(r["amp_series"][:m].max()) for r in series) or 1.0
    beats = [r["beat_series"][:m] for r in series]
    levels = [r["amp_series"][:m] / peak for r in series]

    segs, tol_b, tol_l = segment(t, beats, levels, args.beat_tol,
                                 args.level_tol, args.max_segs,
                                 args.min_segment_seconds)

    out = {
        "name": name,
        "source": os.path.basename(path),
        "seconds": int(round(total_s)),
        "beat_tol": tol_b,
        "level_tol": tol_l,
        "layers": [
            {
                "carrier_left": r["fl_med"],
                "carrier_right": r["fr_med"],
                "band_centre": r["carrier"],
                "half": r["half"],
                "beat": r["beat"],
                "beat_from_freqs": r["beat_from_freqs"],
                "disagree": r["disagree"],
                "mad": r["mad"],
                "local_mad": r["local_mad"],
                "mono": r.get("mono", False),
            } for r in series
        ],
        "segments": segs,
        "judged": judged,
    }

    for l in out["layers"]:
        print(f"    L {l['carrier_left']:7.2f} Hz  R {l['carrier_right']:7.2f} Hz"
              f"  beat {l['beat']:+7.3f}  steady {l['local_mad']:.4f}"
              + ("  (mono)" if l["mono"] else ""), file=sys.stderr)
    print(f"    {len(segs)} segments", file=sys.stderr)
    return out


def to_progfile(rec):
    """The on-disk format core/progfile.c reads."""
    lines = ["@%s|%d" % (rec["name"], rec["seconds"])]
    lines.append("C" + ",".join("%.3f" % l["carrier_left"]
                                for l in rec["layers"]))
    for s in rec["segments"]:
        groups = "|".join("%.4f,%.4f,%.4f,%.4f" % g for g in s["layers"])
        lines.append("S%d|%s" % (s["seconds"], groups))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio", nargs="+")
    ap.add_argument("--name", default=None,
                    help="program name; defaults to the file's stem")
    ap.add_argument("--json", default=None)
    ap.add_argument("--progfile", default=None)
    ap.add_argument("--excerpts", type=int, default=4)
    ap.add_argument("--excerpt-seconds", type=float, default=100.0)
    ap.add_argument("--max-mad", type=float, default=0.05,
                    help="steadiness threshold; real layers sit under 0.01 "
                         "and noise above 0.1")
    ap.add_argument("--min-beat", type=float, default=0.3)
    ap.add_argument("--mono-max-mad", type=float, default=0.02,
                    help="steadiness a beat-zero tone must hold to be kept")
    ap.add_argument("--mono-full-mad", type=float, default=0.01,
                    help="steadiness a mono tone must hold over the WHOLE "
                         "track; this is what separates an oscillator from a "
                         "narrator, and matching frequencies did not")
    ap.add_argument("--mono-votes", type=int, default=3,
                    help="excerpts a mono tone must appear in, at the same "
                         "frequency, before it counts as a tone and not a "
                         "voice")
    ap.add_argument("--max-disagree", type=float, default=0.25,
                    help="how far the two routes to the beat may differ")
    ap.add_argument("--max-layers", type=int, default=8)
    ap.add_argument("--max-segs", type=int, default=110)
    ap.add_argument("--beat-tol", type=float, default=0.25)
    ap.add_argument("--level-tol", type=float, default=0.08)
    ap.add_argument("--min-segment-seconds", type=float, default=20.0)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    recs = []
    for path in args.audio:
        name = args.name or os.path.splitext(os.path.basename(path))[0]
        try:
            rec = measure(path, name, args)
        except subprocess.CalledProcessError as e:
            print(f"  ffmpeg failed on {path}: {e}", file=sys.stderr)
            continue
        if rec:
            recs.append(rec)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(recs, fh, indent=1)
    if args.progfile:
        with open(args.progfile, "w", encoding="utf-8") as fh:
            fh.write("\n".join(to_progfile(r) for r in recs) + "\n")

    return 0 if recs else 1


if __name__ == "__main__":
    raise SystemExit(main())
