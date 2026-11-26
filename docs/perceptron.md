# BPX-1: the perceptron default predictor (owner requirement of 26 Nov 2026)

The owners require the default predictor to be from the perceptron family, with the tagged-table design kept as
the fallback (tag `g3-tage`). This document is the exploration and the decisions of that round. Numbers marked
*simulator* are from `explore/bpsim` on all benchmark programs (69 in the aggregates, 51 tuning / 18 held-out, as in
docs/exploration.md); the numbers measured on the RTL are in docs/cosim.md and docs/verification.md and are the
ones to quote. Evidence labels as in the requirements specification.

## 1. What was chosen

A **hashed perceptron** (the GEHL family: Seznec's O-GEHL without its dynamic history lengths), 640 bits of its own:

| Part | Value |
| --- | --- |
| Weight tables | 4 tables × 32 entries × 5-bit signed weights |
| History per table | 0, 8, 9 and 20 outcomes (table 0 is the bias: identifier only) |
| Index | identifier bits ⊕ folded outcome history ⊕ folded **path history** (one identifier bit of each of the last 16 branches) |
| Sum | Σ ±(2w+1) over the four tables, plus the reference predictors as inputs: bimodal counter × 2, gshare counter × 4 (each as 2c−3), tournament prediction ± 4 |
| Prediction, confidence | sign of the sum; confident if \|sum\| > θ |
| Training | on a misprediction or when \|sum\| ≤ θ, every selected weight moves one step (saturating) |
| Threshold θ | adaptive: a 4-bit counter of mispredictions against low-margin hits moves θ (starts at 12, range 1 to 63) |
| **Negation in memory** | the outcome just before a table's history segment negates that table's weight: the stored bits are read **complemented** (−(2w+1) = 2(~w)+1), so one stored weight serves two opposite history patterns at the cost of five XOR gates per table and no adder |

The three reference predictors are unchanged and train by their own rules.

Measured on the RTL **(VERIFIED, docs/cosim.md, cosim_dual.md, cosim_safe.md)**: 1.1324× over static not-taken
with one request per CPU cycle and 1.1361× with two, at 91.42 % accuracy (tagged-table design: 1.1317× / 1.1353× /
91.24 %); safe mode 1.1277× / 89.97 %; tournament 1.1239× / 88.92 %.

## 2. How it was found

All sweeps ran on a rented 128-thread machine (Vast, `bpx1-percep`), every candidate on all 71 programs.

| Sweep | Candidates | Purpose |
| --- | --- | --- |
| 1 | 4,000 random, 256 to 1,024 own bits, at 10-bit and at 8-bit identifiers | structure: tables, entries, weight bits, histories, reference inputs, threshold, negation, hash |
| 2 | 4,000 random | the exotic options as factors: binary weights, binarised inference, probabilistic updates, path history |
| 3 | 3,000 random around the best 4- and 5-table structures | the same options near the optimum |
| 4 | 6,912: the full grid of histories, input weights, threshold, path length for the chosen structure, 8-bit identifiers | the final parameters |

Selection was on the tuning programs' speedup; the held-out programs confirm it (below). A defect of my own
in sweep 2 and 3 (a shadowed variable made every candidate use probabilistic updates on taken predictions) was found
before any result was used; both sweeps were rerun **(VERIFIED: the rerun binary is the one in the repository)**.

Main effects in sweep 1, mean over the better half of the candidates **(VERIFIED, simulator)**: weight bits
5 > 4 > 3 > 2 (1.1299 / 1.1295 / 1.1287 / 1.1278); 32 entries per table best; a bias table helps; adaptive θ helps
(1.1295 against 1.1290); negation helps (in 110 of the best 200 with half the sample having it); the table-specific
identifier hash makes no difference.

Result of the final grid at 8-bit identifiers **(VERIFIED, simulator)**:

| Predictor | Own bits | Speedup, all | Accuracy, all | Tuning | Held-out |
| --- | --- | --- | --- | --- | --- |
| **Chosen perceptron** | 640 | 1.1360 | 91.42 % | 1.1338 / 90.66 % | 1.1420 / 93.60 % |
| Tagged tables (the fallback `g3-tage`) | 576 | 1.1352 | 91.29 % | 1.1330 / 90.50 % | 1.1414 / 93.53 % |
| Tournament | – | 1.1277 | 88.92 % | 1.1259 / 88.23 % | 1.1326 / 90.89 % |
| gshare | – | 1.1224 | 87.29 % | | |
| Bimodal | – | 1.1203 | 86.47 % | | |

The perceptron is level with the tagged tables on the aggregates and very different per program: +20.9 points on
`embench.nsichneu` (a large state machine), −3.0 on `embench.minver`, −2.1 on `interp.expr`. It is below the best
reference predictor on 2 of 69 programs (`sys.json` −0.68 points, `interp.expr` −0.27).

## 3. What each part is worth (ablation of the chosen design)

**(VERIFIED, simulator, 8-bit identifiers, all programs.)** Each row changes one thing.

| Variant | Speedup | Accuracy | Change |
| --- | --- | --- | --- |
| Chosen | 1.1360 | 91.42 % | |
| Without the negation in memory | 1.1345 | 91.06 % | −0.0015 / −0.36 |
| Without the adaptive threshold | 1.1353 | 91.20 % | −0.0007 / −0.22 |
| Without the reference inputs | 1.1327 | 90.30 % | −0.0033 / −1.12 |
| **All three off (the safe mode)** | 1.1314 | 89.97 % | −0.0046 / −1.45; still ahead of every reference |
| Without path history | 1.1353 | 91.23 % | −0.0007 / −0.19 |
| 4-bit weights (512 bits) | 1.1347 | 91.04 % | −0.0013 / −0.38 |
| 3-bit weights (384 bits) | 1.1316 | 90.19 % | −0.0044 / −1.23 |
| A fifth table (800 bits) | 1.1361 | 91.55 % | +0.0001 / +0.13: not worth 3,400 µm² |
| Probabilistic weight updates | 1.1357 | 91.33 % | −0.0003 / −0.09: rejected |
| Binarised inference (only the signs vote) | 1.1272 | 88.75 % | −0.0088 / −2.67 |
| Binary weights, 8 tables × 64 × 1 bit (512 bits) | 1.1272 | 88.77 % | −0.0088 / −2.65 |

## 4. Computing in or next to the memory: what was tried

The owners asked for in-memory and near-memory computation to be part of the main exploration. The tables are
arrays of standard-cell latches read through an AND-OR tree (docs/SPEC.md 6.2); only digital cells are available in
this flow (analog cells would need an analog Tiny Tapeout tile and a different flow: not this project). Within that:

| Idea | What it is here | Result | Decision |
| --- | --- | --- | --- |
| The history bit negates the stored weight | The weight is read complemented when the history bit is 1: the multiplication of the classic perceptron done by five XOR gates at the table output, no multiplier and no extra adder input. Equivalent to the XNOR of a binarised network, on multi-bit weights. | +0.0015× / +0.36 points for 15 XOR gates per lookup | **In the default** (CTRL bit 13 switches it off) |
| The history bits select the weight | Short histories go straight to the row select of the table (no hash): the selection is the memory's own decode. | built into the index hash of every candidate with a history of at most 5 bits | Available in the RTL; the chosen histories (8, 9, 20) are longer than the 5 index bits and are folded |
| Majority of stored bits (binary weights, XNOR and population count) | Each table stores one bit per entry and votes ±1. | 1.1272× / 88.77 % at the same storage; best of the sample 1.1296× / 89.59 % | Rejected as the default: no hysteresis in a 1-bit weight. Per bit a 1-bit entry also costs more area (27 µm² against 22 µm² per bit of a 5-bit entry: the per-entry overhead does not shrink). |
| Binarised inference with full-precision training | The 5-bit weights train as usual; only their signs vote (a population count instead of the adder). The latent-weight scheme of binarised neural networks. | 1.1272× / 88.75 % | **In the chip as a switchable experiment** (CTRL bit 15, off after reset): the same counters measure both modes on silicon. Costs 4 XOR and a 4-input count. |
| Sum precomputed when the tables are written | Would need the sum stored per combination of the four table indices; the indices are hashed from different histories, so no entry "owns" a sum. Precomputing the bias plus the bimodal vote per identifier is possible, but they are two small numbers: storing their sum costs more bits than adding them. | analysed | Rejected |
| Unary (thermometer) weights summed by a population count | A 5-bit weight would take 31 latches; the population count of 4 × 31 bits is larger than the adder of four 6-bit numbers. In-place increment by a shift needs master-slave storage (transparent latches race through). | area estimate: 6× the storage | Rejected |
| Bit-serial accumulation over several clocks | One small adder, one table per clock. | +3 clocks of latency per prediction | Rejected: latency (objective O3) is worth more than the 1,000 µm² of adder |
| The sum in the read path | The adder tree sits directly on the table outputs in the predict cycle, no register between memory and sum; the same module recomputes the sum from the staging register for training. | this is the design | In. It is what sets the clock period (section 6). |
| Summation in the time domain (two delay lines racing, an arbiter as the sign) | Possible with standard cells, but static timing cannot sign it off, gate-level simulation cannot check it, and place-and-route would rebalance the lines. | not built | Proposed for a later chip with a custom-placed macro (section 8) |

Other proposals tested in the same sweeps: path history (**in**, +0.0007×), probabilistic updates (out),
table-specific identifier hashing (no effect, out), a fifth table (out).

## 5. Switches and the safe mode

Every part beyond the plain hashed perceptron can be switched off over the control port (CTRL bits 15:12), and each
combination is in the golden model, in the random regression (all 16 combinations, changed while requests run)
and in the functional coverage:

| CTRL bit | Feature | After reset |
| --- | --- | --- |
| 12 | Reference predictors as inputs of the sum | on |
| 13 | Negation in memory | on |
| 14 | Adaptive threshold (off: θ stays at the value in raw space 7, word 4, which is writable) | on |
| 15 | Binarised inference (experiment) | off |

**Safe mode:** bits 12 to 15 = 0 gives the plain hashed perceptron (1.1314× / 89.97 % in the simulator, still
ahead of every reference). Independently of all of this, CTRL bits 2:0 can make any reference predictor (or static
not-taken) the driving one: they share no logic with the perceptron's sum, so a defect in the perceptron cannot
stop the chip serving predictions. Changing a switch needs no reset; the weights keep learning under the new rule
(clear the predictors, CMD bit 1, to compare modes from equal starting conditions).

## 6. Cost: area, clock, latency

| | Tagged tables (`g3-tage`) | Perceptron |
| --- | --- | --- |
| Predictor-specific storage | 576 bits | 640 bits + 16-bit path history (two copies) + 10 bits of threshold |
| Acknowledge latency | 3 clocks | **3 clocks (unchanged)** |
| Clock at the slow corner | 25 ns (40 MHz), +4.83 ns | **30 ns (33 MHz), +3.87 ns**; the slowest path is 26.1 ns |
| Utilisation | 75.6 % | 81.2 % |
| Time to answer at the pins | 75 ns | **90 ns** |

The sum of four weights and three reference inputs after a table read does not fit 25 ns at the slow corner: a trial
build at 25 ns ended with −2.58 ns on `CONF` and −1.92 ns on `PRED`, every failing path coming from the gshare
table (128 entries, the slowest read) through the adder **(VERIFIED: trial build of 26 Nov 2026)**. Two ways out:

| Option | Latency | Risk |
| --- | --- | --- |
| **Keep 3 clocks, sign off at 30 ns (chosen)** | 3 × 30 = 90 ns | none: the sequencer and its proofs are untouched |
| Add a pipeline clock for the sum, keep 25 ns (the owners allowed this) | 4 × 25 = 100 ns | a changed sequencer, new proofs |

The chosen option answers sooner in nanoseconds, which is what objective O3 (CPU clock served) measures, at the
price of objective O5 (the chip's clock figure), the lowest of the five.

**Cost in O1:** none for a CPU of up to about 8 MHz with one request per cycle (the answer still arrives inside the
cycle: 90 ns plus about 30 ns of pads and synchroniser); the two-requests-per-cycle adapter (SPEC 9, rule 7) needs
the CPU clock at or below one eighth of the chip clock: 4.1 MHz instead of 5 MHz. The 1 MHz CPU of IR-07 is far
inside both.

## 7. The owners' other points

| Point | Finding | Decision |
| --- | --- | --- |
| **ID width, 8 or 10 bits** | Sweep 1 at both widths, 4,000 candidates on all programs: over the best 200 the change from 10 to 8 bits is −0.00001× and −0.003 points on average; single programs move by up to ±1 point in both directions; the references: bimodal −0.07 points, gshare −0.11, tournament +0.02 **(VERIFIED, simulator)**. The owners' reasoning holds: no table has more than 128 entries and none is tagged, so identifier bits beyond the index only reshuffle which branches collide. Wider identifiers would pay with tagged or much larger tables. | **8 bits.** `ID = ` XOR of the 8-bit slices of `PC[31:2]`. |
| The two freed pins | `uio[0]` and `uio[1]` become outputs: the last predictions of the bimodal and the gshare predictor. With `DBG0` and `DBG1`, which after reset show those of the default predictor and the tournament, **all four predictions are on pins with every `ACK`**. | In; checked on every request of the random regression and the cocotb tests. |
| **Debug treats the four predictors alike** | Statistics: one misprediction counter each. Raw access: every table and register of each. Prediction pins: one each (above). Driving predictor: any of the four. One difference existed: `CONF` was always the default predictor's confidence. | **Fixed:** `CONF` is now the confidence of the driving predictor (strong counter for bimodal and gshare, the chosen counter for the tournament, \|sum\| > θ for the perceptron). |
| **Clock, reset and project selection from the FPGA** | docs/bringup.md 6: the FPGA can drive `clk` and `rst_n` once the demo board's firmware has released them; the selection must stay with the board's microcontroller. | Recorded there and in SPEC 9. |
| **Area** | docs/area.md has the table by block. The perceptron design is at 81.2 % utilisation against 75.6 % for the tagged tables; it routes clean and nothing had to be optimised. The weight tables are 29 % of the synthesised area, the two sums 6 %. | No change of design path. |

## 8. Paths for further experiments (not in this chip)

1. A fifth table and 6-bit weights when the area allows (800 bits: +0.13 points; the tile is at 81 %).
2. Registering the table outputs and summing in a fourth clock: back to 25 ns and room for more inputs.
3. A late-select sum: the four possible gshare counter values are compared in parallel and the slow gshare read
   only selects the result; estimated to recover about 2 ns for about 2,000 µm².
4. A hand-placed weight array with its adder as one hard macro (near-memory summation proper, or the time-domain
   race of section 4): needs a custom layout flow, and the project's decision D4 (no hard macro) would have to change.
5. Local history as a further input (a per-branch history table): not in the sweeps.
