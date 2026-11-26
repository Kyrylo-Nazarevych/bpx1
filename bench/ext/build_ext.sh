#!/bin/bash
# BPX-1 bench/ext: fetch (pinned), build, run (Spike) and trace the external benchmark programs
# listed in programs_ext.txt.
#
#   bash bench/ext/build_ext.sh                          every program of the manifest
#   bash bench/ext/build_ext.sh embench.crc32 coremark   only these
#   bash bench/ext/build_ext.sh --fetch                  only fetch the sources
#
# Sources: each suite is fetched with git from its public repository (suite_url below) at the commit
# pinned in programs_ext.txt, into <project>/out/ext/src/<suite>/ (outside the repository; nothing
# of it is ever copied into the repository). A checkout that is already at the pinned commit is not
# touched; one that differs from that commit makes the build fail.
#
# Output, per program:  <project>/out/bench/<name>/{prog.elf, prog.evt, prog.text, summary.json}
# (formats: bench/README.md), and for all programs <project>/out/ext/summary_ext.tsv (rewritten on
# every call from the summary.json files; same columns as out/bench/summary.tsv).
# A program that fails keeps <name>/build.log (compiler, Spike and rvtrace messages); the exit
# status is then 1. Compiler warnings about the third-party sources are not shown.
# Spike's instruction log is piped straight into rvtrace and never stored. rvtrace is compiled from
# bench/rvtrace.cc into <project>/out/ext/rvtrace.
#
# The programs are built like the in-house ones (bench/build.sh): same compiler flags, same
# runtime (bench/rt: crt0.S, link.ld, rt.c), plus bench/ext/libc (the C library functions that the
# external sources call) and one port per suite (bench/ext/embench, coremark, mibench). The runtime
# objects and the library are compiled once per call into <project>/out/ext/lib/.
# A program passes when Spike exits with status 0, i.e. the suite's own self-check accepted the run
# (README.md says what each check is).
#
# Environment: JOBS  programs run in parallel (default 6)
#              SPIKE path of Spike (default ~/riscv-tools/bin/spike)
#              CROSS toolchain prefix (default riscv64-unknown-elf-)
#              TIMEOUT seconds before a Spike run is stopped (default 1800)
[ -f "$HOME/ttsetup/env.sh" ] && source "$HOME/ttsetup/env.sh"
set -u -o pipefail

EXT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="$(dirname "$EXT")"
ROOT="$(dirname "$BENCH")"
OUT="$ROOT/out/bench"
XOUT="$ROOT/out/ext"
SRC="$XOUT/src"
MANIFEST="$EXT/programs_ext.txt"
SPIKE="${SPIKE:-$HOME/riscv-tools/bin/spike}"
CROSS="${CROSS:-riscv64-unknown-elf-}"
JOBS="${JOBS:-6}"
TIMEOUT="${TIMEOUT:-1800}"
RVTRACE="$XOUT/rvtrace"
LIBDIR="$XOUT/lib"

# As bench/build.sh, without -Wall. An undeclared function is an error: its arguments and result
# would silently be passed as int.
CFLAGS=(-march=rv32im -mabi=ilp32 -O2 -ffreestanding -fno-pic -static -nostdlib -nostartfiles
        -fno-tree-loop-distribute-patterns -Werror=implicit-function-declaration)
LDFLAGS=(-Wl,--no-warn-rwx-segments -lgcc)

# Public repository of each suite.
suite_url() {
    case "$1" in
        embench)  echo "https://github.com/embench/embench-iot" ;;
        coremark) echo "https://github.com/eembc/coremark" ;;
        mibench)  echo "https://github.com/embecosm/mibench" ;;
        *)        return 1 ;;
    esac
}

# Directories of a suite to check out; empty = the whole tree. (The MiBench repository holds
# hundreds of megabytes of input files that are not needed.)
suite_dirs() {
    case "$1" in
        mibench) echo "automotive/bitcount network/dijkstra telecomm/adpcm/src" ;;
    esac
}

# Check out exactly <commit> of <suite> into $SRC/<suite>. Does nothing when it is already there.
fetch_suite() {
    local suite="$1" commit="$2" url d head dirs
    url="$(suite_url "$suite")" || { echo "FAIL fetch $suite: unknown suite"; return 1; }
    d="$SRC/$suite"
    dirs="$(suite_dirs "$suite")"
    export GIT_CEILING_DIRECTORIES="$SRC"       # never look for a repository above the checkouts
    if [ ! -d "$d/.git" ]; then
        mkdir -p "$d" && git -C "$d" init -q && git -C "$d" remote add origin "$url" ||
            { echo "FAIL fetch $suite: cannot create $d"; return 1; }
    fi
    head="$(git -C "$d" rev-parse -q --verify HEAD 2>/dev/null)"
    if [ "$head" != "$commit" ]; then
        local filter=()
        if [ -n "$dirs" ]; then                 # partial clone: only the files of these directories
            filter=(--filter=blob:none)
            # shellcheck disable=SC2086
            git -C "$d" sparse-checkout set $dirs ||
                { echo "FAIL fetch $suite: cannot set the sparse checkout"; return 1; }
        fi
        git -C "$d" fetch -q --depth 1 "${filter[@]}" origin "$commit" &&
            git -C "$d" -c advice.detachedHead=false checkout -q --detach FETCH_HEAD ||
            { echo "FAIL fetch $suite: cannot fetch $commit from $url"; return 1; }
    fi
    head="$(git -C "$d" rev-parse -q --verify HEAD 2>/dev/null)"
    if [ "$head" != "$commit" ]; then echo "FAIL fetch $suite: HEAD is $head, want $commit"; return 1; fi
    if [ -n "$(git -C "$d" status --porcelain)" ]; then
        echo "FAIL fetch $suite: $d differs from commit $commit (local changes)"; return 1
    fi
    echo "ok   fetch $suite $commit"
}

