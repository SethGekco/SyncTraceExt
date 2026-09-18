#!/usr/bin/env python3
"""Decide whether the dirty-rect merge (@0x6D2790) is quadratic, from RENDERPROBE.CSV.

Usage: renderprobe_fit.py RENDERPROBE.CSV

Each data row is one time window:
  tick, period_calls, maxN, work(=sum N*count), hist(N:count ...)

Two independent tests:

1. Per-window shape. Within a window the rect list is rebuilt every frame,
   growing 0->M. If each insert scans the current size (O(N)), the histogram is
   ~flat across [0,M] and work ~= frames * M^2/2. We estimate frames from the
   low-N bins and check work / (frames * M^2/2) ~= 1 (quadratic) vs work ~=
   frames*M (linear, histogram spiked near M).

2. Cross-window scaling. As scenes get busier maxN rises. Fit
   work_per_frame ~ maxN^p by least squares on log-log. p~=2 => quadratic,
   p~=1 => linear. This is the headline number.
"""

import sys
import math


def parse(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",", 4)
            if len(parts) < 5:
                continue
            try:
                tick = int(parts[0]); calls = int(parts[1])
                maxN = int(parts[2]); work = int(parts[3])
            except ValueError:
                continue
            hist = {}
            for tok in parts[4].split():
                if ":" in tok:
                    n, c = tok.split(":")
                    hist[int(n)] = int(c)
            rows.append(dict(tick=tick, calls=calls, maxN=maxN, work=work, hist=hist))
    return rows


def est_frames(hist):
    # the list resets to ~0 each frame; count of calls at small N ~= frames
    return max(1, sum(c for n, c in hist.items() if n <= 2))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    rows = [r for r in parse(sys.argv[1]) if r["maxN"] >= 4 and r["calls"] > 50]
    if len(rows) < 3:
        sys.exit("need >=3 substantial windows (maxN>=4). Play a busier/longer "
                 "game with RenderProbe Enabled=1, or lower DumpSeconds.")

    print(f"{len(rows)} usable windows\n")
    print(f"{'maxN':>6} {'calls':>8} {'frames~':>7} {'work':>12} "
          f"{'work/frame':>11} {'/ (M^2/2)':>10}   shape")
    xs, ys = [], []
    for r in rows:
        F = est_frames(r["hist"])
        M = r["maxN"]
        wpf = r["work"] / F
        ratio = wpf / (M * M / 2.0) if M else 0.0     # ~1 => quadratic-flat
        lin = wpf / M if M else 0.0                    # ~1*F..M => linear-ish
        shape = "QUADRATIC-flat" if 0.4 <= ratio <= 1.6 else \
                ("linear-ish" if lin < M * 0.25 else "mixed")
        print(f"{M:>6} {r['calls']:>8} {F:>7} {r['work']:>12} "
              f"{wpf:>11.0f} {ratio:>10.2f}   {shape}")
        if wpf > 0 and M > 0:
            xs.append(math.log(M)); ys.append(math.log(wpf))

    # least-squares slope p in  log(work/frame) = p*log(maxN) + b
    n = len(xs)
    mx = sum(xs) / n; my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    p = sxy / sxx if sxx else float("nan")

    print("\n== cross-window fit: work_per_frame ~ maxN^p ==")
    print(f"  p = {p:.2f}")
    if p >= 1.6:
        print("  => QUADRATIC (p~2): per-frame render work grows with the SQUARE of")
        print("     scene density. Optimizing the merge is worthwhile.")
    elif p <= 1.3:
        print("  => ~LINEAR (p~1): cost scales with object count, not its square;")
        print("     the merge is not the quadratic. Look elsewhere.")
    else:
        print("  => between linear and quadratic; collect more range in maxN")
        print("     (calmer + busier moments) to sharpen the exponent.")


if __name__ == "__main__":
    main()
