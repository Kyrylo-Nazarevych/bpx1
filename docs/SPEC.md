# BPX-1 design specification (DR-01)

Status: **gate G1: protocol and predictor frozen.** The RTL (`src/bpx_*.v`) and the golden model (`model/bpx1.py`)
implement this document; physical closure (section 8) is in progress. Evidence labels as in the requirements specification, section 10.

## 1. What the chip is

A conditional-branch direction predictor for an external host. Four predictors run side by side on every branch:
the **default** predictor and three classic references (bimodal, gshare, tournament of the two). One of them, or
static not-taken, **drives**: its prediction is the one returned to the host. A second, independent serial port gives
control, statistics and access to every state bit while the predict port keeps running.

```
            ui_in[7:0] uio[3:0]            uio[7]                       uo[0] uo[1] uo[2]  uo[3]  uo[4]
              ID        ID/OP               REQ                          ACK   PRED  CONF   FULL   READY
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
        |   | port per table), tag compare, combine, confidence, training logic   |  state | port (SPI)|  |
        |   +---------------------------------------------------------------------+ access +--+-----^--+  |
        +----------------------------------------------------------------------------------- | --- | ------+
                                                                              uo[7] MISO <---+     +--- uio[4] CSn, uio[5] SCK, uio[6] MOSI
                                                                              uo[5], uo[6]: debug event pins
```

One clock domain (`clk`). Every input pin, including `rst_n`, passes through two flops before it is used (RR-01).
No combinational path from any input to any output (XR-05): every output pin is a flop.

## 2. Pin map

All eight bidirectional pins are **inputs** (`uio_oe = 0` always): the chip can never fight the host on a shared pin.

| Pin | Name | Dir | Function |
| --- | --- | --- | --- |
| `ui_in[7:0]` | `ID[7:0]` | in | Branch identifier, low bits. |
| `uio[1:0]` | `ID[9:8]` | in | Branch identifier, high bits (also the sub-operation when `OP = 00`). |
| `uio[3:2]` | `OP[1:0]` | in | Operation (section 3). |
| `uio[4]` | `CSn` | in | Control port chip select, active low. |
| `uio[5]` | `SCK` | in | Control port clock (SPI mode 0). |
| `uio[6]` | `MOSI` | in | Control port data in. |
| `uio[7]` | `REQ` | in | Request: every **toggle** asks for one operation. |
| `uo_out[0]` | `ACK` | out | Acknowledge: toggles once per request. A request is pending while `REQ != ACK`. |
| `uo_out[1]` | `PRED` | out | Prediction of the driving predictor (1 = taken). Valid from the `ACK` toggle of a predict until the next `ACK` toggle. |
| `uo_out[2]` | `CONF` | out | Confidence of the default predictor's prediction (1 = high). Same validity as `PRED`. |
| `uo_out[3]` | `FULL` | out | The in-flight queue is full: a plain predict would be refused. |
| `uo_out[4]` | `READY` | out | Reset and table initialisation are complete; requests are accepted. |
| `uo_out[5]`, `uo_out[6]` | `DBG0`, `DBG1` | out | Internal events, selected at runtime (section 6.4). |
| `uo_out[7]` | `MISO` | out | Control port data out (0 while `CSn` is high). |

Why these pins **(VERIFIED by the research agent against the demo-board firmware and the RP2350 function table)**:
on the RP2350 demo board `uio[5]`, `uio[6]` and `uo_out[7]` are the SCK, TX and RX pins of hardware SPI1, so the
control port can be driven by `machine.SPI` (IR-03); `ui_in[7:0]`, `uio[3:0]` are twelve consecutive GPIOs
(17–28), which a PIO program drives as one output group, with `REQ` and `ACK`/`PRED` next to them. Tiny Tapeout's
recommended SPI pinout (CS, MOSI, MISO, SCK on `uio[0..3]`) does not line up with any RP2350 hardware SPI and needs an
output on `uio`; it is a recommendation, not a rule, and is not followed.

