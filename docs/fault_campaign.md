# BPX-1 pre-silicon fault-injection campaign

Status: done, 26 Nov 2026, branch `campaign`; coset-mask follow-up in section 7 (27 Nov), whose result is in the chip
since mosaic step 5 (switchable mask CTRL[15], `8'hF0`). The baseline that the silicon measurements are compared against:
predicted extra mispredictions per fault, per predictor and per protection mode. Every number below was run and
read in this campaign **(VERIFIED)** unless marked otherwise; it ran on the owner's PC (61 and 51 minutes of wall
time). Raw results: `~/work/bpx1_fi/out/*.tsv` (WSL); aggregated tables and logs: `explore/fault_results/` (from
branch `campaign`, merged into `mosaic`). Runner: `explore/fault_campaign.py`; validation:
`explore/fault_validate.py`; injection, twin and decay model: `explore/bpsim.cc` (kind `gehlr`, options listed
above `struct GehlRP`).

## 1. Method and assumptions

- **Simulator:** `explore/bpsim`, kind `gehlr` with the chip's configuration (`cosim/bpsim.cfg`, line `def`: 4 × 32 ×
  4-bit weights, histories 0/8/9/20, path history 8, reference weights 2/4/4, θ 10 adaptive, negation on), 8-bit
  identifiers, all 71 programs; aggregates over the usual 69 (`embench.nbody` and `embench.cubic` left out). One run
  gives all four predictors of the chip from one state, as on the chip: the default perceptron (driving) and the
  bimodal, gshare and tournament predictions read from the bimodal (64 × 2), chooser (64 × 2) and gshare (128 × 2)
  tables that also feed the perceptron's sum.
- **Timing:** the tables give the **CPU model** (U1 CPU contract, training at resolve; the model behind the project's
  accuracy figures). The one-in-flight numbers are in the TSV files (Part A: they agree with the CPU model within
  the intervals); they are the ones validated against the golden model (section 2).
- **Time unit: resolved branches.** A fault "at branch k" is applied just before the resolve of branch k.
- **Part A (random bit flips):** one uniformly random bit of the target table is inverted; faults arrive as a
  Poisson process in resolved branches at rate λ per branch **per table** (not per bit), independent of the program.
  Rates 1e-5, 1e-4, 1e-3 (10 seeds), 3e-3 (5 seeds), 1e-2 (3 seeds; 3 seeds elsewhere). Metric per program:
  Σ_seeds (mispredictions with faults − fault-free mispredictions) / Σ_seeds faults; aggregate = mean over the 69
  programs; 95 % interval = percentile bootstrap over programs and, inside each, over seeds (1,000 resamples).
- **Part B (decay of the leaky twin W3L) — assumptions, the real retention is unknown (estimated 0.1 to 1 ms):**
  - every stored bit, from its last write, keeps its value for a lifetime L and then reads as the cell's
    **preferred level**; a write (W3 trains, restore-on-use, clear) recharges all 8 bits of the word;
  - preferred level all 0, all 1, or **mixed** (each cell its own level, drawn once: the same "chip" for every
    program);
  - L **exponential** with mean τ, drawn again at every write (memoryless), or **per-cell fixed** from a log-normal
    with median τ and σ = 0.5 (each cell decays exactly L after every write);
  - a read at time t sees the decayed level if t ≥ t_write + ⌈L⌉; reads do not disturb; no other faults;
    temperature and voltage constant.
  - Modes: CODE 0–3 × RESTORE off/on, TWIN_EN = TWIN_USE = 1; τ = 1e2, 3e2, 1e3, 3e3, 1e4, 3e4, 1e5, 1e6
    branches; 3 seeds; metric: extra mispredictions of the default per 1000 branches against the fault-free run.
- **Shadow evaluation:** the twin never changes training (the chip trains from the static W3), so the learned state
  is the same in every mode; each run carries all 8 modes and 8 values of τ, each mode with its own storage
  (three physical copies suffice: RESTORE off is one storage for every CODE; RESTORE on writes the word as read for
  CODE 0, 1, 3 and the corrected word for CODE 2), and counts each mode's twin-using prediction beside the driving
  one. Exact one in flight; in the CPU model it leaves out the few cycles a twin-induced misprediction adds to the
  CPU's timing (measured below: < 0.003 per 1000 branches).

### Time: resolved branches to seconds

τ_s = τ_br × c / f, with c clocks per resolved branch (at least 3: one PREDICT+RESOLVE request per branch, answered
in 3 clocks) and f the chip clock. In an experiment the host sets the branch rate, so c is usually larger: what
counts is the time between resolves as the FPGA measures it.

| τ (branches) | 30.3 MHz (33 ns), 3 clk | 10 MHz, 3 clk | 1 MHz, 3 clk | host: one branch per 5 µs |
| --- | --- | --- | --- | --- |
| 1e2 | 10 µs | 30 µs | 0.3 ms | 0.5 ms |
| 1e3 | 0.1 ms | 0.3 ms | 3 ms | 5 ms |
| 1e4 | 1 ms | 3 ms | 30 ms | 50 ms |
| 1e5 | 10 ms | 30 ms | 0.3 s | 0.5 s |
| 1e6 | 0.1 s | 0.3 s | 3 s | 5 s |

