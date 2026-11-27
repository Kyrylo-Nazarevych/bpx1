# BPX-1 design specification (DR-01)

Status: **perceptron default predictor** (owner requirement of 26 Nov 2026; exploration and decisions in
docs/perceptron.md). The RTL (`src/bpx_*.v`) and the golden model (`model/bpx1.py`) implement this document. The
earlier design with tagged tables, verified to gate G3, is the tag `g3-tage`. Evidence labels as in the requirements specification, section 10.

## 1. What the chip is

A conditional-branch direction predictor for an external host. Four predictors run side by side on every branch:
the **default** predictor and three classic references (bimodal, gshare, tournament of the two). One of them, or
static not-taken, **drives**: its prediction is the one returned to the host. A second, independent serial port gives
control, statistics and access to every state bit while the predict port keeps running.

```
            ui_in[7:0] uio[3:2]            uio[7]                       uo[0] uo[1] uo[2]  uo[3]  uo[4]
              ID         OP                 REQ                          ACK   PRED  CONF   FULL   READY
               |          |                  |                            ^     ^     ^      ^      ^
        +------v----------v------------------v----------------------------+-----+-----+------+------+-----+
        |  synchronisers (2 flops per input pin)                        output flops                      |
        |        |                                                           ^                            |
        |   +----v--------------------- operation sequencer -----------------+---+        +-----------+   |
        |   | accept / refuse, order predict and resolve, drive read port,       |        | statistics|   |
        |   | write sequencer, in-flight queue (4), speculative + committed      +------->| counters +|   |
        |   | history, misprediction repair, staging of the oldest branch        |        | snapshot  |   |
        |   +----+------------------------------^--------------------------------+        +-----^-----+   |
        |        | read address (id, history)   | read data                                     |         |
        |   +----v------------------------------+---------------------------------+        +----+------+  |
        |   | predictor datapath: hashes, latch tables (one read port, one write  |<------>| control   |  |
        |   | port per table), weight sum, combine, confidence, training logic    |  state | port (SPI)|  |
        |   +---------------------------------------------------------------------+ access +--+-----^--+  |
        +----------------------------------------------------------------------------------- | --- | ------+
                                                                              uo[7] MISO <---+     +--- uio[4] CSn, uio[5] SCK, uio[6] MOSI
                                                                              uo[5], uo[6]: debug event pins; uio[0], uio[1]: prediction pins
```

One clock domain (`clk`). Every input pin, including `rst_n`, passes through two flops before it is used (RR-01).
No combinational path from any input to any output (XR-05): every output pin is a flop.

## 2. Pin map

`uio[7:2]` are **inputs** and `uio[1:0]` are **outputs**, always (`uio_oe = 0x03`): no pin changes direction, so
the chip can never fight a host that leaves `uio[1:0]` undriven.

| Pin | Name | Dir | Function |
| --- | --- | --- | --- |
| `ui_in[7:0]` | `ID[7:0]` | in | Branch identifier (`ID[1:0]` is the sub-operation when `OP = 00`). |
| `uio[0]` | `PBIM` | **out** | Prediction of the bimodal predictor for the last predict. Changes with `ACK`. |
| `uio[1]` | `PGSH` | **out** | Prediction of the gshare predictor for the last predict. Changes with `ACK`. |
| `uio[3:2]` | `OP[1:0]` | in | Operation (section 3). |
| `uio[4]` | `CSn` | in | Control port chip select, active low. |
| `uio[5]` | `SCK` | in | Control port clock (SPI mode 0). |
| `uio[6]` | `MOSI` | in | Control port data in. |
| `uio[7]` | `REQ` | in | Request: every **toggle** asks for one operation. |
| `uo_out[0]` | `ACK` | out | Acknowledge: toggles once per request. A request is pending while `REQ != ACK`. |
| `uo_out[1]` | `PRED` | out | Prediction of the driving predictor (1 = taken). Valid from the `ACK` toggle of a predict until the next `ACK` toggle. |
| `uo_out[2]` | `CONF` | out | Confidence of the driving predictor's prediction (1 = high; 0 for static not-taken). Same validity as `PRED`. |
| `uo_out[3]` | `FULL` | out | The in-flight queue is full: a plain predict would be refused. |
| `uo_out[4]` | `READY` | out | Reset and table initialisation are complete; requests are accepted. |
| `uo_out[5]`, `uo_out[6]` | `DBG0`, `DBG1` | out | Internal events, selected at runtime (section 6.4). After reset: the predictions of the default predictor and of the tournament for the last predict, so that with `PBIM` and `PGSH` all four predictions are on pins. |
| `uo_out[7]` | `MISO` | out | Control port data out (0 while `CSn` is high). |

