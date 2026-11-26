# BPX-1 mosaic: dependability experiments (plan, design and handoff)

Status: **in progress** (26 Nov 2026). The verified perceptron design is tag `g3-perceptron` on `main`; the
tagged-table fallback is `g3-tage`. The mosaic is built on branch **`mosaic`** and merged to `main` only when it is
verified to the same level and CI is green, so `main` stays submittable. This file is the plan, the detailed design
and the handoff: read it after docs/HANDOVER.md. Areas are **(ESTIMATED)** from cell counts unless marked.

## 1. Owners' brief for this round (26 Nov 2026, condensed)

- Research value first; the owners work in dependable computing. Make the chip a **mosaic of techniques**: quirks
  spread over the predictors, each switchable; with everything off every predictor is its verified plain version.
- Go with the full package: fault sources, detectors, protection codes, FPGA-side injection, counting and a golden
  twin. Physically risky storage on one part only. **The reference predictors' tables always get the standard write
  pulse.** Run a small leaky-cell probe through CI precheck before building on it.
- Accuracy and clock may be sacrificed; the chip must still clearly beat static not-taken.
- Optional, if smart and they fit: power-up latch read, replica oscillator, mod-3 residue on the sum, ageing monitor,
  SEC-DED instead of SEC, on-chip bit-flipper if SPI injection is too slow. Say what was left out and why.

## 2. The mosaic

| Part | Quirk | Fault class it creates or measures | Switch (off = plain) |
| --- | --- | --- | --- |
| Bimodal + chooser table | none: the untouched control group | – | – |
| gshare table | a counter leaves a saturated state only on a random bit (probabilistic hysteresis); the bit comes from a pseudo-random register or from oscillator jitter | a learner on physical entropy | FCTL[8]; source FCTL[9] |
| Perceptron W0–W2 (static latches) | write-pulse width: standard (half a clock) or 7 short settings from a delay line | write faults | FCTL[7:5] |
| Perceptron W3 (static latches, standard pulse) | golden reference of its leaky twin | – | – |
| **Leaky twin W3L** (32 × 8 floating nodes: 4 data + 4 check bits) | written with W3; every prediction compares the two; the sum can take W3L instead of W3 | retention faults, counted exactly | FCTL[0] observe, FCTL[1] use |
| W3L protection | none / parity → abstain / SEC-DED (8,4) / detect all → abstain; restore-on-use on or off | value of each code on a leaking memory | FCTL[3:2], FCTL[4] |
| Prediction output | canary flop: a second flop sees the prediction through a delay and disagrees when the timing margin is used up | timing margin (the FPGA shortens the clock) | always observing |
| Driving predictor | automatic demotion to the tournament after 4 detected faults | graceful degradation | FCTL[13] |
| Protected core (sequencer, queue, handshake) | proven invariants as hardware checkers; hang watchdog | control upsets (beam, glitch) | always observing |
| Chip | two ring oscillators (one kept running, one resting): speed, ageing, entropy; a strap at reset keeps the tables: power-up state of the latches, warm reset | process, ageing, power-up bias | FCTL[12:10]; strap |

Faults are reported as sticky flags (register FSTAT) and as events on the DBG pins; counting, time stamps, fault
injection (raw writes, clock shortening) and the golden comparison (a twin of the RTL in the FPGA) are on the FPGA.

## 3. Left out, and why

| Item | Why |
| --- | --- |
| Mod-3 residue check on the sum | 400 µm² on the critical path; a wrong sum shows on the PRED pin, where the FPGA twin sees it. |
| Razor-style shadow flop (late clock) | Needs every short path to the flop padded to half a clock; not practical in this flow. The canary flop (early warning, no padding) replaces it. |
| Recomputation check at resolve | The staged recomputation differs legitimately when tables were trained in between, so it is not a detector by itself. |
| Refresh walker over all entries | Needs a second read port on the twin (1,100 µm²) or the core's read port (proof impact). Restore-on-use covers the entries in use; the FPGA can scrub the rest by raw access. |
| On-chip bit-flipper | Injection by raw write over SPI reaches about one flip per 450 chip clocks, enough for these experiments; saves 400 µm². |
| Write-verify in the chip | Needs the read port in the sequencer (proof impact); the FPGA twin's state dump comparison finds the same faults. |
| Parity on the static tables | No natural faults there; 700 to 1,300 µm². |
| Triplicated control flops | 600 µm²; the invariant checkers measure control upsets without it. |
| Fault counters, log and time stamps in the chip | Offloaded to the FPGA (events on pins, sticky flags in FSTAT). |
| Exact replica of the critical path | A copy of read tree and sum costs 1,500 µm²; generic oscillators of the same cell types are used instead. |
| Binarised inference, 5-bit weights, 16-bit path history | Removed or reduced to make room (weights 4 bits: −0.38 points in the simulator; path history 8 bits). |

