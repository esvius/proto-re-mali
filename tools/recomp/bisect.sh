#!/bin/sh
# Bisects the first engine function whose recompiled version breaks the game (PROTORE_RECOMP=all + RANGE).
# A run passes when the game is still running after $RUN_SECONDS without traps. Assumes the failure is caused by a
# single function and that recompiling more functions never fixes it (the smallest failing prefix is found).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
GAME=${PROTORE_GAME:-$HOME/proto-re-data/game}
RUN_SECONDS=${RUN_SECONDS:-70}
export WINEPREFIX=${WINEPREFIX:-$HOME/proto-re-data/wineprefix} WINEDEBUG=-all DXVK_LOG_LEVEL=none
LIST=$(mktemp)
awk -F, 'NR>1 && $3=="false" {print $1}' "$ROOT/re/functions.csv" | sort > "$LIST"
N=$(wc -l < "$LIST")
va_at() { sed -n "$(( $1 + 1 ))p" "$LIST"; }

MIN_FRAMES=${MIN_FRAMES:-64}

# once <hi>: one run with functions below <hi> recompiled. Passes when the game is still running after
# $RUN_SECONDS, hit no trap, and the engine ran at least $MIN_FRAMES frames (EngineService calls).
once() {
    rm -f "$GAME/proto-re.log"
    (cd "$GAME" && PROTORE_RECOMP=all PROTORE_RECOMP_RANGE="0-$1" timeout "$RUN_SECONDS" wine prototypef.exe >/dev/null 2>&1)
    code=$?
    wineserver -k 2>/dev/null
    frames=$(grep -oE "recomp: frames [0-9]+" "$GAME/proto-re.log" 2>/dev/null | tail -1 | awk '{print $3}')
    frames=${frames:-0}
    if [ $code -eq 124 ] && [ "$frames" -ge "$MIN_FRAMES" ] && ! grep -q "unimplemented" "$GAME/proto-re.log"; then
        echo pass
    else
        echo "fail(exit $code, frames $frames)"
    fi
}

# run <count>: recompile the first <count> functions. A step passes if either of two runs passes, so a single
# flaky failure cannot steer the search.
run() {
    count=$1
    if [ "$count" -ge "$N" ]; then hi=ffffffff; else hi=$(va_at "$count"); fi
    r=$(once "$hi")
    case $r in pass) echo pass; return;; esac
    r2=$(once "$hi")
    case $r2 in pass) echo "pass (after $r)"; return;; esac
    echo "$r / $r2"
}

lo=${START_LO:-0} hi=$N   # invariant: prefix lo passes, prefix hi fails
echo "functions: $N"
r=$(run $hi); echo "all: $r"
case $r in pass*) echo "whole engine passes"; exit 0;; esac
while [ $((hi - lo)) -gt 1 ]; do
    mid=$(( (lo + hi) / 2 ))
    r=$(run $mid)
    echo "prefix $mid (< $(va_at $mid)): $r"
    case $r in pass*) lo=$mid;; *) hi=$mid;; esac
done
echo "first failing function: index $((hi - 1)) va $(va_at $((hi - 1)))"
rm -f "$LIST"
