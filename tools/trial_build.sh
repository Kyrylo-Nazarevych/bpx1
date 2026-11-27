#!/bin/bash
# Trial chip build of the BPX-1 RTL (bpx_chip) in a throw-away directory, without touching the repository top.
# One factor combination per call (docs/HANDOVER.md, build plan):
#   bash ~/work/bpx1/tools/trial_build.sh <tag> <density_pct> <clock_ns> <cells: default|small> [extra-json-file]
# cells=small lets synthesis use drive-strength-1 gates (src/synth_exclude_small.cells instead of the PDK list).
# One LibreLane build at a time on this machine.
source ~/ttsetup/env.sh
TAG=${1:?tag}; DENS=${2:-82}; PERIOD=${3:-20}; CELLS=${4:-default}; EXTRA=$5
S=~/work/bpx1
D=~/work/bpx1-trial-$TAG
rm -rf $D; mkdir -p $D/src $D/docs $D/test
ln -s ~/work/tt-branch-predictor/tt $D/tt
cp $S/src/bpx_ltab.v $S/src/bpx_pred.v $S/src/bpx_core.v $S/src/bpx_spi.v $S/src/bpx_ro.v $S/src/bpx_chip.v $D/src/
cat > $D/src/project.v <<'EOF'
`default_nettype none
module tt_um_kyrylo_nazarevych_bpx1 (
    input  wire [7:0] ui_in,
    output wire [7:0] uo_out,
    input  wire [7:0] uio_in,
    output wire [7:0] uio_out,
    output wire [7:0] uio_oe,
    input  wire       ena,
    input  wire       clk,
    input  wire       rst_n
);
  bpx_chip u_chip (.ui_in(ui_in), .uo_out(uo_out), .uio_in(uio_in), .uio_out(uio_out), .uio_oe(uio_oe), .ena(ena), .clk(clk), .rst_n(rst_n));
endmodule
EOF
sed -e 's/^    - "bpx_ltab.v"$/    - "bpx_ltab.v"\n    - "bpx_pred.v"\n    - "bpx_core.v"\n    - "bpx_spi.v"\n    - "bpx_ro.v"\n    - "bpx_chip.v"/' -e 's/"tt_um_kyrylo_nazarevych_bpx1.v"/"project.v"/' $S/info.yaml > $D/info.yaml
# drive-strength-1 gates allowed: the PDK's synthesis exclusion list without its plain _1 logic gates and flop
T=$PDK_ROOT/$PDK/libs.tech/openlane/sky130_fd_sc_hd
grep -v -E '__(a[0-9]+[a-z]*|o[0-9]+[a-z]*|and[0-9]b*|or[0-9]b*|nand[0-9]b*|nor[0-9]b*|xor2|xnor2|inv|dfxtp)_1$' $T/no_synth.cells > $D/src/synth_exclude_small.cells
# place-and-route exclusion list for extra configs that ask for it: the PDK's list plus the slow delay cells the
# resizer otherwise uses as fan-out buffers (about 1 ns each at the slow corner)
{ cat $T/drc_exclude.cells; printf 'sky130_fd_sc_hd__clkdlybuf4s25_1
sky130_fd_sc_hd__clkdlybuf4s25_2
sky130_fd_sc_hd__clkdlybuf4s50_1
sky130_fd_sc_hd__clkdlybuf4s50_2
'; } > $D/src/pnr_exclude.cells
python3 - "$S" "$D" "$DENS" "$PERIOD" "$CELLS" "$EXTRA" <<'EOF'
import json, sys
s, d, dens, per, cells, extra = sys.argv[1:7]
cfg = {k: v for k, v in json.load(open(s + "/src/config.json")).items() if k != "//"}
cfg["PL_TARGET_DENSITY_PCT"] = int(dens)
cfg["CLOCK_PERIOD"] = float(per)
if cells == "small":
    cfg["SYNTH_EXCLUDED_CELL_FILE"] = "dir::synth_exclude_small.cells"
if extra:
    cfg.update(json.load(open(extra)))
json.dump(cfg, open(d + "/src/config.json", "w"), indent=2)
print({k: cfg[k] for k in cfg if k in ("PL_TARGET_DENSITY_PCT", "CLOCK_PERIOD", "SYNTH_EXCLUDED_CELL_FILE") or (extra and k in json.load(open(extra)))})
EOF
echo "excluded cells: default $(wc -l < $T/no_synth.cells), small $(wc -l < $D/src/synth_exclude_small.cells)"
cd $D
git init -q . 2>/dev/null; git add -A >/dev/null 2>&1; git -c user.name=trial -c user.email=trial@example.invalid commit -qm trial >/dev/null 2>&1
./tt/tt_tool.py --pdk $PDK --create-user-config > create.log 2>&1 || { echo "create-user-config FAILED"; tail -20 create.log; exit 1; }
START=$(date +%s)
./tt/tt_tool.py --pdk $PDK --harden > harden.log 2>&1; RC=$?
echo "harden rc=$RC in $(( $(date +%s) - START )) s"
grep -E "ERROR" harden.log | head -6
cd runs/wokwi
python3 - <<'EOF'
import json, os, re
def load(p):
    try: return json.load(open(p))
    except Exception: return None
steps = sorted(x for x in os.listdir(".") if re.match(r"\d+-", x))
print("last step:", steps[-1] if steps else None)
for d in steps:
    if any(d.endswith(s) for s in ("floorplan", "repairdesignpostgpl", "cts", "resizertimingpostcts", "detailedrouting")):
        m = load(d + "/or_metrics_out.json")
        if m: print(f"  {d:36s} util={m.get('design__instance__utilization', 0):.3f} stdcell={m.get('design__instance__area__stdcell', 0):.0f} "
                    f"seq={m.get('design__instance__area__class:sequential_cell', 0):.0f} comb={m.get('design__instance__area__class:multi_input_combinational_cell', 0):.0f} "
                    f"repair={m.get('design__instance__area__class:timing_repair_buffer', 0):.0f} clock={m.get('design__instance__area__class:clock_buffer', 0):.0f}")
m = load("final/metrics.json")
if m:
    for k in ("design__instance__utilization", "route__wirelength", "timing__setup__ws__corner:max_ss_100C_1v60", "timing__setup__ws__corner:nom_tt_025C_1v80",
              "timing__hold__ws", "timing__setup_vio__count", "timing__hold_vio__count", "design__max_slew_violation__count", "design__max_cap_violation__count",
              "design__max_fanout_violation__count", "route__drc_errors", "magic__drc_error__count", "design__lvs_error__count", "antenna__violating__nets", "power__total"):
        if k in m: print(f"  {k} = {m[k]}")
else:
    print("NO final metrics")
EOF
