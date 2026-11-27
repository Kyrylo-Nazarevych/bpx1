# BPX-1 timing (DR-03, PF-04, XR-04, XR-05)

Sign-off STA of local LibreLane builds (since the end of the mosaic round also of the rented machine with the same
image). Mosaic steps are **(VERIFIED, progress log)** = recorded in docs/mosaic.md section 9 by the session that
built them. The final build's reports per corner are in `docs/mosaic_step5_build/` (OpenSTA in the LibreLane
container, final netlist and SPEF, the flow SDC plus the `einvp_1` exception).

## Result (final mosaic build, density 80)

All values **(VERIFIED: progress log, step 5, and docs/mosaic_step5_build/)**.

| Corner | Setup worst slack at 33 ns (30.3 MHz) | Hold |
| --- | --- | --- |
| Slow: ss, 100 °C, 1.60 V | +3.53 ns (CONF canary); real CONF flop +6.14, real PRED flop +7.40, PRED canary +5.00; next real endpoints +6.90 | |
| Typical: tt, 25 °C, 1.80 V | +14.89 ns | |
| All corners | +3.53 ns, 0 violations | +0.058 ns (ff), 0 violations |

**Clock: 33 ns (30.3 MHz)**, owners' decision of 27 Nov 2026 (was 30 ns): margin for the perceptron sum and the
leaky twin's decoder in the predict cycle, and room for a canary lead of about 1 ns typical (below). Worst path at
the slow corner: flop `_5994_` → twin read-address fan-out → twin read tree → mask `g_twm` → decoder → sum → CONF D →
`u_cb` → 2 × `dlygate4sd3_1` → CONF canary; data arrival 30.13 ns against 33.66 ns required. Slow-corner warnings:
2,682 max-slew, 9 max-cap, 0 max-fanout.

**Ring loops.** Static timing reports "There are 4 combinational loops in the design" (two per ring), each cut at
`u_ro.u_r[ab].g_st[0].u1` A → X; no constrained path runs through a ring (`docs/mosaic_step5_build/sta_loops.txt`)
**(VERIFIED, progress log, step 5)**.

## Timing history of the mosaic round

| Build | Clock | Slow-corner setup | Worst path |
| --- | --- | --- | --- |
| `g3-perceptron` | 30 ns | +3.87 ns | the perceptron sum in the predict cycle |
| Step 2 (trims) | 30 ns | +1.87 ns | BC table → reference terms → CONF; probably the ×3 weights, which need an adder where ×4 was a shift **(ESTIMATED in the log)** |
| Step 3, build 1 | 30 ns | −2.60 ns, 27 endpoints | false path: pulse flop `b` → twin write gate → `einvp_1` TE → Z → read tree → decoder → sum → CONF |
| Step 3, build 2 (`src/bpx.sdc`) | 30 ns | +3.21 ns | address flop → twin read → mask → decoder → sum → CONF (reference weights back to ×4) |
| Step 4, build 1 (canary 2 × `dlygate4sd3_1`, XOR before the shadow flop) | 30 ns | −0.39 ns | CONF canary (CONF itself +2.18) |
| Step 4, build 2 (one `dlygate4sd3_1`) | 30 ns | −0.34 ns | CONF canary (CONF itself +1.16) |
| Step 4, build 3 (one `dlygate4sd1_1`, XOR after the shadow flop) | 30 ns | +2.06 ns | CONF canary; real CONF +2.46, PRED +3.54, its canary +3.06; typical corner +13.87, hold +0.066 |
| Final (two `dlygate4sd3_1` per canary, offload, density 80) | 33 ns | +3.53 ns | CONF canary; real CONF +6.14, PRED +7.40, its canary +5.00; typical corner +14.89, hold +0.058 |