The estimated retention of 0.1–1 ms is τ ≈ 1e3–1e4 branches at 30.3 MHz and full speed, τ ≈ 20–200 branches for
a host resolving one branch every 5 µs **(ESTIMATED: from the retention estimate of docs/mosaic.md 5)**.

## 2. Validation of the injection (done before any number was used)

- **Against the golden model `model/bpx1.py`** (`fault_validate.py`, log `explore/fault_results/validate.log`):
  first 30,000 branches of `embench.nsichneu` and `team.coremarkish`, one in flight; the model runs PREDICT, the raw
  writes of that branch (`raw_write`), then RESOLVE. Compared exactly: misprediction counts of all four predictors
  (model statistics widened to 32 bits) and the counts of TW_ERR, TW_COR, TW_DET, TW_SDC (FSTAT bits set per
  predict). Cases: 402 flips over bimodal, chooser, gshare, W0–W3 (FCTL = 0); 305 twin flips including double and
  triple errors in one word, under every CODE × RESTORE; decay (scenarios e300pm, l1000p0, e3000p1), where bpsim
  logs every decay that takes effect and the model applies it as a raw write of space 12, under every CODE × RESTORE.
  **66 of 66 comparisons equal (VERIFIED).** Mutation: with every model event one branch late, 58 of 66 differ (the
  other 8 have no effective event). Every case could be matched: "before the resolve of k" is a raw write between
  the PREDICT and the RESOLVE of k, which the chip can do over SPI too. Not compared with the model: the CPU timing
  (the model has no CPU); it runs the same injection code on the same training sequence.
- **Sanity checks** (`fault_campaign.py checks`, log `checks.log`), **all VERIFIED:**
  - zero faults: **90.9322 %** mean accuracy of the default over the 69 programs (CPU model; 90.9376 % one in
    flight), weights 2/4/4, θ 10; stand-alone bimodal / gshare / tournament 86.4660 / 87.2862 / 88.9192 %;
  - one in flight, the four predictions from the chip's one state equal the four stand-alone predictors exactly on
    all 71 programs; in the CPU model the observed reference predictors train at the driving default's timing and
    differ slightly from stand-alone (86.4541 / 87.2860 / 88.9190 %); faulty and fault-free runs are compared like
    with like;
  - twin on in each of the 8 modes (driving), no decay: all counts equal the plain run, all flags 0, 71 programs;
    TWIN_USE off with decay (three scenarios): all counts equal the plain run;
  - twin driving (decay e1e3pm, 8 modes × 71 programs) against the shadow count of a run the static default drives:
    one in flight equal (mispredictions, flags, other predictors); CPU model −0.0025 to 0.0000 per 1000 branches
    against effects of +6.2 to +11.4: the shadow evaluation is good to < 0.05 % of the effect;
  - fault counts against λ × branches (W0, all 71 programs, all seeds): 1e-5: 1,479 / 1,418.5 (+1.6 sd); 1e-4:
    14,128 / 14,184.6 (−0.5 sd); 1e-3: 142,117 / 141,845.6 (+0.7 sd); 1e-2: 1,418,275 / 1,418,455.9 (−0.2 sd);
  - reproducible: the pilot (28 configurations × 6 programs) rerun gives a byte-identical TSV.

## 3. Part A: random bit flips in the tables

Extra mispredictions per fault, CPU model, **rate 1e-3, 10 seeds**, mean over 69 programs [95 % interval]
**(VERIFIED)**. "0" = no path from that table to that predictor: exactly 0 one in flight, |x| < 0.001 in the CPU
model (timing coupling through the driving default).

| Target (bits) | default (perceptron) | bimodal | gshare | tournament |
| --- | --- | --- | --- | --- |
| bimodal (128) | 0.084 [−0.002, 0.195] | −0.027 [−0.62, 0.41] | 0 | 0.114 [−0.031, 0.249] |
| chooser (128) | 0.000 [−0.078, 0.081] | 0 | 0 | 0.135 [0.027, 0.259] |
| gshare (256) | 0.010 [−0.128, 0.133] | 0 | **0.288** [0.139, 0.468] | −0.154 [−0.500, 0.096] |
| W0 (128) | **0.228** [0.138, 0.342] | 0 | 0 | 0 |
| W1 (128) | 0.175 [0.021, 0.308] | 0 | 0 | 0 |
| W2 (128) | 0.195 [0.062, 0.331] | 0 | 0 | 0 |
| W3 (128) | 0.215 [0.063, 0.371] | 0 | 0 | 0 |

Per-fault effect against the rate (own predictor; CPU model):

