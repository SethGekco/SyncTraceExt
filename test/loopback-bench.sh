#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Automated loopback bench: two gamemd-spawn instances + AI players, run for a
# fixed duration with zero clicking, then report
#   1. sim-rate (frames reached / wall time) — the FPS yardstick
#   2. a CPU profile of instance A (tools/pmp_sample.py, works unprivileged
#      because the sampler launches instance A and is therefore its ancestor)
#   3. sync verdict (tools/synctrace_diff.py over the two SYNCTRACE logs)
#
# Launch/sync machinery adapted from CountryLimitExt/test/run-taunt-test.sh
# (see that file for the mutex/two-prefix rationale). A is authoritative:
# map, rules and DLLs are copied A->B before launch.
#
# Env overrides:
#   DURATION=180     total in-game seconds before the instances are killed
#   WARMUP=45        seconds after launch before sampling starts (loading)
#   SAMPLE_SECONDS=60  profile window
#   AIPLAYERS=6      AI count added to the 2 human slots (map must have spots)
#   SEED=13370099    lockstep seed
#   GAMESPEED=0      0 = fastest
# ---------------------------------------------------------------------------
set -uo pipefail

PREFIX_A="${PREFIX_A:-/home/rex/snap/cncra2yr/common/.wine}"
INSTALL_A="${INSTALL_A:-$PREFIX_A/drive_c/Westwood/RA2}"
PREFIX_B="${PREFIX_B:-/home/rex/.wine-ra2-b}"
INSTALL_B="${INSTALL_B:-$PREFIX_B/drive_c/Westwood/RA2}"
WINE="${WINE:-wine}"

DURATION="${DURATION:-180}"
WARMUP="${WARMUP:-45}"
SAMPLE_SECONDS="${SAMPLE_SECONDS:-60}"
AIPLAYERS="${AIPLAYERS:-6}"
SEED="${SEED:-13370099}"
GAMESPEED="${GAMESPEED:-0}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$HERE/../tools"
OUTDIR="${OUTDIR:-$HERE/bench-out}"
mkdir -p "$OUTDIR"

die() { echo "error: $*" >&2; exit 1; }

[[ -d "$INSTALL_A" ]] || die "install A missing: $INSTALL_A"
[[ -d "$INSTALL_B" ]] || die "install B missing: $INSTALL_B (run CountryLimitExt/test/clone-install.sh)"

# BENCHMAP=/path/to/8player.map installs that map as the bench spawnmap.
if [[ -n "${BENCHMAP:-}" ]]; then
    [[ -f "$BENCHMAP" ]] || die "BENCHMAP not found: $BENCHMAP"
    cp -f "$BENCHMAP" "$INSTALL_A/spawnmap.ini"
fi
[[ -f "$INSTALL_A/spawnmap.ini" ]] || die "no spawnmap.ini in A — host any game once, or pass BENCHMAP=/path/to/map. It needs >= $((AIPLAYERS+2)) start positions."
STARTS=$(sed -n '/^\[Waypoints\]/,/^\[/p' "$INSTALL_A/spawnmap.ini" | grep -cE '^[0-7]=')
[[ "$STARTS" -ge $((AIPLAYERS+2)) ]] || die "spawnmap has $STARTS start positions, need $((AIPLAYERS+2)) (lower AIPLAYERS or pass a bigger BENCHMAP)"

# --- sync B to A (map, rules, DLLs) ----------------------------------------
cp -f "$INSTALL_A/spawnmap.ini" "$INSTALL_B/spawnmap.ini"
for f in rulesmd.ini artmd.ini aimd.ini; do
    [[ -f "$INSTALL_A/$f" ]] && cp -f "$INSTALL_A/$f" "$INSTALL_B/$f"
done

