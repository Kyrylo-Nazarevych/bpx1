# BPX-1 verification (DR-02, VR-01 to VR-06)

Evidence labels as in the requirements specification, section 10. Everything marked **(VERIFIED)** was run in the
session that wrote this file, on the RTL of the commit that contains it; the commands are at the end.

## 1. Summary

| Requirement | Evidence | Result |
| --- | --- | --- |
| VR-01 golden model, bit-exact | 71 programs × (CPU in-flight run + one-in-flight replay), every request replayed through `model/bpx1.py`; 10 × 100,000 constrained-random operations with statistics and full-state comparison | **0 mismatches** in 280,336,456 program requests and 1,000,134 random requests **(VERIFIED)** |
| VR-02 functional coverage | `docs/coverage.md`, `docs/coverage_bins.md` | 52 of 52 reachable bins of the required cross, 90 of 90 other bins **(VERIFIED)**; 16 cross bins excluded, each by a formal assertion |
| VR-03 formal | section 4 | all assertions proven by k-induction (unbounded) and checked by BMC to 60 clocks; 12 cover traces **(VERIFIED)**; found two defects in the RTL, both fixed |
| VR-04 gate level | CI job `gl_test` on the built netlist; `tools/harden.sh` locally | passes **(VERIFIED by CI)** |
| VR-05 CI reports reviewed | section 6 | gds, precheck, gl_test, viewer, test, docs green |
| VR-06 system co-simulation | `docs/cosim.md`, `docs/cosim_dual.md` | default predictor 1.1317× over static not-taken (1.1353× with two requests per cycle), 91.24 % accurate **(VERIFIED)** |
| PF-01 | cosim.md 2 | default 1.1317 > tournament 1.1239 > gshare 1.1183 > bimodal 1.1161 > 1 **(VERIFIED)** |
| PF-02 | cosim.md 2, 3 | default 91.24 % > tournament 88.90 % > gshare 87.40 % > bimodal 86.54 %; worse than the best reference on no program **(VERIFIED)** |
| PF-03 | cosim.md 4 | in-flight 91.24 % = one-in-flight 91.24 %; largest loss on a program below 0.01 points **(VERIFIED)** |

## 2. The golden model and what "bit-exact" covers (VR-01)

`model/bpx1.py` is a transaction-level model: one call per request, no notion of clocks. The RTL has to give the
same `PRED` and `CONF` for every request whatever the timing, because both follow the same order rules
(header of `src/bpx_core.v`). Three independent harnesses compare them:

| Harness | Stimulus | Compared | Volume |
| --- | --- | --- | --- |
| `cosim/run.sh` (Verilator, pins of `bpx_chip`) | the U1 CPU contract on each benchmark program: up to 4 branches in flight, wrong-path predicts, mispredictions, FLUSH after register-indirect jumps; and each program again one branch at a time | every reply; the five statistics counters at the end | 71 programs, both adapter modes |
| `cosim/random.sh` (Verilator, pins) | `model/randgen.py`: random operations including misuse (resolve with nothing in flight, predict when full), random gaps (0 to 9 clocks) and set-up (0 to 2 clocks), changes of the driving predictor and freeze, clear predictors, clear statistics, soft reset | every reply; statistics after about 1,000 snapshots (with and without clear, including saturated counters and the overflow flag); **every state bit** through the raw-access port about 1,300 times, with and without branches in flight | 10 seeds × 100,000 |
| `test/test.py` (cocotb + Icarus; CI runs it on RTL and on the gate-level netlist) | random operations with request edges at random picosecond offsets inside the clock period; a trace replay; a directed test of raw-access writes (whole entries, single fields, neighbours, the small registers, and that a written counter is the one predicted from) | every reply, statistics, every state bit | 3,000 + 3,000 per run (700 at gate level); 47,500 in earlier runs |

The Verilator harnesses use the flip-flop model of the tables (`BPX_BEHAV`); the cocotb tests run both that model
and the real latch cells with their write pulses, and the gate-level test runs the placed-and-routed netlist.