| Target → predictor | 1e-5 | 1e-4 | 1e-3 | 3e-3 | 1e-2 |
| --- | --- | --- | --- | --- | --- |
| W0 → default | −0.84 [−4.7, 2.0] | 0.28 [−0.54, 1.28] | 0.228 [0.138, 0.342] | 0.165 [0.107, 0.225] | 0.130 [0.095, 0.168] |
| W3 → default | −0.45 [−4.3, 2.6] | 0.26 [−0.44, 1.13] | 0.215 [0.063, 0.371] | 0.179 [0.108, 0.250] | 0.146 [0.107, 0.193] |
| gshare → gshare | 1.76 [0.15, 6.4] | 0.21 [−0.08, 0.45] | 0.288 [0.139, 0.468] | 0.261 [0.187, 0.362] | 0.202 [0.172, 0.235] |
| bimodal → bimodal | 0.05 [−0.62, 0.71] | −1.79 [−6.5, 1.6] | −0.03 [−0.62, 0.41] | 0.130 [−0.08, 0.32] | 0.120 [0.055, 0.179] |
| chooser → tournament | −0.81 [−3.9, 0.15] | 0.27 [−0.56, 1.28] | 0.135 [0.027, 0.259] | 0.101 [0.059, 0.146] | 0.072 [0.054, 0.094] |

Per-program spread at 1e-3 (10th percentile / median / 90th percentile of the per-program value): W0 → default
0.008 / 0.126 / 0.479; gshare → gshare 0.050 / 0.226 / 0.474; bimodal → bimodal −0.077 / 0.150 / 0.471 with one
program at −13.6 (flips there mostly help); all rows in `partA.tsv`.

Fault lifetime (rate 1e-5, all programs and seeds; a fault-free predictor runs in lockstep; `partA_lifetime.tsv`):

| Target | flipped entry back at its fault-free value: share, mean branches | whole state back: share |
| --- | --- | --- |
| bimodal | 56 %, 35,000 | 6.0 % |
| chooser | 45 %, 59,000 | 5.7 % |
| gshare | 64 %, 30,000 | 5.4 % |
| W0 / W1 / W2 / W3 | 63 / 84 / 85 / 88 %, 34,000 / 20,000 / 23,000 / 14,000 | 0.3–0.8 % |

**What the numbers say.**
- **Most sensitive per fault: the gshare table for gshare (0.29) and the weight tables for the default (0.18–0.23,
  equal within the intervals; W0, the bias table, highest).** Per bit, the weight tables are the most sensitive part
  of the chip: 128 bits each against 256 for gshare. The default predictor is robust to faults in the shared
  reference tables (bimodal 0.08, chooser 0.00, gshare 0.01 per fault): they enter its sum with small weights.
  The tournament absorbs gshare faults (its interval includes 0; presumably the chooser steers away from a damaged
  gshare entry, ESTIMATED).
- **Linear regime.** The per-fault effect falls by 30–45 % from 1e-3 to 1e-2 for every table (faults interact:
  at 1e-2 a weight entry is hit every 3,200 branches, inside the 14,000–34,000-branch time it needs to return to its
  fault-free value). 3e-3 is already 10–30 % below 1e-3. Rates ≤ 1e-3 are consistent with one constant per-fault
  value, but 1e-4 and 1e-5 cannot test it: their intervals (±0.5 to ±10 per fault) are set by a heavy-tailed
  per-fault effect, not by the number of faults. **Use the 1e-3 values as the linear-regime baseline (ESTIMATED as
  linear: consistent with 1e-4, not resolvable below).**
- **Faults do not heal.** A single flip almost never leaves the predictor on its fault-free trajectory again
  (whole state back in ≤ 6 % of faults before the end of the trace): the perceptron's state is chaotic in this
  sense, and one flip shifts later training, the adaptive threshold and the counters' phase. 12–55 % of flipped
  entries never return to their fault-free value (many are entries no branch of the program uses). The cost
  per fault is therefore a long-run average, and its sign per program is mixed.

## 4. Part B: decay of the leaky twin W3L

Exponential lifetime, mixed preferred level, TWIN_USE on. Extra mispredictions of the default per 1000 branches
[95 % interval]; events per 1000 branches; extra mispredictions per faulty word read (pooled) **(VERIFIED)**.
All τ, all six decay families and paired differences between modes: `partB.tsv`, `partB_paired.tsv`, `report.md`.