Sixteen pins can be inputs. The control port takes three and the handshake three (`REQ`, `OP`), which leaves ten
for the identifier. Section 7 shows that ten bits do not limit accuracy on the benchmark set (IR-05) and documents
the host-side fold.

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
| 0x01 | CTRL | RW | `[2:0]` driving predictor: 0 default, 1 bimodal, 2 gshare, 3 tournament, 4–7 static not-taken; `[3]` freeze learning; `[7:4]` DBG0 event; `[11:8]` DBG1 event |
| 0x02 | CMD | W | bit 0 soft reset; bit 1 clear predictors (tables, histories, in-flight work); bit 2 clear statistics and error flags; bit 3 snapshot the statistics; bit 4 snapshot and clear the counters in one step (no event lost or counted twice) |
| 0x03 | STATUS | R | `[2:0]` in-flight count; `[3]` ready; `[4]` staging valid; `[5]` request pending; `[6]` table write in progress; `[7]` raw access pending; `[8]` resolve refused (queue empty); `[9]` predict refused (queue full); `[10]` request withdrawn; `[11]` statistics overflow (flags are sticky) |
| 0x04 | ERRCNT | R | `[7:0]` refused or withdrawn requests, saturating |
| 0x05–0x0E | STATS | R | Snapshot taken by CMD bit 3, two words (low, high) per counter: resolved branches, then mispredictions of default, bimodal, gshare, tournament. Counters are 16 bits and saturate (never wrap; overflow is flagged in STATUS). For exact totals of longer runs use snapshot-and-clear at least every 65,535 branches and add the snapshots up (FR-08). |
| 0x0F | RAW_ADDR | RW | `[6:0]` entry; `[10:8]` space: 0 bimodal+chooser, 1 gshare, 2… tagged tables, 7 small registers; `[13:12]` field write enables; `[15]` address + 1 after each RAW_DATA read |
| 0x10 | RAW_DATA | RW | Read: the addressed entry (fetched after RAW_ADDR is written and after each read). Write: the entry fields enabled in RAW_ADDR. |

Raw entries: space 0 `{chooser[1:0], bimodal[1:0]}`; space 1 `gshare[1:0]`; tagged `{u[1:0], tag, ctr[2:0]}`.
Space 7: 0–1 committed history, 2–3 speculative history (12 bits each), 4 `{LFSR[7:0], use-alternate[3:0]}`
(writable), 5 queue pointers and flags. Every bit of predictor state and
configuration is readable while requests run (FR-10); tables and the two small predictor registers are writable.

DBG events (CTRL): 1 request seen, 2 predict done, 3 resolve done, 4 misprediction repair, 5 flush, 6 refusal or
withdrawal, 7 write-pulse cycle, 8 staging valid, 9 queue empty, 10–13 last predictions (default, bimodal, gshare,
tournament), 14 control-port word, 15 staging read.

## 5. Reset, ready, `ena`

