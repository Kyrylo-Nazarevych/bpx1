![](../../workflows/gds/badge.svg) ![](../../workflows/docs/badge.svg) ![](../../workflows/test/badge.svg)

# BPX-1: a branch prediction coprocessor for Tiny Tapeout (SKY130, 2×2 tiles)

BPX-1 is a stand-alone conditional-branch direction predictor. A host (a pipelined RISC-V CPU on an FPGA, a
microcontroller, or the Tiny Tapeout demo board) sends a branch identifier and gets a taken / not-taken prediction;
later it reports the real outcome and the chip learns from it. Four predictors run side by side on the same branch
stream, a tagged geometric-history predictor and three classic references, so algorithms can be compared on silicon.

**Status: gate G0.** The repository currently builds the *area probe* of the storage tables, not the predictor
(see the header of `src/tt_um_kyrylo_nazarevych_bpx1.v`). The predictor RTL replaces it at gate G2.

| Where | What |
| --- | --- |
| `docs/SPEC.md` | Design specification: pins, host protocol, control port, microarchitecture, the CPU contract. |
| `docs/spec_review.md` | Review of the requirements specification and the owners' decisions on it. |
| `docs/info.md`, `info.yaml` | Tiny Tapeout datasheet page and project description. |
| `src/` | Verilog sources of the chip. `bpx_ltab.v`: the latch table and its write sequencer. |
| `model/bpx1.py` | Golden model: the executable form of the protocol and the predictor algorithm. |
| `explore/` | Design-space exploration: `bpsim.cc` (candidate predictors inside the CPU timing model), `sweep.py`. |
| `bench/` | Benchmark programs (RV32IM, run in Spike) and the event-trace format the CPU model replays. |
| `test/` | cocotb tests (RTL and gate level). |
| `tools/wsl_sync.sh` | Mirrors this folder into the Linux file system, where every tool runs. |

## Building

Everything runs in WSL (Ubuntu) with the Tiny Tapeout tool set: Icarus Verilog, Verilator, cocotb, Yosys,
LibreLane, a RISC-V GCC and Spike.

```
bash tools/wsl_sync.sh                      # from the project folder: mirror into ~/work/bpx1
cd ~/work/bpx1
bash bench/build.sh                         # build, run and trace the benchmark programs -> out/bench/
python3 explore/sweep.py run families       # explore predictor families -> out/explore/
cd test && make -B                          # RTL test
./tt/tt_tool.py --pdk sky130A --create-user-config && ./tt/tt_tool.py --pdk sky130A --harden   # chip flow
```

## Licence

Apache-2.0 (see `LICENSE`). External benchmark suites are fetched by script and are not part of this repository.
