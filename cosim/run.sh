#!/bin/bash
# BPX-1 system co-simulation of every benchmark program against the RTL (spec VR-06, VR-01 on programs).
#   bash cosim/run.sh [K] [program ...]      K = chip clocks per CPU cycle (default 8); default: all programs
#   FORCE=1 repeats programs that already have results; OUT=<dir> selects another result directory.
# Per program: the CPU contract with each predictor driving (0 default, 1 bimodal, 2 gshare, 3 tournament,
# 4 static not-taken through the chip), and a one-in-flight trace replay.  The default-driven run and the trace
# replay log every request and reply; model/replay.py then checks them against the golden model.
# Results: out/cosim/<program>.{d0..d4,seq}.txt, *.model.txt; summary by cosim/report.py.
source ~/ttsetup/env.sh 2>/dev/null
set -e
cd "$(dirname "$0")/.."
BIN=out/cosim/obj/cosim
export O=${OUT:-out/cosim}; mkdir -p $O out/cosim
K=${1:-8}; shift || true
if [ ! -x $BIN ] || [ cosim/cosim.cpp -nt $BIN ] || [ src/bpx_core.v -nt $BIN ] || [ src/bpx_pred.v -nt $BIN ]; then
  verilator --cc --exe --build -O3 -Wno-fatal -Wno-WIDTHEXPAND -Wno-UNUSEDSIGNAL -DBPX_BEHAV --top-module bpx_chip \
    -CFLAGS "-O2" --Mdir out/cosim/obj -o cosim src/bpx_ltab.v src/bpx_pred.v src/bpx_core.v src/bpx_spi.v src/bpx_chip.v cosim/cosim.cpp > out/cosim/build.log 2>&1 \
    || { tail -20 out/cosim/build.log; exit 1; }
fi
PROGS="$@"; [ -z "$PROGS" ] && PROGS=$(ls out/bench | grep -v -E '^summary|^\.' )
one() {
  p=$1; K=$2; B=out/bench/$p; X=out/cosim/obj/cosim
  [ -f $B/prog.evt ] || exit 0
  [ -s $O/$p.seq.model.txt ] && [ -z "$FORCE" ] && exit 0      # finished earlier
  for d in 1 2 3 4; do $X $B/prog.evt $B/prog.text $d $K > $O/$p.d$d.txt; done
  $X $B/prog.evt $B/prog.text 0 $K $O/$p.d0.oplog > $O/$p.d0.txt
  BPX_SEQ=1 $X $B/prog.evt $B/prog.text 0 $K $O/$p.seq.oplog > $O/$p.seq.txt
  python3 model/replay.py $O/$p.d0.oplog 0 > $O/$p.d0.model.txt
  python3 model/replay.py $O/$p.seq.oplog 0 > $O/$p.seq.model.txt
  rm -f $O/$p.d0.oplog $O/$p.seq.oplog
  echo "$p done"
}
export -f one
echo $PROGS | tr ' ' '\n' | xargs -P ${JOBS:-8} -I{} bash -c "one {} $K"
