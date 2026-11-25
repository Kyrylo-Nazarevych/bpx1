# Review of the BPX-1 requirements specification (v4.1)

Written by the lead architect before design freeze. It answers three questions the owner asked: is the
specification correct and sensible, where does it dictate *how* instead of asking for a result, and what needs the
owners' decision. Evidence labels follow section 10 of the specification.

**Overall verdict.** The specification is sound and unusually well built: ranked objectives, hard limits kept apart
from permissions, a verification method per requirement, and almost no architecture prescribed. Nothing in it makes
a good chip impossible. It has two requirements that cannot be met as literally written (R1, R2), one objective that
is not defined until the owners fix an input (R3), and one requirement that takes area from the top objective by
construction (H1). Each has a resolution below; none needs a waiver if the owners accept the stated reading.

## 1. Requirements that cannot hold as written

| # | Requirement | Problem | Resolution adopted (needs owner's acceptance) |
| --- | --- | --- | --- |
| R1 | **FR-04**, last clause: after a repair, "every later prediction equals what a strictly sequential run … would produce". | No design can do this for a prediction made while an older branch is still unresolved: the sequential run has already trained on that branch's outcome, the chip cannot have. | Read it as two exact properties, both proven: (a) the **trained state** after resolving branches 1…n always equals the state of a sequential run over 1…n, whatever was speculated or discarded in between; (b) a prediction made with **nothing in flight** equals the sequential prediction. The remaining difference (training of in-flight branches arriving late) is what PF-03 asks to be measured. |
| R2 | **FR-03**, last sentence: a request beyond capacity "is held back, never dropped or reordered" together with **RR-03** (always returns to idle once the host stops). | "Held back" means the chip withholds its acknowledge. With a single request channel the resolve that would free a slot can then never be sent: a deadlock, which RR-03 forbids. Two independent channels would avoid it but make the order of a predict and a resolve depend on synchroniser timing, which FR-06 forbids. | Capacity is sized so the U1 CPU can never reach it (FR-03, first sentence). The chip shows "full" on a pin *before* the host asks, so a correct host holds the request back itself. A predict sent anyway is misuse (IR-09): refused, flagged, counted, never executed out of order, never a deadlock. A combined resolve+predict on a full queue is legal: the resolve goes first. |
| R3 | **O1** (rank 1) is "measured by VR-06", and VR-06 lets the chip designer choose the CPU ("state its stages and penalties"). | The top-ranked number is largely set by that choice. In a 5-stage pipeline (predict in decode, resolve in execute) a misprediction costs 2 cycles and a correct taken prediction still costs 1, so even a *perfect* predictor gains little over static not-taken; one more stage before resolve nearly doubles the gain. Candidates then differ by tenths of a percent in O1 while differing by whole percents in O2. | The owners should fix the U1 pipeline (question Q2 in section 5). Until then: primary model = classic 5-stage; every result is also reported for resolve one and two stages later. |

## 2. Requirements that dictate how (and what they cost)

