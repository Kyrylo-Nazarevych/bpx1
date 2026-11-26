#!/bin/bash
# BPX-1 formal proofs (spec VR-03): bash formal/run.sh [task]   (task: bmc, prove, cover or nowait; default all)
# Log: out/formal/sby_<task>.log; traces and details: out/formal/<task>/bpx_<task>/.
source ~/ttsetup/env.sh 2>/dev/null
export PATH=$HOME/oss-cad-suite/bin:$PATH
cd "$(dirname "$0")/.."
mkdir -p out/formal/${1:-all}
LOG=out/formal/sby_${1:-all}.log
(cd formal && sby -f --prefix ../out/formal/${1:-all}/bpx bpx.sby "$@") > $LOG 2>&1
grep -E "DONE|Reached cover|Unreached|failed assertion|Assert failed|BMC failed|Induction|induction|Status|ERROR" $LOG | grep -v "Trying induction" | tail -40
