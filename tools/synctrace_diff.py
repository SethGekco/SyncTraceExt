#!/usr/bin/env python3
"""Diff two SYNCTRACE<N>.LOG files and report the first divergent frame.

Usage: synctrace_diff.py SYNCTRACE0.LOG SYNCTRACE1.LOG

Each trace line looks like:
  F=123 CRC=DEADBEEF RN=17,42 RT=01234567 NI=5 NU=3 NA=0 NB=12 NH=4 [CI=.. CU=.. CA=.. CB=.. CH=..]

The tool aligns lines by frame number (F=) and compares field by field.
Field meanings:
  CRC  engine frame CRC            RN  RNG Next1,Next2 indices
  RT   XOR of the RNG table        NI/NU/NA/NB/NH object counts
  CI/CU/CA/CB/CH per-category object CRCs (infantry/units/aircraft/
  buildings/houses), present every Cadence frames only.

Exit status: 0 = traces identical over the compared range, 1 = divergence
found, 2 = usage/parse error.
"""

import re
import sys

FIELD_HINTS = {
    "CRC": "engine frame CRC (any sim state the engine checksums)",
    "RN": "RNG draw count differs -> something called the synced RNG a different number of times",
    "RT": "RNG table content differs -> different values were drawn",
    "NI": "infantry count differs",
    "NU": "unit count differs",
    "NA": "aircraft count differs",
    "NB": "building count differs",
    "NH": "house count differs",
    "CI": "infantry state differs (position/health/mission/facing...)",
    "CU": "unit state differs",
    "CA": "aircraft state differs",
    "CB": "building state differs",
    "CH": "house state differs (money/visibility/allies...)",
}

LINE_RE = re.compile(r"F=(\d+) (.*)")


def parse(path):
    frames = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = LINE_RE.match(line.strip())
            if not m:
                continue
            fields = {}
            for token in m.group(2).split():
                if "=" in token:
                    key, _, val = token.partition("=")
                    fields[key] = val
            frames[int(m.group(1))] = fields
    return frames


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        return 2

    a, b = parse(sys.argv[1]), parse(sys.argv[2])
    if not a or not b:
        print("error: one of the traces has no parsable F= lines")
        return 2

    common = sorted(set(a) & set(b))
    if not common:
        print("error: the traces share no frame numbers")
        return 2

    print(f"{sys.argv[1]}: frames {min(a)}..{max(a)} ({len(a)} lines)")
    print(f"{sys.argv[2]}: frames {min(b)}..{max(b)} ({len(b)} lines)")
    print(f"comparing {len(common)} common frames {common[0]}..{common[-1]}")

    for frame in common:
        fa, fb = a[frame], b[frame]
        diffs = [k for k in fa if k in fb and fa[k] != fb[k]]
        if diffs:
            print(f"\nFIRST DIVERGENCE at frame {frame}:")
            for key in diffs:
                hint = FIELD_HINTS.get(key, "")
                print(f"  {key}: {fa[key]} vs {fb[key]}   {hint}")
            # Context: last matching frame's line for orientation
            prev = [f for f in common if f < frame]
            if prev:
                print(f"  (last identical compared frame: {prev[-1]})")
            print("\nNext step: open the SYNCSTATE<N>.TXT dumps (Antares object CRCs)")
            print("and SYNC<N>.TXT (Phobos event history incl. RNG callers) around")
            print(f"frame {frame} to identify the exact object and code path.")
            return 1

    print("\nno divergence in any compared field over the common range.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