## 4. Budget

Now 58,947 µm² routed (81.2 %). Additions about 5,600 µm² (leaky twin with read tree 2,950; codec, staging,
compare 500; pulse selector 200; oscillators and divider 500; gshare randomness 300; checkers and watchdog 450;
canary, flags, registers, demotion 700). Trims about 3,300 µm² (4-bit weights 2,900; binarised inference 200; path
history 200). Net about +2,300 µm² → about 84 %. If it does not route, drop in this order: SEC-DED → parity only,
the resting oscillator, the gshare randomness, the invariant checkers.

## 5. The leaky cell and its probe

`bpx_ltab` parameter `DYN = 1`: each bit is the output node of a tri-state inverter (`sky130_fd_sc_hd__einvp_1`,
5.0 µm² against 15.0 µm² for a latch) enabled only during the write pulse; the node then floats on the input
capacitance of the read tree. The read tree's select inputs isolate every unselected entry. Simulation: the cell
model in `test/sky130_cells_sim.v` holds its value for ever; the PDK's gate-level model shows the node as unknown, so
gate-level tests run with the twin off (FCTL[0] = 0 gates its read data to 0 before anything else sees it).

Probe (branch `probe-leaky`, commit `b428b21`: one 16 × 8 `DYN` table and nothing else). Local build **(VERIFIED)**:
128 `einvp_1` cells in the final netlist, routing and Magic DRC 0, LVS 0, antenna 0, setup +9.50 ns at the slow
corner, hold +0.069 ns, gate-level test passes with the read data masked. The flow's notice "found 2 floating nets"
names VGND and VPWR at that stage, not the storage nodes. CI gds and precheck: see section 9.

Retention is not known: about 0.1 to 1 ms at room temperature **(ESTIMATED: leakage of the two off stacks against
a few fF)**. An entry is rewritten whenever its weight trains, so entries in use are refreshed by use; the clock
frequency (the FPGA sets it) and the temperature are the experiment's knobs.

## 6. Verification plan

- Everything off (FCTL = 0): the plain design, bit-exact against the golden model, verified to the level of
  `g3-perceptron` (after the trims).
- Each quirk is in the golden model `model/bpx1.py`. The twin is a separate array in the model; in simulation it
  never leaks, so faults are injected by raw writes to its own raw space (and to W3) and the codes, the abstain
  rule, restore-on-use, flags, events and demotion are checked bit-exactly. The gshare randomness is exact with the
  pseudo-random source; the oscillator bit is constant 0 in simulation.
- Detectors: each is fired at least once in simulation (twin flags by injection, canary by its self-test bit, the
  checkers and the watchdog by forcing a signal in a cocotb test).
- Formal: the existing tasks stay green; the checker flags are asserted to stay 0, so the proofs show they cannot
  fire without a physical fault.
- Cannot be simulated: retention, short write pulses, real timing faults, oscillators. Those are the silicon
  experiments; in gate-level simulation the pulse setting stays 0 and the twin and the oscillators stay off.

## 7. Detailed design

Rules for every part: with FCTL = 0 the behaviour is exactly the plain design; no experiment may change the
handshake, the queue or the write sequencer's protocol; the bimodal/chooser table, the gshare table, W3 and W3L are
written only by the standard pulse (`bpx_wseq.u_wp`), structurally: the short pulse is a separate gate that feeds
only W0–W2.

### 7.1 Trims (step 2)