Why these pins **(VERIFIED by the research agent against the demo-board firmware and the RP2350 function table)**:
on the RP2350 demo board `uio[5]`, `uio[6]` and `uo_out[7]` are the SCK, TX and RX pins of hardware SPI1, so the
control port can be driven by `machine.SPI` (IR-03); `ui_in[7:0]` are eight consecutive GPIOs, which a PIO
program drives as one output group, with `OP`, `REQ` and `ACK`/`PRED` next to them. Tiny Tapeout's
recommended SPI pinout (CS, MOSI, MISO, SCK on `uio[0..3]`) does not line up with any RP2350 hardware SPI and needs an
output on `uio`; it is a recommendation, not a rule, and is not followed.

**Identifier width.** Eight bits. Measured on 4,000 predictor candidates and all programs, 8 against 10 bits
changes the best 200 candidates by −0.00001× of speedup and −0.003 points of accuracy on average (docs/perceptron.md
7): no table has more than 128 entries and none is tagged, so further identifier bits only reshuffle which
branches collide. The two pins this frees carry the bimodal and gshare predictions.

## 3. Host protocol (predict port)

### 3.1 Handshake

Two-phase, level-insensitive, no timing requirement in either direction (IR-01, IR-04):

1. The host puts `OP` and `ID` on the pins.
2. Not earlier than that, the host toggles `REQ`.
3. The chip performs the operation, sets `PRED` and `CONF`, and toggles `ACK` in the same clock edge.
4. The host may change `OP`/`ID` and toggle `REQ` again any time after it has seen `ACK` toggle.

```
ID, OP  ====<  valid, held until ACK  >======<  next  >====
REQ     ______/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\______________      one toggle = one request
clk     _/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_
              e1  e2  e3                                    REQ is sampled at e1, synchronised at e2,
ACK     _________________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\________    the operation runs between e2 and e3,
PRED    =================<  valid until next ACK >======    ACK and PRED change together at e3
```

