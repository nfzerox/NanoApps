#!/bin/bash
# Survey where every track puts its carriers, before choosing any bands.
#
# Bands derived from one wave do not generalise: Wave I clusters at 60/102/114/
# 162/300 Hz, and Wave II is dominated by a 49 Hz carrier that none of those
# cover. Analysing with the wrong band list does not degrade gracefully - the
# layer is simply absent from the port.
set -u
PY="/c/Program Files/Python313/python.exe"
ROOT="${1:-T:/downloads/complete/Misc/Hemi-Sync - The Gateway Experience [FLAC] (corrected)}"
cd "$(dirname "$0")/.." || exit 1

mapfile -t files < <(find "$ROOT" -name '*.flac' | sort)
echo "surveying ${#files[@]} tracks"

"$PY" tools/survey-carriers.py "${files[@]}" --lo 30 --hi 1000 --peaks 26
