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

As built in step 3: all 16 FCTL bits are storage (bits without a function yet are read back as written); FSTAT
bits 0–3 and fault events 0–3 and 10 exist, the others read 0. Events 0–3 are one-clock pulses that the DBG flops
show together with the `ACK` of the predict (like the prediction events); event 10 is a level, the W3-only
default prediction of the last predict. **Added rule:** a write of FCTL that changes TWIN_EN or CODE clears the
staging-valid bit, so the oldest branch is looked up again (see 7.3, restore-on-use); a write that changes neither
does not touch the staging register, so writing FCTL = 0 over 0 changes nothing at all.

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

**As built in step 3 (where it differs or is more precise than the above):**

- Staging and exactness. The core already keeps the golden model transaction-exact by one rule: the staging
  register is a copy of the oldest branch's commit lookup, and every event that can change what that lookup would
  read clears its valid bit (a resolve, a raw write of any table, a clear, a flush); the branch is then looked up
  again before it resolves. The twin follows the same rule: its staged word is the masked (TWIN_EN), decoded (CODE)
  word as restore-on-use would write it, so a change of TWIN_EN or CODE must also invalidate the staging register
  (the added FCTL rule in 7.2). With it the model can read the twin at resolve time, as it reads every table, and
  stays exact; without it a resolve would restore a word read under the old setting (a mutation that removes the
  rule is caught by the random regression, section 9). RESTORE itself is taken at the resolve.
- "Corrected if the error was single" is implemented as the decoder sees it: if SEC-DED corrected the word (odd
  parity), the corrected code word is written back; a triple error is miscorrected and written back miscorrected.
  Nothing on the chip can tell a single from a triple error, so this is the nearest exact rule.
- Flags are computed at every executed predict with TWIN_EN = 1, with or without TWIN_USE. TW_COR is set for a
  corrected data bit, a corrected check bit and a miscorrection alike; TW_SDC means "not abstained and the decoded
  twin weight differs from W3's", whether or not the sum uses it, so a miscorrection sets TW_COR and TW_SDC.
- Raw space 12 reads through the same TWIN_EN mask (0 while the twin is off); writes always work.
- The mask is eight named `and2_1` cells (`keep`) right at the twin's read output, so no restructuring can let an
  unknown through: with TWIN_EN = 0 the decoder, the staging bits, the comparison, the raw read and the flags all
  see 0 (gate-level test with FCTL[0] = 0: passes, section 9).
- Event 10 needs the prediction with W3 as well as the one with the twin: `bpx_psum` shares the sum of tables 0–2
  and the reference terms and adds the last term twice (one more 8-bit adder). Abstain is a per-table `skip` input.
- The twin's RW is `LGT > 4 ? 3 : 2` like the weight tables (3 on the chip: 8 rows × 4 columns).

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

**As built in step 4 (where it differs from or is more precise than 7.4–7.6):**

- Timing exception audit (item 0). Every flop that can take data derived from a table read is enabled only by
  `p_exec` (PRED, CONF, the queue entry, the speculative history, the last predictions `lp` and their DBG events,
  `lpw` and fault event 10, the twin flags into FSTAT and fault events 0–3, the staging register of a predict with
  an empty queue), `do_stage` (staging) or `drd_exec` with a table space (raw read data `dq`); all three require
  the write sequencer idle (`~wbusy`, cycles 1 and 2 excluded, so the pulse cycle too). DBG takes the prediction
  events through `p_exec ? preds : lp`, and the canary flops sample PRED/CONF's D, which is a table value only
  under `p_exec`. The statistics counters take the queue's stored predictions (flops), never read data. Stated as
  formal assertions (`!((p_exec | do_stage | drd_exec & d_tab) & wbusy)`, and with `wbusy` PRED/CONF's D is the
  hold or refusal value) and proven. No flop violates it.
- 7.4. The setting is copied from FCTL[7:5] into three flops of `bpx_wseq` only while the sequencer is idle, so a
  write of FCTL that coincides with a pulse cannot change the tap during it (FCTL can be written at any clock). The
  tap multiplexer is `bpx_aor` (N = 8, hand-mapped AOI cells, one path per tap); the pulse gate is
  `sky130_fd_sc_hd__and3b_1 u_wpx` (A_N = tap, B = a, C = b); the delay line is 12 named
  `sky130_fd_sc_hd__dlygate4sd3_1` (`u_dly`). The flow may not resize or remove any `u_dly` instance
  (`RSZ_DONT_TOUCH_RX`, src/config.json): the resizer could otherwise swap a delay cell for a buffer.
  Typical delays of `dlygate4sd3_1` A → X from the liberty files (input slew 0.1 ns, load 4 fF; rise / fall):
  tt_025C_1v80 0.561 / 0.582 ns, ss_100C_1v60 1.071 / 1.158 ns, ff_n40C_1v95 0.390 / 0.394 ns (area 10.0 µm²).
  The latch's own minimum GATE_N pulse (liberty, smallest slew) is 0.14 ns tt, 0.29 ns ss. The pulse widths per
  setting from the post-layout netlist are in section 9. `test/sky130_cells_sim.v` models the cell with 0.57 ns;
  the latch-cell test `pulse_settings` measures 0.57 × k ns on W0–W2 (k = 1, 2, 3, 4, 6, 8, 12) and the standard
  half clock on BC, G, W3 and the twin, at every setting, and checks raw writes and training against the model.
- 7.5 canary. On PRED and on CONF. The two real flops are named `dfxtp_1` cells too (`bpx_canary.u_q`), so that the
  delay chain taps exactly the flop's D net. The chain is one `dlygate4sd1_1` (liberty: 0.21 / 0.26 ns tt,
  0.40 / 0.50 ns ss, rise / fall), much shorter than the "about 2 ns" planned: the real CONF flop's slack at the
  slow corner moved between builds (+2.18, +1.16, +2.46 ns), and the canary must keep +0.3 ns. The CTEST XOR
  (`xor2_1`) sits on the shadow flop's output, off the timed path (first built in front of the shadow flop with
  two, then one `dlygate4sd3_1`: the CONF canary missed setup by 0.39 and 0.34 ns; section 9). Measured lead of
  the canary over its flop, step-4 build: 0.20 ns (CONF) and 0.22 ns (PRED) typical, 0.40 and 0.48 ns at the slow
  corner; final build (step 5, two `dlygate4sd3_1`): 1.26 / 1.23 ns typical, 2.61 / 2.41 ns slow (section 9). The comparison is made in
  the clock after PRED/CONF were loaded (an executed or a refused predict), so fault event 4 is a pulse one clock
  after the ACK (it cannot come with the ACK); the random co-simulation checks DBG one clock after every ACK.
