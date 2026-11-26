# BPX-1 area (DR-03, XR-02, AL-02)

All numbers **(VERIFIED)** from local LibreLane builds (same flow version as the CI); the result table is the
build of the perceptron design (docs/perceptron.md) with its final parameters, 26 Nov 2026.

## Result

| Quantity | Value |
| --- | --- |
| Core area | 72,565 µm² |
| Standard cells, routed | 58,947 µm² |
| Utilisation: LibreLane `design__instance__utilization` | 81.2 % |
| Utilisation: Tiny Tapeout `--print-stats` | 80.2 % |
| Sequential cells (1,232 table and snapshot latches, 97 latches of the write path, 472 flops) | 27,951 µm² |
| Combinational cells | 23,889 µm² |
| Timing-repair buffers | 4,288 µm² |
| Clock tree | 930 µm² |
| Growth from synthesised (54,112) to routed cell area | +9 % |
| Wire length | 194 mm |
| Routing DRC, Magic DRC, LVS, antenna | 0, 0, 0, 0 |

## Area by block

Synthesised area, Yosys with the repository's cell settings and the hierarchy kept **(VERIFIED, 26 Nov 2026)**:

| Block | µm² | Share |
| --- | --- | --- |
| Perceptron: four weight tables, 32 × 5 bits each | 15,594 | 29 % |
| Perceptron: the two sums (predict, and from the staging register for training) | 3,042 | 6 % |
| Bimodal + chooser table, 64 × 4 bits | 6,350 | 12 % |
| gshare table, 128 × 2 bits | 6,300 | 12 % |
| Index hashes of all tables (identifier, outcome and path history folds) | 1,190 | 2 % |
| Predictor: address multiplexers, training logic, threshold | 3,770 | 7 % |
| Sequencer, in-flight queue, four history registers, statistics counters, raw access | 14,274 | 26 % |
| Statistics snapshot latches | 1,204 | 2 % |
| Write sequencer | 100 | 0.2 % |
| Control port (SPI) | 1,886 | 3 % |
| Synchronisers and pins | 404 | 1 % |
| **Total synthesised** | **54,112** | |

Against the tagged-table design (48,250 synthesised, 75.6 % routed): the weight tables are 3,350 µm² larger than
the three tagged tables, the two sums and the path history replace the tag comparators and the allocation logic,
and the result is 5.6 points of utilisation more. Nothing had to be optimised to fit: the build routes clean at
81 %. What is left is about 13,600 µm² of the core, of which a routable design can use perhaps a third (trial A
below stopped at 83.8 %).

## Flow settings that differ from the Tiny Tapeout template (AL-02), each with its evidence

| Setting | Value | Why | Evidence | Fallback |
| --- | --- | --- | --- | --- |
| `SYNTH_EXCLUDED_CELL_FILE` | `src/synth_exclude_small.cells`: the PDK list minus its plain drive-strength-1 logic gates and flop (114 instead of 199 cells) | The template bars every `_1` gate from synthesis, which makes logic about 1.6× larger; the design does not fit without them | 57.0k → 49.9k µm² synthesised; builds B, C and the repository build clean | Default list, then cut logic or one tagged table |
| `PNR_EXCLUDED_CELL_FILE` | `src/pnr_exclude.cells`: the PDK list plus `clkdlybuf4s25/s50` | The resizer used these slow delay cells as fan-out buffers: about 1 ns each at the slow corner | Worst slow-corner path 24.7 → 21.7 ns; 1,500 µm² fewer buffers (trial B → C) | Default list |
| `MAX_FANOUT_CONSTRAINT` | 16 (template: 10) | Fewer fan-out buffers on the wide nets of the tables | With the hold margins: repair buffers 9,915 → 5,645 µm² (trial A → B) | 10 |
| `PL_/GRT_RESIZER_HOLD_SLACK_MARGIN` | 0.05 / 0.03 ns (template 0.1 / 0.05) | Fewer hold buffers | Hold met in all corners, worst slack +0.06 ns | Template values |
| `PL_TARGET_DENSITY_PCT` | 84 (template 60) | The design needs 81 %; the target must be above it | Routed clean (5 detailed-routing passes to zero) | – |
| `CLOCK_PERIOD` | 30 ns | See docs/timing.md | | |

## Trial builds (the path here)

| Build | Cells | Density | Extra | Utilisation after repair | Outcome |
| --- | --- | --- | --- | --- | --- |
| Probe p5 (tables + placeholder logic) | default | 80 | – | 82.4 % | Clean; established the latch tables; CI and precheck green |
| A | small | 84 | – | 83.8 % | Stopped: 12,734 routing violations, 399 mm estimated wire |
| B | small | 80 | fan-out 16, hold margins | 78.0 % | Clean after 17 routing passes; slow-corner setup −4.7 ns at 20 ns |
| C | small | 80 | B + slow delay cells excluded | 75.9 % | Clean; slow-corner setup −1.65 ns at 20 ns |
| Repository at G2 | as C | 80 | clock 25 ns | 75.9 % | Clean; all corners met |
| Tagged-table design (tag `g3-tage`) | as C | 80 | clock 25 ns | 75.6 % | Clean; all corners met; gate-level tests pass |
| Perceptron, first trial (no path history) | as C | 80 | clock 25 ns | 78.0 % | Clean, but slow-corner setup −2.58 ns (14 endpoints, all through the gshare table and the sum) |
| **Repository: perceptron, final parameters** | as C | 84 | clock 30 ns | 81.2 % | Clean; all corners met; gate-level tests pass |

Not yet run: the default-cell column of the planned factorial (it would need about 7,000 µm² cut from the logic)
and density 86.

## Known deviations

XR-03 (SHOULD): 2,259 max-slew and 5 max-capacitance warnings at the slow corner, on the wide nets of the tables
(write data, row selects, gate terms). Their delays are included in the sign-off timing. Reducing them (repair at
the slow corner) is an open improvement.
