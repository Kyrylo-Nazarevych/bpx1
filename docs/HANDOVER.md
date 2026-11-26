# BPX-1 handover

Everything a fresh session needs to continue: state, decisions, next steps, commands. Last rewritten at the end of
the perceptron round (26 Nov 2026). Evidence labels as in the requirements specification, section 10. Read
`docs/SPEC.md` next, then `docs/perceptron.md` and `docs/verification.md`.

## 1. State in one paragraph

BPX-1 is a branch-prediction coprocessor for Tiny Tapeout (SKY130, 2×2 tiles). On the owners' requirement of
26 Nov 2026 the default predictor is now a **hashed perceptron**; the earlier tagged-table design, verified to gate
G3, is the tag **`g3-tage`** (fallback; `g3` is the same RTL with the last test harness). The perceptron design
equals the golden model on all 71 benchmark programs in three runs (420.9 million requests) and on 2 × 10⁶ random
operations (half of them with the control port working concurrently); coverage is complete; the sequencer proofs
pass; the local build is clean at 81.2 % utilisation and 30 ns with the gate-level tests passing. Measured on the
RTL: 1.1324× over static not-taken (1.1361× with the two-requests-per-cycle adapter) at 91.42 % accuracy, against
1.1317× / 1.1353× / 91.24 % for the tagged tables. **Not done:** section 5.

## 2. Where things are

