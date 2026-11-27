# BPX-1 handover

Everything a fresh session needs to continue: state, decisions, next steps, commands. Last rewritten at the end of
the mosaic round (27 Nov 2026). Evidence labels as in the requirements specification, section 10. Read
`docs/SPEC.md` next, then `docs/mosaic.md` (the round's plan, design and progress log), `docs/verification.md` and
`docs/experiments.md`. Figures still to come from the last runs are written `TBD(final)`.

## 1. State in one paragraph

BPX-1 is a branch-prediction coprocessor for Tiny Tapeout (SKY130, 2×2 tiles): a hashed perceptron and three
reference predictors (bimodal, gshare, tournament) on one branch stream. In this round (26–27 Nov 2026) it became a
**mosaic of dependability experiments** on the owners' brief: a leaky twin of one weight table (256 floating-node
cells) with switchable codes, a write-pulse selector, canary flops, invariant checkers and a watchdog, demotion to
the tournament, gshare randomness, two ring oscillators and a reset strap, all switched by FCTL (and CTRL[15]
MASK); with FCTL = 0 and CTRL[15] = 0 every predictor is its plain verified version. To make room the perceptron was
trimmed (4-bit weights, 8-bit path history, binarised inference removed: 90.93 % against 91.42 % in the simulator).
Steps 1–4 are verified and built clean (step 4: 84.34 %, +2.06 ns at 30 ns) **(VERIFIED, progress log, mosaic.md
9)**. Step 5's RTL is in (`f78ae8f`), its design with all five statistics counters did not route (86 %), so the
per-predictor misprediction counts moved to the FPGA (statistics offload, D30), and the clock is 33 ns (D29). The
final build, the full regression and CI are being run: their figures are `TBD(final)`. `main` stays at the
perceptron design (tag `g3-perceptron`) until the mosaic is merged.

## 2. Where things are