| # | Requirement | What it forces | Cost against the objectives | Position |
| --- | --- | --- | --- | --- |
| H1 | **FR-07**: three classic reference predictors on the chip, "never shrink them". | Storage for bimodal, gshare and a chooser, on a chip whose whole predictor budget is about 1.5 Kbit **(VERIFIED: synthesised table cost 22–25 µm²/bit, core 72,565 µm²)**. | Every bit that only serves a reference is taken from O1/O2. The alternative product, references in FPGA soft logic at any size and the whole chip for the default predictor, would score higher on O1/O2 but gives up comparing algorithms *on silicon*, which is the point of U2/U3. | Keep FR-07. Mitigation under evaluation: the default predictor uses the reference tables as its own base component (as TAGE uses a bimodal base), so no bit is spent on comparison only. There is no neutral rule for "the right" reference size; proposed rule: each reference at the knee of its own accuracy-versus-size curve on the frozen benchmark set. |
| H2 | **IR-01** + **RR-01**: no shared clock, every input synchronised. | A two-flop synchroniser on the request: the answer cannot leave the chip earlier than the third clock edge after the request arrives. | Sets the floor for O3: latency = 3 chip clocks + pads, whatever the predictor logic does. A source-synchronous mode would save one to two clocks. | Accept. One mechanism for every host is worth more than the saved clock; the FPGA may still drive `clk` (C-03), which makes the same handshake's latency deterministic. No second, bypassed mode: a mode bit that removes synchronisers is a first-silicon hazard. |
| H3 | **FR-01/FR-02**: a predict never changes trained state; all training at resolve. | Rules out predict-time bookkeeping that large predictors use (speculative loop counters, checkpointed tables). | None measurable at an in-flight depth of 1–3 **(ESTIMATED: literature, JILP 2000 "Speculative updates of local and global branch history"; to be VERIFIED in PF-03)**. | Accept; it is what makes R1(a) provable and makes a change of driving predictor harmless. |
| H4 | **FR-08**: counts "read as a consistent snapshot" while the stream runs. | A shadow copy of every counter (a snapshot cannot be taken by reading live counters one byte at a time). | About 15 µm² per counter bit on top of about 33 for the counter itself. | Accept; counter width is chosen in the area budget. |
| H5 | **C-05**: importable into Vulkos (≤ 90 files, ≤ 2 MB each, ≤ 3.5 MB total). | Benchmark traces cannot be committed; programs are grouped several per source file; every tool must regenerate its data. | No cost to the chip; some to convenience. A tooling limit, not a product need. | Kept for the new repository (assumed still wanted: Q4). |
| H6 | **VR-01**: the golden model is Python. | Language. | A pure-Python model replays about 10⁵ branches per second, so the bit-exact benchmark runs take hours of CPU, not minutes. | Accept; runs are parallel and unattended. |
| H7 | **XR-05**, **IR-04**, **IR-03**, **IR-10**, **RR-02** | Registered outputs, level handshakes, a standard serial control port, template handling of `ena`, synchronised reset. | None: this is what a careful design does anyway. | Accept. |

## 3. Ambiguities and gaps (resolved by definition in docs/SPEC.md)

1. **Strictly ordered objectives.** "A lower objective never buys back a loss on a higher one" cannot be applied to
   noisy measurements: read literally, 0.001 % of speedup justifies giving up all timing margin. Definition used:
   differences inside the benchmark-set noise (stated with each result) are ties, and the next objective decides.
2. **The aggregate is not defined** (mean of what, weighted how). Used: speedup = geometric mean over programs;
   accuracy = arithmetic mean over programs of per-program accuracy; every program has equal weight; the worst
   program is always shown next to the aggregate.
3. **PF-02** "meaningfully worse" has no threshold. Used: every program where the default is worse than the best
   reference is listed with the reason; "meaningful" is left to the owners' reading of that list.
4. **FR-07** "takes effect from a defined prediction onward" while IR-02 lets the setting change during a live
   stream on an unrelated clock. Definition: a setting written through the control port applies to every predict the
   chip *accepts* after the write completes; a host that needs an exact boundary pauses its requests around the write.
5. **IR-09** "overlapping requests … is counted". In a two-phase (toggle) handshake a second toggle is by
   construction a new request; what can be detected, and is counted, is a request withdrawn before its acknowledge.
6. **VR-02** "back-pressure" is undefined for a chip that never stalls a correct host. Bin used: the request arrives
   while the previous operation is still completing, or while the in-flight queue is full.
7. **EX-01** "representative" of what is not said. The team's real programs execute only 8–58 distinct branch
   instructions **(VERIFIED: 11 team programs traced)**; at this budget table capacity, not history length, is the
   limit, so such programs cannot separate designs however long they run. The extension adds programs with more
   branch sites and data-dependent behaviour. If the real U1 demos will be tiny loops, tell me: it changes the best
   design.