- 7.5 checkers (FSTAT 5–8): queue (`qcnt > 4` or `FULL ≠ (qcnt = 4)`); staging (staging valid or a resolve
  pending with an empty queue, or resolve-first done with no request seen); write sequencer (`s1 & a`, two states
  at once, or a table-write request while busy, which would be lost); handshake (in a clock that followed a ready,
  non-reset clock, ACK toggled exactly when a request was answered: catches a toggle without an answer and an
  answer without a toggle).
- 7.5 watchdog: counts the clocks a request is pending, restarting when it is answered and at every control-port
  write (a write may legitimately hold requests: clear predictors; and the proofs leave the control port free in
  every clock). SPI words are at least 128 clocks apart, so a real hang still reaches 64. The count wraps, so WDOG
  (event 6) fires every 64 clocks while a hang lasts. Liveness bound, proven (k-induction, free inputs): the
  count never exceeds 12, i.e. a request pending without a control-port write is answered by its 13th clock; a
  formal cover reaches 12 and not 13. The watchdog limit, 64, is five times that. (A first version counted all
  pending clocks; with REQ free in every clock, requests that follow each other without a gap added up past 23:
  found by the cover, hence the restart at each answer.)
- 7.6: faults in the same clock count once (they cannot coincide in fault-free operation: TW_DET comes with the
  predict, the canary a clock later). The substitution needs DEMOTE = 1 as well as DEMOTED, so DEMOTE = 0
  suspends it (DEMOTED and the count stay until CMD bit 2 or reset) and FCTL = 0 is always the plain chip. FSTAT[10]
  is DEMOTED; fault event 7 is its level (with the ACK of a predict: the state before that predict; one clock later:
  including the predict's own TW_DET, not yet its canary). Soft reset clears it like reset.

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

**As built in step 5 (owners' changes of 26 Nov, and where the RTL is more precise than the above):**

- GRAND (FCTL[8]) and GSRC (FCTL[9]) as planned, in `bpx_pred`: `g_hold = GRAND & (counter at the limit training
  moves away from) & ~bit`; the counter is then rewritten unchanged. The LFSR is 7 bits, x^7 + x^6 + 1
  (`{q[5:0], q6 ^ q5}`), 0x55 after reset and after every clear (like the threshold), and steps once per training
  (every resolve that trains, i.e. not frozen), whatever FCTL says, so its state never depends on GRAND or GSRC. The
  random bit is its bit 6 before the step (GSRC = 0) or the oscillator bit (GSRC = 1). gshare feeds the tournament
  and the reference inputs of the default predictor's sum; with GRAND = 0 nothing changes anywhere. Golden model:
  exact for the LFSR; the oscillator bit is 0 (`OSC_BIT`), so with GSRC = 1 a counter never leaves its limit in any
  simulation.
- Fault event 9, defined: the random bit (of the source GSRC selects, whatever GRAND says) used at the last training
  before the last executed predict. It is a level that changes only when a predict executes (flop `lrb`, like event
  10): the bit of the last training is kept in `rb_q` at each training and copied at the predict. In a combined
  request the predict comes first (also in the RTL, where the resolve may follow in the same clock or later), so the
  bit of a request's own training shows at the next predict. This is the only definition the golden model can check
  at the ACK and one clock later whatever the timing: a level that followed each training would change at a deferred
  resolve, at a time the host does not control. With a predict per training (trace replay, one branch in flight) the
  FPGA reads one bit per branch, one request late.
- Ring oscillators (`src/bpx_ro.v`): the probe's ring A twice (`bpx_ring u_ra, u_rb`: nand2 + 4 × (a222oi, xor2,
  a22oi, nand3) + xnor2, 18 cells, 137.6 µm² each); the probe's ring B is gone, so A (kept running) and B (rested)
  are identical. FCTL[10] runs A, [11] runs B, [12] selects the ring for the divider and the random bit. Divider and
  random bit as in the probe (8 ripple toggle flops with an asynchronous reset while the selected ring is stopped;
  bit 0 through two flops is the random bit); the divided signal (bit 7) gets one synchroniser flop before the DBG
  flop: fault event 8. The block sits in `bpx_chip`; the core gets the random bit and the divided signal and gives
  out FCTL[12:10]. Behavioural and formal builds: no rings, both outputs 0 (in the formal top both are free inputs,
  so the proofs hold whatever they do). Gate-level tests keep FCTL[11:10] = 0 (a running ring is a zero-delay loop
  with the PDK's models). Latch-cell RTL: `test/sky130_cells_sim.v` gives `xnor2_1` (used nowhere else) a 4 ns
  inertial delay, one per ring: both rings simulate at 125 MHz; cocotb `ring_oscillators` measures it from event 8.
  The resizer may not touch the ring cells or nets (`RSZ_DONT_TOUCH_RX = u_dly|u_ro\.u_r[ab]\.([gnr]|u_en|u_dl)`),
  so the two rings stay cell for cell identical (set before the build, not as a reaction to it: a resized stage in
  one ring only would spoil the comparison). Each ring's run pins are driven by a named `buf_2` (`u_bf`, not
  protected): the first build with the whole ring protected stopped in post-placement repair with
  `[RSZ-3006] Failed to insert buffer before loads for net u_chip.u_core.fctl[11]` (`[ODB-1211] ... Load pin
  'u_chip.u_ro.u_rb.g_st[0].u1/B' is dont_touch`): the resizer cannot repair a net whose loads are all untouchable.
