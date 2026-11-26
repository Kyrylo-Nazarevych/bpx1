# BPX-1 handover

Everything a fresh session needs to continue: state, decisions, next steps, commands. Written at gate G1
(27 Nov 2026). Evidence labels as in the requirements specification, section 10. Read `docs/SPEC.md` next.

## 1. State in one paragraph

BPX-1 is a branch-prediction coprocessor for Tiny Tapeout (SKY130, 2×2 tiles). The protocol and the predictor are
**frozen**; **the real RTL is the repository top**, equals the golden model in RTL and at gate level on random
operations and a trace replay, and builds locally clean (75.9 % utilisation, DRC/LVS/antenna 0, all corners met at
40 MHz). **Not done:** reading the CI run of that push, benchmark programs against RTL (VR-01 on programs, VR-06,
PF-01…03 from RTL), 10⁶ random operations, formal proofs, coverage, and `docs/coverage.md`, `verification.md`,
`bringup.md`, the host kit.

## 2. Where things are

| What | Where |
| --- | --- |
| Project folder (source of truth, Windows) | `C:\\Users\\Kyrylo\\Desktop\\Claude Folder\\Branch Predictor Hackathon 2` |
| Linux mirror where every tool runs | `~/work/bpx1` (WSL Ubuntu), filled by `tools/wsl_sync.sh`; generated data in `~/work/bpx1/out` |
| GitHub | `Kyrylo-Nazarevych/bpx1`, public, branch `main`; Pages enabled (source: GitHub Actions) |
| Requirements | `..\\Branch Predictor Hackaton 1\\plan\\FABLE_ALL_IN_BRIEF.md` (v4.1) and the owners' decisions in `docs/spec_review.md` section 6 |
| Old prototype | `~/work/tt-branch-predictor` (not to be reused; only its `tt/` support tools are symlinked) |

Repository layout: `src/` RTL; `model/bpx1.py` golden model; `test/` cocotb; `explore/` sweep engine and reports;
`bench/` benchmarks (`bench/ext` fetches external suites); `tools/` scripts; `docs/` documents.

## 3. Decisions (each with its evidence in the named document)

| # | Decision | Evidence |
| --- | --- | --- |
| D1 | Host port: two-phase REQ/ACK, 10-bit ID, 2-bit OP; answer at the third clock after the request. PREDICT+RESOLVE = predict, then resolve the oldest (resolve first if the queue is full). Queue depth 4. | SPEC 2–3; owners accepted the readings of FR-03 and FR-04 |
| D2 | Control port: SPI mode 0 with 16-bit registers on `uio[4..6]` + `uo[7]` (RP2350 hardware SPI1); all `uio` are inputs. | SPEC 4 |
| D3 | Storage: latch tables written by flop-generated pulses; one write sequencer; one read port per table; training from a staging register. Clock-derived gates are impossible in this flow (clock-tree synthesis adds 44,700 µm²). | SPEC 6; probes p2–p5 **(VERIFIED)** |
| D4 | No hard macro: the 32×32 register file is 1.45× denser in practice, has no source, no LVS netlist, no licence. | spec_review 4 **(VERIFIED by the research agent)** |
| D5 | Predictor: bimodal 64 + chooser 64, gshare 128 (4 history bits), three tagged tables 16 × 12 bits with histories 4/9/20 over the tournament, 8-bit allocation LFSR; 1,088 bits. | SPEC 7; exploration.md **(VERIFIED, model)** |
| D6 | U1 CPU contract: five stages with a 256-entry branch-target buffer at fetch; owners delegated the choice. | SPEC 9 |
| D7 | Benchmark set frozen: 71 programs; `embench.nbody`/`cubic` out of the aggregates; 51 tuning / 18 held-out. | exploration.md 1 |
| D8 | Statistics: 16-bit saturating counters, snapshot and snapshot-and-clear; no raw writes to the histories. | SPEC 4 (trimmed for area) |
| D9 | Single clock domain; data pins use one capture flop qualified by the 2-flop synchronised REQ. | `src/bpx_chip.v` header |

## 4. Verified facts worth not rediscovering

- LibreLane's synthesis may not use drive-strength-1 gates (`no_synth.cells`); explicitly instantiated cells are
  unaffected. Logic costs about 1.6× textbook areas. The read tree is hand-mapped (`bpx_aor`).
- Table cost after synthesis: `550 + entries × (20.5 × bits + 6.5)` µm².
- Probe (1,344 latch bits + 16.6k logic): 48.9k µm² synthesised → 82–84 % LibreLane utilisation (77 % by
  `--print-stats`), routing DRC 0, Magic DRC 0, LVS 0, antenna 0, gate-level signature = RTL; **CI green** on commit
  992f958: gds, precheck, gl_test, viewer, test, docs.
- Buffers for fan-out and timing add about 19 % to the synthesised area. The slow corner is about 2.1× slower than
  typical. The resizer uses slow `clkdlybuf4s25` cells as fan-out buffers (worth excluding).
- Static timing reports the latch data and gate pins as unconstrained (1 per latch); they are safe by construction
  (SPEC 6.2). About 1,800 slew warnings at the slow corner on the tables' high-fan-out nets.
