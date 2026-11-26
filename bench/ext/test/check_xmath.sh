#!/bin/bash
# BPX-1 bench/ext test: accuracy of bench/ext/libc/xsqrt.c and xmath.c against the host C library.
#
#   bash bench/ext/test/check_xmath.sh
#
# Compiles them natively (functions renamed x_sqrt, x_cos, ...; no contraction, no built-ins)
# and check_xmath.c with the host compiler into <project>/out/ext/test/, then runs the comparison.
[ -f "$HOME/ttsetup/env.sh" ] && source "$HOME/ttsetup/env.sh"
set -eu -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXT="$(dirname "$HERE")"
ROOT="$(dirname "$(dirname "$EXT")")"
T="$ROOT/out/ext/test"
mkdir -p "$T"

RENAME=()
for f in sqrt sin cos atan acos exp log pow; do RENAME+=("-D$f=x_$f"); done

for f in xsqrt xmath; do
    gcc -O2 -ffp-contract=off -fno-builtin -Wall -Wextra -I"$EXT/libc/include" "${RENAME[@]}" \
        -c "$EXT/libc/$f.c" -o "$T/${f}_host.o"
done
gcc -O2 -Wall -Wextra "$HERE/check_xmath.c" "$T/xsqrt_host.o" "$T/xmath_host.o" -lm -o "$T/check_xmath"
"$T/check_xmath"