`rst_n` is asserted asynchronously to nothing: it is synchronised (2 flops) and used as a synchronous reset.
After release, the chip clears every table (one write pulse, section 6.2), sets `ACK := REQ`, then raises `READY`:
a fixed number of clocks (stated with the RTL), independent of the host (FR-12). A soft reset from the control port
does the same. `ena` is not used inside the design (Tiny Tapeout powers and selects the project around it; the
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
and the remaining output is needed for `MISO`.

## 7. Predictor algorithm (frozen at G1; evidence in docs/exploration.md)

Total predictor storage: **1,088 bits** in five latch tables. `fold(v, n)` is the XOR of the consecutive n-bit
slices of v; `h[L-1:0]` are the L most recent outcomes of the history in use (bit 0 = most recent).

| Table | Entries × bits | Index | Content, initial value |
| --- | --- | --- | --- |
| BC | 64 × (2 + 2) | `fold(ID, 6)` | bimodal counter (01) and chooser counter (01), separately writable |
| G | 128 × 2 | `fold(ID, 7) ^ (h[3:0] << 3)` | gshare counter (01) |
| T1, T2, T3 | 16 × (3 + 7 + 2) each, history lengths L = 4, 9, 20 | `fold(ID, 4) ^ fold(h[L-1:0], 4)` | `{u, tag, ctr}` = `{0, 0, 3}`; tag = `fold(ID >> 4, 7) ^ fold(h[L-1:0], 7) ^ (fold(h[L-1:0], 6) << 1)` |

Small registers: use-alternate counter (4 bits, starts at 8), allocation LFSR (8 bits, taps 0xB8, starts at 0xE1),
committed and speculative history (20 bits each).

**Reference predictors (FR-07).** bimodal = BC counter MSB. gshare = G counter MSB. tournament = gshare if the
chooser MSB is 1, else bimodal. They train on every resolved branch by the classic rules: both counters move toward
the outcome; the chooser moves toward the component that was right when the two disagree.

**Default predictor.** A table *hits* when its stored tag equals the computed tag. The longest-history hit is the
*provider*, the next one the *alternate*; without a second hit the alternate is the tournament, and without any hit
the prediction is the tournament. The provider is *weak* when its counter is 3 or 4 and its `u` is 0. Prediction =
alternate if the provider is weak and the use-alternate counter is 8 or more, else the provider's counter MSB.
**Confidence** (FR-11) = high when the provider's counter is saturated (0 or 7) and was used, or, without a
provider, when the counter the tournament used is saturated.

**Training** at resolve, from the committed lookup of that branch and the outcome only:

1. Provider: counter toward the outcome. If provider and alternate predictions differ: `u` up if the provider was
   right, down if wrong; and, if the provider was weak, the use-alternate counter up if the alternate was right,
   down if wrong. If the provider was weak and the alternate is a tagged table, its counter also moves.
2. If the default prediction (as recomputed at resolve) was wrong and a longer table exists: among the longer
   tables whose entry has `u` = 0, one is chosen (the LFSR steps; candidate k in increasing order is taken where k is
   the number of leading ones of the LFSR, capped at the last) and overwritten with the computed tag, counter 4 or 3
   by outcome, `u` = 0. If none is free, the `u` of each longer table's entry is decremented.
3. The reference tables train as above.

**Sizes of the references, and why.** Bimodal + chooser 256 bits, gshare 256 bits: the reference tournament has 512
bits, the default predictor adds 576. Each reference is somewhat below its own saturation (a 128-entry bimodal alone
would score 1.0 point more, a 4096-entry one 1.9 more); making them larger does not help the default predictor
(the best candidates with a 128-entry bimodal score the same) and costs area the tagged tables use better. The
gshare history of 4 bits is the best history for a 128-entry tournament on the benchmark set. Nothing was shrunk to
widen the gap: with these very tables the default predictor is 2.3 points more accurate than the tournament.

**Identifier width (IR-05).** 10 bits: accuracy is flat from 6 to 16 bits (91.16, 91.29, 91.24, 91.28, 91.27 % at
6, 8, 10, 12, 16). No static hint bit: agree-coded variants gain or lose under 0.05 point.

## 8. Budget (EX-04), re-baselined at G1

| Block | Synthesised area, µm² | Basis |
| --- | --- | --- |
| Tables: BC 5,400; G 5,400; T1–T3 3 × 4,080 | 23,000 | **(VERIFIED)** Yosys, flow's cell exclusions |
| Sequencer, queue, histories, statistics, raw access (`bpx_core`) | 16,000 (13,000 with drive-strength-1 gates allowed) | **(VERIFIED)** Yosys |
| Predictor logic: hashes, compare, training (`bpx_pred`, folds) | 9,000 (7,500) | **(VERIFIED)** Yosys |
| Statistics snapshot latches | 1,300 | **(VERIFIED)** Yosys |
| Control port, synchronisers | 2,800 (2,400) | **(VERIFIED)** Yosys |
| **Total** | **57,000 (49,900)** | |
| Routable, demonstrated | 48,900 synthesised → 82–84 % utilisation, DRC/LVS/antenna clean | **(VERIFIED)** probes p5 and the repository probe, local and CI |

The design fits only with drive-strength-1 gates allowed in synthesis (AL-02: `SYNTH_EXCLUDED_CELL_FILE`), which is
being proven in a trial build; the fallback, if that fails, is to cut logic (statistics width, raw-access paths) or
one tagged table. Critical path budget: one clock for hash + table read + compare + select, about 12–13 ns at the
slow corner by the probe's cell delays **(ESTIMATED)**, so 50 MHz is the target and 66 MHz is not promised.

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

Host-side identifier fold (IR-05): `ID = fold10(PC >> 2)`, the XOR of consecutive 10-bit slices of `PC[31:2]`.
