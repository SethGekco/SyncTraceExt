# SyncTraceExt

Desync forensics for Yuri's Revenge multiplayer. A small standalone Syringe
DLL that co-loads with Antares + Phobos + CnCNet and does two things:

1. **Continuous per-frame sync trace.** Every MP sim frame it appends a line
   of cheap divergence signals to `SYNCTRACE<player>.LOG`: the engine frame
   CRC, the synced RNG's internal state (draw indices + table XOR), object
   counts, and — every `Cadence` frames — full per-category object CRCs
   (infantry/units/aircraft/buildings/houses) computed through the engine's
   own `ComputeCRC`. Diff two players' logs with
   `tools/synctrace_diff.py` to get the exact first divergent frame and the
   subsystem that moved first, instead of waiting for the engine's own
   out-of-sync detection to fire frames later.

2. **Desync-dump preservation.** On a detected desync, Antares writes its
   per-object CRC state dump to `SYNC<N>.TXT` (hooking the vanilla writer
   `0x64DEA0`) — and Phobos, hooking `0x64736D` immediately after, truncates
   the same file for its event-history dump. With both loaded you always lose
   the Antares dump. SyncTraceExt hooks the two vanilla *call sites* of
   `0x64DEA0` (`0x647368`, `0x64CCBA`), lets the call run, then copies
   `SYNC<N>.TXT` to `SYNCSTATE<N>.TXT` before Phobos clobbers it. After a
   desync you get all three artifacts: Phobos's event history, Antares'
   object dump, and the per-frame trace.

## Config

`SYNCTRACE.INI` next to gamemd (all optional):

```ini
[SyncTrace]
Enabled=1        ; per-frame trace on/off
Cadence=8        ; per-category object CRCs every N frames (0 = never)
PreserveDumps=1  ; SYNC -> SYNCSTATE copy on/off
```

Parsed values are echoed in the trace header.

## Hooks (3, all verified unclaimed in the YR Hook Encyclopedia)

| Address    | Size | Purpose |
|------------|------|---------|
| `0x647327` | 0x6  | Queue_AI MP path, right after `CurrentFrameCRC` is finalized; once per sim frame; writes the trace line |
| `0x647368` | 0x5  | vanilla `call 0x64DEA0` in Queue_AI's OOS branch; re-invokes, then snapshots the dump |
| `0x64CCBA` | 0x5  | vanilla `call 0x64DEA0` in ExecuteDoList's OOS branch; same |

Known caveat: with `EnableMPSyncDebug` set the engine takes the per-slot
MPDEBUG path (`0x6516F0`) and the two dump-preserve sites never execute.

## Build

CI (GitHub Actions, MSVC) builds `DevBuild\SyncTraceExt.dll`. Load via
Syringe: add `-i=SyncTraceExt.dll` to the injection list.