Limits of this evidence: the model and the RTL were written by the same author from the same specification, so a
misreading of the specification would be in both. The cross-check against an independently written simulator
(`explore/bpsim`, cosim.md 6) agrees exactly for the three reference predictors' in-flight runs on 66 to 69 of 69
programs, and differs slightly for the default predictor (mean accuracy 91.27 % against 91.24 %). The cause is
the allocation randomiser: with it switched off in both, the simulator and the golden model give identical
misprediction counts (82 and 82 on `team.interpreter`, 70,648 and 70,648 on `sort.quick`); with it on, the two step
their pseudo-random sequence at different moments (67 against 65, 70,499 against 70,529) **(VERIFIED)**. So the
difference is noise between two equally valid random sequences, not a loss in the RTL. The exploration numbers of
docs/exploration.md differ from the RTL numbers by that much; the RTL numbers are the ones to quote.

## 3. System co-simulation (VR-06)

`cosim/cosim.cpp` is the CPU of docs/SPEC.md section 9 reduced to what determines cycle counts: a five-stage
in-order RV32IM pipeline with a 256-entry branch-target buffer. It replays the instruction stream of each program
(recorded with Spike) and drives the chip's pins: predict in IF on a BTB hit, in ID on a miss; follow the
prediction; fetch and predict down the wrong path until the branch resolves in EX; report the outcome in the next
cycle; FLUSH after a register-indirect jump. Penalties: misprediction 2 cycles, taken branch found in ID 1,
JAL 0 or 1, JALR 2, load-use 1, divide 32. Static not-taken on the same CPU without a chip is the baseline.

Each program runs with each of the four predictors driving and with the chip forced to static not-taken, and every
run reports all four predictors' mispredictions (the chip counts them side by side). Results: `docs/cosim.md`
(one request per CPU cycle) and `docs/cosim_dual.md` (two requests in a cycle where needed, SPEC 9 rule 7).

What the co-simulation found that the exploration model had not modelled: a resolve that reports a misprediction
cannot be combined with the predict of the same cycle (the chip would discard that predict with the wrong path).
Sent alone, it costs the CPU one bubble on about half of the mispredictions: 0.0036 (1.1317× against 1.1353×) of speedup. Rule 7 removes
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
| Predict changes no trained state (FR-01) | A predict without a resolve changes neither the committed history, nor the use-alternate counter, nor the allocation register, nor the statistics, and starts no table write; those registers change only through a resolve, a clear or a raw write; a table write starts only for a resolve (and only when not frozen), a clear, a raw write or a snapshot. |
| One acknowledge per request (IR-01) | `ACK` changes only when a request is pending, and then to the request level. |
| Stable replies (RR-04) | `PRED` and `CONF` change only in the clock in which `ACK` changes. |
| Misuse (IR-09) | A resolve with nothing in flight is acknowledged, counted and flagged, and changes neither the queue nor the histories. Everything above holds under free inputs. |
| Helper invariants | A deferred resolve or a staged entry implies a non-empty queue; the resolve-first flag is set only while its request is still waiting; at most one kind of operation executes per clock. |
| Liveness (RR-03) | Whenever the inputs have been quiet for 20 clocks, the chip is idle and ready: no request pending, no deferred resolve, no write in progress, no command pending, the oldest in-flight branch staged. Since a soft reset is one of the free inputs, recovery after it is included. |

| Task | What | Table sizes | Result |
| --- | --- | --- | --- |
| `prove` | k-induction, k = 24: every assertion from **any** state that satisfies the assertions, so no depth limit | 8 / 16 / 8 entries | **proven** (3.4 minutes) **(VERIFIED)** |
| `bmc` | bounded check from reset, 60 clocks | 8 / 16 / 8 | pass **(VERIFIED)** |
| `bmcfull` | bounded check from reset, 30 clocks | the chip's 64 / 128 / 16 | no violation in clocks 0 to 25; stopped by the 55-minute limit while checking clock 26 **(VERIFIED)** |
| `cover` | 12 traces: each operation completing; the queue filling; a refused predict and a refused resolve; resolve-first with a full queue; a mispredicted resolve with a full queue, the predict after it, and the return to idle with three branches resolved | 8 / 16 / 8 | all reached within 15 clocks **(VERIFIED)** |
| `nowait` | legal host and silent control port: the requests that can never be made to wait (docs/coverage.md) | 8 / 16 / 8 | **proven** by k-induction (1.2 minutes) **(VERIFIED)** |

