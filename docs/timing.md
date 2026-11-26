# BPX-1 timing (DR-03, PF-04, XR-04, XR-05)

All numbers **(VERIFIED)** from the local LibreLane sign-off STA of the build of the final RTL, 26 Nov 2026.

| Corner | Setup worst slack at 25 ns (40 MHz) | Hold |
| --- | --- | --- |
| Slow: ss, 100 °C, 1.60 V | +4.83 ns | |
| Typical: tt, 25 °C, 1.80 V | +11.51 ns | |
| All corners | 0 violations | worst slack +0.06 ns, 0 violations |

**Signed-off clock: 40 MHz**, with 4.8 ns (19 %) of margin at the slow corner; at the typical corner the same
netlist would run at about 74 MHz. PF-04 asks for a clock fast enough for IR-07 (a 1 MHz CPU): met about nine
times over (below).

**Critical path** (slow corner: 20.2 ns in this build, 21.7 ns in the build at G2). Its composition was read in
the G2 build and not again in this one **(ESTIMATED to be the same path)**: request-pending decode → read-port
select → table address → row select with its fan-out buffers → latch read tree → column mux → tag compare →
provider select → queue and output flops. About a third is fan-out buffering of the read selects. Reserve for 50 MHz and beyond: register the lookup one clock
earlier from the captured identifier (it hides in the synchroniser delay and adds no latency).

**Latency and the CPU clock it serves (O3, IR-07).** The acknowledge and the prediction leave the chip at the third
clock edge after `REQ` arrives: at most 3 × 25 ns plus output delay. With the 20 ns round trip of the pads and about
10 ns in the FPGA to synchronise `ACK`, a prediction is back within about 105 ns **(ESTIMATED: clock periods plus
the platform figures of C-03)**, so a CPU of up to about 9 MHz gets its answer in the cycle it asks. Measured in
the co-simulation at the pins **(VERIFIED)**: with one request per CPU cycle no answer is late at 4 chip clocks per
CPU cycle (some are at 3), and in the CPU runs every acknowledge comes exactly 3 clocks after the request (475
million requests); two requests in one CPU cycle need 7 chip clocks (SPEC 9, rule 7). Back-to-back requests in
the random tests are acknowledged after 3 clocks in 92 % of cases and after 4 to 6 clocks when the training of the
previous resolve is still being written **(VERIFIED)**.

**What static timing does not cover.** The data and gate pins of the 1,168 latches are reported as unconstrained:
no clock reaches a latch, by design (docs/SPEC.md 6.2). Their timing holds by construction: data and select terms
change a full clock before the write pulse and not until half a clock after it; the pulse is half a clock wide
(12.5 ns at 40 MHz, against gate delays of 1 to 3 ns). The complete read path is covered by static timing. The
gate-level test exercises every table through these paths (unit delays).

**XR-04, XR-05.** The design is fully static: one clock, no minimum frequency; the latches hold without a clock.
Every output pin is driven by a flop, so there is no combinational path from an input pin to an output pin
**(VERIFIED by inspection of `src/bpx_chip.v`: `uo_out` is a concatenation of flop outputs)**.