8. **Missing lever.** The largest speedup available to U1 is outside the chip: a direction predictor asked in decode
   leaves one bubble on every correctly predicted taken branch. A small branch-target buffer in the FPGA CPU, so that
   BPX-1 is asked at fetch, removes that bubble. The chip's interface already supports it.

## 4. Statements in the specification checked against the platform

| Statement | Finding |
| --- | --- |
| C-02: core ≈ 72,500 µm² | **(VERIFIED)** 72,564.6 µm² (329.36 × 220.32 µm), from the local build's metrics. |
| AL-03: 32×32 register-file macro, "≈ 1,200 bits per tile" | **(VERIFIED by the research agent, sources read)** The macro is 132.6 × 118.7 µm. Two fit in 2×2 tiles: 2,048 bits in 43 % of the core, about 1.45× the density of a latch table, not 2.3×. Its source is not published, there is no netlist for LVS, its timing library is a single-corner placeholder, the read is synchronous (one more clock of latency), and its files state no licence, so it cannot go into a public repository. **Not used.** |
| AL-04: latches ≈ 512 bits per tile | **(VERIFIED)** My table, with a hand-mapped read tree and glitch-free write gates from the library's clock-gating cells, synthesises to 22–25 µm²/bit all-in, against about 52 for flip-flops. |
| 3.3: "the local build equals the CI" | Not re-checked for the new design yet (no CI until the repository exists). |
| 3.3: `gh` "is installed but not logged in" | **(VERIFIED)** It holds an invalid token for account `Kyrylo-Nazarevych`; `gh auth login` is needed before any repository can be created or CI read. |
| (not in the specification) | **(VERIFIED: read from the installed PDK's LibreLane configuration)** The flow's synthesis may not use any drive-strength-1 gate, so synthesised logic costs about 1.6× the minimum cell areas (an XOR is 16.3 µm², not 8.8). Explicitly instantiated cells are not affected. This shapes the budget more than any single requirement. |

## 5. Conflicts between the specification and the owner's instructions of 25 Nov 2026

| Specification | Instruction in chat | Followed |
| --- | --- | --- |
| C-05: develop in `tt-branch-predictor` on branch `bpx1`; `main` keeps the prototype until G3 | Build everything in a new folder, `Branch Predictor Hackathon 2`, and host it in a new, separate GitHub repository | The chat instruction. The old repository is untouched. |
| AL-09: Verilog, SystemVerilog or a generator | "Code the digital system using Verilog" | Verilog-2005 only. |
| Section 0: the prototype is "a placeholder to replace" | Do not follow or trust anything in the prototype | Only platform files were taken from the old repository (the Tiny Tapeout workflow files and flow defaults, which are the template's) and the benchmark C sources (EX-01). No design content was read for reuse. |

## 6. Owners' decisions (26 Nov 2026)

| # | Question | Decision |
| --- | --- | --- |
| Q1 | Repository | New public repository `Kyrylo-Nazarevych/bpx1`. |
| Q2 | U1 CPU pipeline (R3) | Delegated to the architect: "choose the pipeline that gets the most out of BPX-1, including a branch-target buffer at fetch", documented as the contract the team builds, with the sensitivity reported. The contract is docs/SPEC.md section 9. |
| Q3 | External benchmark suites | Allowed (Embench, CoreMark, MiBench and similar) where the licence permits; nothing that may not be redistributed is committed. They are fetched by `bench/ext/build_ext.sh`. |
| Q4 | Vulkos import limits (H5) | Do not apply to the new repository. |
| R1, R2 and the other readings in sections 1–3 | | Accepted. |
| Extra compute | | Rented machines on the owners' Vast.ai account may be used up to $12 in total, labelled `bpx1-*`, destroyed when idle, spend reported at every gate; sweeps run on the full benchmark set, not a sample. |
