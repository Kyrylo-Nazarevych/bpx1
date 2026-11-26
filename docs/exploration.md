# BPX-1 design-space exploration (DR-02)

> **Status (26 Nov 2026):** this is the first exploration round, which chose the tagged-table design (tag
> `g3-tage`, the fallback). On the owners' requirement the default predictor is now a perceptron: docs/perceptron.md
> has that round. The benchmark set, the split and the method below are unchanged; identifiers are now 8 bits.

What was explored, on what, how the design was chosen, and the evidence. Full generated tables:
`docs/exploration_tables.md`. Every number here is **(VERIFIED)**: produced by `explore/bpsim.cc` on traces of the
programs named, in this project, unless marked otherwise. The engine is cross-checked against the golden model
(identical misprediction counts for all four predictors on three programs) and the RTL equals the golden model
(docs/HANDOVER.md), but the numbers below are *model* results: the VR-06 co-simulation against RTL is still to run.

## 1. Benchmark set (EX-01), frozen

71 RV32IM programs, 47.3 million conditional branches, 344 million instructions, run in Spike and converted to event
traces (`bench/README.md`). Distinct branch sites per program: 6 to 640, median 49.

| Group | Programs | Source |
| --- | --- | --- |
| Team | 11 | The six real programs of the team's 45 (at -O2, workloads scaled up to ≥ 200,000 branches) and the five "hard" ones |
| New | 37 | Written for this project: sorting, searching, strings, compression, interpreters, numerics, graphs, games, dynamic programming, system code (`bench/src`) |
| Embench-IoT 1.0 | 19 | Fetched at a pinned commit by `bench/ext/build_ext.sh`; not stored here |
| CoreMark | 1 | Same |
| MiBench | 3 | bitcount, dijkstra, adpcm; same |

**Dropped:** the team's nine synthetic `pattern_*` programs (one fixed pattern each: they saturate every predictor
and hide differences) and the -O0/-O1 builds. **Out of the aggregates by the owners' decision, reported
separately:** `embench.nbody` and `embench.cubic`, whose branches are dominated by this project's own `sqrt` (the
toolchain has no maths library). On them the frozen design scores 84.0 % / 1.064× and 79.8 % / 1.051×, the
tournament 80.7 % / 1.057× and 76.9 % / 1.046×.

**Split.** 51 tuning programs and 18 held-out programs, by family (within each family, sorted by name, every fourth
program from the third: `sweep.split`). Designs were ranked on the tuning programs only.

## 2. Method

- **Timing model:** the U1 CPU contract of docs/SPEC.md section 9 (five stages, branch-target buffer at fetch,
  misprediction 2 cycles), training delayed until the branch resolves, predict served before resolve.
- **Aggregates:** speedup = geometric mean over programs of cycles(static not-taken) / cycles(candidate driving);
  accuracy = mean over programs; every program has equal weight.
- **Cost:** storage bits and an area estimate: tables by a fit to synthesis of the real table RTL
  (`550 + entries × (20.5 × bits + 6.5)` µm²), logic in the cells the flow may use.
- **Whole-chip candidates:** the reference tables are part of every candidate, and the default predictor may use
  them as its base, so no bit serves comparison only.
- **Compute:** all sweeps ran on the full benchmark set, none on a sample, on a rented 256-thread machine
  (docs/HANDOVER.md, spend).

## 3. Breadth (EX-02): 16 families, 486 candidates

Best candidate of each family within a storage budget, tuning programs (speedup / accuracy %):

| Family | ≤ 600 bits | ≤ 1,200 bits | Remark |
| --- | --- | --- | --- |
| Tagged geometric history (TAGE-like) over bimodal | best | best | Leads at every budget from 300 bits up |
| GEHL (summed counters, geometric histories) | about −0.2 pt | within 0.1 pt | Runner-up; needs an adder tree on the predict path |
| Alpha-style local + global, perceptron | −0.7 pt | −0.6 pt | |
| Tournament, gshare (plain and agree), gselect, alloyed, gskew, YAGS, bi-mode, local | −1 to −2 pt | −1 to −2 pt | |
| Bimodal (2-bit, agree, shared hysteresis) | −2 pt | −3 pt | Saturates at 88.1 % |

