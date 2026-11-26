# BPX-1 timing (DR-03, PF-04, XR-04, XR-05)

All numbers **(VERIFIED)** from the local LibreLane sign-off STA of the build of the perceptron design with its
final parameters, 26 Nov 2026.

| Corner | Setup worst slack at 30 ns (33.3 MHz) | Hold |
| --- | --- | --- |
| Slow: ss, 100 °C, 1.60 V | +3.87 ns | |
| Typical: tt, 25 °C, 1.80 V | +14.00 ns | |
| All corners | 0 violations | worst slack +0.06 ns, 0 violations |

**Signed-off clock: 33.3 MHz (30 ns)**, with 3.9 ns (13 %) of margin at the slow corner: the slowest path is
26.1 ns there (38 MHz), 16.0 ns at the typical corner (62 MHz). PF-04 asks for a clock fast enough for IR-07 (a
1 MHz CPU): met about eight times over (below).

**Why 30 ns and not the 25 ns of the tagged-table design.** The perceptron's sum is in the predict cycle. A trial
build at 25 ns closed everywhere except on that sum: −2.58 ns on `CONF`, −1.92 ns on `PRED`, 14 failing endpoints at the slow corner, every
one of them request-pending decode → read-port select → gshare table (128 entries, the slowest read) → adder of the
four weights and three reference inputs → sign or threshold compare → driving-predictor select → output flop
**(VERIFIED: path report of that build)**. The next non-sum path had +6.98 ns. The alternatives are in
docs/perceptron.md 6: the chosen one keeps the acknowledge at the third clock (3 × 30 = 90 ns), the other adds a
clock (4 × 25 = 100 ns) and changes the sequencer. Reserve for a later revision: a late-select sum (the gshare
counter only selects among four precomputed comparisons), estimated at 2 ns **(ESTIMATED, not built)**.

**Latency and the CPU clock it serves (O3, IR-07).** The acknowledge and the prediction leave the chip at the third
clock edge after `REQ` arrives: at most 3 × 30 ns plus output delay. With the 20 ns round trip of the pads and about
10 ns in the FPGA to synchronise `ACK`, a prediction is back within about 120 ns **(ESTIMATED: clock periods plus
the platform figures of C-03)**, so a CPU of up to about 8 MHz gets its answer in the cycle it asks (9 MHz with the
tagged-table design at 25 ns). Measured in the co-simulation at the pins, in chip clocks **(VERIFIED)**: with one
request per CPU cycle every acknowledge of the CPU runs comes exactly 3 clocks after the request (475 million
requests), so 4 chip clocks per CPU cycle suffice (8.3 MHz at this clock); two requests in one CPU cycle need 7 chip
clocks (SPEC 9, rule 7; 4.1 MHz with the 8 clocks the adapter needs). Back-to-back requests in the random tests are
acknowledged after 3 clocks in 92 % of cases and after 4 to 6 clocks when the training of the previous resolve is
still being written **(VERIFIED)**.

**What static timing does not cover.** The data and gate pins of the 1,329 latches are reported as unconstrained:
no clock reaches a latch, by design (docs/SPEC.md 6.2). Their timing holds by construction: data and select terms
change a full clock before the write pulse and not until half a clock after it; the pulse is half a clock wide
(15 ns at 33 MHz, against gate delays of 1 to 3 ns). The complete read path is covered by static timing. The
gate-level test exercises every table through these paths (unit delays).

**XR-04, XR-05.** The design is fully static: one clock, no minimum frequency; the latches hold without a clock.
Every output pin is driven by a flop, so there is no combinational path from an input pin to an output pin
**(VERIFIED by inspection of `src/bpx_chip.v`: `uo_out` is a concatenation of flop outputs)**.
