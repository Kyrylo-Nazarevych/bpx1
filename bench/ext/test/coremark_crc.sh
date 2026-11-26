#!/bin/bash
# BPX-1 bench/ext test: show the CRCs of the CoreMark run (the program has no console).
#
#   bash bench/ext/test/coremark_crc.sh
#
# Runs out/bench/coremark/prog.elf in Spike's debug mode up to the store to HTIF tohost and reads
# the record bpx_coremark_report (bench/ext/coremark/core_portme.c) out of the simulated memory.
# Exit status 0 when CoreMark printed "Correct operation validated" and no error message.
[ -f "$HOME/ttsetup/env.sh" ] && source "$HOME/ttsetup/env.sh"
set -u -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$(dirname "$(dirname "$HERE")")")"
ELF="$ROOT/out/bench/coremark/prog.elf"
T="$ROOT/out/ext/test"
SPIKE="${SPIKE:-$HOME/riscv-tools/bin/spike}"
CROSS="${CROSS:-riscv64-unknown-elf-}"

[ -f "$ELF" ] || { echo "coremark_crc: $ELF is not built"; exit 1; }
mkdir -p "$T"
report="$("${CROSS}nm" "$ELF" | awk '$3 == "bpx_coremark_report" { print $1 }')"
store="$("${CROSS}nm" "$ELF" | awk '$3 == "__tohost_store" { print $1 }')"
[ -n "$report" ] && [ -n "$store" ] || { echo "coremark_crc: symbols not found"; exit 1; }

# Debug commands: run to the store, read the 7 words of the record, leave.
{
    echo "until pc 0 $store"
    for i in 0 1 2 3 4 5 6; do printf 'mem 0 %x\n' $((0x$report + 4 * i)); done
    echo "quit"
} >"$T/coremark_crc.cmd"

mapfile -t words < <("$SPIKE" -d --debug-cmd="$T/coremark_crc.cmd" --isa=rv32im "$ELF" 2>&1 | grep -E '^0x[0-9a-f]+$')
if [ "${#words[@]}" -ne 7 ]; then echo "coremark_crc: unexpected Spike output (${#words[@]} values)"; exit 1; fi
names=(validated errors seedcrc crclist crcmatrix crcstate crcfinal)
for i in 0 1 2 3 4 5 6; do
    v=$(( ${words[$i]} & 0xffffffff ))          # "mem" shows 8 bytes at an address that is a multiple of 8
    printf '%-10s 0x%04x\n' "${names[$i]}" "$v"
    [ "$i" -eq 0 ] && validated=$v
    [ "$i" -eq 1 ] && errors=$v
done
if [ "$validated" -eq 1 ] && [ "$errors" -eq 0 ]; then echo "coremark_crc: validated by CoreMark"; exit 0; fi
echo "coremark_crc: NOT validated"
exit 1
