# BPX-1 silicon experiments: a guide for the FPGA team

> **Re-checked on 27 Nov 2026** against `docs/SPEC.md` and the "as built" notes of steps 3, 4 and 5 in
> `docs/mosaic.md` 7.2–7.7 (commit `f78ae8f`), and against the owners' later decisions (statistics offload, 33 ns).
> Registers and bits are named; positions in brackets. Figures of the final build are `TBD(final)`.

Status: written 26 Nov 2026, revised 27 Nov, before any silicon. Nothing here has been run on a chip or a board. Evidence labels:
**(VERIFIED)** = read in the named file of this repository; **(ESTIMATED)** = reasoning or expectation, basis given;
**(DON'T KNOW)** = open, with what settles it. Every expected silicon number (retention, failing pulse width,
oscillator frequency, guard band) is ESTIMATED or DON'T KNOW.

## 1. What all experiments share

- **Every quirk is off after reset.** FCTL resets to 0, also by soft reset; with FCTL = 0 the chip is the plain,
  verified design **(VERIFIED: SPEC 4, register FCTL; mosaic.md 7)**. Writing FCTL = 0 over 0 changes nothing.
- **Golden twin.** The FPGA runs a lock-step copy of the chip's RTL, fed the same pin inputs, and compares every
  reply (`ACK`, `PRED`, `CONF`, `FULL`, `READY`, `PBIM`, `PGSH`, `DBG0`, `DBG1`) and, at the end of a run, the
  statistics and a raw dump of every table. With FCTL = 0 any difference is a hardware fault
  **(VERIFIED: bringup.md 2, the model equals the RTL on 420 million requests)**.
- **Faults are reported twice**: as sticky flags in FSTAT (0x12; cleared by CMD bit 2 and by reset) and as one-clock
  events on `DBG0`/`DBG1` when FCTL.DBGF [14] = 1 and CTRL[7:4] / CTRL[11:8] select the fault event. The FPGA
  counts the events and time-stamps them with its own time base. The chip has no fault counters
  **(VERIFIED: mosaic.md 3 and 7.2)**.
- **Fault events** (with DBGF = 1): 0 TW_ERR, 1 TW_COR, 2 TW_DET, 3 TW_SDC, 4 CANARY, 5 any checker, 6 WDOG,
  7 DEMOTED, 8 divided oscillator, 9 the random bit of the last training before the last predict, 10 default
  prediction computed with W3 instead of the twin; 11–15 read 0 **(VERIFIED: SPEC 4, all built by step 5)**. Events
  0–3 are pulses with the ACK of the predict; event 4 is a pulse **one clock after** the ACK; 5 and 6 come one clock
  after they happen; 7, 9 and 10 are levels. Only two pins: pick two events per run.