- Strap, changed by the owners: OP = 3 (`uio_in[3:2]`) **and** CSn = 0 (`uio_in[4]`) **and** MOSI = 1
  (`uio_in[6]`) at the last clock edge of reset (OP = 3 alone is an ordinary request code). In `bpx_chip` from the
  synchronised pins (OP one flop, CSn and MOSI two); the core takes it into `kept` at every clock of a pin reset (the
  last one counts) and clears it at a soft reset. The reset's clear still runs its write-sequencer cycle (READY at
  the same clock as before), but its table clear is gated: `clr_tab = clr_exec & ~(kept & init_pend & ~clrp_pend)`,
  so a clear requested by CMD bit 1, even one that coincides with the reset's, always clears. Everything else resets
  (histories, threshold, adaptation counter, LFSR, queue, statistics, CTRL, FCTL, FSTAT). FSTAT[11] KEPT is live:
  set until the next reset without the strap (a soft reset included); CMD bit 2 leaves it. The SPI block after such
  a reset: it starts with CSn low, no command bit and SCK low; without an SCK edge nothing happens, and the next 8
  rising SCK edges are a command byte even if CSn never rose (tested). The FPGA must keep SCK low through the reset,
  hold OP, CSn, MOSI from 4 clocks before `rst_n` rises to 4 clocks after, and should take CSn high for at least 4
  clocks before its first SPI word (so a stray SCK edge during the release cannot shift the command).
