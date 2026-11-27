# BPX-1 functional coverage (VR-02)

Coverage is collected on the constrained-random runs (`bash cosim/random.sh`): `model/randgen.py` draws the
operations and records, for each request, what the golden model did; `cosim --rand` plays them on the RTL and
records the acknowledge latency; `cosim/coverage.py` joins the two. Because every reply, the statistics and the
complete state of the RTL equal the model in these runs, a bin hit in the model is a bin hit in the RTL. The hit
counts are in `docs/coverage_bins.md` (generated).

## 1. The required cross: operation × in-flight depth × misprediction × back-pressure

| Dimension | Values |
| --- | --- |
| Operation | PREDICT, PREDICT+RESOLVE, RESOLVE, FLUSH, NOP |
| In-flight depth before the request | 0, 1, 2, 3, 4 (4 = queue full, `FULL` high) |
| Misprediction | for operations that resolve: the outcome contradicts the prediction that was returned, yes or no; one bin otherwise (also for a RESOLVE with nothing in flight, which is refused) |
| Back-pressure | the chip made the host wait: `ACK` came later than the 3 clocks of an idle chip, because the training of the previous resolve was still being written, or because the resolve of a combined request had to go first |

That is 68 combinations. 16 of them cannot occur while the control port is silent, and are excluded; each exclusion
is an assertion of the formal task `nowait` (`formal/bpx.sby`), not an argument:

| Excluded bins | Why they cannot occur |
| --- | --- |
| PREDICT at depth 4 with back-pressure (1) | A refused predict is acknowledged in the first clock it is seen. |
| PREDICT+RESOLVE at depth 4 without back-pressure (2) | With the queue full the resolve goes first; the predict and the acknowledge follow at least one clock later. |
| RESOLVE at depth 0 with back-pressure (1) | A refused resolve is acknowledged in the first clock it is seen. |
| RESOLVE at depth 4 with back-pressure (2) | Whenever the queue is full and a new request can arrive, the oldest entry is staged and no write is in progress. |
| FLUSH and NOP at any depth with back-pressure (10) | They do not use the tables and never wait for them. |

A control-port command that executes in the very clock a request is first seen (clear predictors, a raw table
write) can delay that request by a clock. That path is exercised by the formal proofs, which leave the control port
free (`bmc`, `prove`), and by the concurrent runs (`CONC=1 bash cosim/random.sh`, docs/verification.md 2a), which do
not collect these bins.

## 2. Other bins

| Group | Bins |
| --- | --- |
| Acknowledge latency | 3, 4, 5, 6 clocks |
| Host timing | gap before a request 0, 1–2, 3 or more clocks; `ID`/`OP` placed 0, 1, 2 clocks before `REQ` toggles |
| Driving predictor × prediction | 5 drivers × taken / not-taken (static not-taken cannot predict taken: 9 bins) |
| Prediction × confidence | 4 |
| Driving predictor × confidence | 4 predictors × confident or not (`CONF` is the driving predictor's) |
| Features of the default predictor | each of the 8 settings of CTRL[14:12], at a predict and at a training (16 settings of CTRL[15:12] until binarised inference was removed in the mosaic round) |
| Margin of the default predictor at a predict | \|sum\| at most the threshold, or above |
| Errors | resolve with nothing in flight; predict with the queue full |
| Combined request with the queue full | resolve first, mispredicted or not |
| Flush | at depth 0 to 4 |
| Repair | a mispredicted resolve discarding 0, 1, 2, 3 younger predictions |
| Resolve × frozen × mispredicted | 4; resolve under each of the 5 drivers |
| Training | confident hit (weights unchanged), low-margin hit, miss, miss at low margin; a table counting negated or none; a weight already at its maximum, or at its minimum; the threshold moving up, down, or not |
| Operation sequences | every ordered pair of the 5 operations (25) |

Also checked in every run, as comparisons rather than bins: the statistics after snapshot and after
snapshot-and-clear (including saturated counters and the overflow flag in the long stretch without clears), the
complete predictor state through the raw-access port with and without branches in flight, the four prediction pins
at every accepted predict, clear predictors, clear
statistics and soft reset in the middle of the stream.

## 2a. Bins of the mosaic

Defined in `cosim/coverage.py`; the random generator injects the faults (docs/verification.md 7.2).

| Group | Bins | Cannot occur in simulation |
| --- | --- | --- |
| Twin read at a predict | CODE × decode outcome (clean, corrected data bit, corrected check bit, abstain, miscorrected, silent wrong weight, wrong word with right weight) × observe / TWIN_USE × MASK 0 / 1, only the outcomes each code can produce | outcomes a code cannot produce are not defined (no code: no abstain or correction; parity: no correction; SEC-DED and detect-all: no wrong word with the right weight; detect-all: no correction) |
| Fully decayed twin words | nodes all 0 / all 1 × MASK × CODE; MASK switched with and without a clear after it | – |
| Restore-on-use | rewrote a clean, a corrected, an erroneous word | – |
| FSTAT | each flag set and cleared (TW_ERR … DEMOTED) | the four checkers and WDOG: cannot occur without a physical fault (proven); fired by forcing in `checkers_watchdog` |
| Fault events on DBG | events 0–10 seen | 5 and 6 as above; 8: no ring runs in the random runs (measured in `ring_oscillators`) |
| Canary and demotion | canary by CTEST at an executed / refused predict; count steps; DEMOTED by the fourth fault; fourth fault with DEMOTE = 0; fault from TW_DET / canary; suspended, resumed; predicts while demoted (tournament drives, PRED changed, another predictor selected) | – |
| gshare randomness | GRAND off; LFSR / oscillator bit × not at a limit, held, moved | oscillator bit moved: the bit is 0 in simulation |
| Reset strap | strap kept the tables; OP, CSn or MOSI wrong alone, or two or three wrong: cleared; KEPT cleared by a pin reset without the strap, by a soft reset; strap while KEPT | – |

Results **(VERIFIED, progress log, docs/mosaic.md 9)**: step 3, 10 × 100,000: cross 52/52, other 158/158; step 4:
cross 52/52, other 178/178 reachable, 12 checker and watchdog bins cannot occur without a fault. Final regression
(step 5, 92 × 100,000, 9,200,750 requests; docs/coverage_bins.md is its output): cross 52 of 52 reachable (68
defined, 16 cannot occur), other 243 of 243 reachable (257 defined, 14 cannot occur in simulation), **295 of 295 =
100 %** **(VERIFIED, progress log, step 5, and docs/coverage_bins.md)**.

## 3. Not covered by the random runs

| Item | Where it is covered instead |
| --- | --- |
| Sub-clock host timing (edges anywhere inside a clock period) | `test/test.py` (cocotb, RTL and gate level): request edges at random picosecond offsets |
| A request withdrawn before its acknowledge (counted as misuse) | formal: the inputs are free, so `REQ` may toggle at any time |
| A control-port command in the same clock as a request | formal, as above, and the concurrent random runs (verification.md 2a) |
| Raw-access writes to the tables | `test/test.py`, directed test `raw_access_write` (RTL and gate level); the threshold register is also written in the random runs, the bimodal table in the concurrent runs, twin words and W3 by the fault injection |
| Short write pulses, the canary's real lead, retention, the rings | cannot be simulated: the silicon experiments (docs/verification.md 8, docs/experiments.md) |