| τ | CODE | RESTORE | extra / 1000 br | TW_ERR / COR / DET / SDC per 1000 br | extra / faulty read |
| --- | --- | --- | --- | --- | --- |
| 1e2 | 0 none | off / on | 20.11 [12.13, 30.55] / 20.12 | 837 / 0 / 0 / 709 | 0.024 |
| 1e2 | 1 parity | off / on | 14.65 [8.25, 23.00] / 14.66 | 837 / 0 / 433 / 361 | 0.017 |
| 1e2 | 2 SEC-DED | off | 12.49 [7.49, 18.17] | 837 / 433 / 370 / 315 | 0.015 |
| 1e2 | 2 SEC-DED | on | 12.09 [7.26, 17.48] | 701 / 186 / 290 / 311 | 0.017 |
| 1e2 | 3 detect all | off / on | 8.70 [4.46, 14.40] / 8.71 | 837 / 0 / 803 / 33 | 0.010 |
| 1e3 | 0 | off / on | 11.25 [6.29, 17.51] / 11.24 | 514 / 0 / 0 / 399 | 0.022 |
| 1e3 | 1 | off / on | 8.90 [4.48, 14.40] / 8.86 | 514 / 0 / 304 / 188 | 0.017 |
| 1e3 | 2 | off | 8.29 [4.25, 13.34] | 514 / 304 / 194 / 148 | 0.015 |
| 1e3 | 2 | on | 6.23 [2.92, 10.36] | 264 / 63 / 90 / 118 | 0.023 |
| 1e3 | 3 | off / on | 6.54 [3.14, 11.49] / 6.52 | 514 / 0 / 497 / 16 | 0.012 |
| 1e4 | 0 | off / on | 7.32 [3.07, 12.65] / 7.27 | 254 / 0 / 0 / 195 | 0.029 |
| 1e4 | 1 | off / on | 5.05 [2.38, 8.35] / 5.04 | 254 / 0 / 162 / 83 | 0.020 |
| 1e4 | 2 | off | 4.98 [1.49, 9.58] | 254 / 162 / 84 / 78 | 0.018 |
| 1e4 | 2 | on | 2.67 [0.52, 5.69] | 63 / 10 / 13 / 41 | 0.047 |
| 1e4 | 3 | off / on | 3.54 [1.16, 7.13] / 3.50 | 254 / 0 / 246 / 8 | 0.012 |
| 1e5 | 0 | off / on | 6.10 [1.69, 12.29] / 5.36 | 124 / 0 / 0 / 97 | 0.049 |
| 1e5 | 1 | off / on | 4.28 [1.30, 8.39] / 3.88 | 124 / 0 / 76 / 43 | 0.034 |
| 1e5 | 2 | off | 4.10 [0.89, 8.44] | 124 / 76 / 43 / 36 | 0.032 |
| 1e5 | 2 | on | **0.15** [0.00, 0.46] | 7 / 1 / 2 / 4 | 0.023 |
| 1e5 | 3 | off / on | 2.23 [0.44, 4.76] / 2.01 | 124 / 0 / 119 / 5 | 0.016 |
| 1e6 | 0 | off / on | 1.96 [0.34, 4.11] / 1.78 | 48 / 0 / 0 / 30 | 0.038 |
| 1e6 | 1 | off / on | 1.52 [0.15, 3.56] / 1.63 | 48 / 0 / 35 / 10 | 0.027 |
| 1e6 | 2 | off | 1.00 [0.08, 2.40] | 48 / 35 / 13 / 5 | 0.025 |
| 1e6 | 2 | on | 0.00 | 1 / 0 / 0 / 0 | – |
| 1e6 | 3 | off / on | 1.19 [0.10, 2.99] / 0.94 | 48 / 0 / 48 / 0 | 0.022 |

Comparison points (fault-free chip, CPU model): **always abstaining on W3** (W3's term removed from the sum at
prediction, training unchanged) costs **8.67 [4.39, 14.04]** extra mispredictions per 1000 branches; the
**3-table predictor** (W3 not built, trained without it) costs **4.02 [1.61, 6.79]**. TWIN_USE off: no change at
all (section 2).

Other decay families, extra per 1000 branches at τ = 1e3 / 1e4 (`partB.tsv`):

| Family | C0 R0 | C2 R0 | C2 R1 | C3 R0 | C3 R1 |
| --- | --- | --- | --- | --- | --- |
| exponential, all to 0 | 5.62 / 3.10 | 4.57 / 2.55 | 3.14 / 0.69 | 5.77 / 3.35 | 5.77 / 3.33 |
| exponential, all to 1 | 4.59 / 2.79 | 3.92 / 2.42 | 2.89 / 0.72 | 5.36 / 3.27 | 5.34 / 3.27 |
| log-normal, mixed | 9.52 / 6.80 | 5.87 / 4.50 | 0.19 / 0.01 | 4.36 / 2.62 | 0.08 / 0.00 |
| log-normal, all to 0 | 3.84 / 2.25 | 3.65 / 2.20 | 0.06 / 0.00 | 4.00 / 2.33 | 0.15 / 0.00 |
| log-normal, all to 1 | 3.24 / 2.19 | 3.19 / 2.13 | 0.03 / 0.00 | 3.81 / 2.27 | 0.12 / 0.00 |

Twin flips (Part A's process on W3L's 256 bits; `partA_twin.tsv`), extra mispredictions per flip at 1e-3:
CODE 0: 5.35 [1.52, 10.45]; parity: 3.57; SEC-DED: 3.54, with RESTORE **0.024** [0.000, 0.058]; detect all: 2.06
[0.47, 4.34]. A flip stays in the word until W3 retrains that entry: about 100 faulty reads per flip at 1e-3 and
650 at 1e-5. RESTORE changes nothing for CODE 0, 1, 3 here (a flip read back is written back unchanged).

**What the numbers say.**
- **Which code pays on a leaking memory.** Mixed levels, no RESTORE: *detect all → abstain* (CODE 3) is the best
  code up to τ = 1e5 (paired difference SEC-DED − CODE 3: +1.1 to +4.4 per 1000 branches, significant at τ = 1e2,
  3e2, 3e4, 1e5; equal at 1e6): a decaying word often has several wrong bits, which SEC-DED miscorrects or passes.
  **SEC-DED pays together with restore-on-use:** C2 R1 is the best mode from τ ≈ 1e3 (exponential, mixed; ahead of
  C3 R0 significantly from τ = 1e5, by 2.1 per 1000), and at every τ when all cells decay the same way (by 0.9–2.7
  per 1000). With a per-cell fixed lifetime, restore-on-use is what matters, for every code: entries in use are
  rewritten before they decay (C3 R1 and C2 R1 at ≤ 0.2 per 1000 from τ = 1e3). With the exponential (memoryless)
  law, rewriting a word as read changes nothing, which is why RESTORE helps only CODE 2 there.