# Build, run and trace one program. Prints one line: "ok <name> ..." or "FAIL <name>: ...".
build_one() {
    local name="$1" line suite commit licence scale d prog part rc status
    local defs=() incs=() srcs=() parts=()
    line="$(awk -v n="$name" '$1 == n && NF >= 5 { print $2, $3, $4, $5; exit }' "$MANIFEST")"
    if [ -z "$line" ]; then echo "FAIL $name: not in programs_ext.txt"; return 1; fi
    read -r suite commit licence scale <<<"$line"
    IFS=, read -ra parts <<<"$scale"            # scale: macro definitions, e.g. CPU_MHZ=2
    for part in "${parts[@]}"; do defs+=("-D$part"); done
    case "$suite" in
        embench)                                # name: embench.<directory under src/>
            prog="${name#embench.}"
            incs=(-I"$SRC/embench/support" -I"$SRC/embench/src/$prog")
            srcs=("$SRC/embench/src/$prog"/*.c "$SRC/embench/support/beebsc.c" "$EXT/embench/xmain.c")
            ;;
        coremark)
            incs=(-I"$SRC/coremark" -I"$EXT/coremark")
            srcs=("$SRC/coremark"/core_main.c "$SRC/coremark"/core_list_join.c "$SRC/coremark"/core_matrix.c
                  "$SRC/coremark"/core_state.c "$SRC/coremark"/core_util.c "$EXT/coremark/core_portme.c")
            ;;
        mibench)                                # name: mibench.<program>; driver: mibench/<program>_main.c
            prog="${name#mibench.}"
            # The sources' own main() is renamed and never called; dijkstra_small.c calls malloc,
            # free and exit without including <stdlib.h>.
            defs+=(-Dmain=mibench_upstream_main -include stdlib.h)
            incs=(-I"$EXT/mibench" -I"$BENCH/rt")
            case "$prog" in
                bitcount)
                    incs+=(-I"$SRC/mibench/automotive/bitcount")
                    srcs=("$SRC/mibench/automotive/bitcount"/bitcnt_1.c "$SRC/mibench/automotive/bitcount"/bitcnt_2.c
                          "$SRC/mibench/automotive/bitcount"/bitcnt_3.c "$SRC/mibench/automotive/bitcount"/bitcnt_4.c) ;;
                dijkstra)
                    srcs=("$SRC/mibench/network/dijkstra/dijkstra_small.c") ;;
                adpcm)
                    incs+=(-I"$SRC/mibench/telecomm/adpcm/src")
                    srcs=("$SRC/mibench/telecomm/adpcm/src/adpcm.c") ;;
                *)
                    echo "FAIL $name: no driver for this MiBench program"; return 1 ;;
            esac
            srcs+=("$EXT/mibench/${prog}_main.c")
            ;;
        *)
            echo "FAIL $name: unknown suite $suite"; return 1 ;;
    esac
    d="$OUT/$name"
    mkdir -p "$d"
    rm -f "$d/prog.elf" "$d/prog.evt" "$d/prog.text" "$d/summary.json" "$d/build.log" "$d/trace.log"
    # The runtime objects come first, as in bench/build.sh; the macro definitions of the program
    # (defs) must not reach them, so they are compiled separately (below, once).
    if ! "${CROSS}gcc" "${CFLAGS[@]}" "${defs[@]}" -I"$EXT/libc/include" "${incs[@]}" -T"$BENCH/rt/link.ld" \
            "$LIBDIR/crt0.o" "$LIBDIR/rt.o" "${srcs[@]}" \
            -L"$LIBDIR" -Wl,--start-group -lbpxext "${LDFLAGS[@]}" -Wl,--end-group \
            -o "$d/prog.elf" >"$d/build.log" 2>&1; then
        echo "FAIL $name: compiler error, see $d/build.log"
        return 1
    fi
    : >"$d/build.log"                           # drop the warnings about the third-party sources
    # The trailer line tells rvtrace Spike's exit status (it goes into summary.json).
    { timeout "$TIMEOUT" "$SPIKE" --isa=rv32im -l --log=/dev/stdout "$d/prog.elf" 2>>"$d/build.log"
      echo "spike_status $?"; } | "$RVTRACE" "$name" "$d/prog.elf" "$d" 2>"$d/trace.log"
    rc=$?
    status="$(sed -n 's/.*"spike_status": *\(-\{0,1\}[0-9]*\).*/\1/p' "$d/summary.json" 2>/dev/null)"
    if [ "$rc" -ne 0 ] || [ "$status" != "0" ]; then
        cat "$d/trace.log" >>"$d/build.log"
        rm -f "$d/trace.log"
        echo "FAIL $name: rvtrace status $rc, Spike status ${status:-?}: $(tr '\n' ' ' <"$d/build.log")"
        return 1
    fi
    if [ -s "$d/trace.log" ]; then              # rvtrace notes
        echo "note $name: $(tr '\n' ' ' <"$d/trace.log")"
    fi
    rm -f "$d/build.log" "$d/trace.log"
    echo "ok   $name"
}