- **Statistics offload.** The chip counts only resolved branches and the mispredictions of the answer it gave on
  PRED (STATS 0x05–0x08; 0x09–0x0E read 0). **The FPGA counts each predictor's mispredictions** from the four
  prediction pins (PRED or DBG0 default, PBIM, PGSH, DBG1 tournament, with the reset DBG selections, valid with ACK)
  and the outcomes it sends (owners' decision, 27 Nov). An experiment that puts fault events on the DBG pins loses
  the default's and the tournament's pins for that run: then count those two in the lock-step twin, or run twice.
- **Baseline.** Each number measured here has a pre-silicon prediction in `docs/fault_campaign.md`. The plain-chip
  numbers are in `docs/cosim.md` and `docs/cosim_dual.md`.
- **Host names.** Function names are those of `host/bpx1.py` (`set_fctl`, `fstat`, `dump`, `raw_write`,
  `twin_word`, `set_drive`, `stats`, `command`, `reset`). `set_fctl` has a keyword for every FCTL bit;
  `set_drive(mask=…)` sets CTRL[15] MASK; `reset(strap=True)` holds the strap pins **(VERIFIED: host/bpx1.py)**.
  `stats()` still reads five counters: it changes with the offload (TBD(final)).

**Safe state, used by every experiment to recover.** (1) `set_fctl()` (FCTL = 0) and `set_drive(…, mask=False)`
(CTRL[15] = 0); (2) `stats()` to save the counts, then `command(CMD_CLEAR_STATS)` (clears statistics, FSTAT, the
fault count and DEMOTED; KEPT stays); (3) `command(CMD_CLEAR_PRED)` with PULSE = 0 (a clear on a short pulse can
leave W0–W2 partly uncleared); (4) if the control port does not answer: pin reset **without the strap** (CSn high
during the reset is enough), then `sync()`. The strap is OP = 3 **and** CSn = 0 **and** MOSI = 1 at the last clock
edge of reset; OP = 3 alone (a request code the FPGA may leave on the pins) does not trigger it
**(VERIFIED: SPEC 5; mosaic.md 7.7 as built in step 5)**.

## 2. Overview

| # | Experiment | Fault class | Switch | What is measured | Compared with |
| --- | --- | --- | --- | --- | --- |
| 1 | Sanity, safe state | none (reference) | FCTL = 0 | replies, statistics, dumps against the twin | `docs/cosim.md`, `cosim_dual.md` |
| 2 | Retention of W3L | retention | TWIN_EN [0] | TW_ERR rate against clock, temperature, supply; weak-cell map; decay level | decay model, `fault_campaign.md` |
| 3 | Codes on the leaky memory | retention, masked or not | TWIN_USE [1], CODE [3:2], RESTORE [4], CTRL.MASK [15] | extra mispredictions of the default; TW_* counts per code | `fault_campaign.md` 4 and 7, per mode |
| 4 | Injection by raw writes | bit flips, every table | none (SPI raw writes) | extra mispredictions per fault per predictor | `fault_campaign.md` (direct counterpart) |
| 5 | Write-pulse width | write faults, W0–W2 | PULSE [7:5] | failing setting, per-bit failure map, accuracy | run 1; delays from the liberty file |
| 6 | Timing margin, canary | timing | clock (FPGA); CTEST [15] | period of first canary, of first wrong reply; guard band | static timing (`docs/timing.md`) |
| 7 | Graceful degradation | detected faults | DEMOTE [13] | hand-over to the tournament, speedup kept, return | run 1, tournament column |
| 8 | Control upsets | control state | always on | checker and WDOG flags and events | formal proofs: expected 0 |
| 9 | Learning on physical entropy | randomness | GRAND [8], GSRC [9] | accuracy of gshare, tournament, default; bias and correlation of the bit | run 1; pseudo-random source |
| 10 | Oscillators | process, ageing | ROA [10], ROB [11], ROSEL [12] | ring frequency, drift with supply, temperature, time | cell-library estimate |
| 11 | Power-up state, warm reset | power-up bias | strap (`OP = 3`, `CSn = 0`, `MOSI = 1` at the end of reset) | per-bit bias and stability; learning kept | none (new data) |

---

## 3. Experiment 1: sanity and the safe state

**Purpose.** Show that the plain chip equals the RTL, and record the reference every other experiment is compared
with: mispredictions of default, bimodal, gshare and tournament per benchmark program.

**Set-up.** Pin reset with `OP = 0` and `CSn` high; FCTL = 0 (read back 0); FSTAT = 0; CTRL as after reset
(`0x7DA0`: default driving, all features on, MASK 0, DBG0 = default, DBG1 = tournament). The FPGA drives `clk` and
`rst_n` with the board in `ASIC_MANUAL_INPUTS`; the microcontroller selects the project and holds the selection
(bringup.md 6). Clock 30.3 MHz (33 ns, signed off) and a low clock (1 MHz) for comparison.

**Procedure.**
1. `sync()`, `chip_id()` = `0xB101`, `command(CMD_CLEAR_PRED | CMD_CLEAR_STATS)`.
2. Dump every space (0, 1, 7, 8–11) and compare with the initial values (64 × 5, 128 × 1, weights 0, −1, 0, −1).
3. Run each benchmark program through the U1 CPU, once per adapter mode (one request per cycle; two per cycle,
   SPEC 9 rule 7), the twin comparing every reply. Read `stats(clear=True)` at least every 65,535 branches.
4. At the end of each program: `fstat()`, full dump, compare with the twin.
5. Repeat for `set_drive(1..4)` (each reference driving, static not-taken) on a subset of programs.
6. Repeat step 3 on two programs at the start and end of every bench day: the drift check.

**What to record.** Per program and mode: branches and driving mispredictions (chip), mispredictions of all four
predictors (FPGA, from the pins), CPU cycles (speedup over
static not-taken), number and first time of any reply difference, FSTAT, chip temperature and supply if available.

**How to read the result.** Expected: zero differences; mispredictions equal `docs/cosim.md` exactly for the same
mode, because the model and RTL agree there **(VERIFIED: cosim.md, mosaic.md 9 step 3 "FCTL = 0" runs)**;
`docs/cosim.md` for the mosaic's RTL: TBD(final). A difference at 30.3 MHz but not at 1 MHz is a timing fault (go
to experiment 6). A difference at every clock: dump and localise as in bringup.md 4. Any FSTAT bit set with FCTL = 0
(canary, checker, WDOG) is a finding in itself (section 10). With 4-bit weights the W1/W3 initial value −1 reads 15
**(VERIFIED: SPEC 7, weights −8…7)**.

## 4. Experiment 2: retention of the leaky table W3L

**Purpose.** Measure how long the floating nodes of W3L hold, as a rate of retention faults under use and as a map
per cell, and which level a node decays to.