- RTL = model: `test/test_bpx.py`, 47,500 random operations in 9 runs, cell-level latch tables and flip-flop
  model: every reply, the statistics and every state bit read over SPI. Acknowledge 3 clocks after the request in
  about 93 % of requests, never more than 6 in the test.
- Whole RTL, Yosys: 57.0k µm² with the flow's default cells, 49.9k with drive-strength-1 gates allowed.
- `gh` is logged in on Windows. The Vast CLI's `create instance --raw` prints a per-instance key: do not use `--raw`
  there. `vastai destroy instance` needs `-y`.

## 5. Next steps, in order

1. **Read the CI run** of the commit that made the RTL the top (`gh run list -R Kyrylo-Nazarevych/bpx1`): gds,
   precheck and gl_test must be green; compare its utilisation with the local 75.9 % / 74.3 %.
2. Done: area closure and top switch (docs/area.md, docs/timing.md). Open improvements: slew warnings at the slow
   corner; 50 MHz by registering the lookup one clock earlier; the default-cell and density-86 trial builds.
3. **VR-06 / VR-01 on programs.** Verilator harness (`cosim/`): the CPU contract replaying `out/bench/*/prog.evt`
   against the RTL through the pins, including wrong-path predicts (decode `prog.text`) and FLUSH on JALR; log the
   operation stream; replay it through `model/bpx1.py`. Cross-check cycles and mispredictions against
   `explore/bpsim` (must be equal for the correct path). Report PF-01…PF-03 from RTL.
4. **Random regression to 10⁶ operations** (VR-01) and functional coverage (VR-02): extend `test_bpx.py` or the
   Verilator harness; rent a machine for seeds if needed.
5. **Formal (VR-03)** with SymbiYosys on `BPX_BEHAV`: handshake liveness, queue bounds, FR-01 (predict changes no
   trained state), FR-05, stable outputs between acknowledges, queue empty ⇒ speculative = committed history.
6. **Documents:** `docs/area.md`, `timing.md`, `coverage.md`, `verification.md`, `bringup.md`, update `info.md`;
   MicroPython host and PIO program in `host/`.
7. Lint clean-up: Verilator still reports width-extension warnings in `bpx_core.v`/`bpx_pred.v` (intended
   zero-extensions) and one unused bit; LibreLane lists them as warnings, not errors.

## 6. Commands

```
# from Windows (PowerShell), project folder: mirror into Linux, then run anything from the mirror
wsl.exe -d Ubuntu --cd "<project folder>" -- bash tools/wsl_sync.sh
wsl.exe -d Ubuntu -- bash /home/kyrylo_nazarevych_wsl/work/bpx1/<script>      # never inline $VARIABLES through wsl.exe

# inside WSL, ~/work/bpx1  (source ~/ttsetup/env.sh first)
bash bench/build.sh ; bash bench/ext/build_ext.sh        # benchmarks -> out/bench/<name>/prog.evt (about 2 minutes)
cd test && make -B -f Makefile.bpx [BEHAV=yes]           # RTL against the golden model; OPS=<n> SEED=<n>
cd test && make -B [GATES=yes]                           # repository top (the probe today)
bash tools/harden.sh                                     # local chip build of the repository top + gate-level test
bash tools/trial_build.sh <tag> <density> <clock_ns> <default|small>   # trial build of bpx_chip
python3 explore/sweep.py list <refs|families|chips|chips2|rand> <file>  # candidate lists
out/explore/bpsim btb=256 R=1 [seq=0] <cfg> <out.tsv> out/bench/*/prog.evt
python3 explore/report.py <refs|families|pick [budget]|sens|charts> ; python3 explore/sweep.py effects rand tune
python3 model/bpx1.py out/bench/<name>/prog.evt          # golden model replay of one trace
```

Sweeps on a rented machine: build `bpsim` statically (`g++ -O2 -static -pthread`), tar `out/bench/*/prog.evt`
(13 MB gzipped), copy with the project key `~/.ssh/bpx1_vast_ed25519` (WSL, not in git), run, fetch the `.tsv`
files into `out/explore/`, **verify they are on disk, then destroy the machine**.

## 7. Vast.ai account use (owners' rules)

Up to $12 in total; machines labelled `bpx1-*`; never touch others (`fsk-*`); destroy when idle; report the spend at
every gate. **Spent so far: about $0.80 (ESTIMATED from run times at $0.875/h: 11, 22 and 20 minutes).** One of
the three runs was wasted: the machine was destroyed before its results were fetched.

## 8. Risks

| Risk | Status |
| --- | --- |
| Real design does not fit or route | Open: trial builds (step 1). Fallbacks listed there. |
| Timing at the slow corner: read path about 12–13 ns estimated | Open until the real build; target 50 MHz. A pipeline register hidden in the synchroniser delay is the reserve (SPEC 6 discussion in the session notes: lookup one cycle early from the captured ID). |
| Latch write paths outside static timing | Accepted by construction; shown at gate level; to be stated in `docs/timing.md`. |
| Model numbers, not RTL numbers, behind PF-01…03 | Open until step 3. |
| Benchmark programs have few branch sites (median 49) | Known limit of small embedded programs; Embench's `nsichneu` (640 sites) is the stress case. |
