#!/bin/sh
# Disassemble whole engine functions by address, bounded by their size in re/functions.csv.
# Usage: tools/disfn.sh <hex address without 0x> [...]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DLL=${PROTORE_ENGINE:-"$HOME/Downloads/Prototype (2009)/Prototype/prototypeenginef.dll"}
for addr in "$@"; do
    size=$(awk -F, -v A="$addr" '("" $1) == ("" A) && $3 == "false" {print $2; exit}' "$ROOT/re/functions.csv")
    if [ -z "$size" ]; then
        echo "=== $addr: not a known function" >&2
        continue
    fi
    start=$((0x$addr))
    echo "=== $addr ($size bytes)"
    llvm-objdump -d --x86-asm-syntax=intel --no-show-raw-insn \
        --start-address=$start --stop-address=$((start + size)) "$DLL" |
        grep -E '^ *[0-9a-f]+:' | sed -E 's/<[^>]*>//; s/^ +//; s/[[:space:]]+/ /g'
done