**Set-up.** FCTL.TWIN_EN [0] = 1, TWIN_USE [1] = 0 (observe only: predictions unchanged), CODE [3:2] = 0, RESTORE
[4] = 0, DBGF [14] = 1, CTRL DBG0 = event 0 (TW_ERR), DBG1 = event 3 (TW_SDC) or event 10. The twin's raw space 12
reads 0 unless TWIN_EN = 1 **(VERIFIED: SPEC 4, RAW_ADDR and space 12)**. The FPGA's twin never leaks, so in this
experiment the comparator ignores FSTAT bits 0–3, the twin events and space 12; `PRED` and statistics must still
match, because with TWIN_USE = 0 the twin cannot change a prediction **(VERIFIED: mosaic.md 7.3)**.

**Procedure.** Part A, rate under use:
1. Run one benchmark program in a loop at chip clock f, counting TW_ERR events and predicts.
2. Lower f step by step (30, 16, 8, 4, 2, 1 MHz, then lower). The time between rewrites of an entry grows as f
   falls, because an entry is rewritten only when W3 trains.
3. If the board allows: repeat at two or more temperatures and supply voltages.

Part B, cell map (no requests running):
1. `set_drive(0, freeze=True)` so that no training rewrites the twin (MASK 0: raw space 12 shows the nodes as
   they are either way).
2. For each pattern P in {all 0, all 1, 0x55, 0xAA, each code word of `twin_word(w)`}: `raw_write(12, a, P,
   fields=1)` for a = 0…31.
3. Wait t, then `dump(12, 32)`. Sweep t over 10 µs … 1 s (log steps); repeat each point several times.
4. A bit that reads the opposite of what was written has decayed; the level it reads after a long wait is its
   decay level (polarity).

**What to record.** Part A: TW_ERR per 10⁶ predicts against f (and temperature, supply), with time stamps. Part B:
per bit, the first t at which it fails, for each written value; the level after the longest wait; repeatability.

**How to read the result.** Retention is not known: about 0.1 to 1 ms at room temperature **(ESTIMATED:
mosaic.md 5, leakage against a few fF)**. Expect TW_ERR to rise steeply once the rewrite interval of the least used
entries passes the retention time, and retention to fall about by half per 10 °C **(ESTIMATED: usual junction
leakage behaviour, not measured on SKY130)**. Surprising: no errors even at 1 s (nodes far less leaky, or held by
the read tree); errors at 30 MHz under use (retention far below the estimate); a decay level that differs between
bits of the same column (useful for experiment 11). The raw read fetches one entry ahead (SPEC 4, RAW_DATA), so
take t from the RAW_ADDR write, not from the read. Hazard: none to the chip; return with the safe state.

## 5. Experiment 3: protection codes on the leaking memory

**Purpose.** Measure what each protection does for a memory that really leaks: mispredictions of the default
predictor and the mix of corrected, detected and silent errors.

**Set-up.** TWIN_EN [0] = 1, TWIN_USE [1] = 1, DBGF [14] = 1. Sixteen configurations: CODE [3:2] ∈ {0 none,
1 parity → abstain, 2 SEC-DED, 3 detect all → abstain} × RESTORE [4] ∈ {0, 1} × MASK (CTRL[15]) ∈ {0, 1}. With
MASK = 1 the nodes hold `code word ^ 0xF0` (check bits inverted), so a fully decayed word abstains under SEC-DED and
detect-all instead of reading as weight 0 or −1. Clock low enough that experiment 2 showed a clear TW_ERR rate. A
change of TWIN_EN, CODE or MASK invalidates the staging register; this is part of the design, not a fault
**(VERIFIED: mosaic.md 7.2 "added rule", 7.7)**.

**Procedure.**
1. For each configuration: `set_drive(0, mask=m)`, `set_fctl(twin=True, use=True, code=c, restore=r, dbgf=True)`,
   `command(CMD_CLEAR_PRED | CMD_CLEAR_STATS)` (words written under the other MASK read as faulty until rewritten).
2. Run the programs of experiment 1. Count events in two passes (TW_ERR + TW_SDC, then TW_COR + TW_DET), or one
   pass with TW_SDC and event 10: a predict where `PRED` differs from event 10 is one the twin changed.
3. Read `stats()` and `fstat()` at the end.
4. Variant, FPGA-side scrubbing: with RESTORE = 0, the FPGA reads W3 (space 11) and the twin (space 12) entry by
   entry and rewrites every twin word that is not `twin_word(W3)`. Sweep the scrub period.
5. Repeat at two clock frequencies (one where retention errors are rare, one where they are common).

**What to record.** Per configuration and program: mispredictions of the default against experiment 1, counts of
TW_ERR, TW_COR, TW_DET, TW_SDC per 10⁶ predicts, scrub period and number of rewrites.

