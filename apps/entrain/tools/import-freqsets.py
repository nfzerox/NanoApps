#!/usr/bin/env python3
"""Import a frequency-set archive into Entrain's on-device bundle format.

Reads `rgcs.frequency_session/v1` JSON sessions and writes one compact text
file the device parses with no allocation and no JSON reader.

Why a bundle rather than the JSON itself:

  * Four hundred sets is four hundred file opens to build a list, and a
    directory listing is a syscall per entry on this hardware.
  * The device has no JSON parser and does not want one. core/ is pure C99
    with no allocation, and this format is scannable with a pointer.
  * The sessions carry a lot that only matters upstream - provenance, tags,
    raw pre-fold frequencies. Only the schedule is playable.

The format, one set per line:

    <name>|<step_seconds>|<hz>,<hz>,...

Names are sanitised: any '|' becomes '/', and the originating products are not
named, per the archive's own convention of describing what a thing is rather
than whose it was. The frequency data is carried through unchanged.

The patterns that do that last part are not in this repository - see the note
above _PRODUCTS_FILE. Without them this refuses to run rather than producing a
bundle with the names left in.

    python3 import-freqsets.py <sessions-dir> -o ../../../data/Entrain/frequencies.set
"""

import argparse
import glob
import json
import os
import re
import sys

# Trailers and boilerplate that carry no information, removed from the title.
# Nothing here names anything; the patterns that do are loaded separately.
_GENERIC_PATTERNS = [
    re.compile(r"\s*\bvia\b[^,()]*$", re.I),
    re.compile(r"\s*\[[^\]]*\]\s*"),
    re.compile(r"\s*\(SS\)\s*"),
    # Boilerplate carried by 181 of the 416 titles. It distinguishes nothing
    # and ate the name budget, which is why so many were truncating mid-word.
    re.compile(r"\s*[-–]?\s*\bCorrection\s*(?:and|&)\s*Balance\b", re.I),
    re.compile(r"\s*\(no Meds?\)\s*", re.I),
]

# The patterns that match the originating products by name, kept OUT of this
# file and out of the repository.
#
# This tool exists to strip those names, and a stripper that carries its
# targets in its own source publishes them in every clone of the project -
# which is the thing the convention was trying to avoid, achieved backwards.
# The archive's own policy is to describe what a thing is rather than whose it
# was, and that has to apply to the importer as much as to the output.
#
# So they live in a sibling file that .gitignore excludes: one regex per line,
# blank lines and # comments skipped. Written once on a machine that has the
# archive, and never needing to change again.
#
#     # strip-products.txt
#     \s*\((?:[^()]*\b(?:<short forms and initials>)\b[^()]*)\)\s*
#     \b(?:<the product names, alternated>)\b
#
# Absent, this refuses rather than proceeding. A run that quietly skipped the
# scrubbing would produce a bundle with the names baked in, and the bundle is
# exactly the artefact that gets committed and shipped - so the failure has to
# be loud, and it has to come before the writing rather than after it.
_PRODUCTS_FILE = "strip-products.txt"


