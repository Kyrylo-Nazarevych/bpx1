# BPX-1 handover

Everything a fresh session needs to continue: state, decisions, next steps, commands. Last rewritten at the end of
the verification round (gate G3 work, 26 Nov 2026). Evidence labels as in the requirements specification,
section 10. Read `docs/SPEC.md` next, then `docs/verification.md`.

## 1. State in one paragraph

BPX-1 is a branch-prediction coprocessor for Tiny Tapeout (SKY130, 2×2 tiles). The protocol and the predictor are
**frozen**; the real RTL is the repository top and builds clean in CI. The RTL equals the golden model on all 71
benchmark programs inside the CPU co-simulation and on 10⁶ random operations; functional coverage is complete;
the control properties are proven formally (unbounded). The formal work found and fixed **two RTL defects** after
gate G2 (a read-port deadlock, and a stale flag after a malformed request). Measured on the RTL: the default
predictor makes the U1 CPU 1.1317× faster than static not-taken (1.1353× with the two-requests-per-cycle adapter) at
91.24 % accuracy, ahead of every reference predictor on speedup and accuracy, and worse than the best reference on
no program. **Not done:** see section 5.

## 2. Where things are

| What | Where |
| --- | --- |
| Project folder (source of truth, Windows; **the git repository is here**) | `C:\\Users\\Kyrylo\\Desktop\\Claude Folder\\Branch Predictor Hackathon 2` |
| Linux mirror where every tool runs | `~/work/bpx1` (WSL Ubuntu), filled by `tools/wsl_sync.sh` (which also overwrites the mirror's `.git`); generated data in `~/work/bpx1/out` |
| GitHub | `Kyrylo-Nazarevych/bpx1`, public, branch `main`; Pages enabled (source: GitHub Actions) |
| Requirements | `..\\Branch Predictor Hackaton 1\\plan\\FABLE_ALL_IN_BRIEF.md` (v4.1) and the owners' decisions in `docs/spec_review.md` section 6 |
| Old prototype | `~/work/tt-branch-predictor` (not to be reused; only its `tt/` support tools are symlinked) |

Repository layout: `src/` RTL; `model/` golden model, random-test generator, replay; `test/` cocotb (CI);
`cosim/` Verilator harness, reports, coverage; `formal/` SymbiYosys; `explore/` sweep engine; `bench/` benchmarks;
`host/` MicroPython host; `tools/` scripts; `docs/` documents.

Generated documents (do not edit by hand): `docs/cosim.md`, `docs/cosim_dual.md` (`cosim/report.py`),
`docs/coverage_bins.md` (`cosim/coverage.py`), `docs/exploration_tables.md`. They are written in the mirror: copy
them back to the Windows folder after generating (the sync is one-way, Windows to Linux).

## 3. Decisions (each with its evidence in the named document)

| # | Decision | Evidence |
| --- | --- | --- |
| D1 | Host port: two-phase REQ/ACK, 10-bit ID, 2-bit OP; answer at the third clock after the request. PREDICT+RESOLVE = predict, then resolve the oldest (resolve first if the queue is full). Queue depth 4. | SPEC 2–3; owners accepted the readings of FR-03 and FR-04 |
| D2 | Control port: SPI mode 0 with 16-bit registers on `uio[4..6]` + `uo[7]`; all `uio` are inputs. | SPEC 4 |
| D3 | Storage: latch tables written by flop-generated pulses; one write sequencer; one read port per table; training from a staging register. Clock-derived gates are impossible in this flow (clock-tree synthesis adds 44,700 µm²). | SPEC 6; probes **(VERIFIED)** |
| D4 | No hard macro. | spec_review 4 **(VERIFIED by the research agent)** |
| D5 | Predictor: bimodal 64 + chooser 64, gshare 128 (4 history bits), three tagged tables 16 × 12 bits with histories 4/9/20 over the tournament, 8-bit allocation LFSR; 1,088 bits. | SPEC 7; exploration.md; cosim.md **(VERIFIED on RTL)** |
| D6 | U1 CPU contract: five stages with a 256-entry branch-target buffer at fetch; owners delegated the choice. | SPEC 9 |
| D7 | Benchmark set frozen: 71 programs; `embench.nbody`/`cubic` out of the aggregates; 51 tuning / 18 held-out. | exploration.md 1 |
| D8 | Statistics: 16-bit saturating counters, snapshot and snapshot-and-clear; no raw writes to the histories. | SPEC 4 |
| D9 | Single clock domain; data pins use one capture flop qualified by the 2-flop synchronised REQ. | `src/bpx_chip.v` header |
| D10 | CPU adapter: a resolve that reports a misprediction is sent alone; the predict of that cycle follows one cycle later, or in the same cycle if the adapter can send two requests per CPU cycle (needs 8 chip clocks per CPU cycle). No chip change. | SPEC 9 rules 4 and 7; cosim.md against cosim_dual.md **(VERIFIED)** |
| D11 | The tables' read port is reserved for a predict only in the clock it executes (deadlock fix). | verification.md 4 **(VERIFIED: proof, regression)** |

## 4. Verified facts worth not rediscovering

- LibreLane's synthesis may not use drive-strength-1 gates (`no_synth.cells`); explicitly instantiated cells are
  unaffected. The repository's `src/config.json` allows the small cells and excludes the slow delay buffers.
- Table cost after synthesis: `550 + entries × (20.5 × bits + 6.5)` µm².
- Local build of the final RTL: 75.6 % utilisation, DRC/LVS/antenna 0, setup +4.83 ns at the slow corner for
  25 ns, hold +0.06 ns, gate-level tests pass (docs/area.md, docs/timing.md); the build takes 17 minutes. CI of `ee6574a` (real chip) and of `dacaf07`
  (deadlock fix): gds, precheck, gl_test, viewer, test, docs all green.
- **Commit and push from the Windows folder** (Git Bash or PowerShell; `gh` is logged in there). Never commit in
  the mirror: the next sync replaces the mirror's `.git` with the Windows one and the commit is gone (this happened
  once in this round; nothing was lost because the working tree is the Windows folder). A `git push` inside WSL
  hangs: it has no credential.
