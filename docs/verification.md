# BPX-1 verification (DR-02, VR-01 to VR-06)

Evidence labels as in the requirements specification, section 10. Everything marked **(VERIFIED)** was run in the
session that wrote this file, on the RTL of the commit that contains it; the commands are at the end.

## 1. Summary

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
1.1353× / 91.24 % there, 1.1324× / 1.1361× / 91.42 % here **(VERIFIED, both on RTL)**.

## 2. The golden model and what "bit-exact" covers (VR-01)

`model/bpx1.py` is a transaction-level model: one call per request, no notion of clocks. The RTL has to give the
same `PRED` and `CONF` for every request whatever the timing, because both follow the same order rules
(header of `src/bpx_core.v`). Three independent harnesses compare them:

| Harness | Stimulus | Compared | Volume |
| --- | --- | --- | --- |
| `cosim/run.sh` (Verilator, pins of `bpx_chip`) | the U1 CPU contract on each benchmark program: up to 4 branches in flight, wrong-path predicts, mispredictions, FLUSH after register-indirect jumps; and each program again one branch at a time | every reply; the five statistics counters at the end | 71 programs: both adapter modes, and the safe mode of the default predictor |
| `cosim/random.sh` (Verilator, pins) | `model/randgen.py`: random operations including misuse (resolve with nothing in flight, predict when full), random gaps (0 to 9 clocks) and set-up (0 to 2 clocks), changes of the driving predictor and freeze, clear predictors, clear statistics, soft reset | every reply; the four prediction pins at every accepted predict; statistics after 996 snapshots (with and without clear, including saturated counters and the overflow flag); **every state bit** through the raw-access port 1,540 times, with and without branches in flight; the 16 feature settings of the default predictor and raw writes of its threshold, changed in mid-stream | 10 seeds × 100,000 |
| `test/test.py` (cocotb + Icarus; CI runs it on RTL and on the gate-level netlist) | random operations with request edges at random picosecond offsets inside the clock period; a trace replay; a directed test of raw-access writes (whole entries, single fields, neighbours, the small registers, CTRL after reset, and that a written counter is the one predicted from) | every reply, the four prediction pins, statistics, every state bit | 3,000 + 3,000 per run (700 at gate level) |

The Verilator harnesses use the flip-flop model of the tables (`BPX_BEHAV`); the cocotb tests run both that model
and the real latch cells with their write pulses, and the gate-level test runs the placed-and-routed netlist.

Limits of this evidence: the model and the RTL were written by the same author from the same specification, so a
misreading of the specification would be in both. The cross-check against the separately written simulator
(`explore/bpsim`, cosim.md 6): with one branch in flight the default predictor's misprediction counts are equal
on all 69 programs, and its mean accuracy in the CPU runs is 91.42 % in both; the references agree on 63 to 69 of
69 programs in the CPU runs **(VERIFIED)**. (The perceptron has no pseudo-random element, unlike the tagged-table
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
run reports all four predictors' mispredictions (the chip counts them side by side). Results: `docs/cosim.md`
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

docs/coverage.md defines the bins; docs/coverage_bins.md lists the hits. Result: 52 of 52 reachable bins of
operation × in-flight depth × misprediction × back-pressure, 125 of 125 other bins, in 1,000,109 requests
**(VERIFIED)**. The 16 excluded cross bins are those that cannot occur for a legal host with a silent control port;
that claim is the formal task `nowait`, not an argument.

## 6. Gate level and CI (VR-04, VR-05)

Local sign-off of this RTL (docs/area.md, docs/timing.md) **(VERIFIED)**: utilisation 81.2 %, routing and
Magic DRC 0, LVS 0, antenna 0; setup slack +3.87 ns at the slow corner for the 30 ns clock, hold
+0.06 ns; the three cocotb tests pass on the placed-and-routed netlist; 2,259 maximum-slew warnings at
the slow corner on the tables' select nets (warnings of the library's characterisation range, not timing
violations; the paths through them are inside the reported slack).

CI: the tagged-table design's last commit (`dffc289`, tag `g3`) was green on gds, precheck, gl_test, viewer, test
and docs **(VERIFIED by reading the runs of its RTL commits)**. The perceptron design, commit `793a339` (this RTL):
gds, precheck, gl_test, viewer, test and docs all success **(VERIFIED by reading the run)**. The utilisation figure
of the CI build was not read (it is on the run's summary page).

## 7. Open items

| Item | Why it is open |
| --- | --- |
| A request withdrawn before its acknowledge is covered by formal only | The golden model has no notion of it; the chip counts it as misuse. |
| microcotb port of the tests, and the host code on real hardware | docs/bringup.md. |

## 8. Commands

```
bash cosim/run.sh ; python3 cosim/report.py                      # 71 programs, one request per cycle -> docs/cosim.md
BPX_FEAT=0 OUT=out/cosim_safe bash cosim/run.sh ; python3 cosim/report.py out/cosim_safe out/cosim/none.tsv   # safe mode -> docs/cosim_safe.md
BPX_DUAL=1 OUT=out/cosim_dual bash cosim/run.sh 16 ; python3 cosim/report.py out/cosim_dual   # -> docs/cosim_dual.md
bash cosim/random.sh 10 100000                                   # random operations and coverage
CONC=1 bash cosim/random.sh 10 100000                            # the same with the control port working concurrently
bash formal/run.sh prove ; bash formal/run.sh bmc ; bash formal/run.sh cover ; bash formal/run.sh nowait ; bash formal/run.sh bmcfull
cd test && make -B -f Makefile.bpx [BEHAV=yes] OPS=6000 SEED=11  # cocotb, latch cells or flip-flop model
```