def load_product_patterns(path, allow_missing):
    """The name-stripping patterns, or None to mean "stop"."""
    if not os.path.exists(path):
        if allow_missing:
            sys.stderr.write(
                "warning: %s is absent and --allow-unscrubbed was given.\n"
                "         Titles will keep whatever names they arrived with.\n"
                % path)
            return []
        sys.stderr.write(
            "error: %s not found.\n\n"
            "  It holds the patterns that strip the originating products'\n"
            "  names out of set titles, and it is deliberately not in the\n"
            "  repository - see the note in this script. Write it once, on a\n"
            "  machine that has the archive: one regex per line, # comments\n"
            "  and blank lines skipped.\n\n"
            "  To import anyway, leaving the names in, pass\n"
            "  --allow-unscrubbed.\n" % path)
        return None

    out = []
    with open(path, "r", encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                out.append(re.compile(line, re.I))
            except re.error as e:
                sys.stderr.write("%s:%d: bad pattern: %s\n" % (path, n, e))
                return None
    return out


# Generic only until main() adds the product patterns to it.
_STRIP_PATTERNS = list(_GENERIC_PATTERNS)

MAX_STEPS = 96          # must match EN_FREQSET_MAX_STEPS in core/freqset.h
MAX_NAME = 56           # must match EN_FREQSET_NAME_MAX


def clean_name(title: str) -> str:
    name = title or "Untitled"
    for pat in _STRIP_PATTERNS:
        name = pat.sub(" ", name)
    name = re.sub(r"\s+", " ", name).strip(" -/")
    name = name.replace("|", "/")
    if not name:
        name = "Untitled"
    return name[:MAX_NAME]


def fmt_hz(hz: float) -> str:
    """Three decimals is a millihertz, far past what any oscillator here
    resolves, and it keeps the file about a third smaller than full repr."""
    return f"{hz:.3f}".rstrip("0").rstrip(".")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("sessions", help="directory of *.frequency_session.json")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--products", default=None,
                    help="patterns that strip product names "
                         "(default: %s beside this script)"
                         % _PRODUCTS_FILE)
    ap.add_argument("--allow-unscrubbed", action="store_true",
                    help="import with no product patterns, leaving "
                         "whatever names the titles arrived with")
    args = ap.parse_args()

    # Before anything is read, let alone written. The point of refusing is
    # that no bundle exists to be committed by mistake.
    global _STRIP_PATTERNS
    products = args.products or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), _PRODUCTS_FILE)
    loaded = load_product_patterns(products, args.allow_unscrubbed)
    if loaded is None:
        return 2
    _STRIP_PATTERNS = list(_GENERIC_PATTERNS) + loaded

    files = sorted(glob.glob(os.path.join(args.sessions, "*.json")))
    if not files:
        print(f"no sessions in {args.sessions}", file=sys.stderr)
        return 1

    rows, skipped, truncated = [], [], 0

    for path in files:
        try:
            with open(path, encoding="utf-8") as fh:
                doc = json.load(fh)
        except (OSError, ValueError) as exc:
            skipped.append((os.path.basename(path), str(exc)))
            continue

        steps = [s for s in doc.get("steps", [])
                 if s.get("kind") == "tone" and s.get("hz")]
        if not steps:
            skipped.append((os.path.basename(path), "no tone steps"))
            continue

        if len(steps) > MAX_STEPS:
            truncated += 1
            steps = steps[:MAX_STEPS]

        # One dwell for the whole set. Every session in the archive uses a
        # single duration throughout; if one ever does not, the longest is
        # kept and the difference is reported rather than silently averaged.
        durations = {float(s.get("duration_s", 10.0)) for s in steps}
        dwell = max(durations)
        if len(durations) > 1:
            print(f"  note: {os.path.basename(path)} has mixed step lengths "
                  f"{sorted(durations)}; using {dwell}s")

        name = clean_name(doc.get("title", ""))
        hz = ",".join(fmt_hz(float(s["hz"])) for s in steps)
        rows.append(f"{name}|{fmt_hz(dwell)}|{hz}")

    rows.sort(key=lambda r: r.split("|", 1)[0].lower())

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# Entrain frequency sets, v1\n")
        fh.write("#\n")
        fh.write("#   <name>|<step seconds>|<hz>,<hz>,...\n")
        fh.write("#\n")
        fh.write("# Imported from a research archive with tools/import-freqsets.py.\n")
        fh.write("# Frequencies are octave-folded into the audio band by the archive;\n")
        fh.write("# they are carried through here unchanged. Entrain plays the numbers\n")
        fh.write("# and claims nothing about them - see README.md, No claims.\n")
        fh.write("#\n")
        fh.write(f"# {len(rows)} sets, {sum(r.count(',') + 1 for r in rows)} steps.\n")
        for row in rows:
            fh.write(row + "\n")

    total_steps = sum(r.count(",") + 1 for r in rows)
    print(f"wrote {args.out}")
    print(f"  {len(rows)} sets, {total_steps} steps, "
          f"{os.path.getsize(args.out)} bytes")
    if truncated:
        print(f"  {truncated} set(s) had more than {MAX_STEPS} steps and were cut")
    for name, why in skipped:
        print(f"  skipped {name}: {why}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