**Depth and why it suffices.** The induction proof has no depth: it shows that no state satisfying the assertions
can step to a state violating one, and the bounded check shows that reset leads into that set. The bounded depth of
60 clocks is redundant with it; from reset it reaches READY (5 clocks), a full queue (4 predicts), a misprediction
with a full queue and its repair (cover trace: 11 clocks) several times over. The liveness bound of 20 clocks is the
longest chain of pending work: a clear, a raw write and its read-back, a snapshot, a deferred resolve with its
staging read and its 3-clock table write.

**Small tables.** The proofs run with tables of 8, 16 and 8 entries instead of 64, 128 and 16. The properties are
about the sequencer, the queue, the histories and the handshake; the table size enters only through the width of
the index, which no property mentions. The full-size design is checked by `bmcfull` to the depth it reaches in
reasonable time, and by simulation.

**What the proofs do not cover.** The values that training writes into the tables (the predictor algorithm): these
are checked against the golden model by simulation only. The latch tables themselves and their write pulse: the
proofs use the flip-flop model; the latch construction is argued in docs/SPEC.md 6.2 and exercised at cell level
and gate level. The synchronisers and the SPI shifter in `bpx_chip`/`bpx_spi`: simulation only.

### The deadlock the liveness proof found

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
operation × in-flight depth × misprediction × back-pressure, 90 of 90 other bins, in 1,000,134 requests
**(VERIFIED)**. The 16 excluded cross bins are those that cannot occur for a legal host with a silent control port;
that claim is the formal task `nowait`, not an argument.

## 6. Gate level and CI (VR-04, VR-05)

CI run of commit `ee6574a` (the first with the real chip as top) **(VERIFIED by reading the run)**: `gds`
success (33 minutes), `precheck` success, `gl_test` success (the cocotb tests on the placed-and-routed netlist),
`viewer`, `test`, `docs` success. Local sign-off of the final RTL (docs/area.md, docs/timing.md) **(VERIFIED)**: utilisation
75.6 %, routing and Magic DRC 0, LVS 0, antenna 0; setup slack +4.83 ns at the slow corner for the 25 ns
clock, hold +0.06 ns; the three cocotb tests pass on its netlist; 2,450 maximum-slew warnings at the slow corner on the tables' select nets (warnings of the
library's characterisation range, not timing violations; the paths through them are inside the reported slack).
The utilisation figure of the CI run was not read **(DON'T KNOW: it is on the run's summary page, which the command
line does not return)**.

CI run of commit `dacaf07` (with the deadlock fix) **(VERIFIED by reading the run)**: gds, precheck, gl_test,
viewer, test, docs all success. The second fix and the raw-access write test are in the commit that contains this
file; its CI run is the sign-off for them (docs/HANDOVER.md, next steps).

## 7. Open items

| Item | Why it is open |
| --- | --- |
| A request withdrawn before its acknowledge is covered by formal only | The golden model has no notion of it; the chip counts it as misuse. |
| Control-port commands in the same clock as a request are covered by formal only | Random simulation alternates them. This is the gap the deadlock went through; a concurrent-SPI mode of `cosim --rand` would close it in simulation too. |
| microcotb port of the tests, and the host code on real hardware | docs/bringup.md. |

## 8. Commands

```
bash cosim/run.sh ; python3 cosim/report.py                      # 71 programs, one request per cycle -> docs/cosim.md
BPX_DUAL=1 OUT=out/cosim_dual bash cosim/run.sh 16 ; python3 cosim/report.py out/cosim_dual   # -> docs/cosim_dual.md
bash cosim/random.sh 10 100000                                   # random operations and coverage
bash formal/run.sh prove ; bash formal/run.sh bmc ; bash formal/run.sh cover ; bash formal/run.sh nowait ; bash formal/run.sh bmcfull
cd test && make -B -f Makefile.bpx [BEHAV=yes] OPS=6000 SEED=11  # cocotb, latch cells or flip-flop model
```
