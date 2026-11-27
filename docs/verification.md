# BPX-1 verification (DR-02, VR-01 to VR-06)

Evidence labels as in the requirements specification, section 10. Updated for the mosaic round (27 Nov 2026).
**(VERIFIED, progress log)** means recorded as run in docs/mosaic.md section 9 by the session that built that step;
the final full regression is the entry "step 5 completed" there, run on the committed source `69d0bf4`. Sections 2–5 describe the harnesses and proofs, which the
mosaic extends (section 7). The commands are at the end.

## 1. Summary

### 1.1 The mosaic (branch `mosaic`)

| Requirement | Evidence | Result |
| --- | --- | --- |
| VR-01 golden model, bit-exact, every quirk | random regression with fault injection, plain and concurrent (section 7); co-simulation of 71 programs in three modes | steps 2–4: 0 mismatches; final: **0 mismatches** in 421,243,679 program requests (140,267,792 / 140,281,743 / 140,694,144 in the three modes) and in 92 × 100,000 random operations plain (9,200,750 requests) and concurrent (9,200,759), plus 8 × 1,000,000 for counter saturation (8,000,637) **(VERIFIED, progress log, step 5)** |
| Plain chip (FCTL = 0) equals the verified design | `RG_PLAIN=1` random runs; co-simulation with FCTL never written | steps 3–4: 0 mismatches; final 92 × 100,000 plain (9,200,796 requests) and concurrent (9,200,776): 0 mismatches **(VERIFIED, progress log, step 5)** |
| VR-02 coverage | docs/coverage.md, coverage_bins.md | step 4: cross 52/52, other 178/178 reachable; final: **295 of 295** reachable bins (cross 52/52, other 243/243; 16 cross and 14 other bins cannot occur) **(VERIFIED, progress log, step 5, and docs/coverage_bins.md)** |
| VR-03 formal, with the mosaic's assertions | section 7.4 | final: prove, nowait, cover, bmc and `bmcfull` (full size, 30 clocks) all pass **(VERIFIED, progress log, step 5)** |
| VR-04 gate level | local build + gate-level test (twin, rings, short pulse off) | step 4: 4 passed + 4 skipped; final build (density 80): 6 passed + 5 skipped (RTL only) **(VERIFIED, progress log, step 5)** |
| VR-05 CI | CI of the `mosaic` head | see section 6 |
| VR-06 speedup and accuracy (plain chip) | cosim.md, cosim_dual.md, cosim_safe.md | default 1.1307× over static not-taken (tuning 1.1280, held-out 1.1384), 90.93 % accurate; 1.1345× with two requests per cycle; safe mode 1.1249× / 89.06 % **(VERIFIED, progress log, step 5, and the three reports)** |
| Mutation checks (the test of the tests) | section 7.3 | every listed mutation caught, steps 3–5 (20 of 20 in step 5) **(VERIFIED, progress log)** |

### 1.2 The perceptron design before this round (tag `g3-perceptron`)

| Requirement | Evidence | Result |
| --- | --- | --- |
| VR-01 golden model, bit-exact | 71 programs × (CPU in-flight run + one-in-flight replay), in three runs (one request per cycle, two requests per cycle, safe mode), every request replayed through `model/bpx1.py`; 10 × 100,000 constrained-random operations with statistics and full-state comparison | **0 mismatches** in 420,866,452 program requests and 1,000,109 random requests **(VERIFIED)** |
| FR-10 in a live run (U4) | section 2a: control port working while requests run, 10 × 100,000 operations | 8,288 background jobs (snapshots, snapshot-and-clears, raw reads and writes), 11,733 checked values, **0 mismatches**; replies and final state still equal the model; the reverted deadlock fix is caught in 4 of 4 runs **(VERIFIED)** |
| VR-02 functional coverage | `docs/coverage.md`, `docs/coverage_bins.md` | 52 of 52 reachable bins of the required cross, 125 of 125 other bins **(VERIFIED)**; 16 cross bins excluded, each by a formal assertion |
| VR-03 formal | section 4 | all assertions proven by k-induction (unbounded) and checked by BMC to 60 clocks; 12 cover traces **(VERIFIED)**; found two defects in the RTL (before this design), both fixed |
| VR-04 gate level | `tools/harden.sh` locally on the built netlist; CI job `gl_test` | passes locally **(VERIFIED)** and in CI **(VERIFIED by CI)** |
| VR-05 CI reports reviewed | section 6 | gds, precheck, gl_test, viewer, test, docs green on commit `793a339` |
| VR-06 system co-simulation | `docs/cosim.md`, `docs/cosim_dual.md`, `docs/cosim_safe.md` | default predictor 1.1324× over static not-taken (1.1361× with two requests per cycle), 91.42 % accurate **(VERIFIED)** |
| PF-01 | cosim.md 2 | default 1.1324 > tournament 1.1239 > gshare 1.1180 > bimodal 1.1161 > 1 **(VERIFIED)** |
| PF-02 | cosim.md 2, 3 | default 91.42 % > tournament 88.92 % > gshare 87.29 % > bimodal 86.47 %; below the best reference on 2 of 69 programs (`sys.json` −0.68 points, `interp.expr` −0.27) **(VERIFIED)** |
| PF-03 | cosim.md 4 | in-flight 91.42 % = one-in-flight 91.42 %; largest loss on a program 0.02 points **(VERIFIED)** |
| Safe mode (owner requirement) | cosim_safe.md; random regression with all 16 feature settings | CTRL[15:12] = 0: 1.1277× / 89.97 %, ahead of every reference, 0 mismatches against the model on all programs **(VERIFIED)** |