All rows **(VERIFIED, progress log)**. The rented machine reproduced step-4 build 3 exactly (+2.057 ns) **(VERIFIED
by the owners' report)**.

**Placement sensitivity (step 4).** The real CONF flop's slow-corner slack was +2.18, +1.16 and +2.46 ns in three
builds that differed only in the canary cells: placement moves the critical path by more than a nanosecond. A
canary with a fixed lead can therefore be ahead in one build and fail setup in the next; this is why step 4 shortened
the chain and why the owners chose 33 ns, which gives room for a longer one. Read the slack of the real flops and of
the canaries after every build.

## The canaries

A shadow flop on PRED and on CONF samples the same D through a delay chain; it disagrees when the margin is used
up. The lead is the chain's delay.

| Build | Chain | Lead, typical | Lead, slow corner |
| --- | --- | --- | --- |
| Step 4, build 3 (30 ns) | one `dlygate4sd1_1` | 0.20 ns (CONF), 0.22 ns (PRED) | 0.40 ns, 0.48 ns **(VERIFIED, progress log)** |
| Final (33 ns) | two `dlygate4sd3_1` | 1.26 ns (CONF), 1.23 ns (PRED) | 2.61 ns, 2.41 ns |

Final build: lead = slack of the flop minus slack of its canary; fast corner (ff) 0.83 ns (CONF), 0.85 ns (PRED)
**(VERIFIED: progress log, step 5, and docs/mosaic_step5_build/sta_*.txt)**. The liberty estimate was about 1.1 ns
typical and 2.2 ns slow (2 × 0.56–0.58 ns, 2 × 1.07–1.16 ns); the measured lead is a little longer. The canary must keep at least 0.3 ns of slack itself: +3.53 ns (CONF) and +5.00 ns (PRED) at the slow corner. The comparison is made in the clock after PRED and CONF were
loaded, so fault event 4 comes one clock after the ACK.

## Write-pulse widths (W0–W2, FCTL[7:5])

At `u_wpx/X`, from the step-4 final netlist and parasitics (OpenSTA, latest edges; ns) **(VERIFIED, progress log)**:

| Setting (delay cells) | 1 (1) | 2 (2) | 3 (3) | 4 (4) | 5 (6) | 6 (8) | 7 (12) | 0 (standard) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| tt | 0.73 | 1.33 | 1.98 | 2.59 | 3.74 | 4.93 | 7.17 | 14.6 at 30 ns |
| ss | 1.49 | 2.68 | 3.94 | 5.12 | 7.36 | 9.73 | 14.10 | 14.2 at 30 ns |
| ff | 0.47 | 0.90 | 1.33 | 1.76 | 2.55 | 3.37 | 4.93 | 14.7 at 30 ns |

The short settings depend on the delay line, not the clock; the standard pulse is half a clock, so at 33 ns about
16 ns **(ESTIMATED: half the period minus the gate skew seen at 30 ns)**. Widths of the final build: not measured
**(DON'T KNOW: OpenSTA on `u_wpx/X` of the final netlist and SPEF, as in step 4, would settle it; the delay line is
the same 12 protected cells, so the short settings should be close to the table above, ESTIMATED)**. The row and gate cells after `u_wpx` reshape these further (not timed: latch gates are not endpoints).
Liberty delay of `dlygate4sd3_1` A → X (rise / fall): tt 0.561 / 0.582 ns, ss 1.071 / 1.158 ns, ff 0.390 / 0.394 ns;
the latch's own minimum gate pulse is 0.14 ns tt, 0.29 ns ss.

## Latency and the CPU clock it serves (O3, IR-07)

The acknowledge and the prediction leave the chip at the third clock edge after `REQ` arrives: 3 × 33 = 99 ns plus
output delay. With the pads' round trip and the FPGA's synchroniser a prediction is back within about 130 ns
**(ESTIMATED: clock periods plus the platform figures of C-03)**. Measured at the pins in co-simulation
**(VERIFIED in the perceptron round)**: every acknowledge of the CPU runs comes exactly 3 clocks after the request,
so 4 chip clocks per CPU cycle suffice (7.6 MHz at 30.3 MHz); two requests in one CPU cycle need 7 chip clocks (8 with
the adapter: 3.8 MHz). The 1 MHz CPU of IR-07 is far inside both.

## What static timing does not cover

- **Latch data and gate pins** (the tables): reported as unconstrained, by design (docs/SPEC.md 6.2). Data and
  select terms change a full clock before the write pulse and not until half a clock after it.
- **The twin's tri-state enable** (`einvp_1` TE → Z): disabled in `src/bpx.sdc` as a false path; the condition (no
  flop takes table read data while the write sequencer is busy) is a proven formal assertion. The data arc and the
  read paths stay timed.
- **The delay line and the short pulse gate** (`u_dly`, `u_wpx`): no constrained path runs through them (OpenSTA:
  no paths), so `bpx.sdc` needed no new exception. Their widths are measured above, and the effect on the latches is
  a silicon experiment (docs/experiments.md, experiment 5).
- **The ring oscillators**: combinational loops of named cells, not timed; their divided output enters the clock
  domain through a synchroniser flop.

**XR-04, XR-05.** The design is fully static: one clock, no minimum frequency; the latches hold without a clock (the
leaky twin does not, by design, and is off after reset). Every output pin is driven by a flop.
