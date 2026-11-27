# BPX-1 area (DR-03, XR-02, AL-02)

Numbers from local LibreLane builds (same flow version as the CI; since the end of the mosaic round also from a
rented machine with the same LibreLane 3.0.14 image, which reproduced the local step-4 build byte for byte). Steps
of the mosaic round are **(VERIFIED, progress log)** = recorded in docs/mosaic.md section 9; figures of the final
build are `TBD(final)`.

## Result (final mosaic build, 33 ns, statistics offload)

| Quantity | Value |
| --- | --- |
| Core area | 72,565 µm² (unchanged) |
| Standard cells, routed | TBD(final) |
| Utilisation: LibreLane `design__instance__utilization` | TBD(final) (about 81 %, owners' report) |
| Utilisation: Tiny Tapeout `--print-stats` | TBD(final) |
| Timing-repair buffers | TBD(final) |
| Routing DRC, Magic DRC, LVS, antenna | TBD(final) |
| Floating-node cells (`einvp_1`) in the final netlist | TBD(final) (256 expected) |

## Area history of the mosaic round

| Design | Cells, routed | Utilisation | Outcome |
| --- | --- | --- | --- |
| `g3-perceptron` (start of the round) | 58,947 µm² | 81.2 % | clean, 30 ns |
| Step 2: trims (4-bit weights, 8-bit sum, 8-bit path history, binarised inference removed) | 53,848 µm² | 74.2 % | clean |
| Step 3: registers and the leaky twin | 59,891 µm² | 82.5 % | clean with `src/bpx.sdc` |
| Step 4: pulse selector, canaries, checkers, watchdog, demotion | 61,204 µm² | 84.34 % | clean |
| Step 5 with all five statistics counters | – | 86 % | routing did not converge **(VERIFIED by the owners' report)** |
| Step 5 with the statistics offload (two counters) | TBD(final) | about 81 % (TBD(final)) | TBD(final) |

The plan (mosaic.md 4) estimated additions of about 5,600 µm² and trims of about 3,300 µm², net about +2,300 µm²
→ about 84 % **(ESTIMATED there)**; step 4 reached 84.34 % before the step-5 additions. The offload removes three
of the five statistics counters with their snapshot latches.

## Area by block

Synthesised area, Yosys with the repository's cell settings and the hierarchy kept; "–" = not recorded for that step.

**Mosaic steps (VERIFIED, progress log):**

| Block | Step 2 | Step 3 | Step 4 |
| --- | --- | --- | --- |
| Whole chip, synthesised | 49,406 | 55,991 | 57,003 |
| Weight tables W0–W3 (32 × 4 bits each) | 4 × 3,237 | – | – |
| The two sums | 2 × 1,107 | 2 × 1,358 | – |
| Leaky twin W3L (256 `einvp_1` and its 32-way read tree) | – | 3,757 | – |
| `bpx_pred` own logic | – | 4,593 | – |
| `bpx_core` own logic | – | 14,697 | 15,283 (checkers, watchdog, demotion, flags, events) |
| `bpx_canary` (four flops, two delay cells, two XORs) | – | – | 115 |
| `bpx_wseq` (12 delay cells, tap multiplexer, `and3b`, three setting flops) | – | – | 410 |

In hierarchical synthesis the unused second last-term adder of the staging sum is not removed (flattened it is).
Step 5 (rings 2 × 137.6 µm² of cells, divider, LFSR, strap, mask, the offload) by block: TBD(final).

**`g3-perceptron`, for reference (VERIFIED in that round):** total 54,112 µm²; four weight tables of 32 × 5 bits
15,594 (29 %); the two sums 3,042; bimodal + chooser 6,350; gshare 6,300; index hashes 1,190; predictor multiplexers,
training and threshold 3,770; sequencer, queue, histories, statistics, raw access 14,274; statistics snapshot latches
1,204; write sequencer 100; SPI 1,886; synchronisers and pins 404.

## Flow settings that differ from the Tiny Tapeout template (AL-02), each with its evidence

| Setting | Value | Why | Evidence | Fallback |
| --- | --- | --- | --- | --- |
| `SYNTH_EXCLUDED_CELL_FILE` | `src/synth_exclude_small.cells`: the PDK list minus its plain drive-strength-1 logic gates and flop (114 instead of 199 cells) | The template bars every `_1` gate from synthesis, which makes logic about 1.6× larger | 57.0k → 49.9k µm² synthesised (trial builds) | Default list, then cut logic |
| `PNR_EXCLUDED_CELL_FILE` | `src/pnr_exclude.cells`: the PDK list plus `clkdlybuf4s25/s50` | The resizer used these slow delay cells as fan-out buffers: about 1 ns each at the slow corner | Worst slow-corner path 24.7 → 21.7 ns; 1,500 µm² fewer buffers (trial B → C) | Default list |
| `MAX_FANOUT_CONSTRAINT` | 16 (template: 10) | Fewer fan-out buffers on the wide nets of the tables | With the hold margins: repair buffers 9,915 → 5,645 µm² (trial A → B) | 10 |
| `PL_/GRT_RESIZER_HOLD_SLACK_MARGIN` | 0.05 / 0.03 ns (template 0.1 / 0.05) | Fewer hold buffers | Hold met in all corners (step 4: +0.066 ns) | Template values |
| `PL_TARGET_DENSITY_PCT` | 84 (template 60) | The design needs more than 80 %; the target must be above it | Routed clean at 84.34 % (step 4) | – |
| `PNR_SDC_FILE`, `SIGNOFF_SDC_FILE` (mosaic step 3) | `src/bpx.sdc`: the flow's base SDC plus `set_disable_timing` on `einvp_1` TE → Z | The tri-state enable of the twin's cells is a false path (the node changes only in the pulse cycle, whose reads nothing captures) | Step 3: −2.60 ns with the arc, +3.21 ns without; the condition is a proven formal assertion (verification.md 7.4) | none needed |
| `RSZ_DONT_TOUCH_RX` (mosaic steps 4 and 5) | `u_dly\|u_ro\.u_r[ab]\.([gnr]\|u_en\|u_dl)` | The delay cells of the pulse selector and the canaries are their delay (the resizer could swap one for a buffer); the two rings must stay cell for cell identical | Step 4: all 12 + 2 delay cells kept, logged. Protecting the ring's run net too stopped the first step-5 build (`RSZ-3006`), so the run pins are driven by an unprotected `buf_2` | – |
| `CLOCK_PERIOD` | 33 ns (was 30, and 25 before the perceptron) | Owners' decision of 27 Nov: margin for the sum and the twin, and about 1 ns of canary lead | docs/timing.md | 30 ns |

## Trial builds (the path here)

| Build | Cells | Density | Extra | Utilisation after repair | Outcome |
| --- | --- | --- | --- | --- | --- |
| Probe p5 (tables + placeholder logic) | default | 80 | – | 82.4 % | Clean; established the latch tables; CI and precheck green |
| A | small | 84 | – | 83.8 % | Stopped: 12,734 routing violations |
| B | small | 80 | fan-out 16, hold margins | 78.0 % | Clean; slow-corner setup −4.7 ns at 20 ns |
| C | small | 80 | B + slow delay cells excluded | 75.9 % | Clean; slow-corner setup −1.65 ns at 20 ns |
| Tagged-table design (tag `g3-tage`) | as C | 80 | clock 25 ns | 75.6 % | Clean; gate-level tests pass |
| Perceptron (tag `g3-perceptron`) | as C | 84 | clock 30 ns | 81.2 % | Clean; gate-level tests pass |
| Probe `probe-leaky` (one 16 × 8 floating-node table) | as C | – | – | – | Clean locally: 128 `einvp_1`, DRC/LVS/antenna 0; CI precheck TBD(final) |
| Mosaic step 2 | as C | 84 | 30 ns | 74.2 % | Clean |
| Mosaic step 3, build 1 / 2 | as C | 84 | 30 ns; build 2 with `bpx.sdc` | 82.5 % / 82.5 % | Build 1 −2.60 ns (false path); build 2 clean |
| Mosaic step 4, builds 1 / 2 / 3 | as C | 84 | 30 ns, canary variants | 84.36 % / – / 84.34 % | Builds 1, 2 missed setup on the CONF canary; build 3 clean |
| Mosaic step 5, ring fully protected | as C | 84 | 33 ns | – | Stopped in post-placement repair (`RSZ-3006`) |
| Mosaic step 5, five counters | as C | 84 | 33 ns | 86 % | Routing did not converge |
| **Mosaic final (offload)** | as C | 84 | 33 ns | TBD(final) | TBD(final) |

## Known deviations

XR-03 (SHOULD): maximum-slew and maximum-capacitance warnings at the slow corner on the wide nets of the tables
(write data, row selects, gate terms): 2,259 / 5 at `g3-perceptron`, 2,787 / 14 at step 4 **(VERIFIED, progress
log)**, final TBD(final). Their delays are included in the sign-off timing.
