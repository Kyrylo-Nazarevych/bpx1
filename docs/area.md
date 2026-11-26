# BPX-1 area (DR-03, XR-02, AL-02)

All numbers **(VERIFIED)** from local LibreLane builds (same flow version as the CI), 27 Nov 2026.

## Result

| Quantity | Value |
| --- | --- |
| Core area | 72,565 µm² |
| Standard cells, routed | 55,072 µm² |
| Utilisation: LibreLane `design__instance__utilization` | 75.9 % |
| Utilisation: Tiny Tapeout `--print-stats` | 74.3 % |
| Sequential cells (1,168 table and snapshot latches, about 430 flops) | 26,732 µm² |
| Combinational cells | 21,519 µm² |
| Timing-repair buffers | 4,123 µm² |
| Clock tree | 955 µm² |
| Growth from synthesised (48,250) to routed cell area | +14 % |
| Wire length | 187 mm |
| Routing DRC, Magic DRC, LVS, antenna | 0, 0, 0, 0 |

Synthesised split (Yosys, hierarchy kept): tables 23,000 (bimodal+chooser 5,400, gshare 5,400, three tagged tables
4,080 each); sequencer, queue, histories, statistics 13,000; predictor logic 7,500; snapshot latches 1,300; control
port and synchronisers 2,400.

## Flow settings that differ from the Tiny Tapeout template (AL-02), each with its evidence

| Setting | Value | Why | Evidence | Fallback |
| --- | --- | --- | --- | --- |
| `SYNTH_EXCLUDED_CELL_FILE` | `src/synth_exclude_small.cells`: the PDK list minus its plain drive-strength-1 logic gates and flop (114 instead of 199 cells) | The template bars every `_1` gate from synthesis, which makes logic about 1.6× larger; the design does not fit without them | 57.0k → 49.9k µm² synthesised; builds B, C and the repository build clean | Default list, then cut logic or one tagged table |
| `PNR_EXCLUDED_CELL_FILE` | `src/pnr_exclude.cells`: the PDK list plus `clkdlybuf4s25/s50` | The resizer used these slow delay cells as fan-out buffers: about 1 ns each at the slow corner | Worst slow-corner path 24.7 → 21.7 ns; 1,500 µm² fewer buffers (trial B → C) | Default list |
| `MAX_FANOUT_CONSTRAINT` | 16 (template: 10) | Fewer fan-out buffers on the wide nets of the tables | With the hold margins: repair buffers 9,915 → 5,645 µm² (trial A → B) | 10 |
| `PL_/GRT_RESIZER_HOLD_SLACK_MARGIN` | 0.05 / 0.03 ns (template 0.1 / 0.05) | Fewer hold buffers | Hold met in all corners, worst slack +0.06 ns | Template values |
| `PL_TARGET_DENSITY_PCT` | 80 (template 60) | The design needs 76 % | Routed clean | – |
| `CLOCK_PERIOD` | 25 ns | See docs/timing.md | | |

## Trial builds (the path here)

| Build | Cells | Density | Extra | Utilisation after repair | Outcome |
| --- | --- | --- | --- | --- | --- |
| Probe p5 (tables + placeholder logic) | default | 80 | – | 82.4 % | Clean; established the latch tables; CI and precheck green |
| A | small | 84 | – | 83.8 % | Stopped: 12,734 routing violations, 399 mm estimated wire |
| B | small | 80 | fan-out 16, hold margins | 78.0 % | Clean after 17 routing passes; slow-corner setup −4.7 ns at 20 ns |
| C | small | 80 | B + slow delay cells excluded | 75.9 % | Clean; slow-corner setup −1.65 ns at 20 ns |
| Repository | as C | 80 | clock 25 ns | 75.9 % | Clean; all corners met |

Not yet run: the default-cell column of the planned factorial (it would need about 7,000 µm² cut from the logic)
and density 86.

## Known deviations

XR-03 (SHOULD): 2,611 max-slew and 11 max-capacitance warnings at the slow corner, on the wide nets of the tables
(write data, row selects, gate terms). Their delays are included in the sign-off timing. Reducing them (repair at
the slow corner) is an open improvement.