`CB` 5 → 4 (weights −8…7), `SB` 9 → 8, `PL` 16 → 8, binarised inference (feat[3], CTRL[15]) removed: CTRL[15] reads
0 and is ignored. Raw map unchanged (small registers 6 and 8, the upper path-history words, read 0). Threshold
start and the reference-input weights are re-chosen by a small simulator sweep on the tuning programs only
(`explore/bpsim`, kind `gehlr`, `cb=4 pp=8`): theta ∈ {6, 8, 10, 12} × (ub, ug, ut) ∈ {(2,4,4), (1,2,2), (1,3,2),
(2,3,3)}; take the best mean speedup, report all 16.

### 7.2 Registers

`FCTL` (0x11, RW, reset 0):

| Bits | Name | Meaning |
| --- | --- | --- |
| 0 | TWIN_EN | the twin's read data is let through: comparison, flags, events |
| 1 | TWIN_USE | the default predictor's prediction takes W3L instead of W3 (needs bit 0) |
| 3:2 | CODE | 0 none, 1 parity → abstain, 2 SEC-DED, 3 detect all → abstain |
| 4 | RESTORE | restore-on-use (7.3) |
| 7:5 | PULSE | write pulse of W0–W2: 0 standard, 1–7 short (7.4) |
| 8 | GRAND | gshare probabilistic hysteresis (7.7) |
| 9 | GSRC | random bit: 0 pseudo-random register, 1 oscillator |
| 10, 11 | ROA, ROB | oscillator A, B runs |
| 12 | ROSEL | divider and random bit take oscillator 0 = A, 1 = B |
| 13 | DEMOTE | automatic demotion (7.6) |
| 14 | DBGF | the DBG selectors index the fault events instead of the standard events |
| 15 | CTEST | canary self-test: the shadow sample is inverted |

`FSTAT` (0x12, R, sticky; cleared by CMD bit 2 and by reset; bits 10 and 11 are live):

| Bit | Flag |
| --- | --- |
| 0 | TW_ERR: a predict lookup read a twin word that is not the code word of W3's weight |
| 1 | TW_COR: SEC-DED corrected a word |
| 2 | TW_DET: the code detected an error it did not correct (the weight abstained) |
| 3 | TW_SDC: silent: the weight taken from the twin differed from W3's and was not flagged |
| 4 | CANARY |
| 5–8 | invariant checkers (7.5) |
| 9 | WDOG |
| 10 | DEMOTED (live) |
| 11 | KEPT: the last reset kept the tables (strap) |

Fault events (DBG pins with FCTL[14] = 1; same timing as the standard events): 0 TW_ERR, 1 TW_COR, 2 TW_DET,
3 TW_SDC, 4 CANARY, 5 any checker, 6 WDOG, 7 DEMOTED, 8 divided oscillator, 9 random bit used, 10 default
prediction as it would be without the twin (from W3), others 0. Twin events belong to the predict just
acknowledged.

### 7.3 The leaky twin W3L

- Storage: `bpx_ltab #(.AW(5), .RW(3), .EW0(8), .DYN(1))`; word = {check[3:0], data[3:0]}; check bits
  c0 = d1^d2^d3, c1 = d0^d2^d3, c2 = d0^d1^d3, c3 = d0^d1^d2 (extended Hamming (8,4), distance 4; every code word
  has even parity).
- Written, always (whatever FCTL says) and with the standard pulse: with the code word of W3's new weight whenever
  W3 is written by training; with the code word of W3's initial value by clear; raw writes to space 11 (W3) do
  **not** write the twin; raw space **12** reads and writes the twin's 8 bits. So the FPGA can inject into either.
- Read: same address as W3 in every lookup. With TWIN_EN = 0 the 8 bits are forced to 0 first (gate-level X).
- Decode by CODE: 0 → weight = data, never flagged. 1 → odd parity of the 8 bits: abstain. 2 → syndrome non-zero
  and odd parity: correct the single bit (a check-bit error leaves the data); syndrome non-zero and even parity:
  abstain. 3 → any non-zero syndrome or odd parity: abstain.
- Abstain: table 3's term is left out of the sum (and of |sum| for the confidence).
- TWIN_USE changes only what the chip predicts and its CONF (the lookup sum). Training is always computed from the
  static tables (the staged W3), so the learned state of every predictor is the same with the twin on or off, and
  W3 stays golden. The statistics count the predictions actually given, so they show the twin's cost.
