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
free (`bmc`, `prove`), not by the random runs, where control-port accesses and requests alternate.

## 2. Other bins

| Group | Bins |
| --- | --- |
| Acknowledge latency | 3, 4, 5, 6 clocks |
| Host timing | gap before a request 0, 1–2, 3 or more clocks; `ID`/`OP` placed 0, 1, 2 clocks before `REQ` toggles |
| Driving predictor × prediction | 5 drivers × taken / not-taken (static not-taken cannot predict taken: 9 bins) |
| Prediction × confidence | 4 |
| Provider of the default prediction | none (base), tagged table 0, 1, 2; alternate used or not |
| Errors | resolve with nothing in flight; predict with the queue full |
| Combined request with the queue full | resolve first, mispredicted or not |
| Flush | at depth 0 to 4 |
| Repair | a mispredicted resolve discarding 0, 1, 2, 3 younger predictions |
| Resolve × frozen × mispredicted | 4; resolve under each of the 5 drivers |
| Training | provider none / table 0 / 1 / 2; provider weak or not; alternate present or not; allocation in table 0 / 1 / 2 or none; useful bits decayed or not |
| Operation sequences | every ordered pair of the 5 operations (25) |

Also checked in every run, as comparisons rather than bins: the statistics after snapshot and after
snapshot-and-clear (including saturated counters and the overflow flag in the long stretch without clears), the
complete predictor state through the raw-access port with and without branches in flight, clear predictors, clear
statistics and soft reset in the middle of the stream.

## 3. Not covered by the random runs

| Item | Where it is covered instead |
| --- | --- |
| Sub-clock host timing (edges anywhere inside a clock period) | `test/test.py` (cocotb, RTL and gate level): request edges at random picosecond offsets |
| A request withdrawn before its acknowledge (counted as misuse) | formal: the inputs are free, so `REQ` may toggle at any time |
| A control-port command in the same clock as a request | formal, as above |
| Raw-access writes to the tables | `test/test.py`, directed test `raw_access_write` (RTL and gate level) |
