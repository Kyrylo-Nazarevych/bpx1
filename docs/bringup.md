# BPX-1 bring-up (DR-05, VR-07)

Nothing here has been run on silicon or on a demo board yet **(DON'T KNOW until the chip is back; the sequences are
the ones the simulation tests use)**. The host code is `host/bpx1.py`.

## 1. What you need

- The Tiny Tapeout demo board with the chip, its MicroPython firmware and SDK (use case U2). Nothing else.
- For speedup measurements with a real CPU (use case U1): an FPGA board with the CPU of docs/SPEC.md section 9,
  wired to the three PMOD headers (section 5 below).

## 2. First contact, in order

Each step isolates one thing, so the first failing step says where the problem is.

| Step | Do | Expect | If not |
| --- | --- | --- | --- |
| 1 | Select the project, clock 1 MHz, pulse reset (at least 4 clocks low) with `CSn` high (section 5: the reset strap) | `uo_out[4]` (`READY`) high within 10 clocks; `ACK` equals the `REQ` level; FSTAT = 0 | Clock or reset not reaching the chip; project not selected |
| 2 | `h.chip_id()` | `0xB101` | SPI wiring or speed: `SCK` must stay below one eighth of the chip clock; the bit-banged host is far below that |
| 3 | `h.request(OP_SPECIAL, 0)` (NOP) | `ACK` toggles | Handshake: check `REQ` on `uio[7]` |
| 4 | `h.predict_resolve(5, 1)` 20 times | The first answers are 0, then 1; `h.stats()` shows `resolved` = 20 | Tables not learning: go to step 6 |
| 5 | `demo()` | Branches and mispredictions of the driving predictor (chip counters); `errors` = 0 | Compare with the golden model: `model/bpx1.py` replays the same trace |
| 6 | `h.dump(0, 64)`, `h.dump(1, 128)`, `h.dump(8, 32)` … `h.dump(11, 32)` after `CMD_CLEAR_PRED` | 64 × `5`, 128 × `1`, then 32 × `0`, `15`, `0`, `15` for the four weight tables (the initial values; 15 is −1 in four bits) | A table that does not hold or clear its value: note which entries, see section 4 |

The golden model predicts every pin-level answer: for any sequence of requests, `model/bpx1.py` gives the same
`PRED`, `CONF`, prediction pins, statistics and table contents as the RTL (420 million requests in simulation
without a difference for the perceptron design; 421 million for the mosaic's final RTL: docs/verification.md). A difference on silicon with
FCTL = 0 is therefore a hardware fault, and the raw dump shows where.

## 3. Things worth measuring

- **Maximum clock.** Raise the clock until `demo()` differs from the model. Signed off at 30.3 MHz (33 ns) for the
  slow corner with +3.53 ns of setup slack; typical corner +14.89 ns **(VERIFIED, progress log, step 5;
  docs/timing.md)**. The canaries (FSTAT[4]) should warn first:
  docs/experiments.md, experiment 6. The handshake does not care about the ratio between host speed and chip clock.
- **Single-stepping.** Stop the clock anywhere and step it by hand: the design is fully static, and the tables
  (latches) hold without a clock.
- **Statistics while running.** `h.stats()` can be called between any two requests; `stats(clear=True)` restarts
  the counters without losing an event (16-bit counters: read them at least every 65,535 branches). The chip counts
  resolved branches and the mispredictions of the answers it gave on `PRED`; registers 0x09–0x0E read 0 (statistics
  offload). Each predictor's mispredictions are counted by the host from the four prediction pins (section 5).
  `host/bpx1.py` `stats()` reads the two counters; `replay()` counts each predictor's mispredictions from the pins
  (DBG0, DBG1 through `uo_out`; PBIM, PGSH through `uio_out`, given a `get_uio` function) **(VERIFIED:
  host/bpx1.py; not run on hardware)**.
- **Debug pins.** `CTRL[7:4]` and `CTRL[11:8]` select which internal event appears on `uo_out[5]` and `[6]`
  (docs/SPEC.md 6.4): for a logic analyser when a step above fails.

## 4. If a table misbehaves

The tables are latches written by a pulse that is half a clock period wide, built from two flip-flops
(docs/SPEC.md 6.2). This is the one construction in the chip that static timing analysis does not check completely
(docs/timing.md). If entries fail to write at a high clock but work at a low one, the pulse is too short for that
clock: lower the clock; that is the fix, there is no setting to change. If entries fail at every clock, read them
with `dump`, write them with the raw-access registers (`RAW_ADDR` with the write-mask bits, then `RAW_DATA`), and
read back, to separate a cell that cannot hold from a path that cannot write. FCTL[7:5] (PULSE) shortens the pulse
of W0–W2 only, as an experiment; it is 0 after reset, and must be 0 for this test.

`CTRL` bit 3 (freeze) stops all learning: predictions then depend only on the table contents, which makes a
suspect entry reproducible.

**If the default predictor misbehaves** (its sum is the slowest logic of the chip): lower the clock first. Then
switch its parts off one at a time, `h.set_drive(0, features=…)` (docs/SPEC.md 7: 1 reference inputs, 2 negation,
4 adaptive threshold; 0 is the plain perceptron, the safe mode), comparing the statistics with the model run with
the same `feat`. Whatever the perceptron does, `h.set_drive(1)`, `(2)` or `(3)` makes a reference predictor answer:
they share no logic with the sum. The four prediction pins (`DBG0`, `uio[0]`, `uio[1]`, `DBG1`) show every
predictor's answer for each request without the control port.

## 5. Interface for the U1 FPGA adapter

The complete contract is docs/SPEC.md section 9; the pin-level protocol is section 3. In short:

- Outputs of the FPGA: `ID[7:0]`, `OP[1:0]`, `REQ`; inputs: `ACK`, `PRED`, `CONF`, `FULL`, `READY`, and for
  analysis the four predictions `DBG0` (default), `PBIM`, `PGSH`, `DBG1` (tournament), valid with `ACK`. 3.3 V.
  `uio[0]` and `uio[1]` are **outputs of the chip**: the FPGA and the demo board must leave them undriven.
- Launch `ID`/`OP` and the `REQ` toggle from registers of the FPGA; synchronise `ACK` with two flip-flops, then
  take `PRED`. With the chip at 30.3 MHz (33 ns) the answer is on the pins 3 chip clocks (99 ns) after `REQ` is
  seen, 4 to 6 clocks when the request follows a resolve directly.
- The SPI control port can be driven by the FPGA or left to the demo board's microcontroller; the two ports are
  independent.
- Identifier: `fold_id(pc)` in `host/bpx1.py` (XOR of the 8-bit slices of `pc[31:2]`).
- **Misprediction counts per predictor are the FPGA's job** (statistics offload). With every `ACK` of a predict the
  four predictions are on pins: `PRED` (the driving predictor), `DBG0` (default) and `DBG1` (tournament) with the
  reset selections of CTRL, `PBIM`, `PGSH`. The FPGA keeps each predictor's prediction per in-flight branch and
  compares it with the outcome it sends in the resolve. The chip keeps only the branch count and the driving
  predictor's mispredictions, which a slow SPI-only host can read. When an experiment puts fault events on the DBG
  pins, the default's and the tournament's predictions are not on pins for that run (docs/experiments.md 1).
- **During reset** the FPGA drives `CSn` high (or `OP` ≠ 3, or `MOSI` = 0) and `SCK` low. The **reset strap** is
  `OP = 3`, `CSn = 0` and `MOSI = 1` at the last clock edge of reset: the chip then keeps its tables (warm reset,
  FSTAT[11] KEPT). `OP = 3` alone is harmless, but an FPGA that resets the chip in the middle of an SPI word with
  `MOSI` high could hit the strap. For a deliberate strap, hold the three pins from 4 clocks before `rst_n` rises to
  4 clocks after, keep `SCK` low, then take `CSn` high for at least 4 clocks before the first SPI word
  (`h.reset(strap=True)`; docs/SPEC.md 5).
- **Experiments** (docs/experiments.md): the FPGA provides the clock control (period steps, single short cycles), a
  lock-step twin of the RTL, event counters with time stamps on the DBG pins, a raw-access injector over SPI and a
  frequency counter for the ring oscillators; the table in its section 14 lists what each experiment needs. Every
  experiment returns to the safe state: FCTL = 0, CTRL[15] = 0, CMD bits 2 and 1.

## 6. Who drives clock, reset and project selection

Question of the owners (26 Nov 2026): can the FPGA drive the chip's clock, reset and project selection directly, or
must the demo board's microcontroller stay in between? Sources: Tiny Tapeout documentation (specs/pcb, specs/clock,
specs/gpio), the firmware source `tt-micropython-firmware` and the multiplexer documentation
**(all VERIFIED by a research agent that read them on 26 Nov 2026; not re-read by the author of this document)**.

