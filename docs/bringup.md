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
| 1 | Select the project, clock 1 MHz, pulse reset (at least 4 clocks low) | `uo_out[4]` (`READY`) high within 10 clocks; `ACK` equals the `REQ` level | Clock or reset not reaching the chip; project not selected |
| 2 | `h.chip_id()` | `0xB101` | SPI wiring or speed: `SCK` must stay below one eighth of the chip clock; the bit-banged host is far below that |
| 3 | `h.request(OP_SPECIAL, 0)` (NOP) | `ACK` toggles | Handshake: check `REQ` on `uio[7]` |
| 4 | `h.predict_resolve(5, 1)` 20 times | The first answers are 0, then 1; `h.stats()` shows `resolved` = 20 | Tables not learning: go to step 6 |
| 5 | `demo()` | Mispredictions per predictor printed; `errors` = 0 | Compare with the golden model: `model/bpx1.py` replays the same trace |
| 6 | `h.dump(0, 64)`, `h.dump(1, 128)`, `h.dump(2, 16)` … after `CMD_CLEAR_PRED` | 64 × `5`, 128 × `1`, 16 × `3` (the initial values) | A table that does not hold or clear its value: note which entries, see section 4 |

The golden model predicts every pin-level answer: for any sequence of requests, `model/bpx1.py` gives the same
`PRED`, `CONF`, statistics and table contents as the RTL (140 million requests in simulation without a
difference). A difference on silicon is therefore a hardware fault, and the raw dump shows where.

## 3. Things worth measuring

- **Maximum clock.** Raise the clock until `demo()` differs from the model. Signed off at 40 MHz for the slow
  corner; typical silicon should reach about 70 MHz (docs/timing.md). The handshake does not care about the ratio
  between host speed and chip clock.
- **Single-stepping.** Stop the clock anywhere and step it by hand: the design is fully static, and the tables
  (latches) hold without a clock.
- **Statistics while running.** `h.stats()` can be called between any two requests; `stats(clear=True)` restarts
  the counters without losing an event (16-bit counters: read them at least every 65,535 branches).
- **Debug pins.** `CTRL[7:4]` and `CTRL[11:8]` select which internal event appears on `uo_out[5]` and `[6]`
  (docs/SPEC.md 6.4): for a logic analyser when a step above fails.

## 4. If a table misbehaves

The tables are latches written by a pulse that is half a clock period wide, built from two flip-flops
(docs/SPEC.md 6.2). This is the one construction in the chip that static timing analysis does not check completely
(docs/timing.md). If entries fail to write at a high clock but work at a low one, the pulse is too short for that
clock: lower the clock; that is the fix, there is no setting to change. If entries fail at every clock, read them
with `dump`, write them with the raw-access registers (`RAW_ADDR` with the write-mask bits, then `RAW_DATA`), and
read back, to separate a cell that cannot hold from a path that cannot write.

`CTRL` bit 3 (freeze) stops all learning: predictions then depend only on the table contents, which makes a
suspect entry reproducible.

## 5. Interface for the U1 FPGA adapter

The complete contract is docs/SPEC.md section 9; the pin-level protocol is section 3. In short:

- Outputs of the FPGA: `ID[9:0]`, `OP[1:0]`, `REQ`; inputs: `ACK`, `PRED`, `FULL`, `READY`. 3.3 V.
- Launch `ID`/`OP` and the `REQ` toggle from registers of the FPGA; synchronise `ACK` with two flip-flops, then
  take `PRED`. With the chip at 40 MHz the answer is on the pins 3 chip clocks (75 ns) after `REQ` is seen, 4 to 6
  clocks when the request follows a resolve directly.
- The SPI control port can be driven by the FPGA or left to the demo board's microcontroller; the two ports are
  independent.
- Identifier: `fold_id(pc)` in `host/bpx1.py` (XOR of the three 10-bit slices of `pc[31:2]`).

## 6. microcotb

The Tiny Tapeout firmware runs cocotb-style tests on the board (microcotb). `test/test.py` uses only pin values and
clock cycles, so its two tests port directly; the golden model `model/bpx1.py` is plain Python without imports
beyond `dataclasses` and `typing`, which the firmware may lack: replace the dataclass by a plain class if so.
This port has not been done **(open item)**.
