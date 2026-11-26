#!/bin/bash
# BPX-1 bench/ext test: build the MiBench programs of programs_ext.txt natively (host gcc, host C
# library) with the same drivers and the same scale, and run them.
#
#   bash bench/ext/test/mibench_host.sh
#
# The native run repeats the self-checks of the drivers on a second machine (x86-64 instead of
# RV32IM under Spike), adds the checks that are too slow for the benchmark (dijkstra: every
# distance against Bellman-Ford), and prints each checksum next to the GOLDEN of the manifest.
# This is also how a GOLDEN is obtained after a change of scale: put any value, run, copy the fold.
# The sources must have been fetched (build_ext.sh --fetch). Binaries go to out/ext/test/.
# Exit status 0 when every program passes.
[ -f "$HOME/ttsetup/env.sh" ] && source "$HOME/ttsetup/env.sh"
set -u -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXT="$(dirname "$HERE")"
ROOT="$(dirname "$(dirname "$EXT")")"
SRC="$ROOT/out/ext/src/mibench"
T="$ROOT/out/ext/test"
mkdir -p "$T"

rc=0
while read -r name scale; do
    prog="${name#mibench.}"
    defs=(-DBPX_HOST -Dmain=mibench_upstream_main -include stdlib.h)
    IFS=, read -ra parts <<<"$scale"
    for part in "${parts[@]}"; do defs+=("-D$part"); done
    incs=(-I"$EXT/mibench")
    case "$prog" in
        bitcount) incs+=(-I"$SRC/automotive/bitcount")
                  srcs=("$SRC/automotive/bitcount"/bitcnt_1.c "$SRC/automotive/bitcount"/bitcnt_2.c
                        "$SRC/automotive/bitcount"/bitcnt_3.c "$SRC/automotive/bitcount"/bitcnt_4.c) ;;
        dijkstra) srcs=("$SRC/network/dijkstra/dijkstra_small.c") ;;
        adpcm)    incs+=(-I"$SRC/telecomm/adpcm/src")
                  srcs=("$SRC/telecomm/adpcm/src/adpcm.c") ;;
        *)        echo "FAIL $name: no driver"; rc=1; continue ;;
    esac
    if ! gcc -O2 -w "${defs[@]}" "${incs[@]}" "${srcs[@]}" "$EXT/mibench/${prog}_main.c" -o "$T/host_$prog"; then
        echo "FAIL $name: native build"; rc=1; continue
    fi
    "$T/host_$prog" >/dev/null                  # the MiBench sources print to stdout, the driver to stderr
    status=$?
    if [ "$status" -eq 0 ]; then echo "ok   $name (native, $scale)"; else echo "FAIL $name: native exit status $status"; rc=1; fi
done < <(awk 'NF >= 5 && $1 !~ /^#/ && $2 == "mibench" { print $1, $5 }' "$EXT/programs_ext.txt")
exit $rc
