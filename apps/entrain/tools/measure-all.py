#!/usr/bin/env python3
"""Measure every recording in a library into one program file.

Walks the waves in order, measures each track with measure-program.py's
machinery, and writes a single .prog that core/progfile.c reads plus a .json
with everything the measurement knew - carriers per ear, both routes to the
beat, steadiness, and what was rejected and why.

Names are structural, not the originals: "Series 3 Part 2" is the third
folder's second track. See the trademark note in the README.

    py measure-all.py <library dir> --out ../../../data/Entrain/measured.prog

A track that yields nothing is reported and skipped rather than written as an
empty program. The summary at the end says how many were measured and how many
were not, because "36 tracks in, 31 programs out" is the number that matters
and it should not have to be counted by hand.
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import importlib.util

_spec = importlib.util.spec_from_file_location(
    "measure_program",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "measure-program.py"))
mp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mp)


AUDIO_EXT = (".flac", ".wav", ".m4a", ".mp3", ".aiff", ".aif")


def find_tracks(root):
    """Every audio file, grouped by its immediate folder, both in name order.

    Sorted rather than walked in whatever order the filesystem returns, so a
    rerun produces the same names for the same tracks - a program called
    "Series 3 Part 2" has to mean the same recording next month.
    """
    groups = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        tracks = sorted(f for f in filenames
                        if f.lower().endswith(AUDIO_EXT))
        if tracks:
            groups.append((dirpath, tracks))
    groups.sort(key=lambda g: g[0])
    return groups


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("library")
    ap.add_argument("--out", required=True, help="the .prog to write")
    ap.add_argument("--json", default=None)
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after this many tracks (for a trial run)")
    ap.add_argument("--excerpts", type=int, default=8)
    ap.add_argument("--excerpt-seconds", type=float, default=80.0)
    ap.add_argument("--max-mad", type=float, default=0.05)
    ap.add_argument("--min-beat", type=float, default=0.3)
    ap.add_argument("--mono-max-mad", type=float, default=0.02)
    ap.add_argument("--mono-full-mad", type=float, default=0.01)
    ap.add_argument("--mono-votes", type=int, default=3)
    ap.add_argument("--max-disagree", type=float, default=0.25)
    ap.add_argument("--max-layers", type=int, default=8)
    ap.add_argument("--max-segs", type=int, default=110)
    ap.add_argument("--beat-tol", type=float, default=0.25)
    ap.add_argument("--level-tol", type=float, default=0.08)
    ap.add_argument("--min-segment-seconds", type=float, default=20.0)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    groups = find_tracks(args.library)
    if not groups:
        print("no audio under " + args.library, file=sys.stderr)
        return 1

    total = sum(len(t) for _, t in groups)
    print(f"{total} tracks in {len(groups)} folders", file=sys.stderr)

    recs = []
    empties = []
    failed = []
    done = 0
    t0 = time.time()

    for si, (dirpath, tracks) in enumerate(groups, start=1):
        for ti, fn in enumerate(tracks, start=1):
            if args.limit and done >= args.limit:
                break
            done += 1
            name = "Series %d Part %d" % (si, ti)
            path = os.path.join(dirpath, fn)
            print(f"[{done}/{total}] {name}", file=sys.stderr)
            try:
                rec = mp.measure(path, name, args)
            except Exception as e:            # noqa: BLE001 - one bad file
                # must not end a ninety-minute run.
                print(f"    FAILED: {e}", file=sys.stderr)
                failed.append((name, fn, str(e)))
                continue
            if rec and not rec.get("empty"):
                rec["folder"] = os.path.basename(dirpath)
                recs.append(rec)
            else:
                if rec:
                    # Nothing playable, but what it saw is worth keeping: it
                    # is the case most likely to be a threshold that is one
                    # notch too tight rather than a track with nothing on it.
                    rec["folder"] = os.path.basename(dirpath)
                    empties.append(rec)
                failed.append((name, fn, "no layer survived"))

    dt = time.time() - t0
    print(f"\n{len(recs)} measured, {len(failed)} not, in {dt / 60.0:.1f} min",
          file=sys.stderr)
    for name, fn, why in failed:
        print(f"  {name}: {fn} - {why}", file=sys.stderr)

    if recs:
        with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# Measured from recordings. Frequencies, differences and\n"
                     "# timings only - nothing of the source is retained and\n"
                     "# Entrain synthesises its own tones. No claims made.\n")
            fh.write("\n".join(mp.to_progfile(r) for r in recs) + "\n")
        print("wrote " + args.out, file=sys.stderr)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"measured": recs,
                       "empty": empties,
                       "failed": [{"name": n, "file": f, "why": w}
                                  for n, f, w in failed]}, fh, indent=1)

    return 0 if recs else 1


if __name__ == "__main__":
    raise SystemExit(main())