Exact cells: `docs/exploration_tables.md` section 2; chart: `docs/img/pareto_families.png`. Literature (survey
summarised in docs/HANDOVER.md): no published data point for any family below 2 Kbit except alloyed and gselect;
TAGE's smallest published point is 32 Kbit, so this result is new evidence, not a confirmation.

## 4. Whole-chip candidates (EX-03): 7,376

Three stages: a grid over table shapes, tag widths and reference sizes (1,662); a refinement around the leaders
over history lengths, policies, chooser indexing and allocation (3,460); and a **stratified random sample of the
whole parameter space** (2,342, every factor's levels used equally often) to check for regions the grid missed.
Chart: `docs/img/pareto_chips.png`.

- **Plateau.** Within an estimated 33,000 µm², the best 16 candidates lie within 0.0007 of speedup (1.1328 to
  1.1335 on the tuning programs). The best random candidate in budget reaches 1.1327: no better region exists.
- **Held-out check.** Rank correlation of speedup between tuning and held-out programs over the 3,288 candidates in
  budget: 0.89. The leaders on the tuning set lead on the held-out set.
- **Main effects** (random sample, storage trend removed; share of the remaining variance): longest history length
  18 % (12 or more is good, under 8 loses 0.5–1 pt); tournament base instead of bimodal 16 % (+0.8 pt); gshare size
  16 % (at equal storage, small is better: the bits earn more in tagged tables); number of tagged tables 16 % (one
  is poor, three or four best); use-alternate counter 12 % (without it −0.8 pt); bimodal size 7 % (32–128 good);
  tag bits 5 % (5 or more); everything else under 2 % each. Storage itself: +1.2 pt per doubling.

## 5. The chosen design

Bimodal 64 + chooser 64, gshare 128 (4 history bits), three tagged tables of 16 entries (7-bit tag, 3-bit counter,
2 useful bits, histories 4 / 9 / 20), 4-bit use-alternate counter, allocation by an 8-bit LFSR: **1,088 bits**
(docs/SPEC.md section 7). It is on the plateau, is the cheapest of its leaders that keeps both references at 256
bits, and has three tables rather than four.

| Predictor | Speedup, all 69 | Accuracy %, all 69 | Accuracy %, tuning 51 | Accuracy %, held-out 18 |
| --- | --- | --- | --- | --- |
| **Default (frozen design)** | **1.1352** | **91.24** | 90.41 | 93.60 |
| Tournament (same tables) | 1.1277 | 88.90 | 88.17 | 90.98 |
| gshare 128, 4 bits | 1.1226 | 87.40 | 86.53 | 89.88 |
| Bimodal 64 | 1.1204 | 86.54 | 85.89 | 88.37 |
| Static backward-taken | 1.0837 | 74.72 | 73.79 | 77.37 |
| Static not-taken | 1.0000 | 44.2 | | |

- **PF-01, PF-02 (model preview):** the default is ahead of every reference in aggregate, and is less accurate
  than the best reference on **none** of the 69 programs (per-program table: `docs/exploration_tables.md` section 6).
- **PF-03:** accuracy with the CPU's in-flight depth equals one-in-flight to three decimals (91.238 both).
- **Sensitivity:** no branch-target buffer (ask in decode) 1.056; BTB 64 entries 1.132, 1,024 entries 1.136;
  resolve one stage later 1.198, two stages later 1.258; resolve served before predict: identical.
- **Not taken up:** a static hint bit from the host with agree coding (±0.05 pt); a loop predictor (−0.2 to −0.3 pt,
  worse with later resolve); a chooser indexed by history (+0.5 pt in the random sample, nothing on top of the
  refined design, and it would need its own table); 16-bit instead of 8-bit allocation LFSR (equal).

## 6. What is still open

- These are model numbers under the CPU contract; VR-06 (the same programs against the RTL) replaces them.
- The differences between the leaders are smaller than any plausible benchmark-set noise: the choice among them was
  made on cost and risk, not on the fourth decimal.