Against the tagged-table design (tag `g3-tage`, same benchmark set and CPU contract, 10-bit identifiers): 1.1317× /
1.1353× / 91.24 % there, 1.1324× / 1.1361× / 91.42 % for `g3-perceptron` **(VERIFIED on RTL in that round)**. These
figures are for 5-bit weights. The mosaic's trimmed perceptron: 1.1307× / 1.1345× / 90.93 %; tournament 88.92 %,
gshare 87.29 %, bimodal 86.47 % (unchanged); the default is below the best reference on 2 of 69 programs (`sys.json`
−0.78 points, `interp.expr` −0.09), on 31 in safe mode **(VERIFIED, progress log, step 5, and docs/cosim*.md)**.

**Statistics offload (owners' decision, 27 Nov).** The chip now counts only resolved branches and the
mispredictions of the driving predictor (STATS 0x09–0x0E read 0). The test harness counts each predictor's
mispredictions from the prediction pins (PRED, PBIM, PGSH, DBG0, DBG1 with the reset selections) and the outcomes it
sent, and compares them with the golden model; the two chip counters are compared as before. In the final run all
three harnesses carry it **(VERIFIED, progress log, step 5)**: `cosim/cosim.cpp` keeps its own copy of the queue and
counts from the pins as the FPGA would (random runs: 9,631 statistics windows checked, 0 mismatches; concurrent runs:
pin totals equal to the model in 92 of 92 runs, `cosim/conc_check.py` line H; program co-simulation: 568
per-predictor totals per mode equal to the model, and in all 426 runs per mode the chip's two counters equal the
harness's counts); `test/test.py` counts from the pins in its random and trace tests; `host/bpx1.py` `replay()`
counts from the pins (PBIM/PGSH through `uio_out`; not run on hardware). Where the sections below describe the
perceptron round's "five statistics counters", read: the two counters plus the pin counts.

## 2. The golden model and what "bit-exact" covers (VR-01)

`model/bpx1.py` is a transaction-level model: one call per request, no notion of clocks. The RTL has to give the
same `PRED` and `CONF` for every request whatever the timing, because both follow the same order rules
(header of `src/bpx_core.v`). Three independent harnesses compare them:

| Harness | Stimulus | Compared | Volume |
| --- | --- | --- | --- |
| `cosim/run.sh` (Verilator, pins of `bpx_chip`) | the U1 CPU contract on each benchmark program: up to 4 branches in flight, wrong-path predicts, mispredictions, FLUSH after register-indirect jumps; and each program again one branch at a time | every reply; the statistics at the end (since the offload: the two chip counters and the four per-predictor counts taken from the pins) | 71 programs: both adapter modes, and the safe mode of the default predictor |
| `cosim/random.sh` (Verilator, pins) | `model/randgen.py`: random operations including misuse (resolve with nothing in flight, predict when full), random gaps (0 to 9 clocks) and set-up (0 to 2 clocks), changes of the driving predictor and freeze, clear predictors, clear statistics, soft reset | every reply; the four prediction pins at every accepted predict; statistics after 996 snapshots (with and without clear, including saturated counters and the overflow flag); **every state bit** through the raw-access port 1,540 times, with and without branches in flight; the 16 feature settings of the default predictor and raw writes of its threshold, changed in mid-stream | 10 seeds × 100,000 |
| `test/test.py` (cocotb + Icarus; CI runs it on RTL and on the gate-level netlist) | random operations with request edges at random picosecond offsets inside the clock period; a trace replay; a directed test of raw-access writes (whole entries, single fields, neighbours, the small registers, CTRL after reset, and that a written counter is the one predicted from) | every reply, the four prediction pins, statistics, every state bit | 3,000 + 3,000 per run (700 at gate level) |

The Verilator harnesses use the flip-flop model of the tables (`BPX_BEHAV`); the cocotb tests run both that model
and the real latch cells with their write pulses, and the gate-level test runs the placed-and-routed netlist.

Limits of this evidence: the model and the RTL were written by the same author from the same specification, so a
misreading of the specification would be in both. The cross-check against the separately written simulator
(`explore/bpsim`, cosim.md 6): with one branch in flight the default predictor's misprediction counts are equal
on all 69 programs, and its mean accuracy in the CPU runs is the same in both (91.42 % in the perceptron round,
90.93 % for the mosaic); the references agree on 63 to 69 of 69 programs in the CPU runs, the in-flight default on
45 of 69 (the misprediction-alone rule) **(VERIFIED: cosim.md 6; progress log, step 5)**. (The perceptron has no pseudo-random element, unlike the tagged-table
design, so nothing is left to differ.)

## 2a. The control port during a live run (FR-10, U4)

`CONC=1 bash cosim/random.sh` runs the same random requests with the control port working **at the same time**:
the harness has a second, independent SPI driver that advances with every clock (`cosim/cosim.cpp`, background
control port; SCK at the fastest rate the specification allows, 8 clocks per bit). The generator
(`model/randgen.py conc`) starts background jobs without waiting for them:

| Job | What is checked (`cosim/conc_check.py`) |
| --- | --- |
| Snapshot, snapshot-and-clear, then 12 words read | The five counters, added up over all snapshot-and-clear steps, equal the model's running totals after one whole number of requests inside the window in which the snapshot was taken: consistent, nothing lost, nothing counted twice. ERRCNT and the flags lie between their values at the ends of the window. |
| Raw read of 1 to 6 entries of any table, or of the small registers | Each word equals the model's entry after some whole number of requests inside its window: never a half-written value. (The speculative history is left out: it changes inside a request.) |
| Raw write of a bimodal+chooser entry no branch in use maps to | Never dropped; the model applies it too; the state dumps and replies that follow prove it landed once and disturbed nothing. |
| ID and CTRL read | Exact. |

The window of a word is from two requests before the last one acknowledged when the chip fetched it, to the last
one started: typically 2 to 20 requests. Replies, and the complete state at every dump, are still compared exactly.

Result, 10 runs of 100,000 operations, final RTL **(VERIFIED)**: 1,000,095 requests, 0 reply mismatches, 1,393
state dumps with 0 differing words; 8,288 background jobs completed (14,284 more were requested while one was
running and dropped): 953 snapshots, 887 snapshot-and-clears, 2,328 raw reads (7,590 words), 3,657 raw writes, 463
ID and CTRL reads; 11,733 checks, **0 mismatches**.

Two tests of the test **(VERIFIED)**: with the deadlock fix of section 4 reverted, 4 of 4 runs stop with "no
acknowledge" within 21,000 steps, so this simulation would have found the deadlock; and one flipped bit in a logged
raw word, or a snapshot count off by one, is reported.

## 3. System co-simulation (VR-06)

`cosim/cosim.cpp` is the CPU of docs/SPEC.md section 9 reduced to what determines cycle counts: a five-stage
in-order RV32IM pipeline with a 256-entry branch-target buffer. It replays the instruction stream of each program
(recorded with Spike) and drives the chip's pins: predict in IF on a BTB hit, in ID on a miss; follow the
prediction; fetch and predict down the wrong path until the branch resolves in EX; report the outcome in the next
cycle; FLUSH after a register-indirect jump. Penalties: misprediction 2 cycles, taken branch found in ID 1,
JAL 0 or 1, JALR 2, load-use 1, divide 32. Static not-taken on the same CPU without a chip is the baseline.

Each program runs with each of the four predictors driving and with the chip forced to static not-taken, and every
run reports all four predictors' mispredictions (since the statistics offload the harness counts them from the
prediction pins, as the FPGA will; the chip counts only resolved branches and its own wrong answers). Results: `docs/cosim.md`
(one request per CPU cycle), `docs/cosim_dual.md` (two requests in a cycle where needed, SPEC 9 rule 7) and
`docs/cosim_safe.md` (the default predictor in its safe mode, CTRL[15:12] = 0).

What the co-simulation found that the exploration model had not modelled: a resolve that reports a misprediction
cannot be combined with the predict of the same cycle (the chip would discard that predict with the wrong path).
Sent alone, it costs the CPU one bubble on about half of the mispredictions: 0.0037 (1.1324× against 1.1361×) of speedup. Rule 7 removes
that cost without any change to the chip.

## 4. Formal (VR-03)

Tool: SymbiYosys with Boolector. Design under proof: `bpx_core` with `bpx_pred` and the flip-flop table model, all
inputs free in every cycle: the request level, identifier and operation may change at any time, and the
control port may write any register at any time. So every property holds for any host timing (IR-01) and any
misuse (IR-09), including requests withdrawn or overlapped and commands in the middle of a request. Only a reset in
the first cycle is assumed. Properties: the `ifdef FORMAL` block at the end of `src/bpx_core.v`.

| Property (requirement) | Statement |
| --- | --- |
| Capacity (FR-03) | The in-flight count never exceeds 4; `FULL` equals "count is 4"; a refused predict changes neither the queue nor the histories; the count and the head pointer move by exactly one per predict and per resolve. |
| Speculation and repair (FR-04) | At every clock the speculative history equals the committed history followed by the driving predictions of the in-flight branches, oldest first; after a mispredicted resolve the queue is empty and the two histories are equal. |
| Flush (FR-05) | After a flush the queue is empty, speculative = committed history, the committed history is unchanged and no training was started. |
| Predict changes no trained state (FR-01) | A predict without a resolve changes neither the committed history, nor the threshold, nor its adaptation counter, nor the statistics, and starts no table write; those registers change only through a resolve, a clear or a raw write; a table write starts only for a resolve (and only when not frozen), a clear, a raw write or a snapshot. |
| One acknowledge per request (IR-01) | `ACK` changes only when a request is pending, and then to the request level. |
| Stable replies (RR-04) | `PRED` and `CONF` change only in the clock in which `ACK` changes. |
| Misuse (IR-09) | A resolve with nothing in flight is acknowledged, counted and flagged, and changes neither the queue nor the histories. Everything above holds under free inputs. |
| Helper invariants | A deferred resolve or a staged entry implies a non-empty queue; the resolve-first flag is set only while its request is still waiting; at most one kind of operation executes per clock. |
| Liveness (RR-03) | Whenever the inputs have been quiet for 20 clocks, the chip is idle and ready: no request pending, no deferred resolve, no write in progress, no command pending, the oldest in-flight branch staged. Since a soft reset is one of the free inputs, recovery after it is included. |

| Task | What | Table sizes | Result |
| --- | --- | --- | --- |
| `prove` | k-induction, k = 24: every assertion from **any** state that satisfies the assertions, so no depth limit | 8 / 16 / 8 entries | **proven** (6.3 minutes) **(VERIFIED)** |
| `bmc` | bounded check from reset, 60 clocks | 8 / 16 / 8 | pass **(VERIFIED)** |
| `bmcfull` | bounded check from reset, 30 clocks | the chip's 64 / 128 / 32 | no violation in clocks 0 to 26; stopped by the 40-minute limit while checking clock 27 **(VERIFIED)** |
| `cover` | 12 traces: each operation completing; the queue filling; a refused predict and a refused resolve; resolve-first with a full queue; a mispredicted resolve with a full queue, the predict after it, and the return to idle with three branches resolved | 8 / 16 / 8 | all reached within 15 clocks **(VERIFIED)** |
| `nowait` | legal host and silent control port: the requests that can never be made to wait (docs/coverage.md) | 8 / 16 / 8 | **proven** by k-induction (2.2 minutes) **(VERIFIED)** |

**Depth and why it suffices.** The induction proof has no depth: it shows that no state satisfying the assertions
can step to a state violating one, and the bounded check shows that reset leads into that set. The bounded depth of
60 clocks is redundant with it; from reset it reaches READY (5 clocks), a full queue (4 predicts), a misprediction
with a full queue and its repair (cover trace: 11 clocks) several times over. The liveness bound of 20 clocks is the
longest chain of pending work: a clear, a raw write and its read-back, a snapshot, a deferred resolve with its
staging read and its 3-clock table write.

**Small tables.** The proofs run with tables of 8, 16 and 8 entries instead of 64, 128 and 32. The properties are
about the sequencer, the queue, the histories and the handshake; the table size enters only through the width of
the index, which no property mentions. The full-size design is checked by `bmcfull` to the depth it reaches in
reasonable time, and by simulation.

**What the proofs do not cover.** The values that training writes into the tables (the predictor algorithm, with
its sum, threshold and features): these are checked against the golden model by simulation only. The path history
has no property of its own (the outcome history has): it is compared with the model in every state dump. The latch tables themselves and their write pulse: the
proofs use the flip-flop model; the latch construction is argued in docs/SPEC.md 6.2 and exercised at cell level
and gate level. The synchronisers and the SPI shifter in `bpx_chip`/`bpx_spi`: simulation only.

### The deadlock the liveness proof found (in the tagged-table design, before gate G3)

The first run of the liveness property failed after 28 clocks. A pending predict held the tables' read port
(`hold_p = pending & predict`) so that its lookup would not be displaced. But a predict can be pending without
being able to run: when the resolve of the previous combined request is still deferred, or when the queue is full
and its own resolve must go first. Both wait for the staging read of the oldest branch, which the held read port
blocked: each waited for the other, for ever, and the host would never have got its `ACK`.

For a host that follows the protocol the window was narrow: the staging read normally happens in the two or more
clocks between an acknowledge and the next request. It stayed open only if a table write occupied exactly those
clocks, which a statistics snapshot or a raw write from the control port can do. So the chip could have hung when
statistics are read while a fast CPU is predicting, which is the advertised use. None of the simulations hit it:
they issue control-port commands between requests, not during them.

Fix (one line in `src/bpx_core.v`): the read port is reserved for a predict only in the clock in which that predict
executes. No reply changes (the regression of section 2 was repeated on the fixed RTL), and the proof now passes.

### A second defect, reachable only by misuse

Strengthening the proofs with the invariant "the resolve-first flag is set only while its request is still
waiting" failed after 12 clocks: a host that changes `OP` from PREDICT+RESOLVE to a special operation while its
request is pending (a malformed request) left that flag set, and the **next**, correct combined request would have
lost its resolve without the error being counted. IR-09 asks that misuse never corrupts state. Fix: the flag is
cleared by every acknowledge. A host that follows the protocol could not reach it.

Both defects sat in the sequencer, where every simulation passed: the first needs a control-port command inside a
two-clock window, the second a host that breaks the protocol in a specific way. That is the argument for the free
inputs of these proofs.

## 5. Coverage (VR-02)

docs/coverage.md defines the bins; docs/coverage_bins.md lists the hits. `g3-perceptron`: 52 of 52 reachable bins
of operation × in-flight depth × misprediction × back-pressure, 125 of 125 other bins, in 1,000,109 requests
**(VERIFIED in that round)**. The mosaic adds bins for every quirk: step 3 other 158/158, step 4 other 178/178 with
12 checker and watchdog bins that cannot occur without a fault; cross 52/52 throughout **(VERIFIED, progress log)**.
Final (step 5, 92 × 100,000, 9,200,750 requests): cross 52/52, other 243/243 reachable (257 defined, 14 cannot occur
in simulation), **295 of 295 = 100 %** **(VERIFIED, progress log, step 5, and docs/coverage_bins.md)**. The 16 excluded cross bins are those that cannot occur for a legal host with a silent
control port; that claim is the formal task `nowait`, not an argument.

## 6. Gate level and CI (VR-04, VR-05)

`g3-perceptron` **(VERIFIED in that round)**: utilisation 81.2 %, routing and Magic DRC 0, LVS 0, antenna 0; setup
+3.87 ns at the slow corner for 30 ns, hold +0.06 ns; the cocotb tests pass on the routed netlist. CI of `793a339`:
gds, precheck, gl_test, viewer, test and docs all success **(VERIFIED by reading the run)**.

Mosaic: every step was built locally and the gate-level test run on its netlist (figures in docs/area.md and
docs/timing.md). Gate level keeps the twin off (FCTL[0] = 0: the PDK's model shows a floating node as X; eight named
`and2_1` mask cells stop it), PULSE = 0 and both rings stopped (a running ring is a zero-delay loop in the PDK's
models); `mosaic_registers`, `gshare_random` and `strap_reset` also run at gate level, the RTL-only tests are
skipped. Step 4 final build: 4 passed + 4 skipped **(VERIFIED, progress log)**. Final build (density 80, 33 ns):
6 passed + 5 skipped (RTL only) **(VERIFIED, progress log, step 5)**.

CI of the mosaic: `2837285` (step 3) and `ca279c9` (step 4): gds, test, docs success; the verified source
`69d0bf4`: test and docs success, gds still running when last read **(VERIFIED, public GitHub API)**. Open (the lead
reads them): the gds result of `69d0bf4` and the CI of the step-6 commit (documents, `explore/` and the review
fix-ups, with a byte-identical synthesised netlist). The probes: `probe-leaky` (`b428b21`) and `probe-ro`
(`67eaabd`): gds, precheck, gl_test, viewer, test and docs success **(VERIFIED: GitHub Actions runs 45862940558 and
45889703170 and their siblings, read through the public API)**.

## 7. Verification of the mosaic, per step

Principle (mosaic.md 6): every quirk is in the golden model; with FCTL = 0 (and CTRL[15] = 0) the chip is the plain
design; the twin never leaks in simulation, so faults are injected by raw writes; every detector is fired at least
once; the checker flags are asserted to stay 0 in the proofs.

### 7.1 Directed and cocotb tests

`test/test.py` runs three ways: latch cells (`Makefile.bpx`), flip-flop model (`BEHAV=yes`) and the repository top
(`make`, as in CI; also `GATES=yes`).

| Test | What it checks | Step |
| --- | --- | --- |
| `random_ops_against_model`, `raw_access_write`, `trace_replay` | the plain chip, as before | all |
| `mosaic_registers` (also gate level) | FCTL, FSTAT, CTEST, DEMOTE, the staging rule | 3, 4 |
| `twin_directed` | codes, abstain, restore-on-use, flags, events, mask with decayed words | 3, 5 |
| `pulse_settings` (latch cells) | W0–W2 pulses of 0.57 × k ns for settings 1–7 (k = 1, 2, 3, 4, 6, 8, 12), the standard half clock on BC, G, W3 and the twin at every setting; tables equal the model | 4 |
| `checkers_watchdog` | each checker and the watchdog set exactly its FSTAT bit and event (forced signals), cleared by CMD bit 2 | 4 |
| `canary_demotion` | canary by CTEST, fourth fault, DEMOTED, suspension by DEMOTE = 0 | 4 |
| `canary_halves` | each canary half alone: one shadow flop forced to the opposite of its flop at an answered predict (CTEST off) gives CANARY and event 4; none forced, nothing | 6 (review) |
| `mask_staging` | a MASK switch with a staged branch in flight: restore-on-use and training write what the model writes (the MASK staging rule, in a plain run) | 6 (review) |
| `gshare_random` | GRAND with the LFSR against the model | 5 |
| `strap_reset` | the strap combination keeps the tables, KEPT, the SPI block after such a reset; since step 6 also a soft reset issued while the strap pins are true clears the tables and leaves KEPT = 0 | 5, 6 |
| `ring_oscillators` | both rings measured from event 8 (125 MHz in the cell model, a model setting) | 5 |

Results: step 2 3/3, step 3 5/5, step 4 8/8, final (step 5) 11/11 in each of the three ways **(VERIFIED, progress
log)**; after the review fix-ups (step 6) 13/13 in each of the three ways, and random 4 × 50,000 plain and concurrent with
0 mismatches **(VERIFIED, mosaic.md 9 step 6)**. Gate level was not rerun locally after step 6 (the two new tests
are RTL only; `strap_reset` gained a step that also runs at gate level, in CI's gl_test).

### 7.2 Random regression with fault injection

`model/randgen.py` draws random FCTL and CTRL[15] in mid-stream, injects 1-, 2- and 3-bit flips into twin words and new
W3 weights, reads and clears FSTAT, probes the staging rule, drives demotion sequences (double errors under SEC-DED
or detect-all, or CTEST, ended by CMD bit 2 or DEMOTE = 0), random PULSE (no effect in simulation), and since step 5
pin resets with and without the strap, MASK switches and decayed twin words. Every reply, DBG one clock after every
ACK, the statistics, the full state (space 12 included) and the background jobs of the concurrent mode are compared.

| Run | Result |
| --- | --- |
| Step 2: 4 × 50,000 plain and concurrent | 0 mismatches; coverage 100 % **(VERIFIED, progress log)** |
| Step 3: 4 × 50,000 and 10 × 100,000, plain and concurrent | 0 mismatches; every reachable CODE × outcome × TWIN_USE combination hit; restore-on-use rewrote 61,333 clean, 102 corrected, 931 erroneous words; each FSTAT flag set 67–118 times **(VERIFIED, progress log)** |
| Step 4: 4 × 50,000 and 10 × 100,000, plain and concurrent | 0 mismatches; CANARY set 75, DEMOTED set 262, predicts while demoted 76,139 **(VERIFIED, progress log)** |
| `RG_PLAIN=1` (FCTL kept 0), 4 × 50,000 plain and concurrent | steps 3 and 4: 0 mismatches **(VERIFIED, progress log)** |
| Final: 92 × 100,000 plain and concurrent (rented machine, 109 s) | plain 9,200,750 requests, concurrent 9,200,759: 0 reply, statistics, state mismatches; 9,631 statistics windows with the pin counts checked; pin totals equal to the model in 92 of 92 concurrent runs; coverage 295/295 **(VERIFIED, progress log, step 5)** |
| Final: `RG_PLAIN=1`, 92 × 100,000 | plain 9,200,796, concurrent 9,200,776 requests: 0 mismatches; 0 FCTL writes and 0 CTRL writes with bit 15 in the stream **(VERIFIED, progress log, step 5)** |
| Final: counter saturation, 8 × 1,000,000 (new) | 8,000,637 requests, 0 mismatches; saturated counts and the overflow flag checked **(VERIFIED, progress log, step 5)** |

### 7.3 Mutation checks

Each mutation was put into the RTL (or, in step 5, the harness) and the regression run (2 × 20,000 random, in step 5
4 × 20,000 plain + 4 × 20,000 concurrent, or the named cocotb test); all were caught **(VERIFIED, progress log, steps
3, 4 and 5)**:

- Step 3: FCTL staging rule removed (caught only after the staging probe was added to the generator); raw instead of
  corrected word restored; wrong correction position; event 10 from the twin side; raw read without the mask; TW_SDC
  without the abstain condition.
- Step 4: canary compare not gated; demotion on the third fault; demotion to gshare; TW_DET not counted; CMD bit 2
  keeps DEMOTED; no suspension by DEMOTE = 0; event 4 from the wrong signal; queue checker firing on a full queue;
  CONF from the undemoted predictor; tap wiring swapped (`pulse_settings`); handshake checker dead
  (`checkers_watchdog`); W3 on the short pulse (`pulse_settings`).
- Step 5 (20 of 20 caught; an unmutated control copy clean): GRAND hold bit inverted; GRAND hold ignoring the bit;
  LFSR wrong tap; LFSR stepping on every write capture; strap without OP, without CSn, without MOSI; mask missing on
  the training write, on the clear value, on the restore value, on the read; `mis_d` counting every resolve;
  `mis_d` counting another prediction; `mis_d` without saturation (caught only by the 8 × 1,000,000 runs);
  harness: bimodal counted from the gshare pin, no repair in its queue copy, DBG taken as always standard, a
  full-queue predict accepted, tournament counted from DBG0 (shown and complete counts).
- Review fix-ups (step 6; each mutant run in a scratch copy, flip-flop model): the CONF canary half blind
  (`can_hit = cap_q & (pred ^ can_s[0])`), which every earlier test missed, fails `canary_halves`; KEPT taken at a
  soft reset (`kept <= strap`) fails `strap_reset`; the MASK staging rule removed, earlier caught only in concurrent
  random runs, fails `mask_staging` in round 12 **(VERIFIED, mosaic.md 9 step 6)**.

### 7.4 Formal

The proofs of section 4 stay green with these assertions added **(VERIFIED, progress log, step 4)**:

| Assertion | Meaning |
| --- | --- |
| `!((p_exec \| do_stage \| drd_exec & d_tab) & wbusy)`, and PRED/CONF hold or refuse while `wbusy` | no flop takes table read data during a table write: the condition behind `src/bpx.sdc` |
| `chk == 0`, FSTAT 5–9 stay 0 | the checkers and the watchdog cannot fire without a physical fault |
| watchdog count ≤ 12 | liveness bound: a request pending without a control-port write is answered by its 13th clock (k-induction, free inputs); a cover reaches 12 (added in step 6: reached in step 22), 13 is excluded by the assertion; the watchdog trips at 64 |
| `clr_tab == !(f_sc && !clrp_pend)` for every clear outside reset, `f_sc == (kept && init_pend)` (step 6) | the tables are cleared by every clear except the first clear after a strapped pin reset (unless CMD bit 1 asked too); `f_sc` is set from the pins at a strapped pin reset and cleared by the first clear. A cover reaches that clear with `clr_tab = 0`. That the contents are kept is checked in simulation (`strap_reset`, the random runs' strap step): the proofs use the flip-flop table model |

In the formal top the ring outputs and the random bit are free inputs (step 5), so the proofs hold whatever they
do. Final run on the committed source (`v_fv.sh`): prove, nowait, cover, bmc and `bmcfull` all pass **(VERIFIED,
progress log, step 5)**. `bmcfull` (the chip's table sizes, 30 clocks from reset) now completes: 547 s on the rented
machine **(VERIFIED by the owners' report)**, where it timed out locally (section 4). After the review fix-ups
(step 6, properties and covers only): `prove` (4 minutes), `cover` (15 of 15 reached) and `nowait` pass locally
**(VERIFIED, mosaic.md 9 step 6)**; `bmc` and `bmcfull` were not rerun (the design under proof is unchanged; the new
assertions are covered by `prove`, whose base case is a bounded check from reset).

### 7.5 Co-simulation

Steps 2 and 3 ran subsets (6 and 3 programs, FCTL never written): 0 model mismatches, statistics equal the model,
mispredictions equal `explore/bpsim` (one-in-flight on all; in-flight default on 4 of 6, the known difference of
the misprediction-alone rule) **(VERIFIED, progress log)**. Final: 71 programs in three modes (428 s on the rented
machine): 421,243,679 requests replayed (140,267,792 / 140,281,743 / 140,694,144), 0 reply mismatches, 0 programs
with statistics differing from the model, 0 protocol errors, 0 late answers; default one-in-flight mispredictions
equal `explore/bpsim` on 69 of 69 programs, in-flight on 45 of 69; bimodal 69, gshare 63, tournament 68 of 69
**(VERIFIED, progress log, step 5)**. Results: speedup 1.1307× (tuning 1.1280, held-out 1.1384) / 1.1345× (two
requests per cycle; 1.1324 / 1.1402) / 1.1249× (safe; 1.1213 / 1.1351); accuracy default 90.93 % (tuning 90.19,
held-out 93.03), bimodal 86.47 %, gshare 87.29 %, tournament 88.92 %, safe default 89.06 % (docs/cosim.md,
cosim_dual.md, cosim_safe.md).

## 8. What cannot be simulated

These are the silicon experiments (docs/experiments.md). In simulation the quirk is either absent or replaced by a
model; with FCTL = 0 none of it is in the path.

| Effect | What simulation does instead | Silicon experiment |
| --- | --- | --- |
| Retention of the floating nodes (W3L) | the cell model holds for ever; faults injected by raw writes of space 12; the PDK's gate model shows X, so gate level runs with the twin off | experiments.md 4, 5 (experiments 2, 3) |
| Short write pulses (W0–W2) | behavioural and formal models: no effect; latch-cell model with 0.57 ns per delay cell checks that every setting still writes | experiments.md 7 (experiment 5) |
| Timing faults and the canary's lead | behavioural: no delay; CTEST fires the canary logically; each half alone by forcing its shadow flop (`canary_halves`). On silicon CTEST exercises the pair, not each half: a blind half shows only as "the canary never fires before the first wrong reply" | experiments.md 8 (experiment 6) |
| Ring oscillators and the oscillator random bit | behavioural and formal: no rings, outputs 0; the latch-cell model gives `xnor2_1` 4 ns (125 MHz, a model setting, not a prediction) | experiments.md 11, 12 (experiments 9, 10) |
| Power-up state of the latches | the strap's logic is simulated; the power-up values are not | experiments.md 13 (experiment 11) |
| Physical upsets of the control logic | checkers fired by forced signals in cocotb | experiments.md 10 (experiment 8) |

## 9. Pre-silicon fault campaign

`docs/fault_campaign.md` (branch `campaign`, merged into `mosaic` with `explore/`): random bit flips in every table at five rates and a decay model of the
leaky twin under every CODE × RESTORE, then eight code masks, in `explore/bpsim` on all 71 programs. The injection
was validated against the golden model: 66 of 66 comparisons equal, and with every event one branch late 58 of 66
differ **(VERIFIED, fault_campaign.md 2)**. Its results are the baseline for the silicon measurements; its mask
comparison chose the switchable mask `8'hF0` (CTRL[15]). Run on the owner's PC: 61 minutes (main campaign) and 51
minutes (masks) of wall time. Not done: an RTL spot check by raw-write injection (fault_campaign.md 6).

## 10. Open items

| Item | Why it is open |
| --- | --- |
| A request withdrawn before its acknowledge is covered by formal only | The golden model has no notion of it; the chip counts it as misuse. |
| microcotb port of the tests, and the host code on real hardware | docs/bringup.md. |

## 11. Commands

```
bash cosim/run.sh ; python3 cosim/report.py                      # 71 programs, one request per cycle -> docs/cosim.md
BPX_FEAT=0 OUT=out/cosim_safe bash cosim/run.sh ; python3 cosim/report.py out/cosim_safe out/cosim/none.tsv   # safe mode -> docs/cosim_safe.md
BPX_DUAL=1 OUT=out/cosim_dual bash cosim/run.sh 16 ; python3 cosim/report.py out/cosim_dual   # -> docs/cosim_dual.md
bash cosim/random.sh 10 100000                                   # random operations with fault injection, coverage
CONC=1 bash cosim/random.sh 10 100000                            # the same with the control port working concurrently
RG_PLAIN=1 bash cosim/random.sh 4 50000                          # FCTL kept at 0: the plain chip
bash formal/run.sh prove ; bash formal/run.sh bmc ; bash formal/run.sh cover ; bash formal/run.sh nowait ; bash formal/run.sh bmcfull
cd test && make -B -f Makefile.bpx [BEHAV=yes] OPS=6000 SEED=11  # cocotb, latch cells or flip-flop model
```