- Code mask for the twin (owners' item 4, then changed during the step by the owners after their fault campaign:
  value `8'hF0` and switchable). CTRL[15] (free since binarised inference was removed) is **MASK**, reset 0. With
  MASK = 1 the nodes hold `code word ^ TWMASK`; TWMASK is one parameter (`bpx_chip`, passed down; the model's
  `bpx1.TWMASK`, `host/bpx1.py` `TWMASK`), `8'hF0`: the four check bits inverted, data bits untouched, so with
  CODE 0 nothing changes. Training writes, restore-on-use and the clear carry the mask of the present setting (the
  twin's clear value comes through its write data, `bpx_ltab` parameter `CLRD = 1`, since it now depends on MASK);
  the decoder takes it off after the TWIN_EN gate (`tw_c = tw_m ^ (TWMASK & MASK)`; with TWIN_EN = 0 every flag and
  use stays gated as before). Raw space 12 reads and writes the nodes as they are. A write of CTRL that changes MASK
  invalidates the staging register (the step-3 rule for TWIN_EN and CODE), because the staged twin word depends on
  it. Words stored under the other setting read as faulty until rewritten: exact in the golden model; the
  experimenter clears the predictors (CMD bit 1) after switching MASK. Fully decayed words: with MASK = 0, all-0 and
  all-1 nodes are the code words of 0 and −1 (no code sees them); with MASK = 1 they read as 0xF0 and 0x0F (syndrome
  1111, even parity): SEC-DED and detect-all abstain (TW_DET), parity does not see them (tested, `twin_directed`).
  Cost: one flop and the XORs on the four check bits (write and read side) plus the clear-value mux.- Answers to the owners' two open points (item 5):
  (a) W0–W2 take `wpulse_x` for every write: training, raw writes (spaces 8–10) and the table clear. So with
  PULSE ≠ 0 a raw write and a clear by CMD bit 1 use the short pulse. The clear at the end of a reset (pin or soft)
  always uses the standard pulse: FCTL is already 0 in the clock in which the sequencer copies PULSE and captures
  the clear. For the experimenter raw writes on the
  short pulse are what is wanted: writing known patterns at a setting and reading them back measures the write
  failure rate directly. A clear on the short pulse can leave W0–W2 partly uncleared, which the FPGA twin cannot
  predict; set PULSE = 0 before CMD bit 1 (or use reset). Making the clear always standard would cost about three
  gates (force the sequencer's PULSE copy to 0 when it captures a clear: `psel <= clr ? 0 : psel_in`, about
  20 µm², ESTIMATED); not changed.
  (b) Yes: TW_DET counts towards demotion with TWIN_USE = 0. The count takes `TW_DET & p_exec` and the twin flags
  depend only on TWIN_EN and CODE, so in observe mode a detected twin error still counts (and DEMOTED then switches
  the default predictor to the tournament although the twin's weight was not used). Unchanged.

## 8. Steps

1. [done] Probe: branch `probe-leaky`, local build clean, pushed for CI.
2. [done] Trims (7.1), re-verify, local build for the new area baseline.
3. [done] Registers (7.2), leaky twin (7.3): RTL, model, random generator, tests; local build.
4. [done] Pulse selector (7.4); canary, checkers, watchdog (7.5); demotion (7.6); formal.
5. [done, 27 Nov] gshare randomness, oscillators, strap (7.7), code mask, statistics offload; build (section 9).
6. [regression, build and documents done, 27 Nov (section 9); merge to `main`, tag and CI of `main` open: the
   lead's next step] Full regression (co-simulation of the 71 programs, random plain and concurrent, formal, cocotb, local build),
   documents (SPEC, verification, area, timing, bring-up experiments), CI, merge to `main`, tag.

7. [done, 26–27 Nov; branch `campaign`, merged into `mosaic` in step 6; docs/fault_campaign.md] Pre-silicon
   fault-injection campaign (owners' request of 26 Nov, once the design is stable; on Vast, economical):
   random bit flips in each predictor's tables at several rates on all 71 programs, and a decay model of the leaky
   table under each protection mode. Result: predicted extra mispredictions per fault, per predictor and per mode,
   as the baseline for the silicon measurements. Planned in the trace simulator (`explore/bpsim`, which equals the
   RTL), with a spot check on the RTL by raw-write injection.

Vast may be used whenever the PC is the bottleneck (co-simulation, sweeps, random regression, formal, the
campaign): labels `bpx1-*`, destroy when idle, $12 total cap.

## 9. Progress log

- 26 Nov: probe built locally, clean (section 5); pushed as `b428b21`. CI result (read 27 Nov, step 6): gds,
  precheck, gl_test, viewer, test and docs success (run 45862940558 and its siblings); the ring-oscillator probe
  `probe-ro` (`67eaabd`) likewise (run 45889703170) **(VERIFIED, step 6, public GitHub API)**.
- 26 Nov, step 2 (trims) **(VERIFIED, this session)**. Sweep (`explore/bpsim` W=8, built with `-Wshadow`; its one
  warning, the local `pr` in `GehlRP::train`, is old and harmless: the member is used as `this->pr`), speedup / CPU
  accuracy, tuning (51) and held-out (18) programs:

  | Candidate | Tuning | Held-out |
  | --- | --- | --- |
  | baseline cb=5 pp=16 θ12 (2,4,4) | 1.13383 / 90.66 | 1.14200 / 93.60 |
  | cb=4 pp=16 θ12 (2,4,4), for reference | 1.13260 / 90.27 | 1.14071 / 93.23 |
  | **θ10 (2,3,3), chosen** | **1.13232 / 90.18** | **1.14025 / 93.05** |
  | θ6 (2,3,3) | 1.13232 / 90.18 | 1.14010 / 93.03 |
  | θ12 (2,3,3) | 1.13231 / 90.18 | 1.14025 / 93.03 |
  | θ8 (2,3,3) | 1.13231 / 90.18 | 1.14019 / 93.03 |
  | θ12 (2,4,4) | 1.13228 / 90.20 | 1.14015 / 93.02 |
  | θ6 (2,4,4) | 1.13228 / 90.19 | 1.14006 / 93.00 |
  | θ8 (2,4,4) | 1.13228 / 90.20 | 1.14012 / 93.02 |
  | θ10 (2,4,4) | 1.13228 / 90.19 | 1.14021 / 93.03 |
  | θ8 (1,3,2) | 1.13208 / 90.09 | 1.14007 / 93.00 |
  | θ12 (1,3,2) | 1.13208 / 90.08 | 1.13996 / 92.99 |
  | θ10 (1,3,2) | 1.13207 / 90.09 | 1.13996 / 92.98 |
  | θ6 (1,3,2) | 1.13207 / 90.08 | 1.13999 / 92.99 |
  | θ12 (1,2,2) | 1.13196 / 90.04 | 1.13977 / 92.93 |
  | θ6 (1,2,2) | 1.13195 / 90.02 | 1.13985 / 92.94 |
  | θ10 (1,2,2) | 1.13192 / 90.01 | 1.13987 / 92.94 |
  | θ8 (1,2,2) | 1.13191 / 90.01 | 1.13986 / 92.95 |

  All 69 programs: 1.13439 / 90.93 % against 1.13596 / 91.42 % (−0.49 points). θ and the (2,3,3)/(2,4,4) choice are
  within 0.0001 of speedup of each other: noise. Applied: CB 4, SB 8 (|sum| ≤ 78), PL 8, CTRL[15] removed (reads 0,
  writes ignored; reset CTRL still 0x7DA0), θ start 10, reference weights 2, 3, 3; model, random generator (CTRL
  writes now set bit 15 now and then), cosim, coverage (8 feature settings), test.py (reset CTRL and bit 15 checked),
  host, SPEC. Verification: cocotb latch cells / flip-flop model / repository top 3/3 each; random 4 × 50,000 plain
  and concurrent: 0 reply, statistics, state and background mismatches, coverage 100 % (the 16 cross bins not hit
  are the 16 that cannot occur); co-simulation of 6 programs (team.coremarkish, embench.nettle-aes,
  numeric.mandelbrot, embench.nsichneu, coremark, embench.crc32; 11.1 million requests through the model): 0
  mismatches, statistics equal the model, one-in-flight mispredictions equal bpsim on 6 of 6, in-flight
  mispredictions of the default equal bpsim on 4 of 6 (team.coremarkish 18,885 vs 18,800, coremark 72,616 vs
  72,604; gshare also differs on team.coremarkish: the known difference of the misprediction-alone rule, 47 of 69
  before); formal prove, nowait, cover pass. Local build: 74.2 % instance utilisation (53,848 µm² cells, was
  58,947 µm² / 81.2 %), routing DRC 0, Magic DRC 0, LVS 0, antenna 0, setup **+1.87 ns** at the slow corner (was
  +3.87; worst path BC table → reference terms of the sum → CONF; probably the ×3 weights, which need an adder where
  ×4 was a shift, ESTIMATED), hold +0.056 ns, 2,140 max-slew / 3 max-cap warnings (was 2,259 / 5), gate-level test
  3/3. Synthesised 49,406 µm² (was 54,112): weight tables 4 × 3,237, the two sums 2 × 1,107.
- 26 Nov, step 3 (registers 7.2, leaky twin 7.3) **(VERIFIED, this session)**. Reference-input weights back to
  (2, 4, 4), θ 10 (RTL, model, `cosim/bpsim.cfg`, SPEC; |sum| ≤ 4 × 15 + 6 + 12 + 4 = 82 < 128, SB = 8 holds):
  equal to (2, 3, 3) within noise in the sweep above, and ×4 is a shift where ×3 needed an adder on the critical
  path. FCTL (all 16 bits storage), FSTAT bits 0–3, fault events 0–3 and 10, raw space 12, the twin (`bpx_ltab`
  DYN, AW 5, RW 3, EW0 8) with mask, decoder, abstain (`bpx_psum` skip input), TWIN_USE, flags, restore-on-use
  and 8 staging bits; the departures from 7.2/7.3 are listed there ("As built in step 3"). Model, random generator
  (random FCTL in mid-stream, injection of 1/2/3-bit flips into twin words and of new W3 weights, FSTAT reads and
  clears, a staging probe for the FCTL rule; `RG_PLAIN=1` keeps FCTL at 0), cosim (`G` register checks, space 12
  in the state dump), conc_check, coverage (49 new bins), test.py (`mosaic_registers`, also at gate level;
  `twin_directed`, RTL), host helpers. Verification:
  - cocotb latch cells / flip-flop model / repository top: 5/5 each.
  - Random 4 × 50,000 and 10 × 100,000, plain and concurrent: 0 reply, statistics, state and background
    mismatches; coverage 100 % (cross 52/52 reachable, other 158/158). Twin bins over 10 × 100,000: every
    reachable CODE × outcome × TWIN_USE combination hit (smallest: parity / wrong word, right weight / observe,
    30); restore-on-use rewrote a clean word 61,333, a corrected word 102, an erroneous word as read 931; each
    FSTAT flag set (67–118) and cleared (64–109); fault events 0, 1, 2, 3, 10 seen on DBG; TWIN_USE changed the
    default prediction 587 times. Outcomes that cannot occur, by construction of the codes: no code → abstain or
    correction; parity → correction; SEC-DED and detect-all → a wrong word with the right weight; detect-all →
    correction.
  - FCTL = 0: random `RG_PLAIN=1` 4 × 50,000 plain and concurrent, 0 mismatches; co-simulation of
    embench.nettle-aes, numeric.mandelbrot, embench.crc32 (cosim/run.sh, FCTL never written): 0 model
    mismatches, statistics equal the model, mispredictions of all four predictors in flight and of the default
    one-in-flight equal `explore/bpsim` with ug = 4, ut = 4, θ 10 on 3 of 3 (default in flight 22,697 / 6,258 /
    515).
  - Mutation check (2 × 20,000 random each): removing the FCTL staging rule, restoring the raw instead of the
    corrected word, a wrong correction position, event 10 from the twin-side prediction, the raw read without the
    mask, TW_SDC without the abstain condition: each caught (the first only after the staging probe was added to
    the generator; without it 2 × 20,000 missed it).
  - Formal prove, nowait, cover: pass.
  - Synthesised by block (Yosys, hierarchy kept): chip 55,991 µm² (was 49,406); twin table 3,757 (256 `einvp_1`
    and its 32-way read tree), sums 2 × 1,358 (in hierarchical synthesis the unused second last-term adder of the
    staging sum is not removed; flattened it is), `bpx_pred` own logic 4,593, `bpx_core` own 14,697.
  - Local build 1 (flow constraints unchanged): 82.5 % instance utilisation, 59,896 µm² cells (was 53,848 /
    74.2 %), routing DRC 0, Magic DRC 0, LVS 0, antenna 0, hold +0.058 ns, gate-level test 4 passed + 1 skipped
    (`twin_directed` is RTL only), 256 `einvp_1` in the final netlist, the 8 mask cells present; **setup −2.60 ns at
    the slow corner (27 endpoints)**, all on one false path: sequencer flop `b` (falling edge) → write pulse →
    twin write gate → `einvp_1` TE → Z → twin read tree → decoder → sum → CONF. Static timing treats the tri-state
    enable as a combinational arc; the node only changes during the pulse cycle, whose reads nothing captures
    (6.2). The same netlist, SPEF and flow SDC in OpenSTA (LibreLane container) reproduce −2.601; with
    `set_disable_timing` on `einvp_1` TE → Z the worst slack is **+3.706 ns** on the true path address flop →
    twin read → mask → decoder → sum → CONF.
  - Fix: `src/bpx.sdc` = the flow's base.sdc plus that one exception, as PNR_SDC_FILE and SIGNOFF_SDC_FILE
    (`src/config.json`; the clock stays 30 ns). Local build 2: 82.5 % (59,891 µm² cells; timing-repair buffers
    4,405), routing DRC 0, Magic DRC 0, LVS 0, antenna 0, **setup +3.21 ns at the slow corner, 0 violations**
    (worst path: address flop → twin read tree → mask → decoder → sum → CONF; then DBG1 +3.99, DBG0 +4.15,
    speculative history +4.47, PRED +4.52), nominal corner +14.10 ns, hold +0.058 ns, 2,522 max-slew / 9 max-cap
    warnings (were 2,140 / 3), gate-level test 4 passed + 1 skipped, 256 `einvp_1`. The twin's decoder now sits
    on the critical path; with ×4 reference weights it still has 1.3 ns more margin than step 2 (+1.87).
    docs/area.md, which lists every flow setting with its reason, is to be updated at the end of the round.
- 26 Nov, step 4 (pulse selector 7.4, canary, checkers, watchdog 7.5, demotion 7.6) **(VERIFIED, this session)**.
  Implemented as in 7.4–7.6 with the differences listed under "As built in step 4". FCTL[7:5] PULSE, [13] DEMOTE,
  [15] CTEST; FSTAT 4 CANARY, 5–8 checkers, 9 WDOG, 10 DEMOTED; fault events 4–7; flow setting
  `RSZ_DONT_TOUCH_RX = u_dly`. Model (`clear_counters` split from `clear_stats`, `_fault`, `drive_eff`, DBG one
  clock after the ACK), random generator (random PULSE, CTEST, DEMOTE; demotion sequences by double errors with
  SEC-DED / detect-all or by CTEST, ended by CMD bit 2 or DEMOTE = 0), cosim (DBG checked one clock after every
  ACK), conc_check, coverage (31 new bins, 12 marked "cannot occur without a fault"), test.py (`pulse_settings`,
  `checkers_watchdog`, `canary_demotion`, RTL; `mosaic_registers` now with CTEST and DEMOTE, also at gate level,
  PULSE kept 0 there), host helpers, SPEC. Verification:
  - Timing-exception audit (item 0): no flop takes table read data unless the sequencer is idle; formal assertion
    proven (see "As built").
  - cocotb latch cells / flip-flop model / repository top: 8/8 each. `pulse_settings` (latch cells) measured W0–W2
    pulses of 0.57, 1.14, 1.71, 2.28, 3.42, 4.56, 6.84 ns for settings 1–7 and 10 ns (half the 20 ns test clock)
    for setting 0 and for BC, G, W3 and the twin at every setting; tables equal the model at every setting.
    `checkers_watchdog`: each checker and the watchdog set exactly its FSTAT bit (the forced hang also sets the
    staging checker, whose state it corrupts), its event on DBG, cleared by CMD bit 2.
  - Random 4 × 50,000 and 10 × 100,000, plain and concurrent: 0 reply (including DBG one clock after the ACK),
    statistics, state and background mismatches; coverage 100 % of the reachable bins (cross 52/52, other 178/178,
    12 checker/watchdog bins cannot occur without a fault). Over 10 × 100,000: CANARY set 75 / cleared 74, DEMOTED
    set 262 / cleared 255, event 4 seen 970, event 7 56,437, canary at executed / refused predicts 14,311 / 950,
    count steps 307 / 297 / 283, demoted by the fourth fault 262, fourth fault with DEMOTE = 0 17,024, faults from
    TW_DET 7,746 and from the canary 15,261, suspended 504, resumed 456, predicts while demoted 76,139 (PRED changed
    by the demotion 16,745), demoted but another predictor selected 49,459.
  - FCTL = 0: `RG_PLAIN=1` 4 × 50,000 plain and concurrent, 0 mismatches (no FCTL write in the stream).
  - Mutation check (2 × 20,000 random each, or the named cocotb test): canary compare not gated, demotion on the
    third fault, demotion to gshare, TW_DET not counted, CMD bit 2 keeps DEMOTED, no suspension by DEMOTE = 0, event
    4 from the wrong signal, queue checker firing on a full queue, CONF from the undemoted predictor; tap wiring
    swapped (`pulse_settings`), handshake checker dead (`checkers_watchdog`), W3 on the short pulse
    (`pulse_settings`): all caught.
  - Formal prove, nowait, cover: pass, with the new assertions (item 0, `chk == 0`, FSTAT 5–9 stay 0,
    watchdog count ≤ 12).
  - Synthesised by block (Yosys, hierarchy kept): chip 57,003 µm² (was 55,991): `bpx_core` own 15,283 (+586:
    checkers, watchdog, demotion, flags, events), `bpx_canary` 115 (four flops, two delay cells, two XORs; the two
    PRED/CONF flops were in the core before), `bpx_wseq` 410 (about +310: 12 delay cells, tap mux, and3b, three
    setting flops).
  - Local build 1 (canary 2 × `dlygate4sd3_1`, XOR before the shadow flop): 84.36 %, setup −0.39 ns at the slow
    corner on the CONF canary (CONF itself +2.18). Build 2 (one `dlygate4sd3_1`): −0.34 ns on the CONF canary
    (CONF itself +1.16; the next real endpoint +0.63). Both clean otherwise (DRC/LVS/antenna 0, gate-level pass).
  - **Local build 3 (final):** 84.34 % instance utilisation, 61,204 µm² cells (was 59,891 / 82.5 %), timing-repair
    buffers 4,559, routing DRC 0, Magic DRC 0, LVS 0, antenna 0, **setup +2.06 ns at the slow corner, 0
    violations** (worst: the CONF canary; the real CONF flop +2.46 on BC table → sum → CONF; PRED +3.54 and its
    canary +3.06), nominal corner +13.87 ns, hold +0.066 ns, 2,787 max-slew / 14 max-cap warnings, gate-level test
    4 passed + 4 skipped (RTL-only tests). Final netlist: `u_wseq.u_wp` (and2_1) drives, through 3 inserted
    buffers, only the row gates of BC (8), G (8), W3 (8), the twin (8) and the snapshot gate; `u_wseq.u_wpx`
    (and3b_1) drives, through 2 buffers, only the row gates of W0, W1, W2 (8 each). 12 `dlygate4sd3_1` in the
    delay line and 2 `dlygate4sd1_1` in the canaries, all kept (`RSZ_DONT_TOUCH_RX` applied, logged).
  - Pulse widths at `u_wpx/X` from the final netlist and parasitics (OpenSTA, latest edges; ns), settings 1–7:
    tt 0.73, 1.33, 1.98, 2.59, 3.74, 4.93, 7.17; ss 1.49, 2.68, 3.94, 5.12, 7.36, 9.73, 14.10; ff 0.47, 0.90,
    1.33, 1.76, 2.55, 3.37, 4.93. Standard pulse (setting 0 and `u_wp`) at 30 ns: 14.6 (tt), 14.2 (ss), 14.7 (ff).
    At the slow corner setting 7 is nearly the standard pulse. The row and gate cells after `u_wpx` reshape these
    further (not timed: latch gates are not endpoints). No constrained path runs through the delay line or
    `u_wpx` (OpenSTA: no paths), so `src/bpx.sdc` needed no new exception.
- 27 Nov, step 5 completed (gshare randomness, rings, strap, code mask as in 7.7; statistics offload; two flow
  fixes; build) **(VERIFIED, this session, unless marked)**. Commit `f78ae8f` held the step-5 RTL and its
  verification (build pending); the build of that source (five statistics counters) placed at 86 % and did not
  converge in detailed routing, so the owners' pre-authorised fallback, the statistics offload, was taken.

  As built (differences from 7.7 and from SPEC before this step):
  - **Statistics offload.** The chip keeps two counters: `n_res` (resolved branches) and `mis_d` (mispredictions of
    the answer given on PRED, the repair condition `r_misp`; 16 bits, saturating). The queue entry is
    `{pdrv, id}` (was `{pdrv, trn, gsh, bim, def, id}`); the snapshot shadow is 32 bits; STATS 0x05/0x06 resolved,
    0x07/0x08 wrong answers, 0x09–0x0E read 0. The per-predictor counts are the FPGA's: it sees the four
    predictions of every executed predict on DBG0, PBIM, PGSH, DBG1, keeps a copy of the in-flight queue by the
    rules of SPEC 3.2 and compares them with the outcomes it reports. DBG0/DBG1 show the default and tournament
    predictions only with the standard selections (events 10 and 13) and FCTL[14] = 0; under DBGF they show fault
    events, so those two counts are taken only over the predicts made under the standard selection (SPEC 4).
    Synthesised (Yosys, hierarchy kept): chip 54,915 µm² against 58,302 for the five-counter source `f78ae8f`
    (−3,387 µm²); `bpx_core` own logic 12,890, snapshot shadow 484.
  - **Verification of the offload, no weaker than before.** The golden model keeps all four per-predictor counts
    (and, for the harness, default and tournament over the branches whose predict showed them on DBG,
    `mis_shown`). `cosim/cosim.cpp` counts the four per-predictor mispredictions itself from the pins, with its own
    copy of the queue, exactly as the FPGA would; in the random runs every statistics step compares the chip's two
    counters and 0x09–0x0E = 0, and the harness's resolved, wrong-answer, bimodal, gshare and shown default and
    tournament counts with the model (the complete default and tournament counts too, in windows where DBG showed
    every branch); in concurrent runs the harness's totals over the whole run are compared with the model's by
    `cosim/conc_check.py` (line H of the log). In the program co-simulation the per-predictor numbers of the reports
    are the harness's pin counts, compared with the model's replay (runs d0 and seq), and in every run the chip's
    counters equal the harness's resolved and mispredicted counts. `test/test.py` counts from the pins too
    (random and trace tests). `host/bpx1.py` `replay()` counts from the pins (`TTPins.get_uio` added; not run on
    hardware).
  - **Flow fix 1 (RSZ-3006).** The write-pulse delay line (`bpx_wseq`) and the canary chains are fed through their
    own named `buf_2` (`u_dlb`, `u_cb`) and their nets are named `u_dly_tap`, `u_dly_c`, so that
    `RSZ_DONT_TOUCH_RX` (`u_dly`) protects nets and cells and the net driving them has only unprotected loads: an
    earlier step-5 build stopped in repair with RSZ-3006 on net `a`. No RSZ-3006 or ODB-1211 in the final build.
  - **Flow fix 2.** Each ring's output leaves through its own `buf_2` (`u_ob`), so the protected ring net `n[17]`
    has only ring cells and that buffer as loads.
  - **Placement density 80** (`PL_TARGET_DENSITY_PCT`, was 84): builds of the offload source at density 84 and 82
    were still at 57 and 183 detailed-routing violations after 31 and 18 iterations when the density-80 build had
    finished clean; they were stopped. docs/area.md (flow settings) is to be updated with this.

  Verification of the final source (the committed one):
  - cocotb latch cells / flip-flop model / repository top: 11/11 each (`tb3.sh`).
  - Random 92 × 100,000 (`v_random.sh`): plain 9,200,750 requests, 0 reply, statistics, state mismatches; 9,631
    statistics windows with the harness's pin counts checked, 0 mismatches (6,457 of them without the complete
    default/tournament check because DBG showed fault events for part of the window; their shown counts were
    checked); concurrent 9,200,759 requests, 0 mismatches, pin totals equal to the model in 92 of 92 runs. Coverage
    295 of 295 reachable bins = 100 % (cross 52/52, other 243/243; 16 cross and 14 other bins cannot occur,
    docs/coverage_bins.md).
  - Plain mode `RG_PLAIN=1` (FCTL never written: 0 FCTL writes, 0 CTRL writes with bit 15, 0 pin resets in the
    stream of seed 1), 92 × 100,000: plain 9,200,796 requests, concurrent 9,200,776 requests, 0 mismatches, pin
    totals equal in 92 of 92 (the first concurrent attempt lost 28 runs to "Resource temporarily unavailable"
    while other jobs ran on the machine; rerun alone).
  - Counter saturation (new: no earlier run reached it): 8 × 1,000,000 operations, 8,000,637 requests, 0
    mismatches; in seed 101, 455 of 1,031 statistics steps had the resolved count saturated and 236 the
    wrong-answer count, with the overflow flag checked.
  - Mutation (4 × 20,000 plain + 4 × 20,000 concurrent each, unless stated; all caught, an unmutated control
    copy clean): GRAND hold bit inverted, GRAND hold ignoring the bit, LFSR wrong tap, LFSR stepping on every
    write capture, strap without OP, without CSn, without MOSI, mask missing on the training write, on the clear
    value, on the restore value, on the read, `mis_d` counting every resolve, `mis_d` counting another prediction,
    `mis_d` without saturation (caught only by the 8 × 1,000,000 runs: 198–239 statistics mismatches per run),
    harness: bimodal counted from the gshare pin, no repair in its queue copy, DBG taken as always standard,
    a full-queue predict accepted, tournament counted from DBG0 (shown and complete counts).
  - Formal (`v_fv.sh`): prove, nowait, cover, bmc, bmcfull: all pass.
  - Co-simulation of the 71 programs (`v_cosim.sh`): 140,267,792 / 140,281,743 / 140,694,144 requests replayed
    through the model (one request per cycle / two per cycle / safe mode), 0 reply mismatches, 0 programs with
    statistics differing from the model, 0 protocol errors, 0 late answers. Default driving: speedup over static
    not-taken 1.1307 (tuning 1.1280, held-out 1.1384) / 1.1345 / 1.1249 (safe); accuracy of each predictor
    driving: default 90.93 %, bimodal 86.47 %, gshare 87.29 %, tournament 88.92 % (safe default 89.06 %). Default
    below the best reference on 2 programs (sys.json −0.78, interp.expr −0.09 points; 31 in safe mode). Against
    `explore/bpsim` (W = 8, `cosim/bpsim.cfg`): default one-in-flight mispredictions equal on 69 of 69 programs,
    in-flight equal on 45 of 69 (the misprediction-alone rule), bimodal 69, gshare 63, tournament 68 of 69.
    Reports regenerated: docs/cosim.md, cosim_dual.md, cosim_safe.md (the first generated since the perceptron
    round).
  - **Build (density 80, 33 ns, LibreLane 3.0.14 on the rented machine, tag `s5p80`)**: 80.92 % instance
    utilisation, 58,720 µm² standard cells (sequential 25,236; timing-repair buffers 4,404 in 763 cells; clock
    buffers 908), wire length 195,623 µm; routing DRC 0 (detailed routing: 0 after 14 iterations, then 0 after 7
    in the second routing round), Magic DRC 0, LVS 0, antenna 0; **setup +3.53 ns at the slow corner, 0
    violations**, nominal +14.89 ns, hold +0.058 ns (ff), 0 hold violations; 2,682 max-slew and 9 max-cap
    warnings (slow corner), 0 max-fanout. Gate-level test 6 passed + 5 skipped (RTL-only tests). Final netlist:
    256 `einvp_1`; rings A and B 20 cells each, cell for cell identical (18 ring cells + `u_bf` + `u_ob`); 16
    named delay cells `u_dly` (12 in the pulse line, 2 × 2 in the canaries; 114 further `dlygate4sd3_1` are
    hold-repair buffers `hold*`).
  - Static timing (OpenSTA in the LibreLane container, final netlist and SPEF, flow SDC plus the `einvp_1`
    exception; docs/mosaic_step5_build/): worst path at the slow corner is the CONF canary, +3.53 ns
    (flop `_5994_` → twin read-address fan-out → twin read tree → mask `g_twm` → decoder → sum → CONF D → `u_cb`
    → 2 × `dlygate4sd3_1`); real CONF
    flop +6.14, real PRED flop +7.40, PRED canary +5.00; next real endpoints +6.90. Canary lead (slack of the flop
    minus slack of its canary), CONF / PRED: tt 1.26 / 1.23 ns, ss 2.61 / 2.41 ns, ff 0.83 / 0.85 ns.
  - Ring loops in static timing: "Warning: There are 4 combinational loops in the design." (two per ring), each
    cut at `u_ro.u_r[ab].g_st[0].u1` A → X ("| loop cut point"); no constrained path runs through a ring (the
    divider and random bit are taken through synchroniser flops). Reported in the mid-flow STA steps; the
    post-route per-corner logs were not fetched before the remote copy was deleted.
  - Where things are: build `vast/results/s5p80/` (metrics, reports, netlist) and the full fetched run
    `~/work/bpx1_vast/h_s5p80/runs/wokwi` (WSL); copied into the repository: docs/mosaic_step5_build/ (metrics,
    area by block, STA per corner, loop warning).
- 27 Nov, step 6 completed (documents, then the review fix-ups below; no change to the synthesised design, the
  model or the flow) **(VERIFIED, this session, unless marked)**.
  - Merged into `mosaic`: `docs-final` (the round's document rewrite; no conflict) as `b504a9f`, then `campaign`
    (the fault-injection simulator, runner, validation and results under `explore/`) as `b845f5c`. One conflict,
    `docs/fault_campaign.md` (added on both): kept the `docs-final` copy, the corrected one; it is the campaign's
    last commit `4041870` plus corrections (33 ns time table, mask built since step 5, per-predictor counts from the
    pins), nothing of the campaign missing. `docs-exp` was already in as `docs/experiments.md`.
  - `explore/bpsim.cc` after the merge: compiles with `-Wall -Wshadow` without a warning (the old `pr` shadow
    warning is gone); `cosim/bpsim.cfg` with W=8 on coremark gives the same counts as `69d0bf4` (default 74,201 /
    74,094 mispredictions sequential / CPU, bimodal 105,207, gshare 124,195, tournament 94,914); the merged
    simulator only adds two columns.
  - Every gap left by `docs-final` for the final figures filled from step 5 above and `docs/mosaic_step5_build/`, `docs/cosim*.md`,
    `docs/coverage_bins.md`, `src/config.json`; the figures given by the verification session agreed with those
    files. Consistency pass: statistics offload (two counters, 0x09–0x0E read 0, per-predictor counts from the
    pins, `host/bpx1.py`), density 80 with its reason in docs/area.md, 33 ns, mask, strap, canary lead as
    measured, coverage 295/295, the probes' CI, the step-5 mutation list. Not measured for the final build: the
    write-pulse widths and Tiny Tapeout's `--print-stats` utilisation (marked DON'T KNOW in timing.md and area.md).
  - CI of the probes: `probe-leaky` `b428b21` and `probe-ro` `67eaabd`: gds, precheck, gl_test, viewer, test, docs
    success (runs 45862940558, 45889703170). CI of `mosaic`: `2837285` (step 3, the first with `src/bpx.sdc`) and
    `ca279c9` (step 4) gds, test, docs success; `69d0bf4`: test and docs success, gds still running when last read
    (02:50 UTC; public API, then rate-limited) **(VERIFIED, public GitHub API)**. Open: the gds result of `69d0bf4`
    and the CI of the step-6 commit (the lead reads them from the web page).
  - **Review fix-ups** (an independent review of the round found no blocker; its "should fix" items and notes, and
    what was done):
    1. The canary self-test (CTEST) inverts both shadow samples at once, so no test showed that each half works
       alone: the reviewer's mutant `can_hit = cap_q & (pred ^ can_s[0])` (CONF half blind) passed everything. New
       cocotb test `canary_halves` (RTL only): with CTEST off it forces the shadow flop of one half (`u_s` output of
       `g_b[0]` PRED, then `g_b[1]` CONF; in the flip-flop model the matching bit of `rs`) to the opposite of its
       real flop at an acknowledged predict: FSTAT = CANARY and event 4 for one clock, for each half alone; nothing
       when neither is forced. The reviewer's mutant now fails it ("CONF half forced: FSTAT 0x0000"). On silicon
       the limit remains: CTEST exercises the pair, and a blind half would show only as "the canary never fires
       before the first wrong reply" (experiments.md, experiment 6).
    2. A soft reset (CMD bit 0) while OP = 3, CSn = 0, MOSI = 1 are on the pins (a real host produces this: the last
       bit of CMD 0x0001 is 1) must clear the tables and leave KEPT = 0: added to cocotb `strap_reset` (RTL and gate
       level). The reviewer's mutant `kept <= strap` fails it ("must leave KEPT = 0").
    3. The MASK staging rule (a CTRL[15] change invalidates the staging register) was caught only in concurrent
       random runs. New cocotb test `mask_staging` (RTL only): 24 rounds of a staged branch in flight, an optional
       flipped bit in its twin entry, TWIN_EN + SEC-DED + RESTORE, MASK switched, the branch resolved, the twin
       compared with the model. Removing the rule fails it in round 12 (twin entry 1 differs).
    4. Formal (inside `ifdef FORMAL` only): the cover the documents claimed is added, `wd_cnt == 12`, reached in
       step 22 (the assertion `wd_cnt <= 12` stays proven); a cover of the strapped reset's own clear executing
       with `clr_exec && !clr_tab` (reached in step 2), with a comment that the keeping of the contents is checked
       in simulation; the tautological assertion `clr_exec && !clr_tab -> kept && init_pend && !clrp_pend` is
       replaced by one with an independent tracker `f_sc` (set from the pins at a strapped pin reset, cleared by
       the first clear): `clr_tab == !(f_sc && !clrp_pend)` for every clear outside reset, and
       `f_sc == (kept && init_pend)`. `prove` (k-induction, 4 minutes), `cover` (15 of 15 reached) and `nowait`
       pass.
    5. `cosim/random.sh` rebuilt the Verilator model only when `cosim.cpp` changed and `cosim/run.sh` watched only
       two RTL files: both now rebuild when any `src/*.v` or `cosim/cosim.cpp` is newer than the binary.
    6. `host/bpx1.py` `TTPins.reset()` drove REQ = 0 without updating the host object's copy of the pins, so the
       next request could drive no edge and hang: it now keeps the REQ level through the reset (not run on
       hardware). docs/SPEC.md 4: the divided ring signal (event 8) reaches the DBG pin through one synchroniser
       flop and the event multiplexers, so single edges may be metastable; frequency counting only.
    7. The design is unchanged: `src/` of `69d0bf4` and of this commit synthesised with the same Yosys script
       (repository cell exclusions, `synth -top bpx_chip`, `write_verilog -noattr`) give byte-identical netlists;
       preprocessed without FORMAL, the sources differ only in blank lines.
    Runs after the fix-ups: cocotb latch cells / flip-flop model / repository top 13/13 each (`tb3.sh`; gate level
    not rerun here: CI's gl_test runs it); random 4 × 50,000 (`rnd2.sh`): plain 200,022 requests, 0 reply,
    statistics, state and pin-count mismatches, coverage cross 52/52, other 243/243; concurrent 200,023 requests,
    0 mismatches, 3,780 background checks, pin totals equal to the model.
  - `git diff --stat 69d0bf4` of the step-6 commit: `docs/`, `README.md`, `explore/`, and for the review fix-ups
    `test/test.py`, `src/bpx_core.v` (inside `ifdef FORMAL` only), `cosim/random.sh`, `cosim/run.sh`,
    `host/bpx1.py`; `info.yaml` and every flow setting unchanged. Open, the lead's step: merge to `main`, tag, CI
    of `main`; the rented machine's spend and destruction (HANDOVER 8).