The identifier is used from the **first** synchroniser flop in the cycle the synchronised request is detected: it
was captured from the pins at least one full clock after `REQ` toggled, so it only has to be stable no later than
`REQ` (the pads' 2 ns skew is covered). **Latency:** the answer leaves the chip at the third clock edge after the
request arrives, at most 3 clock periods + output delay, whatever the predictor computes. That is the floor IR-01 and
RR-01 set (two synchroniser flops, one cycle of work).

### 3.2 Operations

| `OP` | `ID[1:0]` | Operation | Meaning |
| --- | --- | --- | --- |
| `01` | – | PREDICT(ID) | Predict branch `ID`; append it to the in-flight queue. |
| `10` | – | PREDICT(ID) + RESOLVE not-taken | Both in one request. |
| `11` | – | PREDICT(ID) + RESOLVE taken | Both in one request. |
| `00` | `10` | RESOLVE not-taken | The oldest in-flight branch was not taken. |
| `00` | `11` | RESOLVE taken | The oldest in-flight branch was taken. |
| `00` | `01` | FLUSH | Discard every in-flight prediction; nothing is trained (FR-05). |
| `00` | `00` | NOP | Acknowledged, no effect (handshake test). |

Semantics, identical in the golden model and the RTL and independent of timing (FR-06):

- **PREDICT** reads the trained tables as they are, with the speculative history (the committed history followed by
  the driving predictor's predictions for the in-flight branches, FR-04). It stores `ID` and the four predictions in
  the queue and shifts the returned prediction into the speculative history. It changes nothing else (FR-01).
- **RESOLVE** takes the oldest queue entry, re-reads the tables for that branch with the **committed** history,
  trains all four predictors from that read and the outcome, shifts the outcome into the committed history, and
  counts the branch and each predictor's stored prediction against the outcome (FR-02, FR-08). If the outcome differs
  from the prediction that was returned for it, every younger entry is discarded and the speculative history is set
  to the committed history (FR-04). Because training reads only committed state, the trained state after n resolves
  is exactly the state of a strictly sequential run over the same n branches, whatever was predicted, discarded or
  driving in between.
- **PREDICT + RESOLVE** is the sequence PREDICT, then RESOLVE: the prediction does not see this resolve's training;
  the resolve applies to the oldest entry after the predict was appended. With an empty queue this is "predict branch
  X, then train on X": one request per trace record (U2). With a non-empty queue it is what a pipelined CPU sends in
  a cycle where one branch is decoded and an older one has resolved (U1). Exception, by state only: if the queue is
  full, the resolve goes first (it frees the slot the predict needs).
- **Capacity** is 4 in-flight predictions, the peak of the U1 CPU of section 9 (FR-03).

### 3.3 Misuse (IR-09)

Every request is acknowledged, so the handshake can never hang (RR-03). Misuse is refused without touching predictor
state, sets a sticky flag and increments the saturating error counter:

| Case | Behaviour |
| --- | --- |
| RESOLVE with nothing in flight | Refused. In a combined request the predict part is still performed. |
| PREDICT with the queue full and no resolve in the same request | Refused; `PRED` = 0. `FULL` was high before the request: a correct host waits. |
| `REQ` toggled back before `ACK` (request withdrawn) | If the chip had not started, nothing happens and the withdrawal is counted; otherwise it is a new request. |
| Reset (pin or soft) during a request | All in-flight work is dropped; after reset `ACK` is set equal to the synchronised `REQ`, so no stale request is seen. |
| `ID`/`OP` changed while a request is pending | The values present in the cycle of acceptance are used (cannot be detected). |

## 4. Control port (FR-07…FR-10, IR-02, IR-03)

SPI mode 0, MSB first, `CSn` frames a transaction. First byte: bit 7 = 1 write / 0 read, bits 5:0 = address;
then 16-bit words, the address incrementing after each. `SCK` is sampled by `clk` (it is not a clock on the chip):
each `SCK` phase must last at least four `clk` periods (`f_SCK ≤ f_clk / 8`). The port works at any `clk`, including
single steps. Raw table reads use cycles the predict port leaves free and never delay a request; a raw table
*write* may delay a coinciding request by up to two clocks (IR-02).

| Addr | Name | Access | Content |
| --- | --- | --- | --- |
| 0x00 | ID | R | `0xB101` |
| 0x01 | CTRL | RW | `[2:0]` driving predictor: 0 default, 1 bimodal, 2 gshare, 3 tournament, 4–7 static not-taken; `[3]` freeze learning; `[7:4]` DBG0 event; `[11:8]` DBG1 event; `[14:12]` features of the default predictor (section 7): `[12]` reference inputs, `[13]` negation in memory, `[14]` adaptive threshold; `[15]` MASK: the leaky twin's code mask (space 12 below; docs/mosaic.md 7.7). A write that changes MASK invalidates the staging register, like FCTL[0] and FCTL[3:2]; twin words stored before the change read as faulty until rewritten, so clear the predictors (CMD bit 1) after switching it. After reset: `0x7DA0`. |
| 0x02 | CMD | W | bit 0 soft reset; bit 1 clear predictors (tables, histories, in-flight work); bit 2 clear statistics, error flags, FSTAT, the fault count and DEMOTED; bit 3 snapshot the statistics; bit 4 snapshot and clear the counters in one step (no event lost or counted twice) |
| 0x03 | STATUS | R | `[2:0]` in-flight count; `[3]` ready; `[4]` staging valid; `[5]` request pending; `[6]` table write in progress; `[7]` raw access pending; `[8]` resolve refused (queue empty); `[9]` predict refused (queue full); `[10]` request withdrawn; `[11]` statistics overflow (flags are sticky) |
| 0x04 | ERRCNT | R | `[7:0]` refused or withdrawn requests, saturating |
| 0x05–0x0E | STATS | R | Snapshot taken by CMD bit 3, two words (low, high) per counter: resolved branches, then mispredictions of default, bimodal, gshare, tournament. Counters are 16 bits and saturate (never wrap; overflow is flagged in STATUS). For exact totals of longer runs use snapshot-and-clear at least every 65,535 branches and add the snapshots up (FR-08). |
| 0x0F | RAW_ADDR | RW | `[6:0]` entry; `[11:8]` space: 0 bimodal+chooser, 1 gshare, 7 small registers, 8–11 weight tables 0–3, 12 the leaky twin of W3; `[13:12]` field write enables; `[15]` address + 1 after each RAW_DATA read |
| 0x10 | RAW_DATA | RW | Read: the addressed entry (fetched after RAW_ADDR is written and after each read). Write: the entry fields enabled in RAW_ADDR. |
| 0x11 | FCTL | RW | Mosaic experiments (docs/mosaic.md 7.2); 0 = the plain chip. `[0]` TWIN_EN: the leaky twin's read data is let through (otherwise forced to 0); `[1]` TWIN_USE: the default predictor sums the twin's weight instead of W3's (needs `[0]`); `[3:2]` CODE of the twin: 0 none, 1 parity → abstain, 2 SEC-DED, 3 detect all → abstain; `[4]` RESTORE: restore-on-use; `[7:5]` PULSE: write pulse of the weight tables W0–W2: 0 standard (half a clock), 1–7 shortened to the delay of 1, 2, 3, 4, 6, 8, 12 delay cells (docs/mosaic.md 7.4; a silicon experiment, no effect on the function in simulation; the other tables always get the standard pulse); `[13]` DEMOTE: after the fourth detected fault the tournament drives wherever CTRL selects the default predictor (docs/mosaic.md 7.6); `[14]` DBGF: the DBG selections of CTRL index the fault events; `[15]` CTEST: canary self-test, the canary fires in the clock after every answered predict request; `[8]` GRAND: gshare probabilistic hysteresis, a gshare counter at a limit (0 or 3) that training would move away from it moves only if the random bit is 1; `[9]` GSRC: the random bit is 0 bit 6 of a 7-bit LFSR (x^7 + x^6 + 1, 0x55 after reset and clear, one step per training whatever FCTL says), 1 the oscillator bit (the selected ring's divider bit 0 through two flops); `[10]` ROA, `[11]` ROB: ring oscillator A, B runs (two identical rings; A is meant to be kept running, B rested); `[12]` ROSEL: the divider (fault event 8) and the oscillator bit take ring 0 = A, 1 = B (docs/mosaic.md 7.7). Reset 0, also by soft reset. A write that changes `[0]` or `[3:2]` invalidates the staging register (it is read again). |
| 0x12 | FSTAT | R | Fault flags, cleared by CMD bit 2 and by reset: `[0]` TW_ERR a predict read a twin word that is not the code word of W3's weight; `[1]` TW_COR SEC-DED corrected the word; `[2]` TW_DET the code detected an error it did not correct (the weight abstained); `[3]` TW_SDC the twin's weight was not abstained and differed from W3's (bits 0–3 set only at executed predicts with TWIN_EN = 1); `[4]` CANARY: a canary flop of PRED or CONF disagreed with its flop in the clock after a predict was answered (timing margin used up, or CTEST); `[5]` queue checker (count above 4, or FULL inconsistent with it); `[6]` staging checker (staging valid or a resolve pending with an empty queue, or resolve-first done with no request waiting); `[7]` write-sequencer checker (two sequencer states at once, or a table write requested while a write is in progress); `[8]` handshake checker (ACK toggled without answering a request, or a request was answered without toggling ACK); `[9]` WDOG: a request stayed pending for 64 clocks with no control-port write in between; `[10]` DEMOTED (live: the demotion count reached its fourth fault with DEMOTE = 1; the tournament drives while DEMOTE = 1). Bits 0–9 are sticky. The checkers and the watchdog cannot fire without a physical fault (formal proof). CMD bit 2 also clears the fault count (docs/mosaic.md 7.6). `[11]` KEPT (live): the last reset had the reset strap and kept the tables (section 5); CMD bit 2 leaves it, the next reset without the strap (soft reset included) clears it. `[15:12]` read 0. |

Raw entries: space 0 `{chooser[1:0], bimodal[1:0]}`; space 1 `gshare[1:0]`; weight tables `w[3:0]` (two's
complement, write enable bit 12). Space 7: 0–1 committed outcome history, 2–3 speculative outcome history (12 bits
per word), 4 `{theta[5:0], adaptation counter[3:0]}` (writable), 5 committed path history, 7 speculative path history (6 and 8, the upper path-history
words, read 0), 9 queue pointers and flags. Space 12, the leaky twin W3L (32 entries, docs/mosaic.md 7.3):
the node values `{check[3:0], data[3:0]}`, the code word of W3's weight (check bit k = XOR of the three data bits
other than `data[k]`), XOR the code mask TWMASK while CTRL[15] MASK = 1 (TWMASK is an 8-bit parameter of the RTL,
`8'hF0` in this version: the four check bits inverted, so that a word whose nodes have all decayed the same way, all
0 or all 1, is no code word and SEC-DED and detect-all abstain; with MASK = 0 both are valid code words). Every write
by the chip (training, restore-on-use, the clear) applies the mask of the present setting, the decoder takes it off
before decoding; raw reads and writes of space 12 see the nodes as they are. Reads 0 unless FCTL[0]; written (write enable bit 12) whatever FCTL says. A raw write of W3 (space 11)
leaves the twin as it is, so faults can be injected into either. Every bit of predictor state and
configuration is readable while requests run (FR-10); tables and the two small predictor registers are writable.

