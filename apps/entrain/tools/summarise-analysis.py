#!/usr/bin/env python3
"""Summarise a measured beat curve, and reduce it to breakpoints.

The analyser reports one row per window. This turns that into the handful of
numbers a program is: which beats are actually held, for how long, and on what
carrier.

Segmentation is deliberately blunt. Rounding the beat to a tenth of a hertz and
merging equal neighbours finds holds without inventing structure - anything the
recording does not hold for long enough to survive a minimum run length is
noise or a transition, not a stage.
"""

import argparse
import json
import statistics
import sys


def load(path, min_conf):
    with open(path, encoding="utf-8") as fh:
        rows = json.load(fh)
    return [r for r in rows if r["conf"] >= min_conf]


def runs(rows, quant=0.1, min_run_s=45.0):
    """Contiguous stretches holding the same quantised beat."""
    if not rows:
        return []
    out, cur = [], None
    for r in rows:
        key = round(r["beat"] / quant) * quant
        if cur and abs(key - cur["key"]) < 1e-9:
            cur["rows"].append(r)
        else:
            if cur:
                out.append(cur)
            cur = {"key": key, "rows": [r]}
    if cur:
        out.append(cur)

    merged = []
    for run in out:
        span = run["rows"][-1]["t"] - run["rows"][0]["t"]
        if span < min_run_s:
            continue
        beats = [x["beat"] for x in run["rows"]]
        carriers = [x["carrier"] for x in run["rows"]]
        merged.append({
            "from_s": run["rows"][0]["t"],
            "to_s": run["rows"][-1]["t"],
            "span_s": round(span, 1),
            "beat": round(statistics.median(beats), 3),
            "carrier": round(statistics.median(carriers), 2),
            "n": len(run["rows"]),
        })
    return merged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json", nargs="+")
    ap.add_argument("--min-conf", type=float, default=8.0)
    ap.add_argument("--min-run", type=float, default=45.0)
    args = ap.parse_args()

    for path in args.json:
        rows = load(path, args.min_conf)
        print(f"\n=== {path.split('/')[-1]}")
        if not rows:
            print("  nothing above the confidence floor")
            continue

        beats = [r["beat"] for r in rows]
        carriers = [r["carrier"] for r in rows]
        print(f"  {len(rows)} usable windows, "
              f"{rows[0]['t']:.0f}s to {rows[-1]['t']:.0f}s")
        print(f"  beat    median {statistics.median(beats):7.3f}  "
              f"min {min(beats):7.3f}  max {max(beats):7.3f}")
        print(f"  carrier median {statistics.median(carriers):7.2f}  "
              f"min {min(carriers):7.2f}  max {max(carriers):7.2f}")

        held = runs(rows, min_run_s=args.min_run)
        if not held:
            print("  no stretch held one beat long enough to call it a stage")
            continue
        print(f"  held:")
        for h in held:
            print(f"    {h['from_s']:7.0f} - {h['to_s']:7.0f}s "
                  f"({h['span_s']:6.0f}s)  beat {h['beat']:6.3f}  "
                  f"carrier {h['carrier']:7.2f}  n={h['n']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
