#!/bin/bash
# Local chip build, the same flow as the CI (spec 3.3): LibreLane through tt-support-tools, then the gate-level test.
#   bash tools/wsl_sync.sh && bash ~/work/bpx1/tools/harden.sh
# One build at a time on this machine.  Results: runs/wokwi/, digest printed at the end.
source ~/ttsetup/env.sh
cd ~/work/bpx1 || exit 1
./tt/tt_tool.py --pdk $PDK --create-user-config > out/harden_config.log 2>&1 || { echo "create-user-config FAILED"; tail -20 out/harden_config.log; exit 1; }
START=$(date +%s)
./tt/tt_tool.py --pdk $PDK --harden > out/harden.log 2>&1
RC=$?
echo "harden rc=$RC in $(( $(date +%s) - START )) s"
grep -E "ERROR" out/harden.log | head -8
[ $RC -eq 0 ] || exit $RC
./tt/tt_tool.py --pdk $PDK --print-stats 2>&1 | tail -6
python3 - <<'PY'
import json
m = json.load(open("runs/wokwi/final/metrics.json"))
keys = ["design__instance__utilization", "design__instance__area__stdcell", "design__instance__area__class:sequential_cell",
        "design__instance__area__class:multi_input_combinational_cell", "design__instance__area__class:timing_repair_buffer",
        "design__instance__area__class:clock_buffer", "route__wirelength", "timing__setup__ws__corner:max_ss_100C_1v60",
        "timing__setup__ws__corner:nom_tt_025C_1v80", "timing__hold__ws", "timing__setup_vio__count", "timing__hold_vio__count",
        "design__max_slew_violation__count", "design__max_cap_violation__count", "design__max_fanout_violation__count",
        "route__drc_errors", "magic__drc_error__count", "design__lvs_error__count", "antenna__violating__nets", "power__total"]
for k in keys:
    if k in m:
        print(f"  {k} = {m[k]}")
PY
echo "--- gate-level test"
TOP=$(./tt/tt_tool.py --pdk $PDK --print-top-module)
cp runs/wokwi/final/pnl/$TOP.pnl.v test/gate_level_netlist.v
cd test && rm -rf sim_build/gl results.xml && make -B GATES=yes 2>&1 | grep -E "signature|PASS|FAIL|TESTS=|rror" | head -12
python3 -m cocotb_tools.check_results results.xml && echo "GATE-LEVEL RESULTS OK"