DBG events (CTRL): 1 request seen, 2 predict done, 3 resolve done, 4 misprediction repair, 5 flush, 6 refusal or
withdrawal, 7 write-pulse cycle, 8 staging valid, 9 queue empty, 10–13 last predictions (default, bimodal, gshare,
tournament), 14 control-port word, 15 staging read.

Fault events (the same selections when FCTL[14] = 1, same timing rule as the standard events): 0 TW_ERR,
1 TW_COR, 2 TW_DET, 3 TW_SDC (each a one-clock pulse with the `ACK` of the predict whose twin read raised the flag);
4 CANARY (a one-clock pulse one clock after the `ACK` of the predict request: the comparison is made in the clock
after PRED and CONF were loaded); 5 any invariant checker; 6 WDOG (a pulse every 64 clocks while a request hangs);
both one clock after they happen; 7 DEMOTED (a level; with the `ACK` of a predict it shows the state before that
predict, one clock later including that predict's own TW_DET); 10 the default prediction of the last predict
computed with W3 instead of the twin (equal to event 10 of the standard bank unless TWIN_USE changed it); 8 the
selected ring oscillator divided by 256 (asynchronous: one synchroniser flop, then the DBG flop; 0 while that ring is
stopped); 9 the random bit (of the source GSRC selects, whatever GRAND says) used at the last training before the
last executed predict (a level that changes only with the `ACK` of a predict; in a combined request the predict comes
first, so the bit of that request's own training shows at the next predict); 11–15 read 0. A detected fault (TW_DET of a predict,
CANARY, any checker, WDOG) increments a 2-bit saturating count; the fourth with DEMOTE = 1 sets DEMOTED.

## 5. Reset, ready, `ena`

`rst_n` is asserted asynchronously to nothing: it is synchronised (2 flops) and used as a synchronous reset.
After release, the chip clears every table (one write pulse, section 6.2), sets `ACK := REQ`, then raises `READY`:
a fixed number of clocks (stated with the RTL), independent of the host (FR-12). A soft reset from the control port
does the same.

**Reset strap** (docs/mosaic.md 7.7). If at the last clock edge of reset OP = 3 (`uio_in[3:2]`), CSn = 0
(`uio_in[4]`) and MOSI = 1 (`uio_in[6]`), the clear that ends the reset leaves the tables as they are (bimodal and
chooser, gshare, W0–W3, the leaky twin); everything else resets as usual (histories, threshold and its adaptation
counter, the gshare LFSR, in-flight work, statistics, CTRL, FCTL, FSTAT) and FSTAT[11] KEPT is set until the next
reset without the strap. After power-up the tables then show the latches' power-up state; later it is a warm reset
that keeps what was learned. OP = 3 alone is an ordinary request code a host may leave on the pins; the three
together are not a state of an idle SPI port. Timing: the chip takes OP through one flop and CSn, MOSI through two,
and the reset through two, so the pins must be stable from 4 clocks before `rst_n` rises until 4 clocks after it.
SCK must stay low throughout. The control port after such a reset: it starts with CSn low and no command bit
received, and without an SCK edge nothing happens; the next 8 SCK rising edges are taken as a command byte even if
CSn never rose (checked in test.py `strap_reset`). The FPGA should nevertheless take CSn high for at least 4 clocks
before its first SPI word, as every word does anyway, so that a stray SCK edge during the release cannot shift the
command. A soft reset is a reset without the strap. `ena` is not used inside the design (Tiny Tapeout powers and selects the project around it; the
template requires nothing else).

## 6. Microarchitecture

### 6.1 One read port, staged training

Each table has **one** read port and one write port. A cycle's read is used by, in priority order: a predict
that executes in this cycle (speculative history; a predict that is still waiting, for a write to finish or for the
resolve that must go before it, does not hold the port: holding it would deadlock, as the formal liveness proof
showed, docs/verification.md 4); the **staging read** of the oldest in-flight branch (committed history), done as soon as that
branch becomes the oldest and kept in a small register; a control-port read. A resolve then needs no read: it
computes the new table values from the staging register and the outcome and hands them to the write sequencer. A
predict that finds the queue empty is its own staging read (same index, same data). So a combined request costs one
cycle of predictor work, and no table needs a second read port (a read port is about 20 % of a table's area).

### 6.2 Storage: latch tables written by flop-generated pulses (AL-04)

`src/bpx_ltab.v`. A bit is one `dlxtn` latch (15.0 µm²). All tables share one write sequencer (`bpx_wseq`):

| Cycle | What happens |
| --- | --- |
| 0 | The request (table enables, addresses, data) is captured in flops at the clock edge. |
| 1 | Set-up: everything captured is stable. |
| 2 | `wpulse` is high for the first half of the cycle and opens exactly the addressed latches. Reads of this cycle are not used. |

`wpulse = a AND b`, where `a` is a flop on the rising edge and `b` a flop on the falling edge holding the complement
of `a` half a cycle late: each edge of the pulse has a single cause. Every other signal that reaches a latch changed
a full cycle before the pulse and cannot change until half a cycle after it. Write gates are glitch-free by
construction with half a period of margin on set-up, hold and pulse width; the two gate levels are instantiated by
name. This is the scheme of the silicon-proven TT06 latch RAM (project 620).

Three findings fixed this choice:

- **(VERIFIED, LibreLane probe)** Gates derived from the clock (clock-gating cells, as AL-05 suggests) are not
  usable for per-entry writes in this flow: clock-tree synthesis builds a buffered tree on every gated net with two
  or more latches (570 nets, 1,825 clock buffers, 44,700 µm² in the probe), and the flow offers no way to exempt
  nets. With flop-generated pulses the whole clock tree is 44 buffers (618 µm²).
- **(VERIFIED, LibreLane configuration)** Synthesis in this flow may not use drive-strength-1 gates. The read path
  (an AND-OR over the rows, then a column multiplexer) is therefore mapped by hand onto minimum-size cells
  (`bpx_aor`), which the resizer may still upsize.
- **(VERIFIED, Yosys with the flow's cell exclusions)** A table costs `550 + entries × (20.5 × bits + 6.5)` µm²
  after synthesis: 22–25 µm²/bit, against about 52 for flip-flops.

What static timing covers: every flop-to-flop path, including the complete read path from the address flops through
the tables to the output and staging flops. What it does not cover: the latch data and gate pins, which the tool
reports as unconstrained endpoints because no clock reaches a latch. Those paths are safe by the construction above
(they have half a clock period; their logic depth is two gates) and are exercised in the gate-level test. This is
the one place where AL-04's "static timing covers the latch paths" is met by argument rather than by the tool;
see docs/timing.md.

### 6.3 Clearing and freezing

Clear (reset, soft reset, "clear predictors" command) selects every row and column of every table and writes the
initial values with one pulse. Freeze disables every training write of a resolve (tables and the predictor's small
registers). The histories keep moving, because they are context, not learning, and statistics keep counting: a
frozen chip still predicts with real history and can be scored (FR-09).

### 6.4 Debug pins (IR-08)

`DBG0` and `DBG1` each show one of 16 internal events chosen in the control register (request detected, predict
done, resolve done, flush, misprediction of the driving predictor, refusal, write pulse cycle, staging valid, queue
empty, each predictor's raw prediction, control-port byte strobe, …; list fixed at G1). Two pins: with `ACK`,
`PRED`, `FULL` and `READY` already on pins, any operation or error can be reconstructed from two selectable events,
and the remaining output is needed for `MISO`. After reset `DBG0` = event 10 and `DBG1` = event 13: with the fixed
`PBIM` and `PGSH` pins the last prediction of each of the four predictors is on a pin, valid with `ACK` (the
prediction events change in the same clock as `ACK`; the other events one clock after they happen).

## 7. Predictor algorithm (evidence in docs/perceptron.md and docs/exploration.md)

Total predictor storage: **1,024 bits** in six latch tables (plus the leaky twin of W3, 256 dynamic bits, an
experiment that changes nothing while FCTL = 0: register FCTL above and docs/mosaic.md 7.3). `fold(v, n)` is the XOR of the consecutive n-bit
slices of v; `h[L-1:0]` are the L most recent outcomes of the history in use (bit 0 = most recent); `p[7:0]` is the
path history: one bit, `ID[0] ^ ID[3]`, of each of the last 8 branches.

| Table | Entries × bits | Index | Content, initial value |
| --- | --- | --- | --- |
| BC | 64 × (2 + 2) | `fold(ID, 6)` | bimodal counter (01) and chooser counter (01), separately writable |
| G | 128 × 2 | `fold(ID, 7) ^ (h[3:0] << 3)` | gshare counter (01) |
| W0 | 32 × 4 | `ID[4:0] ^ ID[7:5]` | signed weight; 0 |
| W1, W2, W3 | 32 × 4 each, history lengths L = 8, 9, 20 | `ID[4:0] ^ ID[7:5] ^ fold(h[L-1:0], 5) ^ (fold(h[L-1:0], 4) << 1) ^ rot2(fold(p[min(L,8)-1:0], 5))` | signed weight; −1, 0, −1 |

`rot2` rotates five bits left by two. Small registers: threshold θ (6 bits, starts at 10), its adaptation counter
(4 bits, signed, starts at 0), committed and speculative outcome history (21 bits each), committed and speculative
path history (8 bits each).

**Reference predictors (FR-07).** bimodal = BC counter MSB. gshare = G counter MSB. tournament = gshare if the
chooser MSB is 1, else bimodal. They train on every resolved branch by the classic rules: both counters move toward
the outcome; the chooser moves toward the component that was right when the two disagree. A reference is confident
when the counter it used is saturated (0 or 3).

**Default predictor: a hashed perceptron.** With the features of CTRL[14:12] as after reset:

```
neg_i = h[L_i]                      for i = 1..3 (the outcome just before the table's history segment); neg_0 = 0
v_i   = neg_i ? -(2 w_i + 1) : (2 w_i + 1)          = 2 (w_i ^ neg_i) + 1: the stored bits read complemented
sum   = v_0 + v_1 + v_2 + v_3 + 2 (2 bim - 3) + 4 (2 gsh - 3) + (tournament ? +4 : -4)
prediction = (sum >= 0)            confidence (FR-11) = |sum| > theta
```

**Training** at resolve, from the committed lookup of that branch (committed outcome and path history) and the
outcome only: if the prediction recomputed at resolve was wrong, or `|sum| <= theta`, every `w_i` moves one step
(up if `outcome != neg_i`, else down), saturating at −8 and +7 (|sum| <= 4 × 15 + 6 + 12 + 4 = 82: an 8-bit sum). Then the threshold adapts: on a wrong prediction
the adaptation counter goes up, and when it is at +7 it returns to 0 and θ goes up by one (to at most 63); on a right
prediction with `|sum| <= theta` it goes down, and at −8 it returns to 0 and θ goes down by one (to at least 1). The
reference tables train as above. The speculative path history is repaired with the outcome history (FR-04).

**Features (CTRL[14:12]).** `[12]` = 0 removes the three reference terms from the sum. `[13]` = 0 sets every
`neg_i` to 0. `[14]` = 0 stops the threshold adapting (θ stays; it is writable through raw space 7). All 8
combinations are in the golden model and are verified alike. (CTRL[15], binarised inference until 26 Nov 2026, was
removed in the mosaic round; since step 5 it is MASK, the leaky twin's code mask, section 4.) **Safe mode:** `[14:12] = 0`, the plain hashed perceptron; or any reference predictor driving
(CTRL[2:0]), which shares no logic with the sum.

**Sizes of the references, and why.** Bimodal + chooser 256 bits, gshare 256 bits: the reference tournament has 512
bits, the default predictor adds 512. Each reference is somewhat below its own saturation (a 128-entry bimodal alone
would score 1.0 point more, a 4096-entry one 1.9 more); the gshare history of 4 bits is the best history for a
128-entry tournament on the benchmark set. Nothing was shrunk to widen the gap (docs/exploration.md).

## 8. Budget (EX-04)

The area by block, the build settings and the measured utilisation are in docs/area.md; the clock and the critical
path in docs/timing.md. In short **(VERIFIED there)**: the perceptron design builds clean with all checks at zero;
the sum of the perceptron in the predict cycle sets the clock period, signed off at the slow corner: 33 ns (30.3 MHz)
since mosaic step 5 (was 30 ns; the mosaic's leaky twin sits in the predict cycle too, and the canaries need a lead),
with the acknowledge still at the third clock: 99 ns from request to answer. The FPGA sets the real clock.

## 9. The U1 CPU: the contract the FPGA team builds (FR-03, VR-06)

The owners delegated the choice of pipeline (decision of 26 Nov 2026: "choose the pipeline that gets the most out of
BPX-1, including a branch-target buffer at fetch"). The choice is the classic five-stage pipeline with one addition,
a small branch-target buffer, because a direction predictor asked in decode leaves a bubble on every correctly
predicted taken branch, and that bubble is most of what a five-stage pipeline can gain. The pipeline is **not** made
deeper to enlarge the number: a later resolve only makes the baseline worse.

In-order RV32IM, stages IF ID EX MEM WB, no compressed instructions.

**Branch-target buffer:** 256 entries, direct-mapped on `PC[9:2]`, each entry = full tag, target, kind (conditional
branch or JAL). Read in IF (distributed RAM, same cycle). Written when a conditional branch or JAL is decoded.

| Event | What the CPU does | Cost in cycles |
| --- | --- | --- |
| Conditional branch, BTB hit | Asks BPX-1 in IF and fetches from the predicted side | 0 if correct |
| Conditional branch, BTB miss | Found in ID, asks BPX-1 there | 0 if predicted not-taken and correct, 1 if predicted taken and correct |
| Misprediction (either case) | Resolved in EX, younger instructions squashed | 2 |
| JAL | Follows the BTB in IF; on a miss redirects from ID | 0 on a hit, 1 on a miss |
| JALR | Resolved in EX; the CPU sends FLUSH | 2 |
| Load followed by a use | – | 1 |
| DIV, REM | Iterative | 32 |
| The same CPU without BPX-1 (static not-taken) | Taken conditional branch resolved in EX | 2 |

Rules the CPU's adapter follows:

1. **One request per CPU cycle**, launched from registers at the start of the cycle: the predict of the branch that
   has just entered IF (BTB hit) or ID (BTB miss), combined with the resolve of the branch that left EX in the
   previous cycle if there is one (`OP = 10/11`), or the resolve alone (`OP = 00`).
2. If a BTB-miss branch asks from ID in the cycle a BTB-hit branch behind it is in IF, the one in ID goes first and
   the one in IF waits a cycle (one bubble). If the one in ID is predicted taken, or is a JAL, the one in IF is
   squashed and never asks.
3. A branch asks once, however long it is stalled; the adapter keeps `PRED` until the branch reaches EX.
4. Outcomes are reported in program order, each in the cycle after its branch leaves EX. A misprediction needs no
   extra request: the resolve that reports it makes the chip discard the wrong-path predictions. That resolve is
   sent **alone** (`OP = 00`), never combined with a predict: the chip discards everything younger than the
   mispredicted branch, and a predict combined with it (predict first, then resolve) would be discarded too. The
   branch that is in IF in that cycle, the first one of the correct path, asks one cycle later (one bubble). The
   same holds for the cycle of a FLUSH.
5. After a JALR (and after any exception or interrupt) the CPU sends FLUSH in the cycle after it resolves. Every
   older branch has been reported by then, so FLUSH removes exactly the wrong-path predictions.
6. In-flight peak: 4 (branches in IF, ID and EX plus the one whose outcome is being reported), equal to the queue
   depth; `FULL` covers a CPU that reports later than this.
7. **Two requests in one CPU cycle (recommended whenever the CPU clock is at most one eighth of the chip clock,
   which includes the 1 MHz CPU of IR-07).** The two bubbles of rules 2 and 4 exist only because of rule 1. An
   adapter that can issue a second request as soon as the first is acknowledged removes both: the misprediction
   resolve (or FLUSH), then the predict of the branch in IF; or the predict from ID, then the predict from IF. The
   pair needs at most 7 chip clocks at the pins (measured: 3 for the first, 1 to turn around, 3 for the second),
   plus the adapter's own synchronisers. Nothing in the chip changes: it is the same protocol, used twice in a
   cycle.

The minimum for rule 1 alone is 4 chip clocks per CPU cycle (measured in the co-simulation: no late answer at 4,
late answers at 3).

Results are reported for this contract in both adapter modes (docs/cosim.md: one request per cycle;
docs/cosim_dual.md: rule 7) and, as sensitivity from the exploration model, without the BTB (every branch asks in
ID) and with the resolve one and two stages later (docs/exploration.md).

Host-side identifier fold (IR-05): `ID = fold8(PC >> 2)`, the XOR of consecutive 8-bit slices of `PC[31:2]`.

**Clock, reset and project selection on the bench.** The FPGA can drive the chip's `clk` and `rst_n` directly once
the demo board's firmware has released them (`RPMode.ASIC_MANUAL_INPUTS`); the project selection must be done, and
held, by the board's microcontroller. Details, sources and what is still unknown: docs/bringup.md 6. The FPGA must
leave `uio[0]` and `uio[1]` undriven: they are outputs of the chip.
