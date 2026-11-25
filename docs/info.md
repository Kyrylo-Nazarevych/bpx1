<!---

This file is used to generate your project datasheet. Please fill in the information below and delete any unused
sections.

You can also include images in this folder and reference them in the markdown. Each image must be less than
512 kb in size, and the combined size of all images must be less than 1 MB.
-->

## How it works

BPX-1 is a conditional-branch direction predictor for an external host: a pipelined RISC-V CPU on an FPGA, a
microcontroller, or the Tiny Tapeout demo board. The host sends a 10-bit branch identifier and gets back a
taken / not-taken prediction; later it reports the real outcome and the chip learns from it.

Four predictors see every branch: the default predictor (tagged tables indexed with geometric history lengths over
a bimodal base) and three classic references (bimodal, gshare, and a tournament of the two). One of them, chosen at
runtime, drives the prediction returned to the host. The chip counts the resolved branches and the mispredictions
of each predictor, so algorithms can be compared on the same branch stream, on silicon.

Up to four predictions can be in flight. Predictions made while older branches are unresolved use the predicted
directions; when an outcome contradicts the prediction that was returned, the chip discards everything younger and
restores its history. Training reads only committed state, so what the chip has learned never depends on what was
speculated.

The predictor state lives in latch tables written by pulses built from flip-flop outputs. The host port is a
two-phase handshake that works at any clock ratio; a separate SPI port reads statistics and every state bit while
predictions run. The full specification is `docs/SPEC.md`.

**Note:** until gate G2 the repository holds the area probe of the storage tables (see the top-level source), not
the predictor.

## How to test

Predict port: put `OP` and `ID` on the pins, toggle `REQ`, wait for `ACK` to toggle, read `PRED`. No timing
requirement in either direction: it can be bit-banged. One request of `OP = 2` or `3` with an empty queue predicts
branch `ID` and trains on its outcome, which replays a recorded trace one record per request.

Control port: SPI mode 0 on `CSn` / `SCK` / `MOSI` / `MISO`, `SCK` at most one eighth of the chip clock.

The MicroPython host for the demo board and the bring-up procedure are in `host/` and `docs/bringup.md`.

## External hardware

None for the demo board alone. For real-program speedup measurements: an FPGA board with a pipelined RV32 soft
core, wired to the demo board's three PMOD headers (`plan/FPGA_WIRING.md` of the team's notes).
