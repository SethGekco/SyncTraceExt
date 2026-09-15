#!/usr/bin/env python3
"""Poor-man's sampling profiler for a running (wine) process. No root needed.

Repeatedly ptrace-attaches to the busiest thread of the target, reads the
instruction pointer, and detaches. Each cycle stops the thread for well under
a millisecond, so even 100 Hz sampling perturbs the game by <5%.

Works under kernel.yama.ptrace_scope=1 ONLY if the target is a descendant of
this process (launch the game from the same script — see loopback-bench.sh).
With ptrace_scope=0 it can attach to any same-user process.

Usage:
  pmp_sample.py --pid 1234 --seconds 60 --hz 50 --out samples.txt
  pmp_sample.py --auto-gamemd --seconds 60 --out samples.txt
  pmp_sample.py --warmup 45 --seconds 60 --out samples.txt -- <launch command...>

The third form launches the command itself (so the sampler is an ancestor of
every game process, which is what yama ptrace_scope=1 requires), waits
--warmup seconds for loading, finds the gamemd-spawn.exe descendant, samples
it, and leaves the game running when done (the caller owns its lifetime).

Output: one hex instruction pointer per line in --out, plus <out>.maps (a
copy of /proc/PID/maps for module attribution). Feed both to
profile_resolve.py.
"""

import argparse
import ctypes
import os
import sys
import time

PTRACE_ATTACH = 16
PTRACE_DETACH = 17
PTRACE_GETREGSET = 0x4204
NT_PRSTATUS = 1
WALL = 0x40000000

libc = ctypes.CDLL("libc.so.6", use_errno=True)
libc.ptrace.restype = ctypes.c_long
libc.ptrace.argtypes = [ctypes.c_long, ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p]


class IoVec(ctypes.Structure):
    _fields_ = [("iov_base", ctypes.c_void_p), ("iov_len", ctypes.c_size_t)]


def ptrace(req, tid, addr, data):
    ctypes.set_errno(0)
    r = libc.ptrace(req, tid, addr, data)
    return r, ctypes.get_errno()


def sample_ip(tid):
    """Attach, read IP, detach. Returns int IP or None."""
    r, _ = ptrace(PTRACE_ATTACH, tid, None, None)
    if r != 0:
        return None
    ip = None
    try:
        try:
            os.waitpid(tid, WALL)
        except ChildProcessError:
            pass
        buf = (ctypes.c_char * 256)()
        iov = IoVec(ctypes.cast(buf, ctypes.c_void_p), 256)
        r, _ = ptrace(PTRACE_GETREGSET, tid, NT_PRSTATUS, ctypes.byref(iov))
        if r == 0:
            raw = bytes(buf)
            if iov.iov_len >= 216:      # x86_64 user_regs_struct, rip at qword 16
                ip = int.from_bytes(raw[128:136], "little")
            elif iov.iov_len >= 68:     # i386 user_regs_struct, eip at dword 12
                ip = int.from_bytes(raw[48:52], "little")
    finally:
        ptrace(PTRACE_DETACH, tid, None, None)
    return ip


def thread_times(pid):
    """{tid: cpu_ticks} for every thread of pid."""
    out = {}
    try:
        tids = os.listdir(f"/proc/{pid}/task")
    except FileNotFoundError:
        return out
    for t in tids:
        try:
            with open(f"/proc/{pid}/task/{t}/stat", "rb") as f:
                st = f.read().decode("ascii", "replace")
            # utime+stime are the 14th/15th stat fields, counted after the
            # parenthesized comm (which may itself contain spaces)
            rest = st.rsplit(")", 1)[1].split()
            out[int(t)] = int(rest[11]) + int(rest[12])
        except (OSError, IndexError, ValueError):
            pass
    return out


def busiest_tid(pid, window=0.25):
    a = thread_times(pid)
    time.sleep(window)
    b = thread_times(pid)
    best, best_delta = None, -1
    for tid, ticks in b.items():
        d = ticks - a.get(tid, ticks)
        if d > best_delta:
            best, best_delta = tid, d
    return best