| What | Where |
| --- | --- |
| Project folder (source of truth, Windows; **the git repository is here**) | `C:\\Users\\Kyrylo\\Desktop\\Claude Folder\\Branch Predictor Hackathon 2` |
| Linux mirror where every tool runs | `~/work/bpx1` (WSL Ubuntu), filled by `tools/wsl_sync.sh` (which also overwrites the mirror's `.git`); generated data in `~/work/bpx1/out` |
| GitHub | `Kyrylo-Nazarevych/bpx1`, public, branch `main`; Pages enabled (source: GitHub Actions). Tags: `g3-tage`, `g3` (tagged-table design), `g3-perceptron` (this design) |
| Requirements | `..\\Branch Predictor Hackaton 1\\plan\\FABLE_ALL_IN_BRIEF.md` (v4.1), the owners' decisions in `docs/spec_review.md` section 6, and the owners' message of 26 Nov 2026 (perceptron; summarised in docs/perceptron.md) |

Repository layout: `src/` RTL; `model/` golden model, random-test generator, replay; `test/` cocotb (CI);
`cosim/` Verilator harness, reports, coverage, concurrent-control-port checker; `formal/` SymbiYosys; `explore/`
sweep engine (`percep.py`: the perceptron round); `bench/` benchmarks; `host/` MicroPython host; `tools/` scripts;
`docs/` documents.

Generated documents (do not edit by hand): `docs/cosim.md`, `docs/cosim_dual.md`, `docs/cosim_safe.md`
(`cosim/report.py`), `docs/coverage_bins.md` (`cosim/coverage.py`), `docs/exploration_tables.md`. They are written in
the mirror: copy them back to the Windows folder after generating (the sync is one-way, Windows to Linux).

## 3. Decisions (each with its evidence in the named document)

| # | Decision | Evidence |
| --- | --- | --- |
| D1 | Host port: two-phase REQ/ACK, **8-bit ID**, 2-bit OP; answer at the third clock after the request. PREDICT+RESOLVE = predict, then resolve the oldest (resolve first if the queue is full). Queue depth 4. | SPEC 2–3 |
| D2 | Control port: SPI mode 0 with 16-bit registers on `uio[4..6]` + `uo[7]`. | SPEC 4 |
| D3 | Storage: latch tables written by flop-generated pulses; one write sequencer; one read port per table; training from a staging register. | SPEC 6 |
| D4 | No hard macro. | spec_review 4 |
| D5 | **Default predictor: hashed perceptron.** 4 tables × 32 × 5-bit weights, histories 0/8/9/20, 16-bit path history, reference counters as inputs, adaptive threshold, negation in memory. 640 bits (tagged tables: 576). | perceptron.md 1–3; cosim.md **(VERIFIED on RTL)** |
| D6 | U1 CPU contract: five stages with a 256-entry branch-target buffer at fetch. | SPEC 9 |
| D7 | Benchmark set frozen: 71 programs; `embench.nbody`/`cubic` out of the aggregates; 51 tuning / 18 held-out. | exploration.md 1 |
| D8 | Statistics: 16-bit saturating counters, snapshot and snapshot-and-clear. | SPEC 4 |
| D9 | Single clock domain; data pins use one capture flop qualified by the 2-flop synchronised REQ. | `src/bpx_chip.v` header |
| D10 | CPU adapter: a mispredicted resolve is sent alone. Not a chip decision (owners, 26 Nov): both figures are quoted, the FPGA side tries both modes. | SPEC 9 rules 4 and 7 |
| D11 | The tables' read port is reserved for a predict only in the clock it executes (deadlock fix). | verification.md 4 |
| D12 | **ID width 8 bits** (10 before): no measurable loss; `uio[1:0]` become outputs `PBIM`, `PGSH`; with `DBG0`/`DBG1` (reset selections) all four predictions are on pins with every `ACK`. | perceptron.md 7 **(VERIFIED, 4,000 candidates)** |
| D13 | `CONF` is the confidence of the driving predictor (was: always the default's). Debug treats the four predictors alike. | perceptron.md 7; SPEC 2 |
| D14 | **Clock 30 ns (33.3 MHz), latency still 3 clocks = 90 ns**, instead of a fourth clock at 25 ns (100 ns). | perceptron.md 6; timing.md **(VERIFIED)** |
| D15 | Switches CTRL[15:12]: reference inputs, negation, adaptive threshold (on after reset), binarised inference (experiment, off). Safe mode = all off, or a reference predictor driving. | perceptron.md 5; cosim_safe.md **(VERIFIED)** |
| D16 | Rejected with data: probabilistic weight updates, binary weights, a fifth table, unary storage, bit-serial sum, precomputed sums. | perceptron.md 3–4 |

## 4. Verified facts worth not rediscovering

- LibreLane's synthesis may not use drive-strength-1 gates (`no_synth.cells`); `src/config.json` allows the small
  cells and excludes the slow delay buffers. Table cost: `550 + entries × (20.5 × bits + 6.5)` µm².
- Local build of this design: 81.2 % utilisation (density target 84), DRC/LVS/antenna 0, setup +3.87 ns at the
  slow corner for 30 ns, hold +0.06 ns, gate-level tests pass. At 25 ns the sum misses by 2.58 ns (first trial).
  A build takes 20 to 40 minutes on this PC.
- **Commit and push from the Windows folder** (`gh` is logged in there). Never commit in the mirror. A `git push`
  inside WSL hangs.
- `tools/wsl_sync.sh` during a running build is safe only if no source changes; the gate-level test at the end of
  the build uses `test/` of the mirror.
- The harness must keep CTRL[15:4] when it selects the driving predictor: writing `drive` alone clears the feature
  bits (safe mode) and the DBG selections. `cosim.cpp` writes `drive | 0x0DA0 | feat << 12`; `m.ctrl_word()` in the
  model gives the same word. (A first co-simulation ran in safe mode by this mistake: 20 million "mismatches".)
- Heavy runs on a rented 128-thread machine: a sweep of 4,000 candidates on 71 programs takes 9 minutes; the
  co-simulation of 71 programs in three modes 11 minutes (a static Verilator binary and `cosim/run.sh` copied
  there). Locally the co-simulation takes 18 minutes per mode with 10 jobs.
- Never `pkill -f <pattern>` through `wsl.exe -- bash -c "..."`; put kills in a script file.
- The Vast CLI's `create instance` prints a per-instance key with or without `--raw`: do not paste its output.
  `vastai destroy instance` needs `-y`.
- In `explore/bpsim.cc` a local named like a member silently changes behaviour (the `pr` defect of the perceptron
  round): compile with `-Wshadow` when editing it.

## 5. Next steps, in order

1. **Read the CI run of the last push** (`gh run list -R Kyrylo-Nazarevych/bpx1`): gds, precheck, gl_test, test,
   docs. If gds fails on routing in CI (utilisation is 81 %; CI machines place slightly differently), lower the
   risk with `PL_TARGET_DENSITY_PCT` 82, or drop the binarised-inference switch, and rebuild; the fallback tag
   `g3-tage` is always submittable.
2. Owners: review docs/perceptron.md (choice, switches, 33 MHz) and tell the FPGA team about the pin change:
   8-bit `ID`, `uio[1:0]` are chip outputs, `CONF` follows the driving predictor.
3. Open the demo-board schematic PDF to settle the header pins for `clk`/`rst_n` (docs/bringup.md 6).
4. Host kit on hardware: `host/bpx1.py` has not run on a demo board; microcotb port of `test/test.py`.
5. Optional: late-select sum to regain 25 ns (perceptron.md 8); a formal property for the path history like the
   one for the outcome history; Verilator width lint.
6. G4: submission to the shuttle is the owners' step. To submit the fallback instead: `git checkout g3-tage`.

## 6. Commands

```
# from Windows (PowerShell), project folder: mirror into Linux, then run anything from the mirror
wsl.exe -d Ubuntu --cd "<project folder>" -- bash tools/wsl_sync.sh
wsl.exe -d Ubuntu -- bash /home/kyrylo_nazarevych_wsl/work/bpx1/<script>      # never inline $VARIABLES through wsl.exe

# inside WSL, ~/work/bpx1  (source ~/ttsetup/env.sh first)
bash bench/build.sh ; bash bench/ext/build_ext.sh        # benchmarks -> out/bench/<name>/prog.evt (about 2 minutes)
cd test && make -B [GATES=yes]                           # what CI runs: repository top against the golden model
cd test && make -B -f Makefile.bpx [BEHAV=yes]           # bpx_chip with latch cells or the flip-flop model
bash cosim/run.sh ; python3 cosim/report.py              # 71 programs against the RTL -> docs/cosim.md
BPX_DUAL=1 OUT=out/cosim_dual bash cosim/run.sh 16 ; python3 cosim/report.py out/cosim_dual    # -> docs/cosim_dual.md
BPX_FEAT=0 OUT=out/cosim_safe bash cosim/run.sh ; python3 cosim/report.py out/cosim_safe out/cosim/none.tsv   # safe mode
out/explore/bpsim W=8 cosim/bpsim.cfg out/cosim/bpsim.tsv out/bench/*/prog.evt                 # cross-check data for the report
bash cosim/random.sh 10 100000                           # random operations, coverage -> docs/coverage_bins.md
CONC=1 bash cosim/random.sh 10 100000                    # control port concurrent with requests (verification.md 2a)
bash formal/run.sh <prove|bmc|cover|nowait|bmcfull>      # formal tasks
bash tools/harden.sh                                     # local chip build of the repository top + gate-level test
python3 explore/percep.py <rand|list|file|top|effects> ...    # perceptron exploration (bpsim option W=8)
```

## 7. Vast.ai account use (owners' rules)

Up to $12 in total; machines labelled `bpx1-*`; never touch others (`fsk-*`); destroy when idle; report the spend at
every gate. **Spent: about $0.80 in the first exploration round and about $0.60 in the perceptron round (one
128-thread machine, `bpx1-percep`, 1.6 hours at $0.367/h; destroyed) (ESTIMATED from run times); about $1.40 of
$12 in total.** No `bpx1-*` machine exists now **(VERIFIED: instance list after the destroy)**.

## 8. Risks

| Risk | Status |
| --- | --- |
| Routing at 81 % utilisation in CI | Local build clean after 5 routing passes. CI is the confirmation (step 1). |
| A defect neither simulation nor the proofs see | Reduced: the proofs cover the sequencer under free inputs; the perceptron arithmetic is covered by simulation against the model in all 16 feature settings, and the model equals the separately written simulator exactly on all programs. |
| The FPGA side still assumes 10-bit ID and input-only `uio` | Open until the owners pass on D12 (step 2). An FPGA driving `uio[1:0]` would fight the chip's outputs. |
| Latch write paths outside static timing | Accepted by construction; exercised at cell and gate level (docs/bringup.md 4). |
| Benchmark programs have few branch sites (median 49) | Known limit; `embench.nsichneu` (640 sites) is the stress case, and there the perceptron gains 21 points on the tagged tables. |
| The FPGA CPU does not exist yet | The contract (SPEC 9) is what the numbers assume. |

## 9. Open questions for the owners

1. Is 33 MHz with 90 ns to answer acceptable, or do you want the 40 MHz variant with a fourth clock (100 ns)?
   (perceptron.md 6; my recommendation is the present one.)
2. Should binarised inference stay in the chip as an experiment (cost: about 40 cells on the predict path)?