- Flags at each executed predict (TWIN_EN = 1): TW_ERR (raw word ≠ code word of W3's weight), TW_COR, TW_DET,
  TW_SDC (not abstained and weight used ≠ W3's weight).
- Restore-on-use (RESTORE = 1, TWIN_EN = 1): when a branch resolves and trains but W3 is not written (no learning
  step), the twin's entry of that branch is rewritten with the word read at its commit lookup, corrected if CODE = 2
  and the error was single, otherwise as read. Eight more staging bits. In simulation this changes nothing unless a
  fault was injected.

### 7.4 Write-pulse selector (W0–W2 only)

`wpulse_x = a & b & ~d`, d = 0 for PULSE = 0, else tap k of a delay line on `a` (the sequencer's rising-edge flop):
the pulse rises with `a` and falls when the tap rises; d falls after `a` has fallen, so each edge still has one
cause. Taps after 1, 2, 3, 4, 6, 8, 12 delay cells (`sky130_fd_sc_hd__dlygate4sd3_1` or the nearest cell; give the
typical delays from the liberty file in the document). Cells instantiated by name with `keep`; the tap mux is an
AND-OR with static selects. Behavioural and formal models: no effect. `test/sky130_cells_sim.v` gets a delay-cell
model with a real delay so that the cells-level test can check that every setting still writes.

### 7.5 Canary, checkers, watchdog

- Canary: on the PRED output flop (and CONF if cheap). A second flop (`dfxtp_1`, by name, `keep`) clocked by the
  same edge samples the same D through a delay chain (about 2 ns typical; by name, `keep`). Flag when the two
  differ in the cycle after an acknowledge. CTEST inverts the shadow sample. Behavioural and formal: no delay. If
  the canary path misses timing at 30 ns, shorten the chain; report the margin.
- Checkers: four cheap invariants taken from the proven assertions in `bpx_core.v` (candidates: queue count in
  range and consistent with the valid bits, staging/sequencer state combinations that cannot occur, no table write
  capture while the sequencer is busy, ACK only answering REQ). One FSTAT bit each. Add `assert (!flag)` for each
  in the formal section.
- Watchdog: a request pending (REQ ≠ ACK) for 64 clocks without an answer while READY sets WDOG. Flag only.
  State the bound of the proven liveness property and keep the count above it.

### 7.6 Demotion

A 2-bit saturating count of detected faults (TW_DET, CANARY, any checker, WDOG); on the fourth, with DEMOTE = 1,
DEMOTED is set and the driving predictor is the tournament whenever CTRL selects the default. Cleared by CMD bit 2
and reset.

### 7.7 gshare randomness, oscillators, strap (step 5)

- GRAND: when the gshare counter is saturated (0 or 3) and training would move it away, it moves only if the
  random bit is 1. GSRC = 0: a 7-bit maximal-length shift register that steps once per training; GSRC = 1: the
  selected oscillator's divider LSB sampled through two flops (constant 0 in simulation).
- Oscillators: two rings of named cells (NAND enable + inverters of the cell types on the critical path), each
  with its run bit; a ripple divider (÷256) on the selected one; the divided signal is fault event 8. Stress:
  A kept running, B run only to be measured. The flow must accept the loops: check with a local build first.
- Strap: if OP = 3 (`uio_in[3:2]`) on the last clock edge of reset, the chip does not clear its tables (all else
  resets) and sets KEPT. After power-up the raw port then reads the latches' power-up state; later it is a warm
  reset that keeps what was learned.

## 8. Steps

1. [done] Probe: branch `probe-leaky`, local build clean, pushed for CI.
2. Trims (7.1), re-verify, local build for the new area baseline.
3. Registers (7.2), leaky twin (7.3): RTL, model, random generator, tests; local build.
4. Pulse selector (7.4); canary, checkers, watchdog (7.5); demotion (7.6); formal.
5. gshare randomness, oscillators, strap (7.7); local build.
6. Full regression (co-simulation of the 71 programs, random plain and concurrent, formal, cocotb, local build),
   documents (SPEC, verification, area, timing, bring-up experiments), CI, merge to `main`, tag.

## 9. Progress log

- 26 Nov: probe built locally, clean (section 5); pushed as `b428b21`. CI result: pending.