def ancestors_of_self():
    """Own ancestor pids. A wrapping shell's command line can contain the
    string 'gamemd-spawn.exe' (e.g. it launched us); matching an ancestor
    both is wrong and cannot be ptraced under yama scope 1."""
    out = set()
    pid = os.getpid()
    while pid > 1:
        out.add(pid)
        try:
            with open(f"/proc/{pid}/status") as f:
                ppid = next(int(l.split()[1]) for l in f if l.startswith("PPid:"))
        except (OSError, StopIteration):
            break
        pid = ppid
    return out


def find_gamemd():
    skip = ancestors_of_self()
    for p in os.listdir("/proc"):
        if not p.isdigit() or int(p) in skip:
            continue
        try:
            with open(f"/proc/{p}/cmdline", "rb") as f:
                cmd = f.read().replace(b"\0", b" ").lower()
        except OSError:
            continue
        if b"gamemd-spawn.exe" in cmd and b"syringe" not in cmd:
            return int(p)
    return None


def main():
    argv = sys.argv[1:]
    launch_cmd = None
    if "--" in argv:
        split = argv.index("--")
        launch_cmd = argv[split + 1:]
        argv = argv[:split]

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pid", type=int)
    ap.add_argument("--auto-gamemd", action="store_true",
                    help="find the gamemd-spawn.exe process automatically")
    ap.add_argument("--warmup", type=float, default=45.0,
                    help="seconds to wait after -- launch before sampling")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--hz", type=float, default=50.0)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)

    pid = args.pid
    if launch_cmd:
        import subprocess
        print(f"launching: {' '.join(launch_cmd)}", flush=True)
        subprocess.Popen(launch_cmd,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
        deadline = time.monotonic() + max(args.warmup, 5)
        time.sleep(5)
        while pid is None and time.monotonic() < deadline:
            pid = find_gamemd()
            if pid is None:
                time.sleep(1)
        if pid is None:
            sys.exit("launched, but no gamemd-spawn.exe appeared before warmup end")
        remain = deadline - time.monotonic()
        if remain > 0:
            print(f"gamemd pid {pid}; warming up {remain:.0f}s more", flush=True)
            time.sleep(remain)
    if pid is None and args.auto_gamemd:
        pid = find_gamemd()
        if pid is None:
            sys.exit("no gamemd-spawn.exe process found")
    if pid is None:
        sys.exit("need --pid, --auto-gamemd, or '-- <launch command>'")

    interval = 1.0 / args.hz
    deadline = time.monotonic() + args.seconds
    samples = []
    failures = 0
    tid = busiest_tid(pid) or pid
    next_rebalance = time.monotonic() + 1.0

    print(f"sampling pid {pid} (busiest thread {tid}) at {args.hz:g} Hz "
          f"for {args.seconds:g}s ...", flush=True)

    # Snapshot maps early: the process may die before we finish.
    try:
        with open(f"/proc/{pid}/maps") as f:
            maps = f.read()
        with open(args.out + ".maps", "w") as f:
            f.write(maps)
    except OSError as e:
        sys.exit(f"cannot read /proc/{pid}/maps: {e}")

    while time.monotonic() < deadline:
        if time.monotonic() >= next_rebalance:
            tid = busiest_tid(pid, window=0.05) or tid
            next_rebalance = time.monotonic() + 1.0
        ip = sample_ip(tid)
        if ip is not None:
            samples.append(ip)
        else:
            failures += 1
            if failures > 50 and not samples:
                sys.exit("ptrace keeps failing -- is the target a descendant "
                         "of this process (yama ptrace_scope=1), and same user?")
            if not os.path.isdir(f"/proc/{pid}"):
                print("target exited; stopping early", flush=True)
                break
        time.sleep(interval)

    with open(args.out, "w") as f:
        f.writelines(f"{ip:#x}\n" for ip in samples)
    print(f"{len(samples)} samples ({failures} failed) -> {args.out}")
    return 0 if samples else 1


if __name__ == "__main__":
    sys.exit(main())