**How to read the result.** With TWIN_USE the statistics count the predictions actually given, so the extra
mispredictions are the twin's cost; training always uses W3, so the learned state is identical in all eight
configurations **(VERIFIED: mosaic.md 7.3)**. Expected, from the campaign's decay model: without RESTORE detect-all
costs least and no code most; SEC-DED pays with RESTORE (best from τ ≈ 1e3 branches); with one-directional decay and
no mask every code is blind to a fully decayed word, and MASK = 1 makes TW_SDC fall about 100× under detect-all
**(VERIFIED in the simulator: fault_campaign.md 4 and 7; ESTIMATED that silicon behaves so)**.
TW_SDC with SEC-DED or detect-all means three or more errors in one word (miscorrection sets TW_COR and TW_SDC)
**(VERIFIED: mosaic.md 7.3 as built)**; frequent TW_SDC there would mean errors are correlated within a word,
a surprising and important result. Scrubbing: one full sweep costs about 32 reads and up to 32 writes over SPI, of
the order of 1 ms at 30 MHz **(ESTIMATED: SCK ≤ f_clk / 8, 24 bits per access, plus waits in `raw_write`)**, similar
to the estimated retention: scrubbing may not keep up at room temperature. Lock-step: with one branch in flight
(`predict_resolve`) only `PRED`, `CONF` and the default's statistics can differ from the twin; with several in
flight the speculative history and the repair follow the changed predictions, so compare statistics only
**(ESTIMATED: from SPEC 3.2)**. Hazard: none; safe state.

## 6. Experiment 4: fault injection by raw writes

**Purpose.** The direct counterpart of the pre-silicon campaign: the same bit flips, injected into each predictor's
tables on silicon, and the extra mispredictions per fault per predictor.

**Set-up.** FCTL = 0 (or the configuration of experiment 3 for twin injection). Raw spaces 0 (bimodal + chooser,
two fields), 1 (gshare), 8–11 (W0–W3), 12 (twin), 7 entry 4 (θ and its counter). A raw write of W3 does not write
the twin and the reverse **(VERIFIED: SPEC 4)**. The SPI port reaches about one flip per 450 chip clocks
**(VERIFIED: mosaic.md 3, figure given there, not measured on hardware)**.

**Procedure.**
1. Choose a table, a rate (flips per 1,000 branches) and a multiplicity (1, 2, 3 bits per entry).
2. While a program runs: read the entry (`dump`), flip the chosen bits, `raw_write` it back.
3. Two ways to run the twin: (a) the twin gets the same SPI traffic: it must stay identical, which checks that the
   injection did what was meant; (b) the twin gets a raw write of the unchanged value at the same moment, so the
   timing is the same and only the data differs: the difference in mispredictions is the effect of the fault.
4. Repeat for every table, every rate, all programs of the campaign.

**What to record.** Per run: table, entry, bits, branch count at injection, mispredictions of all four predictors
(counted by the FPGA from the pins), and the difference to mode (b)'s twin or to experiment 1.

**How to read the result.** Mode (a) must show zero differences; a difference means the raw port or the table
misbehaves. Mode (b) gives extra mispredictions per fault; it should match `fault_campaign.md` within statistical
noise, since the campaign runs in a simulator validated against the golden model **(VERIFIED: fault_campaign.md 2,
66 of 66; ESTIMATED that silicon agrees)**. Read-flip-write is not atomic: if the entry trains between the read and the write,
the write also undoes that training step **(ESTIMATED from SPEC 4: reads and writes are separate SPI transactions)**;
mode (b) has the same race, so it cancels. A raw write may delay a coinciding request by up to two clocks
**(VERIFIED: SPEC 4)**. Hazard: injected state persists; `command(CMD_CLEAR_PRED)` at the end.

## 7. Experiment 5: write-pulse width (W0–W2 only)

**Purpose.** Find the shortest write pulse that still writes the static latches, and how accuracy degrades below it.

**Set-up.** FCTL.PULSE [7:5]: 0 standard (half a clock), 1–7 short, from taps after 1, 2, 3, 4, 6, 8, 12 delay cells
`dlygate4sd3_1` **(VERIFIED: mosaic.md 7.4, built in step 4)**. Only W0–W2 get the short pulse. **The
bimodal/chooser table, gshare, W3 and W3L always get the standard pulse; this is structural** (`u_wp` feeds only those,
`u_wpx` only W0–W2; checked in the step-4 netlist) **(VERIFIED, progress log, step 4)**. Pulse widths at `u_wpx` in
the step-4 netlist, settings 1–7 (ns): tt 0.73, 1.33, 1.98, 2.59, 3.74, 4.93, 7.17; ss 1.49 … 14.10; ff 0.47 … 4.93
(full table in timing.md); final build TBD(final). The latch needs at least 0.14 ns tt, 0.29 ns ss (liberty). The
setting is copied into the write sequencer only while it is idle, so an FCTL write never changes a pulse under way.

**Procedure.**
1. For PULSE = 1 … 7: `set_drive(0, freeze=True)`; write a pattern to W0–W2 with `raw_write` (all 0, all 1,
   checkerboard, a value per entry); read back with `dump`.
