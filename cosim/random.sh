#!/bin/bash
# BPX-1 constrained-random operations against the golden model (spec VR-01, VR-02).
#   bash cosim/random.sh [runs=10] [operations per run=100000] [first seed=1]
# Each run: model/randgen.py writes the steps and the expected results, `cosim --rand` plays them on the RTL and
# compares every reply, the statistics and the complete state; cosim/coverage.py then reports the coverage.
source ~/ttsetup/env.sh 2>/dev/null
set -e
cd "$(dirname "$0")/.."
O=out/random; mkdir -p $O out/cosim
RUNS=${1:-10}; N=${2:-100000}; S0=${3:-1}
if [ ! -x out/cosim/obj/cosim ] || [ cosim/cosim.cpp -nt out/cosim/obj/cosim ]; then
  verilator --cc --exe --build -O3 -Wno-fatal -Wno-WIDTHEXPAND -Wno-UNUSEDSIGNAL -DBPX_BEHAV --top-module bpx_chip \
    -CFLAGS "-O2" --Mdir out/cosim/obj -o cosim src/bpx_ltab.v src/bpx_pred.v src/bpx_core.v src/bpx_spi.v src/bpx_chip.v cosim/cosim.cpp > out/cosim/build.log 2>&1 \
    || { tail -20 out/cosim/build.log; exit 1; }
fi
one() {
  s=$1; N=$2; O=out/random
  python3 model/randgen.py $N $s $O/s$s.ops $O/s$s.meta > $O/s$s.gen.txt
  out/cosim/obj/cosim --rand $O/s$s.ops $O/s$s.lat > $O/s$s.txt 2> $O/s$s.err || true
  rm -f $O/s$s.ops
  echo "seed $s: $(cat $O/s$s.txt) $(head -c 300 $O/s$s.err)"
}
export -f one
seq $S0 $((S0 + RUNS - 1)) | xargs -P ${JOBS:-8} -I{} bash -c "one {} $N"
python3 cosim/coverage.py $(for s in $(seq $S0 $((S0 + RUNS - 1))); do echo $O/s$s; done)
