![](../../workflows/gds/badge.svg) ![](../../workflows/docs/badge.svg) ![](../../workflows/test/badge.svg)

# BPX-1: a branch prediction coprocessor for Tiny Tapeout (SKY130, 2×2 tiles)

BPX-1 is a stand-alone conditional-branch direction predictor. A host (a pipelined RISC-V CPU on an FPGA, a
microcontroller, or the Tiny Tapeout demo board) sends a branch identifier and gets a taken / not-taken prediction;
later it reports the real outcome and the chip learns from it. Four predictors run side by side on the same branch
stream, a tagged geometric-history predictor and three classic references, so algorithms can be compared on silicon.

**Status: gate G2 passed, G3 in progress.** The repository builds the real chip. The design is frozen; the RTL equals
the golden model on every benchmark program and on random operations. Start with `docs/HANDOVER.md`.

| Where | What |
| --- | --- |
| `docs/HANDOVER.md` | State, decisions, next steps, commands. |
| `docs/SPEC.md` | Design specification: pins, host protocol, control port, microarchitecture, the CPU contract. |
| `docs/spec_review.md` | Review of the requirements specification and the owners' decisions on it. |
| `docs/exploration.md` | How the predictor was chosen: 71 programs, 16 families, held-out validation. |
| `docs/cosim.md`, `docs/verification.md`, `docs/coverage.md` | Results on the RTL: speedup and accuracy per program, golden-model equivalence, formal proofs, coverage. |
| `docs/area.md`, `docs/timing.md` | Chip build: utilisation, timing per corner. |
| `docs/info.md`, `info.yaml` | Tiny Tapeout datasheet page and project description. |
| `src/` | Verilog sources of the chip. `bpx_ltab.v`: the latch table and its write sequencer. |
| `model/` | Golden model `bpx1.py`: the executable form of the protocol and the predictor algorithm; random-test generator. |
| `explore/` | Design-space exploration: `bpsim.cc` (candidate predictors inside the CPU timing model), `sweep.py`. |
| `bench/` | Benchmark programs (RV32IM, run in Spike) and the event-trace format the CPU model replays. |
| `cosim/` | System co-simulation: the CPU contract against the RTL (Verilator), random operations, coverage. |
| `formal/` | SymbiYosys proofs of the properties at the end of `src/bpx_core.v`. |
| `test/` | cocotb tests (RTL and gate level), run by CI. |
| `tools/` | `wsl_sync.sh` mirrors this folder into the Linux file system, where every tool runs; `harden.sh` builds the chip locally. |

## Building

Everything runs in WSL (Ubuntu) with the Tiny Tapeout tool set: Icarus Verilog, Verilator, cocotb, Yosys,
LibreLane, a RISC-V GCC and Spike.

```
bash tools/wsl_sync.sh                      # from the project folder: mirror into ~/work/bpx1
cd ~/work/bpx1
bash bench/build.sh                         # build, run and trace the benchmark programs -> out/bench/
python3 explore/sweep.py run families       # explore predictor families -> out/explore/
cd test && make -B                          # RTL test against the golden model (cocotb)
bash cosim/run.sh && python3 cosim/report.py    # every program against the RTL -> docs/cosim.md
bash cosim/random.sh 10 100000              # 10 x 100,000 random operations, coverage -> docs/coverage_bins.md
bash formal/run.sh                          # formal proofs
bash tools/harden.sh                        # chip flow (LibreLane) and gate-level test
```

## Licence

Apache-2.0 (see `LICENSE`). External benchmark suites are fetched by script and are not part of this repository.
