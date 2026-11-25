# BPX-1 design specification (DR-01)

Status: **draft for gate G0**. Sections 1–6 and 9 are decided and may still be tuned until the protocol freezes at
G1. Section 7 (the predictor algorithm and its sizes) and the numbers of section 8 (budget) are filled from the
exploration (docs/exploration.md). Evidence labels as in the requirements specification, section 10.

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

SPI mode 0, MSB first, `CSn` frames a transaction. First byte: bit 7 = 1 write / 0 read, bits 6:0 = address;
then data bytes, the address incrementing after each. `SCK` is sampled by `clk` (it is not a clock on the chip):
`f_SCK ≤ f_clk / 8`. The port works at any `clk`, including single steps. The register map is fixed at G1
(control, drive select, freeze, debug select, commands, status, error flags, statistics snapshot, histories, queue,
table address/data); all table access goes through the tables' one read port and one write sequencer in cycles the
predict port does not use, so it never stalls a request (IR-02).

## 5. Reset, ready, `ena`

`rst_n` is asserted asynchronously to nothing: it is synchronised (2 flops) and used as a synchronous reset.
After release, the chip clears every table (one write pulse, section 6.2), sets `ACK := REQ`, then raises `READY`:
a fixed number of clocks (stated with the RTL), independent of the host (FR-12). A soft reset from the control port
does the same. `ena` is not used inside the design (Tiny Tapeout powers and selects the project around it; the
template requires nothing else).

## 6. Microarchitecture

### 6.1 One read port, staged training

Each table has **one** read port and one write port. A cycle's read is used by, in priority order: a predict
(speculative history); the **staging read** of the oldest in-flight branch (committed history), done as soon as that
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

## 7. Predictor algorithm

*To be fixed at G1 from docs/exploration.md.* Decided so far: training never uses anything remembered from predict
time (section 3.2); the reference predictors train by their classic rules on every branch (FR-07); identifier width
10 bits unless the exploration shows a static hint bit is worth more than the tenth identifier bit.

## 8. Budget (EX-04)

*Numbers to be filled at G1.* Framework **(VERIFIED unless marked)**:

| Item | Value | Source |
| --- | --- | --- |
| Core area | 72,565 µm² | local build metrics |
| Tap cells | 1,297 µm² | probe |
| Table, synthesised | `550 + entries × (20.5 × bits + 6.5)` µm² | Yosys fit, error < 3 % |
| Growth from synthesis to routed (buffers for fan-out and timing, clock tree) | about 18–20 % in the probe | LibreLane probe p4 (in progress) |
| Synthesised logic | drive-strength-2 cells: flop 21.3, XOR 16.3, NAND2 6.3, mux2 11.3 µm² | PDK liberty, flow exclusion list |
| Target utilisation | to be set from the probe's routing result | (DON'T KNOW until the probe finishes) |

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
   extra request: the resolve that reports it makes the chip discard the wrong-path predictions.
5. After a JALR (and after any exception or interrupt) the CPU sends FLUSH in the cycle after it resolves. Every
   older branch has been reported by then, so FLUSH removes exactly the wrong-path predictions.
6. In-flight peak: 4 (branches in IF, ID and EX plus the one whose outcome is being reported), equal to the queue
   depth; `FULL` covers a CPU that reports later than this.

Results are reported for this contract and, as sensitivity, without the BTB (every branch asks in ID) and with the
resolve one and two stages later.

Host-side identifier fold (IR-05): `ID = fold10(PC >> 2)`, the XOR of consecutive 10-bit slices of `PC[31:2]`.
