#!/usr/bin/env python3
"""Resolve pmp_sample.py output into a ranked hot-spot report.

Usage:
  profile_resolve.py samples.txt [--maps samples.txt.maps] [--symbols FILE] [--top 25]

Attribution:
  - IPs inside gamemd's fixed image (0x400000..0xB80000): nearest preceding
    symbol from the Antares-PDB symbol map (default
    ~/Claude/Antares/gamemd_names_from_antares_pdb.txt, '0xADDR Name' lines).
    The map names only ~1464 functions, so unknown-but-hot addresses are
    reported as gamemd+0xADDR buckets (64-byte granularity) — take those to
    the disassembler.
  - Other IPs: the module owning the address range in the maps snapshot
    (Ext DLLs, Phobos.dll, wine's ntdll/win32u, libc...).

Interpretation for the lag question:
  - Samples concentrated in gamemd sim functions  -> CPU-bound: fix that code.
  - Samples concentrated in ntdll/wineserver wait -> the busy thread is
    blocked: in MP that usually means the frame limiter / waiting on peers,
    i.e. network-bound, and no engine optimization will help.
"""

import argparse
import bisect
import os
import re
import sys
from collections import Counter

GAMEMD_BASE = 0x400000
GAMEMD_END = 0xB80000  # image top incl. data; .text ends lower but keep wide
DEFAULT_SYMBOLS = os.path.expanduser(
    "~/Claude/Antares/gamemd_names_from_antares_pdb.txt")

WAIT_MODULES = ("ntdll", "wineserver", "kernelbase", "kernel32", "win32u",
                "libpthread", "libc.so")


def load_symbols(path):
    addrs, names = [], []
    try:
        with open(path) as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2 and parts[0].startswith("0x"):
                    addrs.append(int(parts[0], 16))
                    names.append(parts[1])
    except OSError as e:
        print(f"warning: no symbol map ({e}); gamemd IPs shown as raw buckets",
              file=sys.stderr)
    pairs = sorted(zip(addrs, names))
    return [a for a, _ in pairs], [n for _, n in pairs]


def load_maps(path):
    regions = []
    if not path or not os.path.exists(path):
        return regions
    rx = re.compile(r"^([0-9a-f]+)-([0-9a-f]+)\s+\S+\s+\S+\s+\S+\s+\S+\s*(.*)$")
    with open(path) as f:
        for line in f:
            m = rx.match(line.strip())
            if m:
                lo, hi = int(m.group(1), 16), int(m.group(2), 16)
                name = m.group(3).strip() or "<anon>"
                regions.append((lo, hi, os.path.basename(name)))
    regions.sort()
    return regions


def attribute(ip, sym_addrs, sym_names, regions):
    if GAMEMD_BASE <= ip < GAMEMD_END:
        i = bisect.bisect_right(sym_addrs, ip) - 1
        off = ip - sym_addrs[i] if i >= 0 else 1 << 30
        # The Antares PDB map is PARTIAL: a large offset means the real
        # (unnamed) function is elsewhere and this name is just the nearest
        # preceding label -- do NOT trust it. Only small offsets are real hits.
        if off < 0x400:
            return f"gamemd!{sym_names[i]}", "gamemd"
        if off < 0x1000:
            return f"gamemd!{sym_names[i]}(approx+{off:#x})", "gamemd"
        return f"gamemd+{(ip & ~0x3F):#x} (unnamed)", "gamemd"
    lo = bisect.bisect_right([r[0] for r in regions], ip) - 1
    if 0 <= lo < len(regions) and regions[lo][0] <= ip < regions[lo][1]:
        base, name = regions[lo][0], regions[lo][2]
        if name == "<anon>":
            # anonymous executable memory: wine JIT / mapped system libs, or
            # small allocations like Syringe hook trampolines
            return f"<anon {base:#x}>", "<anon>"
        return name, name
    return f"?{ip:#x}", "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples")
    ap.add_argument("--maps")
    ap.add_argument("--symbols", default=DEFAULT_SYMBOLS)
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    maps_path = args.maps or args.samples + ".maps"
    sym_addrs, sym_names = load_symbols(args.symbols)
    regions = load_maps(maps_path)

    ips = []
    with open(args.samples) as f:
        for line in f:
            line = line.strip()
            if line:
                ips.append(int(line, 16))
    if not ips:
        sys.exit("no samples")

    fine = Counter()
    modules = Counter()
    for ip in ips:
        label, mod = attribute(ip, sym_addrs, sym_names, regions)
        fine[label] += 1
        modules[mod] += 1

    n = len(ips)
    print(f"{n} samples\n")
    print("== by module ==")
    for mod, c in modules.most_common():
        print(f"  {c/n:6.1%}  {mod}")

    gamemd_pct = modules.get("gamemd", 0) / n
    wait_pct = sum(c for m, c in modules.items()
                   if any(w in m.lower() for w in WAIT_MODULES)) / n
    print("\n== verdict hint ==")
    print(f"  in-engine (gamemd): {gamemd_pct:.1%}   "
          f"system/wait-ish: {wait_pct:.1%}")
    if gamemd_pct > 0.5:
        print("  -> mostly CPU-bound inside the engine: the functions below "
              "are the optimization targets.")
    elif wait_pct > 0.5:
        print("  -> the busy thread is mostly in system/wait code: likely "
              "frame-limited or waiting on network peers, not sim CPU.")
    else:
        print("  -> mixed; check the breakdown below.")

    print(f"\n== top {args.top} locations ==")
    for label, c in fine.most_common(args.top):
        print(f"  {c/n:6.1%}  {label}")


if __name__ == "__main__":
    main()