- **Abstaining beats correcting** for mixed levels: always without RESTORE, and with RESTORE below τ ≈ 1e3 branches
  (≈ 0.1 ms at 30.3 MHz; C2 R1 − C3 R0 = +3.4 [+0.2, +6.6] at τ = 1e2, −0.3 [−3.5, +2.3] at 1e3). With one-directional
  decay, correcting wins at every τ. Per faulty read, an abstention costs 0.010–0.022 mispredictions, a silent wrong
  weight (CODE 0) 0.022–0.049.
- **Unidirectional decay defeats the code.** All-0 and all-1 words are code words of the (8,4) code (weights 0 and −1),
  so a word that decays completely in one direction is silent under every CODE: with all-to-0 or all-to-1 decay,
  C3 is no better than no code (5.77 vs 5.62 per 1000 at τ = 1e3). Only SEC-DED with RESTORE catches the first
  flipped bit before the second. A code that detects all unidirectional errors (Berger, m-of-n) would fit this
  memory better **(ESTIMATED: reasoning, not simulated)**. A coset mask closes the blind spot at no cost in cells:
  section 7.
- **When the twin stops being worth using.** Against leaving W3 out at prediction (8.67): the best mode is better
  from τ ≈ 1e3 (C3 R0, −2.1 [−3.8, −0.8]), C2 R1 from τ ≈ 3e3; CODE 0 needs τ ≥ 1e6. Against a 3-table design
  (4.02): the twin with C2 R1 is better from τ ≈ 3e4 (mixed) or 3e3 (one-directional decay). So with the estimated
  retention (τ ≈ 1e3–1e4 at 30.3 MHz, far less at a slow host), a leaky fourth table with SEC-DED and restore is
  roughly break-even against the 3-table predictor and worth having only if retention is at the upper end or the
  branch rate is high.

## 5. What the silicon experiment should measure to compare

