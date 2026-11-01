#!/usr/bin/env python3
"""Turn measured layer curves into Entrain program tables.

Input is analyse-layers.py output: per window, per carrier band, the carrier,
the beat and how present that band is.

Output is a C table of en_prog_seg_t, one segment per stretch over which
nothing changed much, with every band carried as its own layer.

Nothing measured is discarded. Every band that is ever present in a recording
becomes a layer in the ported program, whatever the count comes to; a band
that is silent for a stretch is a layer at gain zero for that stretch, which
is a measurement too. EN_MAX_LAYERS exists to size fixed arrays and is raised
to fit the material, never the other way round.

    py port-measured.py tools/analysis/layers-2.json --name "Practice Two"
"""

import argparse
import json
import statistics
import sys

MIN_CONF = 8.0


def band_stats(doc):
    """Per band: how often it is present, and where its carrier sits."""
    out = {}
    for b in doc["bands"]:
        name = b["name"]
        car = [r[name]["carrier"] for r in doc["rows"]
               if r.get(name) and r[name]["conf"] >= MIN_CONF]
        amp = [r[name]["amp"] for r in doc["rows"]
               if r.get(name) and r[name]["conf"] >= MIN_CONF]
        if not car:
            out[name] = None
            continue
        beats = [r[name]["beat"] for r in doc["rows"]
                 if r.get(name) and r[name]["conf"] >= MIN_CONF]
        out[name] = {
            "n": len(car),
            "share": len(car) / len(doc["rows"]),
            "carrier": round(statistics.median(car), 2),
            "carrier_spread": round(max(car) - min(car), 2),
            "amp_max": round(max(amp), 3),
            # How much of the recording this band actually contributes, which
            # is not the same as how often it is detectable.
            "energy": round(sum(amp), 2),
            "first_beat": round(beats[0], 3) if beats else 0.0,
        }
    return out


def bin_rows(doc, bin_s):
    """Average the windows into fixed time bins, per band."""
    names = [b["name"] for b in doc["bands"]]
    total = doc["seconds"]
    nbins = max(1, int(total // bin_s))
    bins = []
    for i in range(nbins):
        t0, t1 = i * bin_s, (i + 1) * bin_s
        chunk = [r for r in doc["rows"] if t0 <= r["t"] < t1]
        entry = {"t": t0}
        for nm in names:
            good = [r[nm] for r in chunk
                    if r.get(nm) and r[nm]["conf"] >= MIN_CONF]
            if good:
                entry[nm] = {
                    "beat": round(statistics.median(g["beat"] for g in good), 3),
                    "amp": round(statistics.median(g["amp"] for g in good), 3),
                }
            else:
                # Absent is a measurement: the layer is not sounding.
                entry[nm] = {"beat": None, "amp": 0.0}
        bins.append(entry)
    return names, bins


def merge(names, bins, beat_tol, amp_tol):
    """Fuse neighbouring bins that agree on every band."""
    def close(a, b):
        for nm in names:
            if abs(a[nm]["amp"] - b[nm]["amp"]) > amp_tol:
                return False
            ba, bb = a[nm]["beat"], b[nm]["beat"]
            if (ba is None) != (bb is None):
                return False
            if ba is not None and abs(ba - bb) > beat_tol:
                return False
        return True

    out = []
    for b in bins:
        if out and close(out[-1]["rep"], b):
            out[-1]["members"].append(b)
        else:
            out.append({"rep": b, "members": [b]})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json")
    ap.add_argument("--name", required=True)
    ap.add_argument("--bin", type=float, default=20.0)
    ap.add_argument("--beat-tol", type=float, default=0.25)
    ap.add_argument("--amp-tol", type=float, default=0.12)
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()

    with open(args.json, encoding="utf-8") as fh:
        doc = json.load(fh)

    stats = band_stats(doc)
    if args.stats:
        print(f"=== {args.json}  ({doc['seconds']:.0f}s)")
        for nm, s in stats.items():
            if not s:
                print(f"  {nm:6s} absent")
                continue
            print(f"  {nm:6s} present {s['share']*100:5.1f}%  "
                  f"carrier {s['carrier']:7.2f} (spread {s['carrier_spread']:5.2f})  "
                  f"peak gain {s['amp_max']:.3f}")
        return 0

    # Every band that actually sounds becomes a layer, however many that comes
    # to. The gain floor is not a budget and does not drop layers to fit
    # anything: it rejects band-EDGE picks, where the peak finder returns the
    # boundary frequency because there is no tone inside the band at all. Those
    # show up as a carrier pinned to the limit with zero spread and a gain in
    # the thousandths - 179.99 Hz at 0.002 in parts four and six - and they are
    # the absence of a tone being reported as one.
    live = [nm for nm, s in stats.items()
            if s and s["share"] > 0.01 and s["amp_max"] > 0.03]
    # By contributed energy, not by how often the band is merely detectable.
    # Ordering by share put a layer present 61% of the time at peak gain 0.21
    # ahead of one present 55% of the time at peak gain 1.00 - so layer zero,
    # which the Now Playing readout follows, was the quiet one.
    live.sort(key=lambda nm: -stats[nm]["energy"])
    if not live:
        print("no band was ever present", file=sys.stderr)
        return 1

    names, bins = bin_rows(doc, args.bin)
    groups = merge(live, bins, args.beat_tol, args.amp_tol)

    # A band's beat while it is silent is meaningless; hold the last real one so
    # the ramp into its return starts from where it left off rather than zero.
    # Seeded with each band's first real measurement rather than an invented
    # number, so the ramp into a layer's entry starts where that layer actually
    # starts.
    last = {nm: stats[nm]["first_beat"] for nm in live}
    for g in groups:
        for nm in live:
            b = g["rep"][nm]["beat"]
            if b is None:
                g["rep"][nm]["beat"] = last[nm]
            else:
                last[nm] = b

    slug = "".join(c if c.isalnum() else "_" for c in args.name.lower())
    print(f"/* {args.name}: measured from the recording, "
          f"{doc['seconds']:.0f}s, {len(live)} layers. */")
    print(f"static const en_prog_seg_t s_{slug}[] = {{")

    for i, g in enumerate(groups):
        secs = int(round(len(g["members"]) * args.bin))
        nxt = groups[i + 1]["rep"] if i + 1 < len(groups) else g["rep"]
        cur = g["rep"]
        prim = live[0]
        print(f"    /* {int(cur['t']//60):3d}:{int(cur['t']%60):02d} */")
        print(f"    {{ .beat_start = {cur[prim]['beat']:.3f}, "
              f".beat_end = {nxt[prim]['beat']:.3f},")
        print(f"      .carrier_hz = {stats[prim]['carrier']:.2f}, "
              f".seconds = {secs}, .noise = EN_NOISE_NONE,")
        print(f"      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,")
        print(f"      .layers = {len(live)}, .layer = {{")
        for nm in live:
            print(f"        {{ {stats[nm]['carrier']:7.2f}, "
                  f"{cur[nm]['beat']:6.3f}, {nxt[nm]['beat']:6.3f}, "
                  f"{cur[nm]['amp']:5.3f}, {nxt[nm]['amp']:5.3f} }},"
                  f"   /* {nm} */")
        print("      } },")
    print("};")
    print(f"/* {len(groups)} segments, layers: {', '.join(live)} */")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