- The co-simulation of all 71 programs takes 18 minutes on this PC with 10 jobs; 10⁶ random operations 35 seconds;
  the formal tasks 0.5 to 12 minutes each with small tables. Full-size tables make each BMC step 10 to 30 s.
- Never `pkill -f <pattern>` through `wsl.exe -- bash -c "..."`: the pattern matches the shell that runs it. Put
  kills in a script file.
- `explore/bpsim` and the golden model differ slightly for the default predictor (0.03 points of mean accuracy):
  they step the allocation randomiser at different moments. With it off they are identical; they agree for the
  references (verification.md 2).
- The Vast CLI's `create instance --raw` prints a per-instance key: do not use `--raw` there. `vastai destroy
  instance` needs `-y`.

## 5. Next steps, in order

1. **Read the CI run of the last push** (`gh run list -R Kyrylo-Nazarevych/bpx1`): gds, precheck, gl_test (now with
   the raw-access write test at gate level). Then update docs/area.md and docs/timing.md if the numbers moved (the
   fixes touch the read-select path; local slack was +3.35 ns). A local `bash tools/harden.sh` gives the numbers CI
   does not print.
2. **Owners' decision on D10** (below, section 9) and anything else they want before G3 is declared.
3. Done (tag `g3`): concurrent-control-port mode of the random regression (`CONC=1 bash cosim/random.sh`,
   verification.md 2a); it catches the deadlock when the fix is reverted.
4. Host kit on hardware: `host/bpx1.py` has not run on a demo board; microcotb port of `test/test.py`
   (docs/bringup.md 6).
5. Optional improvements, none blocking: slew warnings at the slow corner on the table select nets; 50 MHz by
   registering the lookup one clock earlier; Verilator width-extension lint warnings; the remaining factorial
   trial builds (density 86, default cells).
6. G4: submission to the shuttle is the owners' step.

## 6. Commands

```
# from Windows (PowerShell), project folder: mirror into Linux, then run anything from the mirror
wsl.exe -d Ubuntu --cd "<project folder>" -- bash tools/wsl_sync.sh
wsl.exe -d Ubuntu -- bash /home/kyrylo_nazarevych_wsl/work/bpx1/<script>      # never inline $VARIABLES through wsl.exe

# inside WSL, ~/work/bpx1  (source ~/ttsetup/env.sh first)
bash bench/build.sh ; bash bench/ext/build_ext.sh        # benchmarks -> out/bench/<name>/prog.evt (about 2 minutes)
cd test && make -B [GATES=yes]                           # what CI runs: repository top against the golden model
cd test && make -B -f Makefile.bpx [BEHAV=yes]           # bpx_chip with latch cells or the flip-flop model
bash cosim/run.sh ; python3 cosim/report.py              # 71 programs against the RTL -> docs/cosim.md
BPX_DUAL=1 OUT=out/cosim_dual bash cosim/run.sh 16 ; python3 cosim/report.py out/cosim_dual    # -> docs/cosim_dual.md
bash cosim/random.sh 10 100000                           # random operations, coverage -> docs/coverage_bins.md
CONC=1 bash cosim/random.sh 10 100000                    # control port concurrent with requests (verification.md 2a)
bash formal/run.sh <prove|bmc|cover|nowait|bmcfull>      # formal tasks
bash tools/harden.sh                                     # local chip build of the repository top + gate-level test
python3 explore/report.py <refs|families|pick [budget]|sens|charts>     # exploration reports (model)
```

## 7. Vast.ai account use (owners' rules)

Up to $12 in total; machines labelled `bpx1-*`; never touch others (`fsk-*`); destroy when idle; report the spend at
every gate. **Spent so far: about $0.80 (ESTIMATED from run times at $0.875/h), all in the exploration round. The
verification round used no rented machine:** everything ran on the PC in minutes.

## 8. Risks

| Risk | Status |
| --- | --- |
| A defect like the deadlock that neither simulation nor the present proofs see | Reduced, not gone. The proofs cover the sequencer under free inputs; the predictor arithmetic, the SPI shifter and the synchronisers are covered by simulation only. Step 3 above narrows it further. |
| Latch write paths outside static timing | Accepted by construction; exercised at cell and gate level; bring-up has a procedure for it (docs/bringup.md 4). |
| Timing after the fixes | Closed locally: +4.83 ns at the slow corner. |
| Benchmark programs have few branch sites (median 49) | Known limit of small embedded programs; Embench's `nsichneu` (640 sites) is the stress case. |
| The FPGA CPU does not exist yet | The contract (SPEC 9) is what the numbers assume. Sensitivity: docs/exploration.md (no BTB 1.056×; later resolve 1.198× / 1.258×). |

## 9. Open questions for the owners

1. **D10, the adapter mode to quote for O1.** With one request per CPU cycle the speedup is 1.1317×; with two
   requests in the cycles that need them it is 1.1353×. The second needs a slightly more complex adapter on the FPGA
   and a CPU clock of at most one eighth of the chip clock (5 MHz at 40 MHz), which the 1 MHz CPU of IR-07 meets
   easily. Recommendation: quote both, build the second.
2. Whether G3 needs the concurrent-control-port simulation (step 3) or the formal proof suffices for it.