if [ "${1:-}" = "--one" ]; then
    build_one "$2"
    exit $?
fi

fetch_only=0
if [ "${1:-}" = "--fetch" ]; then fetch_only=1; shift; fi

# Every suite of the manifest, at its one pinned commit.
mkdir -p "$SRC" "$OUT"
pins="$(awk 'NF >= 5 && $1 !~ /^#/ { print $2, $3 }' "$MANIFEST" | sort -u)"
if [ -n "$(echo "$pins" | awk '{ print $1 }' | uniq -d)" ]; then
    echo "FAIL: programs_ext.txt pins a suite to more than one commit"; exit 1
fi
while read -r suite commit; do
    [ -n "$suite" ] || continue
    fetch_suite "$suite" "$commit" || { echo "build_ext.sh: fetch FAILED"; exit 1; }
done <<<"$pins"
if [ "$fetch_only" -eq 1 ]; then exit 0; fi

g++ -O2 -std=c++17 -Wall -o "$RVTRACE" "$BENCH/rvtrace.cc" || { echo "FAIL: rvtrace does not compile"; exit 1; }

# The bench runtime (bench/rt), compiled once with the flags of bench/build.sh.
mkdir -p "$LIBDIR"
"${CROSS}gcc" "${CFLAGS[@]}" -Wall -I"$BENCH/rt" -c "$BENCH/rt/crt0.S" -o "$LIBDIR/crt0.o" &&
    "${CROSS}gcc" "${CFLAGS[@]}" -Wall -I"$BENCH/rt" -c "$BENCH/rt/rt.c" -o "$LIBDIR/rt.o" ||
    { echo "FAIL: bench/rt does not compile"; exit 1; }

# bench/ext/libc as a library: a program links only the parts it calls.
objs=()
for f in xlibc xsqrt xmath xstdio xmalloc; do
    "${CROSS}gcc" "${CFLAGS[@]}" -Wall -Wextra -I"$EXT/libc/include" -c "$EXT/libc/$f.c" -o "$LIBDIR/$f.o" ||
        { echo "FAIL: bench/ext/libc/$f.c does not compile"; exit 1; }
    objs+=("$LIBDIR/$f.o")
done
rm -f "$LIBDIR/libbpxext.a"
"${CROSS}ar" rcs "$LIBDIR/libbpxext.a" "${objs[@]}" || { echo "FAIL: ar"; exit 1; }

if [ "$#" -gt 0 ]; then
    names=("$@")
else
    mapfile -t names < <(awk 'NF >= 5 && $1 !~ /^#/ { print $1 }' "$MANIFEST")
fi

failed=0
printf '%s\n' "${names[@]}" | xargs -P "$JOBS" -n 1 bash "$EXT/build_ext.sh" --one || failed=1

# summary_ext.tsv: every manifest program that has a summary.json, in manifest order.
python3 -I - "$OUT" "$MANIFEST" "$XOUT/summary_ext.tsv" <<'PY'
import json, os, sys
out, manifest, tsv = sys.argv[1], sys.argv[2], sys.argv[3]
fields = ["name", "instructions", "cond_branches", "cond_taken", "static_cond_sites", "jal", "jalr",
          "loaduse", "mul", "div", "text_bytes", "spike_status"]
rows = []
for line in open(manifest):
    p = line.split()
    if len(p) < 5 or p[0].startswith("#"):
        continue
    path = os.path.join(out, p[0], "summary.json")
    if not os.path.exists(path):
        continue
    s = json.load(open(path))
    taken = s["cond_taken"] / s["cond_branches"] if s["cond_branches"] else 0.0
    per_ki = 1000.0 * s["cond_branches"] / s["instructions"] if s["instructions"] else 0.0
    rows.append([str(s[f]) for f in fields] + ["%.4f" % taken, "%.2f" % per_ki])
with open(tsv, "w", newline="\n") as f:
    f.write("\t".join(fields + ["taken_ratio", "branches_per_ki"]) + "\n")
    for r in rows:
        f.write("\t".join(r) + "\n")
print("summary: %s (%d programs)" % (tsv, len(rows)))
PY

if [ "$failed" -ne 0 ]; then echo "build_ext.sh: at least one program FAILED"; exit 1; fi
echo "build_ext.sh: all ${#names[@]} programs ok"