2. The first setting with any wrong bit is the failing width. Record every wrong bit.
3. Without freeze, run the benchmark programs at each setting; compare mispredictions with experiment 1.
4. Repeat at two clock frequencies and, if possible, temperatures and supplies.

**What to record.** Per setting: wrong bits (table, entry, bit, written value), the default's mispredictions.

**How to read the result.** The short pulse is `a & b & ~d`, so it is never longer than the standard half clock;
once half a clock exceeds the tap delay the setting should not depend on the clock **(ESTIMATED: from the gate
equation in mosaic.md 7.4)**. Expect writes to fail only at the shortest taps, if at all **(ESTIMATED: a latch needs
a few hundred ps; a tap is one delay cell or more; DON'T KNOW the real margin)**. Raw writes (spaces 8–10) and a clear
by CMD bit 1 **do** use the short pulse; the clear at the end of a reset always uses the standard one
**(VERIFIED: mosaic.md 7.7, answers to the owners' points)**. So the read-back test of step 1 measures the write path
directly. Set PULSE = 0 before `CMD_CLEAR_PRED`, or the clear may leave W0–W2 partly uncleared. Surprising:
failures in bimodal, gshare or W3 (impossible by construction: a wiring fault); failures that depend on the
written value (asymmetric latch). The FPGA twin has no pulse model, so it diverges once writes fail: compare
statistics. Hazard: wrong weights persist; set PULSE = 0 and `command(CMD_CLEAR_PRED)`.

## 8. Experiment 6: timing margin and the canary

**Purpose.** Measure how much margin the chip has, and whether the canary warns before the first wrong reply.

**Set-up.** The FPGA drives `clk` and can shorten the period, or single cycles. There is a canary on `PRED` and one
on `CONF`: a shadow flop takes the output flop's D through two `dlygate4sd3_1` (step 5; about 1.1 ns typical, 2.2 ns
at the slow corner **(ESTIMATED: liberty delays; measured lead TBD(final))**). The comparison is made in the clock
after PRED and CONF were loaded, so fault event 4 is a pulse **one clock after the ACK**
**(VERIFIED: mosaic.md 7.5 as built)**. FSTAT.CANARY [4]. Runs with FCTL = 0 and with TWIN_EN [0] = 1 (the twin
path is a true path only then).

**Procedure.**
1. CTEST [15] = 1: the shadow sample is inverted, so the canary must fire at the first acknowledged predict. Check
   FSTAT.CANARY and event 4, then CTEST = 0 and `command(CMD_CLEAR_STATS)`.
2. Run a program with the twin comparing. Shorten the period step by step (for example 250 ps steps) from 33 ns.
3. At each step: count canary events and reply differences, read `fstat()`, dump at the end.
4. Variant: keep a long period and shorten single cycles (one per request, then the predict cycle only).
5. Variant: shorten only the low phase and keep the high phase, so the half-clock write pulse keeps its width and
   only the logic paths are stressed.
6. Repeat at temperature and supply corners if the board allows.

**What to record.** Period of the first canary event, period of the first reply difference (which pin, which
predictor), guard band = difference of the two; FSTAT at each step; ring frequency (experiment 10) as process and
temperature reference.

**How to read the result.** Slowest path at sign-off at 33 ns: TBD(final) (step 4 at 30 ns: CONF +2.46 ns, PRED
+3.54 ns at the slow corner; typical +13.87 ns) **(VERIFIED, progress log, step 4)**. Input pads run to 66 MHz
**(VERIFIED by a research agent: bringup.md 6)**, so typical or fast silicon may not fail below the pad limit: then
use single short cycles or a lower supply **(ESTIMATED)**. The canaries watch PRED and CONF only; the next real
endpoints (DBG flops, speculative history) had +3.99 to +4.47 ns in step 3, so a path that fails without a canary
is possible but less likely than before **(ESTIMATED: slacks in mosaic.md 9)**. A negative guard band (a wrong
reply before the canary) is still the main negative result to watch for; the lead is about 1 ns typical
**(ESTIMATED; TBD(final))**, and step 4 showed that placement moves the CONF path by more than 1 ns between builds.
When the clock is shortened with a 50 % duty cycle, the write pulse shortens too: write faults may then appear in
every table (compare with experiment 5). Checker flags may fire here legitimately (timing faults, not upsets): note
them. Hazard: wrong state persists; go back to 33 ns, safe state, `command(CMD_CLEAR_PRED)`; the twin needs a reset
in the same cycle.

## 9. Experiment 7: graceful degradation

**Purpose.** Show that after detected faults the chip hands the driving role to the tournament, keeps most of the
speedup, and returns after clearing.

**Set-up.** FCTL.DEMOTE [13] = 1; CTRL drives the default. A 2-bit saturating count of detected faults (TW_DET of a
predict, CANARY, any checker, WDOG; faults in the same clock count once) sets DEMOTED, FSTAT [10] (live), on the
fourth with DEMOTE = 1; the tournament then drives whenever CTRL selects the default **and** DEMOTE = 1: DEMOTE = 0
suspends the substitution (DEMOTED and the count stay), so FCTL = 0 is always the plain chip. Cleared by CMD bit 2,
soft reset and reset **(VERIFIED: mosaic.md 7.6 as built in step 4)**. Event 7 = DEMOTED, a level: with the ACK of a
predict it shows the state before that predict, one clock later including that predict's own TW_DET.

**Procedure.**
1. Run a program; snapshot `stats()` (CMD bit 2 also clears the statistics).
2. Provoke detected faults, one way per run: (a) CTEST = 1 for four predicts; (b) TWIN_EN = 1 and CODE = 3
   (any error abstains) or CODE = 1 with single-bit errors (parity misses two), injected into twin words of active
   branches with `raw_write(12, …)`; (c) a shortened clock.
3. Watch event 7: record the time and request count at demotion. Continue the program.
4. `command(CMD_CLEAR_STATS)`; check DEMOTED = 0 and the default drives again.

**What to record.** Time of each detected fault and of demotion; `PRED` against the tournament's prediction
(the twin, or `DBG1` if it is not used for event 7) after demotion; speedup before, during and after;
mispredictions per predictor.

**How to read the result.** After demotion `PRED` must equal the tournament's prediction on every predict. Speedup
kept: the tournament's share of experiment 1 (on the benchmark set the default is ahead of the tournament)
**(ESTIMATED: cosim.md columns)**. With (a) and (b) the faults go through pins and the SPI, and the golden model has
the canary self-test and demotion (verified bit-exact in step 4), so the twin demotes at the same request and stays
in lock-step **(VERIFIED: mosaic.md 9 step 4; ESTIMATED for the FPGA twin)**. TW_DET counts towards demotion with
TWIN_USE = 0 too: in observe mode a detected twin error still demotes **(VERIFIED: mosaic.md 7.7, answer (b))**.
Surprising: demotion without four detected faults; no demotion after four; the default driving again before the
clear while DEMOTE = 1. Hazard: CTEST left on demotes after four predicts; turn it off before clearing.

## 10. Experiment 8: control upsets (checkers and watchdog)

**Purpose.** Catch upsets of the protected core (sequencer, queue, handshake) that would otherwise be invisible.

**Set-up.** Always observing; no switch. Four invariant checkers **(VERIFIED: mosaic.md 7.5 as built in step 4)**:
FSTAT[5] queue (count above 4, or FULL ≠ count = 4); [6] staging (staging valid or a resolve pending with an empty
queue, or resolve-first done with no request seen); [7] write sequencer (two states at once, or a table write
requested while busy); [8] handshake (ACK toggled without answering a request, or a request answered without a
toggle). FSTAT.WDOG [9]: the clocks a request is pending are counted, restarting at every answer and at every
control-port write; at 64 WDOG is set, and event 6 fires every 64 clocks while a hang lasts. Events 5 (any checker)
and 6 (WDOG).

**Procedure.**
1. In every run of experiments 1–11: read `fstat()` at the end and every few seconds; if a spare DBG pin exists,
   count events 5 and 6.
2. On a flag: record time, STATUS, ERRCNT, the last 100 requests and replies, a full dump, the twin's state.
3. Beam or glitch campaign (only if one is ever done): DBG0 = event 5, DBG1 = event 6 throughout; every reply
   compared; time stamps of flags and of the first reply difference; after a WDOG or a hang, a soft reset by SPI,
   and a pin reset if the SPI does not answer.

**What to record.** Every flag with time, run, clock, temperature, beam fluence if any; whether a reply difference
came with it, before it or without it.

**How to read the result.** The checker properties are proven for the RTL, so with a correct chip at a safe clock
a checker flag is impossible; a flag in normal operation is itself a finding (a physical upset, a timing fault, or
a gap between the RTL and the silicon) **(VERIFIED: mosaic.md 9 step 4, formal proves the flags stay 0)**. The
proven liveness bound: a request pending without a control-port write is answered by its 13th clock (count ≤ 12,
k-induction with free inputs); the watchdog trips at 64, five times that **(VERIFIED: mosaic.md 7.5 as built)**. SPI
words are at least 128 clocks apart, so a real hang still reaches 64. A reply difference without a flag shows what
the checkers miss. Hazard: the watchdog only flags;
recovery is the FPGA's soft or pin reset.

## 11. Experiment 9: learning on physical entropy

**Purpose.** Compare gshare with probabilistic hysteresis driven by a pseudo-random register against the same
driven by oscillator jitter.

**Set-up.** FCTL.GRAND [8] = 1: a saturated gshare counter moves away only if the random bit is 1. GSRC [9] = 0:
7-bit maximal-length register (x^7 + x^6 + 1, 0x55 after reset and every clear, one step per training whatever
FCTL says; the bit is its bit 6); GSRC = 1: the selected oscillator's divider LSB sampled through two flops; ROSEL
[12] chooses the ring, ROA [10] / ROB [11] must run it **(VERIFIED: mosaic.md 7.7 as built in step 5)**.

**Procedure.**
1. Three configurations: GRAND = 0 (experiment 1); GRAND = 1, GSRC = 0; GRAND = 1, GSRC = 1 with ring A, then B.
2. Run the benchmark programs; `stats()` per program.
3. Entropy bit: DBGF = 1, a DBG pin on event 9; record the bit stream at each training (long runs, 10⁶ bits).
4. Also run with `set_drive(0, features=6)` (no reference inputs), so the default does not use gshare.

**What to record.** Mispredictions of gshare, tournament and default against experiment 1; for the bit: fraction
of ones, autocorrelation at lags 1–64, runs; the ring frequency and the chip clock.

**How to read the result.** GSRC = 0 is exact in the model **(VERIFIED: mosaic.md 6)**, so the twin stays in
lock-step. With GSRC = 1 the model's bit is constant 0 (VERIFIED, same place): gshare, the tournament and, with
reference inputs on, the default diverge from the twin; compare statistics only, or use step 4 to keep the default
comparable. A real gain or loss is expected to be small, since the bit acts only at saturated counters
**(ESTIMATED)**. The bit samples a ring of about 125 MHz with a 30 MHz clock, so beat patterns and strong
autocorrelation are possible **(ESTIMATED from the frequency ratio; the 125 MHz of the simulation is a model
setting, not a prediction)**: a biased or correlated bit is a negative result about the source, not about the
learner. Event 9 carries the bit's **value**: a level, the bit (of the source GSRC selects, whatever GRAND says) used
at the last training before the last executed predict; it changes only with the ACK of a predict. With one branch
per request the FPGA reads one bit per branch, one request late **(VERIFIED: mosaic.md 7.7 as built; SPEC 4)**.
Hazard: GSRC = 1 with the selected ring stopped gives a constant bit
and, if it is 0, gshare counters never leave saturation **(ESTIMATED)**; safe state restores.

## 12. Experiment 10: oscillators

**Purpose.** Measure process speed, supply and temperature dependence, and ageing; give the other experiments an
on-chip thermometer and voltage sensor.

**Set-up.** `src/bpx_ro.v`: two identical rings (the probe's ring A twice: nand2 + 4 × (a222oi, xor2, a22oi, nand3)
+ xnor2, 18 named cells, 137.6 µm² each, protected from the resizer), a ÷256 ripple divider on the selected ring
(reset while that ring is stopped), its bit 7 through one synchroniser flop to fault event 8 **(VERIFIED: mosaic.md
7.7 as built in step 5)**. Switches ROA [10], ROB [11], ROSEL [12]; DBGF = 1, one DBG pin on event 8. The FPGA
counts edges against its own time base.

**Procedure.**
1. Process speed: ring A alone, then B alone; frequency = 256 × the DBG frequency, gate time ≥ 1 s.
2. Supply and temperature: repeat step 1 over the corners the board allows.
3. Ageing: ROA = 1 continuously for days; ROB = 1 only for short measurement windows (for example 10 s per hour);
   measure A and B one after the other in each window; log the ratio f_B / f_A.
4. Thermometer and voltage sensor: calibrate from step 2; during other experiments read ring B briefly.

**What to record.** f_A, f_B with time stamps, chip clock, temperature, supply; every reset (a reset clears FCTL and
stops ring A).

**How to read the result.** Expected ring frequency about 125 MHz typical, 65 MHz slow, 220 MHz fast, so the DBG pin
shows about 490 kHz, 250 kHz, 860 kHz **(ESTIMATED from the cell library, task brief)**. The DBG output is a flop on
the chip clock, so each edge has one chip-clock period of jitter; average over many periods **(ESTIMATED: DBG events
are flopped, SPEC 6.4)**. The ratio of the two rings cancels temperature and supply to first order, so ageing shows
as a slow drift of f_B / f_A; its size over days is **(DON'T KNOW: no ageing data for these cells here)**. Ring
speed should track the maximum clock of experiment 6: a poor correlation means the rings do not represent the
critical path. A running ring adds heat and supply noise: note it during experiments 2, 3 and 6. Hazard: none; the
safe state stops both rings.

## 13. Experiment 11: power-up state and warm reset

**Purpose.** Read the latches' power-up state (bias and stability per bit: a physical fingerprint), the same for the
floating nodes, and show a warm reset that keeps what was learned.

**Set-up.** Strap: if `OP = 3` (`uio_in[3:2]`), `CSn = 0` (`uio_in[4]`) and `MOSI = 1` (`uio_in[6]`) at the last
clock edge of reset, the tables are not cleared, all else resets (histories, threshold, LFSR, queue, statistics,
CTRL, FCTL, FSTAT), and FSTAT.KEPT [11] (live) is set until the next reset without the strap (a soft reset
included; CMD bit 2 leaves it) **(VERIFIED: mosaic.md 7.7 as built in step 5; SPEC 5)**. The FPGA keeps `SCK` low
throughout, holds the three pins from 4 clocks before `rst_n` rises to 4 clocks after, then takes `CSn` high for at
least 4 clocks before its first SPI word (`h.reset(strap=True)` does this). Bits: BC 256, gshare 256, W0–W3 512
(latches), W3L 256 (floating nodes). Space 12 needs TWIN_EN = 1 to read.

**Procedure.**
1. Power off for a fixed time; power on; the FPGA holds the strap pins (`OP = 3`, `CSn = 0`, `MOSI = 1`, `SCK` low)
   from power-up through every reset until READY.
2. Check KEPT = 1. `set_fctl(twin=True)`. Dump spaces 0, 1, 8–11 and 12; note the time since power-up.
3. Repeat 1–2 for N power cycles (100 or more), with the same off time; then vary the off time and temperature.
4. Warm reset: train on a program; pin reset with the strap; check KEPT; continue the same program; compare
   mispredictions with a cold start (`CMD_CLEAR_PRED`) and with the twin given the same reset.

**What to record.** Every dump with time stamps; per bit: probability of 1, stability (share of cycles equal to the
majority value); for W3L the read time after power-up; warm-reset mispredictions against cold start.

**How to read the result.** Static latches should show a stable, cell-specific bias usable as a fingerprint;
floating nodes should read their decay level of experiment 2 rather than a power-up value, because their read
comes milliseconds after power-up **(ESTIMATED: retention estimate in mosaic.md 5 against the selection time)**.
The histories reset with the warm reset, so the first predictions after it differ from an uninterrupted run
**(VERIFIED: "all else resets", mosaic.md 7.7)**. Surprising: KEPT = 0 (the strap missed: check `OP`, `CSn` and
`MOSI` at the release of the synchronised reset); all-equal dumps (a clear did happen). Whether the board can
power-cycle the chip alone, and whether the firmware's project selection pulses reset without the strap first, is
**(DON'T KNOW: board schematic and firmware)**. Hazard: the FPGA must not drive the chip's pins while it is
unpowered **(ESTIMATED: 3.3 V I/O, bringup.md 6)**; after the run, reset without the strap (`CSn` high).

---

## 14. What each experiment needs from the FPGA

| # | Clock control | Lock-step twin | Event counters with time stamps | Raw-access injector | Frequency counter |
| --- | --- | --- | --- | --- | --- |
| 1 | fixed (33 ns, 1 MHz) | yes, every reply | for flags (expected none) | dumps only | – |
| 2 | sweep down to low kHz | yes, twin events masked | TW_ERR, TW_SDC | pattern writes, timed reads | ring as thermometer |
| 3 | two fixed points | yes, statistics | TW_ERR, TW_COR, TW_DET, TW_SDC, event 10 | scrubber | – |
| 4 | fixed | yes, modes (a) and (b) | – | yes, at chosen rates | – |
| 5 | two fixed points | statistics only | – | patterns and read-back | – |
| 6 | fine period steps, single short cycles, duty control | yes, every reply | CANARY, checkers | dumps | ring speed |
| 7 | fixed (or shortened) | yes | DEMOTED and its causes | twin injection | – |
| 8 | any | yes | checkers, WDOG | dumps | – |
| 9 | fixed | GSRC = 0 only | random bit stream | – | ring speed |
| 10 | any steady clock | – | – | – | yes, ≥ 1 s gate |
| 11 | reset with the strap pins held; power cycling | yes, warm reset | – | dumps | – |

In every experiment the FPGA also counts each predictor's mispredictions from the prediction pins and the outcomes
(statistics offload, section 1); the chip's own counters give only branches and driving mispredictions.

## 15. Open questions for the FPGA team

1. Can the FPGA set the chip clock in steps of 250 ps or finer, change single cycles and the duty cycle, and how
   much jitter does that clock have at the pad?
2. Can the board's supply to the chip be varied, and its temperature controlled or at least measured?
3. Can the chip be power-cycled alone, with the FPGA's outputs tristated, and does the firmware's project selection
   reset the chip without the strap before the FPGA takes over (experiment 11)?
4. Does the lock-step twin take the SPI traffic as pin inputs (needed for injection mode (a)), and can it receive a
   different raw-write value at the same moment (mode (b))?
5. Can the twin follow the chip's random bit (event 9 carries its value, one predict late) to stay in lock-step
   with GSRC = 1?

6. Which header pins carry `clk` and `rst_n`, and are there series resistors (bringup.md 6, still open)?
7. Time base and resolution of the event counters: is one FPGA clock per time stamp enough to order events that
   fall in the same chip clock?