| Signal | Demo board | Answer |
| --- | --- | --- |
| `clk` | RP2 GPIO16. The firmware drives it in `RPMode.ASIC_RP_CONTROL` and makes it an **input** in `RPMode.ASIC_MANUAL_INPUTS` and `RPMode.SAFE` (source comment: "leave clk and reset as inputs, for manual operation"). | **The FPGA can drive it directly**: select the project, then put the board in `ASIC_MANUAL_INPUTS` (or call `tt.clock_project_stop()` and release the pin). Never drive it while the RP2 does. |
| `rst_n` | RP2 GPIO14; an input in the same two modes. | **The FPGA can drive it directly**, same condition. |
| Project selection (`ctrl_ena`, `ctrl_sel_inc`, `ctrl_sel_rst_n`) | RP2 GPIO 0 to 2, **always outputs** in the firmware. The selection is a counter inside the chip's multiplexer: reset pulse, N increment pulses, then `ctrl_ena` high; the project's `ena` needs `ctrl_ena` to stay high. | **The microcontroller must do the selection and keep holding these three lines.** It is then out of the signal path: the predict port, the control port, the clock and the reset can all come from the FPGA. |
| The same on the chip's breakout board alone (no demo board) | The breakout's connector symbol carries `proj_clk`, `proj_rst`, `ctrl_ena`, `ctrl_rst`, `ctrl_inc`. | A custom carrier or the FPGA can drive everything, including the selection sequence **(ESTIMATED from the schematic symbol; the pin table of the breakout was not read)**. |

Limits that matter for the FPGA link **(VERIFIED by the same agent, specs/gpio and specs/clock)**: 3.3 V I/O, not
5 V tolerant; input pads to 66 MHz, **output pads to 33 MHz** (BPX-1's outputs change at most once per request, so
the 30.3 MHz chip clock is below this limit even for a signal that toggled every clock); up to 10 ns from the clock pad to the project's clock.


**(DON'T KNOW)**: which header pins of the demo board carry `clk` and `rst_n`, and whether series resistors sit
between the RP2 and those nets. The schematic PDF of the board (`tt-demo-pcb`, file `tt-etr-dbv3.pdf`) settles it;
it could not be opened by the tools used here.

## 7. microcotb

The Tiny Tapeout firmware runs cocotb-style tests on the board (microcotb). `test/test.py` uses only pin values and
clock cycles, so its two tests port directly; the golden model `model/bpx1.py` is plain Python without imports
beyond `dataclasses` and `typing`, which the firmware may lack: replace the dataclass by a plain class if so.
This port has not been done **(open item)**.