| What | Where |
| --- | --- |
| Project folder (source of truth, Windows; **the git repository is here**) | `C:\\Users\\Kyrylo\\Desktop\\Claude Folder\\Branch Predictor Hackathon 2` |
| Linux mirror where every tool runs | `~/work/bpx1` (WSL Ubuntu), filled by `tools/wsl_sync.sh` (which also overwrites the mirror's `.git`); generated data in `~/work/bpx1/out` |
| Fault-campaign raw results | `~/work/bpx1_fi/out/*.tsv` (WSL); aggregated tables and logs in `explore/fault_results/` on branch `campaign` |
| GitHub | `Kyrylo-Nazarevych/bpx1`, public; Pages enabled (source: GitHub Actions) |
| Requirements | `..\\Branch Predictor Hackaton 1\\plan\\FABLE_ALL_IN_BRIEF.md` (v4.1), the owners' decisions in `docs/spec_review.md` section 6, the owners' message of 26 Nov 2026 (perceptron; docs/perceptron.md) and the owners' brief for the mosaic (docs/mosaic.md 1) |

**Branches and tags.**

| Name | What |
| --- | --- |
| `main` | the perceptron design, tag `g3-perceptron`; stays submittable until the mosaic is merged |
| `mosaic` | the mosaic, steps 2–5 (`08ac75f`, `2837285`, `ca279c9`, `f78ae8f`); merged to `main` after the full regression and CI |
| `probe-leaky` | probe: one 16 × 8 table of floating-node cells (`b428b21`); never for `main` |
| `probe-ro` | probe: two ring oscillators of named cells, divider, entropy bit (`67eaabd`); never for `main`; its ring A is the mosaic's ring, twice |
| `campaign` | pre-silicon fault-injection campaign and coset-mask comparison (`fe62aa8`, `4041870`); report copied to `docs/fault_campaign.md` |
| `docs-exp` | guide to the silicon experiments (`856b8d1`); copied to `docs/experiments.md` |
| `docs-final` | this document pass, based on `f78ae8f` |
| tag `g3-tage` | tagged-table design, verified to G3: the fallback |
| tag `g3` | the same RTL with the last test harness of that design |
| tag `g3-perceptron` | the perceptron design, CI green on `793a339` |

Repository layout: `src/` RTL (`bpx_ro.v` the ring oscillators, `bpx.sdc` the timing exception); `model/` golden
model, random-test generator, replay; `test/` cocotb (CI); `cosim/` Verilator harness, reports, coverage,
concurrent-control-port checker; `formal/` SymbiYosys; `explore/` sweep engine and `bpsim` (also the fault
injection of the campaign); `bench/` benchmarks; `host/` MicroPython host; `tools/` scripts; `docs/` documents.

Generated documents (do not edit by hand): `docs/cosim.md`, `docs/cosim_dual.md`, `docs/cosim_safe.md`
(`cosim/report.py`), `docs/coverage_bins.md` (`cosim/coverage.py`), `docs/exploration_tables.md`. They are written in
the mirror: copy them back to the Windows folder after generating (the sync is one-way, Windows to Linux).

## 3. Decisions (each with its evidence in the named document)

D1–D16 are from the earlier rounds; those changed in this round are marked.

| # | Decision | Evidence |
| --- | --- | --- |
| D1 | Host port: two-phase REQ/ACK, **8-bit ID**, 2-bit OP; answer at the third clock after the request. PREDICT+RESOLVE = predict, then resolve the oldest (resolve first if the queue is full). Queue depth 4. | SPEC 2–3 |
| D2 | Control port: SPI mode 0 with 16-bit registers on `uio[4..6]` + `uo[7]`. | SPEC 4 |
| D3 | Storage: latch tables written by flop-generated pulses; one write sequencer; one read port per table; training from a staging register. | SPEC 6 |
| D4 | No hard macro. | spec_review 4 |
| D5 | Default predictor: hashed perceptron, 4 tables × 32 entries, histories 0/8/9/20, reference counters as inputs, adaptive threshold, negation in memory. **Changed by D19:** 4-bit weights (was 5), 8-bit path history (was 16), 512 bits. | perceptron.md 1–3, 9 |
| D6 | U1 CPU contract: five stages with a 256-entry branch-target buffer at fetch. | SPEC 9 |
| D7 | Benchmark set frozen: 71 programs; `embench.nbody`/`cubic` out of the aggregates; 51 tuning / 18 held-out. | exploration.md 1 |
| D8 | Statistics: 16-bit saturating counters, snapshot and snapshot-and-clear. **Changed by D30:** two counters in the chip (resolved branches, mispredictions of the driving predictor). | SPEC 4 |
| D9 | Single clock domain; data pins use one capture flop qualified by the 2-flop synchronised REQ. | `src/bpx_chip.v` header |
| D10 | CPU adapter: a mispredicted resolve is sent alone. Not a chip decision: both figures are quoted, the FPGA side tries both modes. | SPEC 9 rules 4 and 7 |
| D11 | The tables' read port is reserved for a predict only in the clock it executes (deadlock fix). | verification.md 4 |
| D12 | ID width 8 bits; `uio[1:0]` are outputs `PBIM`, `PGSH`; with `DBG0`/`DBG1` (reset selections) all four predictions are on pins with every `ACK`. Since D30 these pins are also how the FPGA counts each predictor's mispredictions. | perceptron.md 7 **(VERIFIED, 4,000 candidates)** |
| D13 | `CONF` is the confidence of the driving predictor. | perceptron.md 7; SPEC 2 |
| D14 | ~~Clock 30 ns (33.3 MHz)~~, latency 3 clocks. **Superseded by D29 (33 ns, 99 ns).** | perceptron.md 6 |
| D15 | Switches CTRL[14:12]: reference inputs, negation, adaptive threshold. Safe mode = all off, or a reference predictor driving. **Changed:** CTRL[15] binarised inference removed (D19); CTRL[15] is now MASK (D27). | perceptron.md 5; cosim_safe.md |
| D16 | Rejected with data: probabilistic weight updates, binary weights, a fifth table, unary storage, bit-serial sum, precomputed sums. | perceptron.md 3–4 |
| D17 | **The mosaic** (owners' brief, 26 Nov): research value first; quirks spread over the predictors, each switchable, off = plain verified predictor; physically risky storage on one part only; the reference predictors' tables always get the standard pulse; accuracy and clock may be sacrificed if the chip still clearly beats static not-taken. Left-out items and reasons in mosaic.md 3. | mosaic.md 1–3 |
| D18 | **Leaky-cell probe first.** One 16 × 8 table of `einvp_1` floating nodes on its own branch through the flow: clean locally (128 cells kept, DRC/LVS/antenna 0, gate-level pass with the read masked) **(VERIFIED, progress log, probe)**. CI precheck of the probe: TBD(final). | mosaic.md 5, 9 |
| D19 | **Trims to make room:** weights 5 → 4 bits, sum 9 → 8 bits, path history 16 → 8, binarised inference removed. Cost 91.42 → 90.93 % accuracy, 1.13596 → 1.13439× (all 69 programs, simulator); utilisation 81.2 → 74.2 %. | mosaic.md 9 step 2 **(VERIFIED, progress log)** |
| D20 | **Reference-input weights 2/4/4, θ start 10.** The 16-candidate sweep put every θ and (2,3,3)/(2,4,4) within 0.0001× (noise); step 2 took (2,3,3), step 3 went back to (2,4,4) because ×4 is a shift and ×3 put an adder on the critical path. | mosaic.md 9 steps 2–3 **(VERIFIED, progress log)** |
| D21 | **Leaky twin W3L** (32 × 8 floating nodes: 4 data + extended Hamming (8,4) check bits), written with W3 by the standard pulse, compared at every predict; codes none / parity / SEC-DED / detect-all; **training always from the static tables**, so W3 stays golden and the learned state is the same with the twin on or off. | mosaic.md 7.3 |
| D22 | **Restore-on-use instead of a refresh walker.** A walker needs a second read port on the twin (1,100 µm²) or the core's read port (proof impact); restore-on-use covers the entries in use, the FPGA can scrub the rest by raw access. | mosaic.md 3, 7.3 |
| D23 | **Canary instead of a late-clock shadow flop.** Razor needs every short path padded to half a clock; the canary (a second flop through a delay, early warning) needs none. On PRED and CONF; event 4 one clock after the ACK. | mosaic.md 3, 7.5 |
| D24 | **Timing exception `src/bpx.sdc`:** the flow's base SDC plus `set_disable_timing` on `einvp_1` TE → Z. The arc made a false path (pulse → twin node → sum) of −2.60 ns; without it +3.21 ns on the true path. **Proven** that no flop takes table read data unless the write sequencer is idle (formal assertion). | mosaic.md 7 (step 4 audit), 9 step 3 **(VERIFIED, progress log)** |
| D25 | **The standard pulse is structurally separate:** `u_wseq.u_wp` drives only BC, G, W3, the twin and the snapshot; `u_wpx` (the short pulse) only W0–W2. Checked in the final step-4 netlist; a mutation putting W3 on the short pulse is caught. | mosaic.md 9 step 4 **(VERIFIED, progress log)** |
| D26 | **Fault campaign findings** (simulator, 69 programs): per bit the weight tables are the most sensitive part (0.18–0.23 extra mispredictions per flip for the default); faults do not heal; on a leaking twin detect-all is best without restore, SEC-DED with restore-on-use from τ ≈ 1e3 branches; all-0 and all-1 words are code words, so one-directional decay is silent under every code. | fault_campaign.md 3–4 **(VERIFIED, fault_campaign.md)** |
| D27 | **Switchable code mask:** CTRL[15] MASK stores `code word ^ 8'hF0` (check bits inverted, data untouched): a fully decayed word abstains under SEC-DED and detect-all, CODE 0 unchanged. Switchable because with one-directional decay and SEC-DED + restore no mask gives fewer mispredictions (0.69 against 1.54 per 1000 at τ = 1e4). A change of MASK invalidates the staging register; clear the predictors after switching. | fault_campaign.md 7; mosaic.md 7.7 |
| D28 | **Strap combination:** OP = 3 **and** CSn = 0 **and** MOSI = 1 at the last clock edge of reset keeps the tables (KEPT). OP = 3 alone is an ordinary request code a host may leave on the pins. | mosaic.md 7.7; SPEC 5 |
| D29 | **Clock 33 ns (30.3 MHz)**, owners' decision of 27 Nov, latency 3 clocks = 99 ns: the canaries (now 2 × `dlygate4sd3_1`) lead by about 1 ns typical. Slack at 33 ns: TBD(final). | `src/config.json`; timing.md |
| D30 | **Statistics offload.** With five counters the design reached 86 % and did not route. The chip keeps resolved branches and the mispredictions of the answer given on PRED (the driving predictor); STATS 0x09–0x0E read 0. The FPGA counts each predictor's mispredictions from the pins (PRED, PBIM, PGSH, DBG0 default, DBG1 tournament with the reset selections) and the outcomes it sends. A slow SPI-only host keeps the branch count and the mispredictions of the answers it received. The test harness counts per-predictor mispredictions from the pins against the golden model. Utilisation about 81 % (final: TBD(final)). | owners' decision, 27 Nov; SPEC 4 (being updated) |
| D31 | **Rejected: an ID narrower than 8 bits** with the mapping in the FPGA: about 400–600 µm² **(ESTIMATED from the flop count)** against accuracy and independence from a clever glue layer. | owners' decision, 27 Nov |
| D32 | **Rented build machine:** chip builds and the big regressions run on a rented 92-CPU machine with the same tool versions (LibreLane 3.0.14 image exported). It reproduced the local step-4 build exactly (84.34 %, +2.057 ns, byte-identical netlist). One build is not faster there; several run at once. Random regression 92 × 100,000 plain and concurrent: 109 s; 71-program co-simulation in three modes: 428 s; full-size bounded proof: 547 s (it timed out locally). | owners' report, 27 Nov **(VERIFIED by the owners' report; not recorded in the repository)** |

The wrapper scripts for the rented machine live in a session scratch folder, **not in the repository**: they would
have to be recreated. What they do: copy the mirror's sources to the machine, load the exported LibreLane 3.0.14
image, start builds or regression jobs in parallel (one per CPU for the random runs and the co-simulation), and copy
the reports and generated documents back. Destroy the machine when idle.

## 4. Verification status

| What | Status |
| --- | --- |
| Steps 2–4: cocotb (latch cells, flip-flop model, repository top), random plain and concurrent, `RG_PLAIN=1` (FCTL = 0), co-simulation subsets, mutation checks, formal prove / nowait / cover, local build and gate level | pass **(VERIFIED, progress log, mosaic.md 9)**; details in verification.md |
| Step 5 (gshare randomness, oscillators, strap, mask, 33 ns, statistics offload) | TBD(final) |
| Full regression: 71 programs in three modes, random 92 × 100,000 plain and concurrent, formal including `bmcfull`, cocotb, final build | TBD(final) |
| Pre-silicon fault campaign | done **(VERIFIED, fault_campaign.md: 61 and 51 minutes on the owner's PC)** |
| CI of the mosaic | TBD(final) |

## 5. Verified facts worth not rediscovering

- LibreLane's synthesis may not use drive-strength-1 gates (`no_synth.cells`); `src/config.json` allows the small
  cells and excludes the slow delay buffers. Table cost: `550 + entries × (20.5 × bits + 6.5)` µm².
- The resizer may swap a delay cell for a buffer: the delay line, the canary cells and the ring cells are protected
  by `RSZ_DONT_TOUCH_RX`. Do not protect a net whose loads are all untouchable: the first step-5 build stopped with
  `[RSZ-3006]` on the ring's run net; each ring's run pins are driven by an unprotected `buf_2`.
- Placement moves slacks: the real CONF flop's slow-corner slack was +2.18, +1.16 and +2.46 ns in three step-4
  builds with small changes. Keep at least 0.3 ns on the canaries and re-read the report after every build.
- With the PDK's gate-level models the floating nodes read X and a running ring is a zero-delay loop: gate-level
  tests keep FCTL[0] = 0, FCTL[11:10] = 0 and PULSE = 0.
- A build takes 20 to 40 minutes on this PC.
- **Commit and push from the Windows folder** (`gh` is logged in there). Never commit in the mirror. A `git push`
  inside WSL hangs.
- `tools/wsl_sync.sh` during a running build is safe only if no source changes; the gate-level test at the end of
  the build uses `test/` of the mirror.
- The harness must keep CTRL[15:4] when it selects the driving predictor: writing `drive` alone clears the feature
  bits (safe mode) and the DBG selections. `m.ctrl_word()` in the model gives the right word.
- Never `pkill -f <pattern>` through `wsl.exe -- bash -c "..."`; put kills in a script file.
- The Vast CLI's `create instance` prints a per-instance key with or without `--raw`: do not paste its output.
  `vastai destroy instance` needs `-y`.
- In `explore/bpsim.cc` a local named like a member silently changes behaviour: compile with `-Wshadow`.

## 6. Next steps, in order

1. Fill every `TBD(final)` in the documents from the final runs (search for the string).
2. Final build at 33 ns with the statistics offload; then CI of the `mosaic` head; then merge to `main` and tag.
3. Tell the FPGA team: the statistics offload (D30), the strap combination and what to drive during reset
   (bringup.md), 33 ns, and `docs/experiments.md`.
4. Open the demo-board schematic PDF to settle the header pins for `clk`/`rst_n` (bringup.md 6).
5. Host kit on hardware: `host/bpx1.py` has not run on a demo board; microcotb port of `test/test.py`.
6. Optional: late-select sum (perceptron.md 8); a formal property for the path history; Verilator width lint.

## 7. Commands

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
bash cosim/random.sh 10 100000                           # random operations, coverage -> docs/coverage_bins.md
CONC=1 bash cosim/random.sh 10 100000                    # control port concurrent with requests
RG_PLAIN=1 bash cosim/random.sh 4 50000                  # FCTL kept at 0: the plain chip
bash formal/run.sh <prove|bmc|cover|nowait|bmcfull>      # formal tasks
bash tools/harden.sh                                     # local chip build of the repository top + gate-level test
python3 explore/fault_campaign.py ...                    # fault campaign (branch campaign; fault_campaign.md)
```

## 8. Rented machines (owners' rules)

Vast.ai: up to $12 in total; machines labelled `bpx1-*`; never touch others (`fsk-*`); destroy when idle; report
the spend at every gate. Spent before this round: about $1.40 **(ESTIMATED from run times)**. This round: the
92-CPU build machine (D32); its spend: TBD(final). Whether any `bpx1-*` machine still exists: TBD(final) (settled by
the instance list).

## 9. Risks

| Risk | Status |
| --- | --- |
| Routing at about 81 % with the offload | Five counters at 86 % did not converge; final figure TBD(final). |
| A defect neither simulation nor the proofs see | The proofs cover the sequencer, the checker flags (stay 0) and the timing-exception condition under free inputs; the predictor and mosaic logic are covered by simulation against the model. |
| Physical behaviour that cannot be simulated (retention, short pulses, timing faults, oscillators) | By design: these are the silicon experiments (experiments.md); with FCTL = 0 none of it is in the path. |
| The FPGA triggers the strap by accident | Needs OP = 3, CSn = 0 and MOSI = 1 together at the end of reset (bringup.md 2). |
| The FPGA side still assumes five statistics counters | Open until the FPGA team is told (step 3). |
| Latch write paths outside static timing | Accepted by construction; exercised at cell and gate level. |
| Benchmark programs have few branch sites (median 49) | Known limit; `embench.nsichneu` (640 sites) is the stress case. |
| The FPGA CPU does not exist yet | The contract (SPEC 9) is what the numbers assume. |