# DLL list: ClientDefinitions.ini is what CnCNet games actually inject.
# (Since 2026-09 it holds ONLY the -i list; exe + game flags live elsewhere.)
CDEF="$INSTALL_A/Resources/ClientDefinitions.ini"
PARAMS=$(grep -m1 '^ExtraCommandLineParams=' "$CDEF") || die "no ExtraCommandLineParams in $CDEF"
mapfile -t INJECT < <(grep -oE '\-i=[A-Za-z0-9_.-]+\.dll' <<<"$PARAMS" | sort -u)
[[ ${#INJECT[@]} -gt 0 ]] || die "could not parse -i list"

# DROP="DoctrineExt.dll IntelExt.dll" removes DLLs from the injection list —
# for bisecting which Ext DLL is responsible for a load crash. Both instances
# get the same reduced list (they must stay identical to avoid a desync).
if [[ -n "${DROP:-}" ]]; then
    for d in $DROP; do
        mapfile -t INJECT < <(printf '%s\n' "${INJECT[@]}" | grep -v "\-i=$d\$")
    done
    echo "DROP: removed [$DROP]; ${#INJECT[@]} DLLs remain"
fi

# Game flags: parse the wine-game.sh launch line (everything after the exe
# name that starts with '-', minus any stray -i= tokens).
WGS="$INSTALL_A/Resources/Compatibility/Unix/wine-game.sh"
mapfile -t GAME_ARGS < <(grep -m1 'Syringe\.exe .*gamemd-spawn\.exe' "$WGS" 2>/dev/null \
    | sed 's/.*gamemd-spawn\.exe"\{0,1\}//' | tr -s ' ' '\n' \
    | grep -E '^-' | grep -v '^-i=')
[[ ${#GAME_ARGS[@]} -gt 0 ]] || GAME_ARGS=(-SPAWN -LOG -CD)
echo "game args: ${GAME_ARGS[*]}"
case " ${GAME_ARGS[*]} " in *" -SPAWN "*) ;; *) GAME_ARGS=(-SPAWN "${GAME_ARGS[@]}");; esac

for tok in "${INJECT[@]}"; do
    dll="${tok#-i=}"
    [[ -f "$INSTALL_A/$dll" ]] || die "DLL missing in A: $dll"
    cmp -s "$INSTALL_A/$dll" "$INSTALL_B/$dll" || cp -f "$INSTALL_A/$dll" "$INSTALL_B/$dll"
done
echo "synced: spawnmap, rules, ${#INJECT[@]} DLLs (A->B)"

# --- spawn.ini for both sides ----------------------------------------------
# The two humans are slots Multi1 (=[Settings]) and Multi2 (=[Other1]); the
# AIs are slots Multi3..Multi(2+AIPLAYERS) — they have NO [OtherN] section, so
# the spawner reads their country/color/difficulty from [HouseCountries] /
# [HouseColors] / [HouseHandicaps] by Multi tag, and their start position from
# [SpawnLocations]. Those defaults are Country=-1 / SpawnLocations=-2; a -1
# country index makes the engine dereference pre-array memory as a country
# name and crash at load (C0000005). So EVERY slot must be given a valid
# country (0..8), a distinct colour, a difficulty, and a distinct waypoint —
# exactly what a real client-generated spawn.ini contains.
NHOUSES=$(( AIPLAYERS + 2 ))
COUNTRIES=(0 1 2 3 4 5 6 7 8)   # valid playable YR country indices

write_spawn() { # $1=file $2=name $3=listenport $4=side $5=color $6=peername $7=peerport $8=peerside $9=peercolor
    {
    cat <<EOF
; generated by loopback-bench.sh
[Settings]
Name=$2
Port=$3
Side=$4
Color=$5
IsSpectator=False
Scenario=spawnmap.ini
UIMapName=Loopback Bench
UIGameMode=Standard
GameMode=1
Seed=$SEED
AIPlayers=$AIPLAYERS
TechLevel=10
Bases=Yes
Credits=10000
UnitCount=10
ShortGame=False
Superweapons=False
Crates=False
BridgeDestroy=True
MultiEngineer=False
AlliesAllowed=False
BuildOffAlly=No
HarvesterTruce=False
FogOfWar=No
MCVRedeploy=True
GameSpeed=$GAMESPEED
Protocol=0
FrameSendRate=2
ReconnectTimeout=2400
DisableGameSpeed=False
AINamesByDifficulty=Yes

[Other1]
Name=$6
Ip=127.0.0.1
Port=$7
Side=$8
Color=$9
IsSpectator=False
EOF

    # AI houses occupy Multi3..Multi(NHOUSES). Give each a valid country and a
    # difficulty; humans keep the Side/Color from their own sections but still
    # need spawn locations and colours below.
    echo; echo "[HouseCountries]"
    for i in $(seq 3 "$NHOUSES"); do echo "Multi$i=${COUNTRIES[$(( (i-1) % ${#COUNTRIES[@]} ))]}"; done
    echo; echo "[HouseColors]"
    for i in $(seq 1 "$NHOUSES"); do echo "Multi$i=$(( (i-1) % 8 ))"; done
    echo; echo "[HouseHandicaps]"
    for i in $(seq 3 "$NHOUSES"); do echo "Multi$i=1"; done   # 1 = medium AI
    # Distinct start waypoint per house (map validated to have >= NHOUSES).
    echo; echo "[SpawnLocations]"
    for i in $(seq 1 "$NHOUSES"); do echo "Multi$i=$(( i-1 ))"; done
    } > "$1"
}

write_spawn "$INSTALL_A/spawn.ini" BenchA 8054 0 0 BenchB 8055 1 1
write_spawn "$INSTALL_B/spawn.ini" BenchB 8055 1 1 BenchA 8054 0 0

# --- stale logs out of the way ----------------------------------------------
rm -f "$INSTALL_A"/SYNCTRACE*.LOG "$INSTALL_B"/SYNCTRACE*.LOG

cleanup() {
    pkill -f 'gamemd-spawn' 2>/dev/null
    pkill -f 'Syringe.exe' 2>/dev/null
}
trap cleanup EXIT

# --- launch ------------------------------------------------------------------
# Instance A is launched THROUGH the sampler so the sampler is an ancestor of
# the game process — required for ptrace under kernel.yama.ptrace_scope=1.
LOG_A="$OUTDIR/launch-A.log"; LOG_B="$OUTDIR/launch-B.log"
# The exe is passed as ' gamemd-spawn.exe' (leading space): wine re-quotes
# argv elements containing spaces when building the Windows command line, and
# Syringe REQUIRES the exe name quoted or it exits with "Invalid command line
# arguments given" — the same load-bearing trick as wine-game.sh.
LAUNCH_A="cd $(printf %q "$INSTALL_A") && WINEPREFIX=$(printf %q "$PREFIX_A")"
LAUNCH_A+=" exec $(printf %q "$WINE") Syringe.exe"
for t in "${INJECT[@]}"; do LAUNCH_A+=" $(printf %q "$t")"; done
LAUNCH_A+=" $(printf %q ' gamemd-spawn.exe')"
for t in "${GAME_ARGS[@]}"; do LAUNCH_A+=" $(printf %q "$t")"; done

echo "launching A (via sampler, warmup ${WARMUP}s, sample ${SAMPLE_SECONDS}s) ..."
python3 "$TOOLS/pmp_sample.py" --warmup "$WARMUP" --seconds "$SAMPLE_SECONDS" \
    --hz 50 --out "$OUTDIR/samples.txt" -- bash -c "$LAUNCH_A" >"$LOG_A" 2>&1 &
SAMPLER=$!
sleep 5
echo "launching B ..."
( cd "$INSTALL_B" && WINEPREFIX="$PREFIX_B" "$WINE" Syringe.exe "${INJECT[@]}" \
    ' gamemd-spawn.exe' "${GAME_ARGS[@]}" ) >"$LOG_B" 2>&1 &

T0=$(date +%s)
sleep 12
# Count REAL game processes: cmdline mentions the exe but is not the
# Syringe/wine wrapper (whose cmdline also contains 'gamemd-spawn.exe').
count_games() {
    local n=0 f
    for f in /proc/[0-9]*/cmdline; do
        if grep -qa 'gamemd-spawn' "$f" 2>/dev/null && ! grep -qia 'syringe' "$f" 2>/dev/null; then
            n=$((n+1))
        fi
    done
    echo "$n"
}
LIVE=$(count_games)
[[ "$LIVE" -ge 2 ]] || { tail -n 6 "$LOG_A" "$LOG_B" >&2; die "expected 2 game processes, found $LIVE"; }
echo "both instances up."

wait "$SAMPLER" 2>/dev/null || echo "warning: sampler exited nonzero (see $LOG_A)"

# If the game died during/after sampling (e.g. a load crash), don't sit idle
# for the rest of DURATION — report what we have.
if [[ "$(count_games)" -lt 1 ]]; then
    echo "WARNING: no game processes remain after sampling — likely a load/runtime crash."
    echo "  check $LOG_A and the game's debug/ folder (snapshot-*/except.txt)."
fi

# --- let the rest of the duration play out ----------------------------------
ELAPSED=$(( $(date +%s) - T0 ))
REMAIN=$(( DURATION - ELAPSED ))
[[ $REMAIN -gt 0 && "$(count_games)" -ge 1 ]] && sleep "$REMAIN"
T1=$(date +%s)
cleanup
trap - EXIT
sleep 2

# --- report ------------------------------------------------------------------
echo; echo "===== bench report ====="
WALL=$(( T1 - T0 ))
TR_A=$(ls -t "$INSTALL_A"/SYNCTRACE*.LOG 2>/dev/null | head -1)
TR_B=$(ls -t "$INSTALL_B"/SYNCTRACE*.LOG 2>/dev/null | head -1)
if [[ -n "$TR_A" ]]; then
    FRAMES=$(tail -1 "$TR_A" | grep -oE '^F=[0-9]+' | cut -d= -f2)
    echo "instance A trace: $TR_A  (${FRAMES:-?} frames in ~${WALL}s wall incl. load)"
    [[ -n "${FRAMES:-}" ]] && echo "  effective sim rate >= $(( FRAMES / WALL )) fps (underestimates: wall includes loading)"
else
    echo "NO SYNCTRACE log in A — game may not have entered the MP frame path; check $LOG_A"
fi
if [[ -n "$TR_A" && -n "$TR_B" ]]; then
    cp "$TR_A" "$OUTDIR/trace-A.log"; cp "$TR_B" "$OUTDIR/trace-B.log"
    python3 "$TOOLS/synctrace_diff.py" "$OUTDIR/trace-A.log" "$OUTDIR/trace-B.log" | tail -12
fi
if [[ -s "$OUTDIR/samples.txt" ]]; then
    echo; python3 "$TOOLS/profile_resolve.py" "$OUTDIR/samples.txt" --top 25
fi
echo "artifacts in $OUTDIR"