| Number here | Silicon measurement |
| --- | --- |
| Part A per-fault cost (table × predictor) | FPGA raw-write injection at a known Poisson rate (≤ 1e-3 per resolved branch for the linear value), mispredictions of all four predictors counted by the FPGA from the prediction pins (the chip itself counts only branches and the driving predictor's mispredictions: statistics offload, docs/HANDOVER.md D30), against a fault-free run of the same program and the same seed; at least 10 seeds × many programs (the per-program effect is heavy-tailed); the FPGA twin of the RTL gives the fault-free counts in lockstep |
| Lifetime | the FPGA twin's state dump against the chip's after each injection (entry and whole state) |
| Part B per mode | per CODE × RESTORE with TWIN_USE on: mispredictions against TWIN_USE off on the same run (event 10 gives the W3-only prediction per predict, so one run suffices), and TW_ERR/COR/DET/SDC as event counts on the DBG pins, per 1000 resolves |
| τ | the branch rate (time between resolves) and the clock, to convert τ to seconds (section 1); sweep the clock or insert idle time to move along the τ axis; temperature logged |
| Decay model | the preferred level per cell and the lifetime distribution: write a pattern by raw access (space 12), read it back after a delay sweep, per cell: decides exponential vs fixed lifetime and the level, which decide whether restore-on-use helps CODE 0/1/3 (section 4) |

## 6. Limits of the model

- The decay model is assumed, not measured: lifetime law, preferred level, independence of cells, no read
  disturb, no temperature or voltage dependence; τ is in branches, so its time value depends on the host's rate.
- Part A flips one stored bit; the chip's real fault sources (write-pulse faults on W0–W2, timing faults) are other
  processes, not simulated.
- Programs: 71 benchmark traces with few branch sites (median 49); many table entries are unused, which lowers
  the per-fault cost of large tables. Correct-path traces only: no wrong-path predicts (on the chip, wrong-path
  predicts also read the twin and count events).
- The CPU-model effect of a fault on the timing of the other predictors is included in Part A (default driving);
  in Part B it is left out by the shadow evaluation (< 0.003 per 1000 branches).
- Below 1e-3 the per-fault cost is not resolved (heavy-tailed per-fault effect); not done: more seeds at 1e-4 and
  1e-5 (about 100× more runs would be needed for ±0.1).
- Not done: the RTL spot check by raw-write injection (docs/mosaic.md step 7 mentions it): the comparison is
  against the golden model, which equals the RTL in the project's co-simulation.

## 7. Coset mask for the twin's code (follow-up, 27 Nov 2026)

**Idea.** The chip stores `codeword XOR MASK` on the floating nodes and XORs MASK back after the read (a fixed mask
would be inverter polarities, no cells; as built it is switchable, one flop and XORs on the four check bits).
A fully decayed word (nodes all 0 or all 1) then decodes as MASK or ~MASK, which is not a
code word if MASK is not. In bpsim: twin scenario suffix `k<hex>` (`e1e4pmkF0`); decay acts on the node values;
the per-cell draws and decay seeds do not depend on the mask, so every mask sees the same chip.

**Algebra (VERIFIED with the model's encoder and decoder, `mask_algebra.log`).** Code words: 00 E1 D2 33 B4 55 66 87
78 99 AA 4B CC 2D 1E FF. Two masks behave alike on a fully decayed word exactly when they lie in the same coset
(they differ by a code word, since ~MASK = MASK ⊕ FF and FF is a code word):

| Mask | Weight | Code word | Syndrome / distance | Nodes 00 → decoded (C0, C1, C2, C3) | Nodes FF → decoded |
| --- | --- | --- | --- | --- | --- |
| 00 | 0 | yes | 0 / 0 | w 0 everywhere (silent) | w −1 everywhere (silent) |
| 03 | 2 | no | 3 / 2 | w 3, w 3, abstain, abstain | w −4, w −4, abstain, abstain |
| 30 | 2 | no | 3 / 2 (same coset as 03) | w 0, w 0, abstain, abstain | w −1, w −1, abstain, abstain |
| 21 | 2 | no | C / 2 | w 1, w 1, abstain, abstain | w −2, w −2, abstain, abstain |
| 01 | 1 | no | E / 1 | w 1, abstain, w 0 (miscorrected), abstain | w −2, abstain, w −1 (miscorrected), abstain |
| 0F | 4 | **no** | F / 2 | w −1, w −1, abstain, abstain | w 0, w 0, abstain, abstain |
| F0 | 4 | no | F / 2 (same coset as 0F) | w 0, w 0, abstain, abstain | w −1, w −1, abstain, abstain |
| 3C | 4 | no | F / 2 (same coset as 0F) | w −4, w −4, abstain, abstain | w 3, w 3, abstain, abstain |

0x0F is not a code word (data F encodes to FF), so it closes the blind spot for SEC-DED and detect-all; but it
inverts the data nodes, so CODE 0 and parity see the data decay in the opposite direction (to 0 now reads −1, to 1
reads 0). **The masks added on the merits are 0xF0 and 0x30 (check bits only):** the data nodes are untouched, so
CODE 0 behaves exactly as without a mask, and a fully decayed word reads as weight 0 / −1 under CODE 0 and parity
and abstains under SEC-DED and detect-all. 0x3C is a third member of the 0x0F coset, added for contrast.

**Checks (VERIFIED).** No decay: all 8 masks (shadow, all modes) and mask 0x21 driving in all 8 modes, 71 programs:
every count equals the plain run, all flags 0. Mask 0x00 in the mask runs reproduces Part B: 30,672 of 30,672 mode
counts identical. Against the golden model with masks 0x21, 0x0F, 0x01 on the decay cases (the model gets the
logical bit each decay leaves): 66 of 66 comparisons equal for each mask. Random single-bit flips on the twin (Part
A's process, all 4 rates, 3 seeds) with mask 0xF0: 852 of 852 runs identical to no mask in every count. As expected:
without decay, decoded = (cw ⊕ m ⊕ e) ⊕ m = cw ⊕ e, whatever m.

**Result** (τ = 1e4 branches, 3 seeds, 69 programs, CPU model, VERIFIED): extra mispredictions of the default per 1000
branches (TW_SDC per 1000 branches); ▲ / ▼ = worse / better than mask 00, paired over programs and seeds, 95 %.
Other τ values (1e3, 1e5 for all masks; 1e2 to 1e6 for 00, F0, 21) are in `explore/fault_results/masks.tsv` and
`maskreport.md`.

| decay | mask | C0 R0 | C0 R1 | C1 R0 | C1 R1 | C2 R0 | C2 R1 | C3 R0 | C3 R1 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| exp, to 0 | 00 | 3.10 (184) | 3.10 (184) | 3.37 (121) | 3.34 (121) | 2.55 (119) | 0.69 (50) | 3.35 (93) | 3.33 (93) |
| exp, to 0 | F0 | 3.09 (185) | 3.15 (184) | 3.20 (120) ▼ | 3.28 (120) ▼ | 2.70 (25) | 1.54 (37) ▲ | 3.15 (1) ▼ | 3.22 (1) ▼ |
| exp, to 0 | 30 | 3.14 (184) | 3.08 (184) | 3.26 (120) | 3.23 (121) ▼ | 2.70 (23) | 2.18 (37) ▲ | 3.17 (1) ▼ | 3.19 (1) ▼ |
| exp, to 0 | 21 | 4.09 (192) ▲ | 4.25 (193) ▲ | 4.36 (130) ▲ | 4.40 (130) ▲ | 2.62 (22) | 1.41 (34) | 3.32 (1) | 3.33 (1) |
| exp, to 0 | 03 | 9.76 (197) ▲ | 9.87 (197) ▲ | 9.90 (131) ▲ | 10.00 (131) ▲ | 2.75 (24) ▲ | 2.65 (36) ▲ | 3.36 (1) | 3.42 (1) |
| exp, to 0 | 01 | 4.11 (191) | 4.22 (192) ▲ | 3.37 (34) | 3.46 (34) | 2.63 (109) ▲ | 1.28 (50) ▲ | 3.37 (2) | 3.40 (2) |
| exp, to 0 | 0F | 2.80 (167) | 2.82 (167) | 3.28 (106) | 3.35 (106) | 2.67 (22) | 1.11 (26) | 3.39 (1) | 3.38 (1) |
| exp, to 0 | 3C | 4.28 (194) | 4.21 (194) | 4.37 (130) | 4.41 (129) | 2.73 (22) | 1.01 (28) | 3.18 (1) ▼ | 3.21 (1) |
| exp, to 1 | 00 | 2.79 (167) | 2.81 (168) | 3.23 (106) | 3.24 (107) | 2.42 (105) | 0.72 (44) | 3.27 (81) | 3.27 (81) |
| exp, to 1 | F0 | 2.81 (167) | 2.81 (167) | 3.30 (107) | 3.29 (107) | 2.72 (22) ▲ | 1.04 (27) | 3.40 (1) | 3.37 (1) |
| exp, to 1 | 30 | 2.85 (167) | 2.81 (167) | 3.32 (106) | 3.32 (106) | 2.76 (21) ▲ | 1.50 (28) | 3.40 (1) | 3.40 (1) |
| exp, to 1 | 21 | 3.03 (184) | 3.00 (184) | 3.45 (121) | 3.38 (120) | 2.56 (21) ▲ | 1.17 (26) ▲ | 3.31 (1) | 3.33 (1) |
| exp, to 1 | 03 | 4.26 (193) | 4.19 (193) | 4.37 (126) | 4.27 (127) | 2.80 (22) ▲ | 1.17 (27) | 3.19 (1) | 3.20 (1) |
| exp, to 1 | 01 | 3.08 (184) | 3.02 (185) | 3.30 (32) | 3.24 (32) | 2.55 (96) ▲ | 1.56 (43) | 3.24 (2) | 3.23 (2) |
| exp, to 1 | 0F | 3.12 (185) ▲ | 3.14 (185) | 3.22 (120) | 3.23 (121) | 2.72 (24) ▲ | 2.17 (36) ▲ | 3.17 (1) | 3.17 (1) |
| exp, to 1 | 3C | 9.76 (197) ▲ | 9.83 (196) ▲ | 9.95 (132) ▲ | 9.92 (131) ▲ | 2.77 (24) ▲ | 3.26 (35) ▲ | 3.42 (1) | 3.42 (1) |
| exp, mixed | 00 | 7.32 (195) | 7.27 (194) | 5.05 (83) | 5.04 (83) | 4.98 (78) | 2.67 (41) | 3.54 (8) | 3.50 (8) |
| exp, mixed | F0 | 7.38 (194) | 7.27 (194) | 5.11 (83) | 5.06 (83) | 5.25 (74) | 2.14 (38) | 3.34 (7) | 3.36 (7) |
| exp, mixed | 30 | 7.26 (194) | 7.36 (194) | 5.11 (83) | 5.08 (83) | 5.06 (78) | 2.12 (40) | 3.59 (9) | 3.54 (9) |
| exp, mixed | 21 | 6.84 (194) | 6.90 (193) | 4.80 (80) | 4.83 (80) | 5.05 (79) | 1.84 (39) | 3.30 (8) | 3.31 (9) |
| exp, mixed | 03 | 7.50 (194) | 7.55 (195) | 5.31 (83) | 5.29 (83) | 5.46 (78) | 2.11 (40) | 3.37 (9) | 3.40 (9) |
| exp, mixed | 01 | 6.91 (194) | 6.92 (193) | 5.61 (84) | 5.67 (84) | 4.09 (71) | 1.72 (39) | 3.54 (5) | 3.53 (5) |
| exp, mixed | 0F | 9.33 (195) | 9.41 (195) | 5.64 (82) | 5.71 (82) | 6.76 (77) | 2.82 (39) | 3.49 (7) | 3.53 (7) |
| exp, mixed | 3C | 8.73 (195) | 8.73 (195) | 5.54 (82) | 5.55 (82) | 6.15 (75) | 2.28 (40) | 3.61 (6) | 3.61 (6) |
| log-n, to 0 | 00 | 2.25 (122) | 0.02 (10) | 2.32 (103) | 0.01 (7) | 2.20 (105) | 0.00 (7) | 2.33 (94) | 0.00 (6) |
| log-n, to 0 | F0 | 2.25 (122) | 0.02 (10) | 2.28 (103) | 0.01 (7) | 2.28 (11) | 0.01 (1) | 2.27 (0) ▼ | 0.00 (0) |
| log-n, to 0 | 21 | 3.27 (130) ▲ | 0.03 (11) | 3.33 (112) ▲ | 0.01 (8) | 2.22 (10) | 0.00 (1) | 2.30 (1) | 0.00 (0) |
| log-n, to 1 | 00 | 2.19 (106) | 0.00 (9) | 2.26 (89) | 0.00 (6) | 2.13 (93) | 0.00 (6) | 2.27 (82) | 0.00 (5) |
| log-n, to 1 | F0 | 2.19 (106) | 0.00 (9) | 2.25 (91) | 0.01 (6) | 2.28 (11) ▲ | 0.00 (1) | 2.32 (1) | 0.00 (0) |
| log-n, to 1 | 21 | 2.37 (121) | 0.03 (9) | 2.45 (103) | 0.02 (6) | 2.25 (10) ▲ | 0.01 (1) | 2.34 (1) | 0.00 (0) |
| log-n, mixed | 00 | 6.80 (127) | 0.05 (11) | 5.43 (68) | 0.04 (5) | 4.50 (57) | 0.01 (5) | 2.62 (4) | 0.00 (0) |
| log-n, mixed | F0 | 6.80 (127) | 0.05 (11) | 5.40 (68) | 0.04 (6) | 4.54 (57) | 0.00 (4) | 2.45 (4) | 0.00 (0) |
| log-n, mixed | 21 | 6.45 (129) | 0.04 (11) | 4.82 (69) ▼ | 0.04 (6) | 4.43 (62) | 0.01 (5) | 2.28 (9) | 0.00 (1) |

**What it shows.**
- **The blind spot is real in the flags and small in mispredictions.** With one-directional decay, any non-code-word
  mask cuts TW_SDC under detect all by about 100× (decay to 0, τ = 1e4: 93 → 1 per 1000 branches), under SEC-DED
  without restore by about 5× (119 → 25), with restore by 1.3× to 2.6×: the decayed words now abstain instead of
  passing silently. Mispredictions move by only ±0.3 per 1000 there,
  because without a mask the decay lands on weights 0 and −1, which cost almost as little as abstaining.
- **Masks with data bits (0x03, 0x21, 0x3C, 0x01, 0x0F) hurt CODE 0 and parity** whenever the decayed data reads as a
  large weight (0x03 to 0 and 0x3C to 1 read ±3 / −4: +6.5 to +7.0 per 1000), and help only when they happen to
  read −1 instead of 0 (0x0F to 0).
- **Masks cost SEC-DED with restore under one-directional decay** (C2 R1: 0x00 0.69 → 0xF0 1.54 to 0, 1.04 to 1 at
  τ = 1e4; up to +1.9 per 1000 at τ = 1e2). Without a mask the commonest stored words (00 and FF: weights 0 and −1)
  sit at or one level away from the preferred state, so they barely decay; a mask exposes their nodes **(ESTIMATED:
  explanation, consistent with the per-mask numbers, not separately measured)**.
- Per-cell fixed lifetimes: with RESTORE everything is ≤ 0.05 per 1000 from τ = 1e4 with or without a mask.

**Recommendation: MASK = 0xF0 (invert the four check bits, leave the data bits).** Reason: it is the mask that does
no harm where the twin is used without a code (CODE 0 is unchanged: identical aggregates in all 30 log-normal
cells; with exponential lifetimes −0.3 to +0.6 per 1000, never significant, because the order of the random
lifetime draws changes), and it makes the flags tell the truth under
one-directional decay. With it, a fully decayed word reads weight 0 or −1 under CODE 0 and parity, exactly as today,
and abstains under SEC-DED and detect-all.
- **Better than no mask:** detect all (CODE 3) with decay to 0 (−0.1 to −0.3 per 1000, significant at τ = 1e3 and
  1e4) and with mixed levels at τ = 1e3 (−0.9); and TW_SDC under CODE 2 and 3 for one-directional decay at every τ
  (e.g. 93 → 1 per 1000), which is what the silicon experiment counts.
- **Worse than no mask:** SEC-DED with one-directional decay, above all with RESTORE (C2 R1 +0.3 to +1.9 per 1000 for
  τ = 1e2 to 1e4; equal from 1e5), and detect all with decay to 1 (+0.1 to +0.9, significant only at τ = 1e3 with
  RESTORE, +0.38).
- Neutral: CODE 0 always; parity −0.16 to +0.6 (significant only as small gains with decay to 0); random
  single-bit flips (identical counts); per-cell fixed lifetimes with RESTORE.

If the decay turns out to be one-directional and the chip is to run in its best mode (SEC-DED + restore) for the
lowest misprediction rate rather than for truthful flags, MASK = 0x00 is better. Measuring the preferred level per
cell on silicon (section 5) decides between them. **Built since (mosaic step 5):** both are available on the chip:
CTRL[15] MASK = 1 stores `code word ^ 0xF0`, MASK = 0 the plain code word; reset 0; in the RTL, the golden model and
the host (docs/mosaic.md 7.7). A change of MASK invalidates the staging register, and words written under the other
setting read as faulty until rewritten, so clear the predictors after switching. Not done: masks other than the
eight listed.


Cost of this follow-up: 11,756 s CPU (mask runs 6,444, log-normal and extra τ 4,334, no-decay check 439, twin flips
539) plus about 300 s of model validation; 51 minutes of bpsim wall time at 4 threads, niced **(VERIFIED,
`cpu_time.log`)**.

## 8. Cost

CPU time of the runs: 13,934 s (3.9 CPU-hours: Part A 4,774 + 1,892 + 1,318, twin flips 652, Part B 3,713, plain
and checks 1,214, pilot and rerun 372) plus about 600 s of validation and aggregation; 61 minutes of bpsim wall
time at 4 threads, niced, beside the other agent's builds (`explore/fault_results/cpu_time.log`) **(VERIFIED)**.
