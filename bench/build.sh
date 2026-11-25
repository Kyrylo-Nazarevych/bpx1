#!/bin/bash
# BPX-1 bench: build, run (Spike) and trace the programs listed in programs.txt.
#
#   bash bench/build.sh                      every program of the manifest
#   bash bench/build.sh sort.quick team.vm   only these
#
# Output, per program:  <project>/out/bench/<name>/{prog.elf, prog.evt, prog.text, summary.json}
# and for all programs: <project>/out/bench/summary.tsv  (rewritten on every call from the summary.json files).
# A program that fails keeps <name>/build.log (compiler, Spike and rvtrace messages); the exit status is then 1.
# Spike's instruction log is piped straight into rvtrace and never stored. rvtrace itself is compiled
# to <project>/out/bench/.tools/rvtrace.
#
# A program passes when Spike exits with status 0, i.e. main returned 0. Status 255 with
# "*** FAILED *** (tohost = <n>)" is a checksum mismatch reported by rt_check(): n >> 8 is the
# checksum the program computed (see rt/rt.h). Other small codes are the program's own self-checks.
#
# Environment: JOBS  programs run in parallel (default 6)
#              SPIKE path of Spike (default ~/riscv-tools/bin/spike)
#              CROSS toolchain prefix (default riscv64-unknown-elf-)
#              TIMEOUT seconds before a Spike run is stopped (default 1800)
[ -f "$HOME/ttsetup/env.sh" ] && source "$HOME/ttsetup/env.sh"
set -u -o pipefail

BENCH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$BENCH")"
OUT="$ROOT/out/bench"
MANIFEST="$BENCH/programs.txt"
SPIKE="${SPIKE:-$HOME/riscv-tools/bin/spike}"
CROSS="${CROSS:-riscv64-unknown-elf-}"
JOBS="${JOBS:-6}"
TIMEOUT="${TIMEOUT:-1800}"
RVTRACE="$OUT/.tools/rvtrace"

# -fno-tree-loop-distribute-patterns: keep copy/fill loops as written (no hidden memcpy/memset calls).
CFLAGS=(-march=rv32im -mabi=ilp32 -O2 -ffreestanding -fno-pic -static -nostdlib -nostartfiles
        -fno-tree-loop-distribute-patterns -Wall)
LDFLAGS=(-Wl,--no-warn-rwx-segments -lgcc)

# Build, run and trace one program. Prints one line: "ok <name> ..." or "FAIL <name>: ...".
build_one() {
    local name="$1" line src sel origin d rc status
    line="$(awk -v n="$name" '$1 == n && $1 !~ /^#/ { print $2, $3, $4; exit }' "$MANIFEST")"
    if [ -z "$line" ]; then echo "FAIL $name: not in programs.txt"; return 1; fi
    read -r src sel origin <<<"$line"
    d="$OUT/$name"
    mkdir -p "$d"
    rm -f "$d/prog.elf" "$d/prog.evt" "$d/prog.text" "$d/summary.json" "$d/build.log"
    if ! "${CROSS}gcc" "${CFLAGS[@]}" -DSEL="$sel" -I"$BENCH/rt" -T"$BENCH/rt/link.ld" \
            "$BENCH/rt/crt0.S" "$BENCH/rt/rt.c" "$BENCH/src/$src" "${LDFLAGS[@]}" -o "$d/prog.elf" \
            >"$d/build.log" 2>&1; then
        echo "FAIL $name: compiler error, see $d/build.log"
        return 1
    fi
    # The trailer line tells rvtrace Spike's exit status (it goes into summary.json).
    { timeout "$TIMEOUT" "$SPIKE" --isa=rv32im -l --log=/dev/stdout "$d/prog.elf" 2>>"$d/build.log"
      echo "spike_status $?"; } | "$RVTRACE" "$name" "$d/prog.elf" "$d" 2>>"$d/build.log"
    rc=$?
    status="$(sed -n 's/.*"spike_status": *\(-\{0,1\}[0-9]*\).*/\1/p' "$d/summary.json" 2>/dev/null)"
    if [ "$rc" -ne 0 ] || [ "$status" != "0" ]; then
        echo "FAIL $name: rvtrace status $rc, Spike status ${status:-?}: $(tr '\n' ' ' <"$d/build.log")"
        return 1
    fi
    if [ -s "$d/build.log" ]; then                  # compiler warnings, rvtrace notes
        echo "note $name: $(tr '\n' ' ' <"$d/build.log")"
    fi
    rm -f "$d/build.log"
    echo "ok   $name"
}

if [ "${1:-}" = "--one" ]; then
    build_one "$2"
    exit $?
fi

mkdir -p "$OUT/.tools"
g++ -O2 -std=c++17 -Wall -o "$RVTRACE" "$BENCH/rvtrace.cc" || { echo "FAIL: rvtrace does not compile"; exit 1; }

if [ "$#" -gt 0 ]; then
    names=("$@")
else
    mapfile -t names < <(awk 'NF >= 4 && $1 !~ /^#/ { print $1 }' "$MANIFEST")
fi

failed=0
printf '%s\n' "${names[@]}" | xargs -P "$JOBS" -n 1 bash "$BENCH/build.sh" --one || failed=1

# summary.tsv: every manifest program that has a summary.json, in manifest order.
python3 -I - "$OUT" "$MANIFEST" <<'PY'
import json, os, sys
out, manifest = sys.argv[1], sys.argv[2]
fields = ["name", "instructions", "cond_branches", "cond_taken", "static_cond_sites", "jal", "jalr",
          "loaduse", "mul", "div", "text_bytes", "spike_status"]
rows = []
for line in open(manifest):
    p = line.split()
    if len(p) < 4 or p[0].startswith("#"):
        continue
    path = os.path.join(out, p[0], "summary.json")
    if not os.path.exists(path):
        continue
    s = json.load(open(path))
    taken = s["cond_taken"] / s["cond_branches"] if s["cond_branches"] else 0.0
    per_ki = 1000.0 * s["cond_branches"] / s["instructions"] if s["instructions"] else 0.0
    rows.append([str(s[f]) for f in fields] + ["%.4f" % taken, "%.2f" % per_ki])
with open(os.path.join(out, "summary.tsv"), "w", newline="\n") as f:
    f.write("\t".join(fields + ["taken_ratio", "branches_per_ki"]) + "\n")
    for r in rows:
        f.write("\t".join(r) + "\n")
print("summary: %s (%d programs)" % (os.path.join(out, "summary.tsv"), len(rows)))
PY

if [ "$failed" -ne 0 ]; then echo "build.sh: at least one program FAILED"; exit 1; fi
echo "build.sh: all ${#names[@]} programs ok"
