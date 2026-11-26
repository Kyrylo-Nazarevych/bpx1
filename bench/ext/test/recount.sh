#!/bin/bash
# BPX-1 bench/ext test: recount programs from a raw Spike log with recount.py (a method that shares
# nothing with rvtrace) and compare with the totals stored in prog.evt.
#
#   bash bench/ext/test/recount.sh embench.crc32 coremark
#
# The program must have been built (out/bench/<name>/prog.elf and prog.evt). Spike runs again; its
# log is piped into recount.py and not stored. Exit status 0 when every program matches.
[ -f "$HOME/ttsetup/env.sh" ] && source "$HOME/ttsetup/env.sh"
set -u -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$(dirname "$(dirname "$HERE")")")"
OUT="$ROOT/out/bench"
SPIKE="${SPIKE:-$HOME/riscv-tools/bin/spike}"
CROSS="${CROSS:-riscv64-unknown-elf-}"

rc=0
for name in "$@"; do
    d="$OUT/$name"
    if [ ! -f "$d/prog.elf" ] || [ ! -f "$d/prog.evt" ]; then echo "recount: $name is not built"; rc=1; continue; fi
    # .text bounds from the section table, the store to tohost from the symbol table
    read -r size lo < <("${CROSS}objdump" -h "$d/prog.elf" | awk '$2 == ".text" { print $3, $4 }')
    hi="$(printf '%x' $((0x$lo + 0x$size)))"
    store="$("${CROSS}nm" "$d/prog.elf" | awk '$3 == "__tohost_store" { print $1 }')"
    echo "== $name  .text [0x$lo, 0x$hi)  tohost store at 0x$store"
    # recount.py stops reading at the store; Spike may then end with SIGPIPE, which is not an error.
    "$SPIKE" --isa=rv32im -l --log=/dev/stdout "$d/prog.elf" 2>/dev/null |
        python3 -I "$HERE/recount.py" "$d/prog.evt" "$lo" "$hi" "$store"
    [ "${PIPESTATUS[1]}" -eq 0 ] || rc=1
done
exit $rc
