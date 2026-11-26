# BPX-1 external benchmarks

Well-known public benchmark suites, built as bare-metal **RV32IM** programs on the BPX-1 bench runtime and traced
into the same files as the in-house programs (`bench/README.md`: `prog.evt`, `prog.text`, `summary.json`).

**No third-party source code is stored in this repository.** `build_ext.sh` fetches each suite with `git` from its
public repository, at a pinned commit, into `out/ext/src/` (outside the repository), and compiles it from there.
Everything in `bench/ext/` was written for BPX-1: the build script, the manifest, a small C library, one port per
suite and the tests.

## What is fetched

| Suite | Repository | Pinned commit | What that commit is |
| --- | --- | --- | --- |
| Embench-IoT 1.0 | `https://github.com/embench/embench-iot` | `0466a18e4f6b47e19598d7c6ba72916d54b68f65` | Tag `embench-1.0` (21 Jan 2021): the 19 programs of release 1.0. |
| CoreMark | `https://github.com/eembc/coremark` | `1f483d5b8316753a742cbf5590caf5bd0a4e4777` | Head of `main` on 1 May 2025. The only tag, `v1.01`, predates the Apache licence headers. |
| MiBench | `https://github.com/embecosm/mibench` | `0f3cbcf6b3d589a2b0753cfb9289ddf40b6b9ed8` | Head of `master` (1 Nov 2012). Embecosm's mirror of the original University of Michigan archive, which is not in a git repository. Only three directories are checked out (about 1 MB instead of 256 MB). |

The commits are pinned in `programs_ext.txt`; the repository addresses are in `build_ext.sh` (`suite_url`).

## Licences

Read from the fetched trees at the pinned commits. Nothing is redistributed by this repository in any case.

| Suite / program | Licence, as stated upstream | Where it is stated | May the source be redistributed? |
| --- | --- | --- | --- |
| Embench-IoT 1.0 (all 19 programs, support code) | GPL-3.0-or-later | `COPYING`, `README.md`, an SPDX line in every source file | Yes, under the GPL (copyleft). |
| CoreMark | Apache-2.0, plus the CoreMark Acceptable Use Agreement for the name | `LICENSE.md`, header of every source file | Yes, under Apache-2.0. The name "CoreMark" may only be used for results of the unmodified benchmark run by its rules. |
| MiBench `automotive/bitcount` | Public domain; the directory's `LICENSE` file says it is from snippets.org and free with no restrictions | `LICENSE`, file headers | Yes, by that statement. |
| MiBench `network/dijkstra` | GPL, version not stated | `LICENSE` (two informal lines, no licence text) | Presumably, under the GPL; the statement is informal. |
| MiBench `telecomm/adpcm` | Permission notice of Stichting Mathematisch Centrum (CWI), 1992: use, copy, modify, distribute freely if the notice is kept (HPND style) | `LICENSE`, header of `adpcm.c` | Yes, with the notice. |

Notes.

- Several Embench files also name where their code came from (public domain: `aha-mont64`, `cubic`, `huffbench`,
  `picojpeg`, `wikisort`; revised BSD: `nbody`; other notices in `crc32`, `sglib-combined`, `slre`, `nettle-*`,
  `nsichneu`, `edn`, `matmult-int`). Embench distributes all of them as GPL-3.0-or-later.
- The `coremark` run here is **not a CoreMark score**: it runs 20 iterations with a stub clock. The name only says
  which workload it is.
- MiBench has no licence for the suite as a whole, only a `LICENSE` file per program. Programs without a clear
  statement were left out (see "Not included").

## How to run

All tools run in WSL (Ubuntu). From Windows PowerShell:

```
wsl.exe -d Ubuntu --cd "<project folder>" -- bash tools/wsl_sync.sh
wsl.exe -d Ubuntu -- bash /home/<user>/work/bpx1/bench/ext/build_ext.sh
```

| Command (in the mirror `~/work/bpx1`) | Effect |
| --- | --- |
| `bash bench/ext/build_ext.sh` | Fetch what is missing, then build, run and trace every program of the manifest (6 at a time). About 45 s, including the first fetch. |
| `bash bench/ext/build_ext.sh embench.slre coremark` | Only these programs. |
| `bash bench/ext/build_ext.sh --fetch` | Only fetch. |
| `bash bench/ext/test/recount.sh <name>...` | Recount a program from a raw Spike log, without rvtrace, and compare with `prog.evt`. |
| `bash bench/ext/test/coremark_crc.sh` | Show the CRCs of the CoreMark run. |
| `bash bench/ext/test/mibench_host.sh` | Build and check the MiBench drivers natively (x86-64). |
| `bash bench/ext/test/check_xmath.sh` | Measure the accuracy of `libc/xsqrt.c` and `libc/xmath.c` against the host C library. |

Output: `out/bench/<name>/{prog.elf, prog.evt, prog.text, summary.json}` and `out/ext/summary_ext.tsv` (same columns
as `out/bench/summary.tsv`). Fetched sources: `out/ext/src/<suite>/`. Tools and libraries: `out/ext/rvtrace`,
`out/ext/lib/`. Spike logs are piped into the tracer and never stored. The build needs the network only when a
checkout is missing or not at its pinned commit; a checkout with local changes makes the build fail.

## Layout

| Path | Content |
| --- | --- |
| `build_ext.sh` | Fetch, build, run, trace, summarise. |
| `programs_ext.txt` | Manifest: `name  suite  pinned-commit  licence  scale`. |
| `libc/` | The C library functions that the external sources call and `bench/rt/rt.c` does not have: headers in `include/`; `xlibc.c` (exit, strchr, ctype), `xsqrt.c`, `xmath.c` (sin, cos, atan, acos, exp, log, pow), `xstdio.c` (output is discarded, no files), `xmalloc.c`. |
| `embench/xmain.c` | Board support and `main` for the Embench programs. |
| `coremark/core_portme.{h,c}` | CoreMark port. |
| `mibench/*_main.c`, `mibench/drv.h` | Drivers for the MiBench programs. |
| `test/` | The checks listed above. |

## How the programs are built

Like the in-house programs (`bench/build.sh`): `riscv64-unknown-elf-gcc -march=rv32im -mabi=ilp32 -O2 -ffreestanding
-fno-pic -static -nostdlib -nostartfiles -fno-tree-loop-distribute-patterns`, the runtime `bench/rt` (`crt0.S`,
`link.ld`, `rt.c`; exit through HTIF), `-lgcc`. No compressed instructions and no hardware floating point:
floating-point programs run on libgcc's soft-float routines. The same `bench/rvtrace.cc` writes the traces.
Differences from `bench/build.sh`:

- `-Wall` is off for the third-party sources and their warnings are not shown;
  `-Werror=implicit-function-declaration` is on.
- `bench/ext/libc` is linked as a library (`out/ext/lib/libbpxext.a`), so a program contains only the parts it
  calls. The toolchain has no C library, so this one is ours, not newlib. It matters for the four programs that
  call the mathematics functions: `embench.cubic` (sqrt, cos, acos, atan, pow), `embench.nbody`, `embench.st` and
  `embench.wikisort` (sqrt). Their branch behaviour inside those functions is that of `libc/xsqrt.c` and
  `libc/xmath.c`. Our bit-by-bit `sqrt` alone is 29% of the instructions and 36% of the conditional branches of
  `embench.nbody`, and 10% and 15% of `embench.cubic`; it is a few percent at most in the other two. The rest of
  the floating-point programs is mostly libgcc's soft-float arithmetic (`__muldf3`, `__adddf3`, …), as it would be
  with any C library.
- A third-party source file is never edited. Where a program's own `main` cannot be used, it is renamed on the
  command line (`-Dmain=...`) and a driver from `bench/ext/mibench` takes its place.

### Embench-IoT

Each program is `src/<program>/*.c` plus the suite's own `support/beebsc.c` (its small C library: random numbers,
heap) and our `embench/xmain.c`, which provides `initialise_board`, `start_trigger`, `stop_trigger` (empty: there is
no timer) and a `main` that calls `initialise_benchmark`, `warm_caches`, `benchmark`, `verify_benchmark`.

- **Scale**: the suite's own knob `CPU_MHZ`. A program runs its kernel `LOCAL_SCALE_FACTOR × CPU_MHZ` times
  (`LOCAL_SCALE_FACTOR` is fixed in each source file), plus one warm-up run (`WARMUP_HEAT` = 1).
- **Self-check**: `verify_benchmark` compares the result (a return value or output arrays; floating-point values
  within 1e-13 for double and 1e-5 for float) with the expected values built into each benchmark. `main` returns 0
  only when it accepts; 1 when it rejects; 3 if a benchmark had no check (none of the 19 does that).

All 19 programs of release 1.0 build and pass. Three use double precision (`cubic`, `nbody`, `st`), one single
(`minver`); `wikisort` and `ud` convert between integers and doubles; `cubic` also uses `long double`, which is
128-bit on RISC-V and therefore runs on libgcc's quad-precision routines.

### CoreMark

The five `core_*.c` files plus our port `coremark/core_portme.{h,c}`.

- **Scale**: `ITERATIONS`. Seeds 0, 0, 0x66 and the default 2000-byte data block on the stack: CoreMark's
  "2K performance run".
- **Self-check**: `core_main.c` compares the list, matrix and state CRCs with the values it knows for these seeds,
  and checks the data types. Its `main` always returns 0 and only prints the verdict, so the port's `ee_printf`
  (there is no console) records whether "Correct operation validated" was printed and whether any error message
  was; `portable_fini` then exits with 0 only when CoreMark validated the run.
- **Stub clock**: there is no timer. `get_time` claims 10 seconds, so that CoreMark's rule "at least 10 seconds"
  does not count as an error.
- CRCs of the run with `ITERATIONS=20` (`test/coremark_crc.sh`): seedcrc `0xe9f5`, crclist `0xe714`, crcmatrix
  `0x1fd7`, crcstate `0x8e3a`, crcfinal `0x4983`. The first four are the constants in `core_main.c`; crcfinal
  depends on the iteration count and is not checked by CoreMark.

### MiBench

MiBench programs read input files and print their results; they have no self-check. Each driver here generates
the input in the program, calls the unchanged MiBench functions, and checks the results.

| Program | MiBench files used | Driver | Scale | Self-check |
| --- | --- | --- | --- | --- |
| `mibench.bitcount` | `automotive/bitcount/bitcnt_1.c` … `bitcnt_4.c` | Calls the six exported bit counters through a table of function pointers, each over the same numbers (start + 13·j), as MiBench's `bitcnts.c` does. | `ITERATIONS` | Every counter's total must equal the total of a seventh, shift-and-count loop in the driver. (In MiBench the seventh method is a static function of `bitcnts.c`; `bitcnts.c` itself needs `argv`, `clock` and `printf` and is not used.) |
| `mibench.dijkstra` | `network/dijkstra/dijkstra_small.c` | Fills the 100 × 100 cost matrix (one edge in four absent, others 1..100) and calls `dijkstra(i, 50 + i)` like MiBench's `main`. | `PATHS`, `GOLDEN` | After each call the path is followed back through `rgnNodes`: it must reach the start over existing edges and cost exactly the reported distance. Distances and paths go into a checksum that must equal `GOLDEN`. |
| `mibench.adpcm` | `telecomm/adpcm/src/adpcm.c` | Synthesises 16 blocks of sound once, then codes and decodes blocks of 1000 samples like `rawcaudio`/`rawdaudio`. | `SAMPLES`, `GOLDEN` | After each block the decoder's state must equal the coder's; the mean reconstruction error must stay below 1/8 of the mean signal; the checksum over coded bytes and decoded samples must equal `GOLDEN`. |

`GOLDEN` is the 22-bit fold of the checksum, checked with `rt_check` (`bench/rt/rt.h`). It belongs to the scale:
after changing `PATHS` or `SAMPLES`, run `test/mibench_host.sh`, which builds the same drivers natively and prints
the folds. That native run is also the cross-check on a second machine, and for dijkstra it proves every distance
to be the shortest one (Bellman-Ford).

The driver's own code is part of the trace. Measured shares of the driver's `main` (attributing each stretch of
instructions to the function of the branch or jump that ends it):

| Program | Instructions | Conditional branches | What it is |
| --- | ---: | ---: | --- |
| `mibench.bitcount` | 41% | 59% | The six loops over the numbers and the shift-and-count reference loop. In MiBench the same work (the loops and its own shift-and-count method) is in `bitcnts.c`. |
| `mibench.dijkstra` | 2% | 2% | Matrix fill and path checks. `malloc` and `free` from `libc/xmalloc.c` add 1.4% of the instructions. |
| `mibench.adpcm` | 22% | 14% | Sound synthesis (once), checksum, error measurement. |

## Programs and results

Measured with this manifest (`out/ext/summary_ext.tsv`). "Runs" is the number of kernel executions.

| Name | Scale | Runs | Instructions | Cond. branches | Taken | Static sites | Branches / 1000 instr. |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| embench.aha-mont64 | CPU_MHZ=2 | 847 | 9,073,275 | 919,002 | 77.1% | 33 | 101.3 |
| embench.crc32 | CPU_MHZ=3 | 511 | 12,039,759 | 523,780 | 99.9% | 6 | 43.5 |
| embench.cubic | CPU_MHZ=1 | 11 | 8,518,777 | 828,190 | 54.7% | 462 | 97.2 |
| embench.edn | CPU_MHZ=2 | 175 | 7,044,619 | 716,110 | 96.8% | 26 | 101.7 |
| embench.huffbench | CPU_MHZ=2 | 23 | 5,206,636 | 1,081,492 | 58.4% | 69 | 207.7 |
| embench.matmult-int | CPU_MHZ=2 | 93 | 6,627,224 | 862,758 | 95.2% | 20 | 130.2 |
| embench.minver | CPU_MHZ=1 | 556 | 5,014,429 | 733,760 | 47.1% | 195 | 146.3 |
| embench.nbody | CPU_MHZ=1 | 2 | 6,431,284 | 712,419 | 51.2% | 168 | 110.8 |
| embench.nettle-aes | CPU_MHZ=5 | 391 | 22,093,235 | 383,895 | 62.6% | 28 | 17.4 |
| embench.nettle-sha256 | CPU_MHZ=5 | 2376 | 20,440,898 | 327,903 | 75.4% | 28 | 16.0 |
| embench.nsichneu | CPU_MHZ=1 | 1232 | 2,238,712 | 770,028 | 24.2% | 640 | 344.0 |
| embench.picojpeg | CPU_MHZ=2 | 13 | 8,216,920 | 747,213 | 79.3% | 157 | 90.9 |
| embench.qrduino | CPU_MHZ=2 | 11 | 6,151,132 | 864,748 | 55.2% | 240 | 140.6 |
| embench.sglib-combined | CPU_MHZ=2 | 59 | 5,102,714 | 1,066,967 | 41.7% | 150 | 209.1 |
| embench.slre | CPU_MHZ=2 | 221 | 5,161,551 | 1,085,115 | 30.1% | 107 | 210.2 |
| embench.st | CPU_MHZ=2 | 27 | 8,160,629 | 847,900 | 44.0% | 127 | 103.9 |
| embench.statemate | CPU_MHZ=6 | 11785 | 7,554,817 | 777,967 | 74.2% | 49 | 103.0 |
| embench.ud | CPU_MHZ=2 | 2957 | 6,693,025 | 961,307 | 58.5% | 42 | 143.6 |
| embench.wikisort | CPU_MHZ=4 | 5 | 6,683,538 | 747,256 | 60.0% | 166 | 111.8 |
| coremark | ITERATIONS=20 | 20 | 6,282,455 | 1,168,714 | 55.8% | 219 | 186.0 |
| mibench.bitcount | ITERATIONS=15000 | 7 × 15000 | 5,909,829 | 944,947 | 92.1% | 11 | 159.9 |
| mibench.dijkstra | PATHS=4 | 4 | 5,795,635 | 1,160,555 | 68.3% | 41 | 200.3 |
| mibench.adpcm | SAMPLES=64000 | 64 blocks | 6,442,682 | 1,410,594 | 75.0% | 41 | 219.0 |

The scales were chosen so that every program executes between about 0.3 and 1.5 million conditional branches,
like the in-house set. The two cipher programs have very few branches (16 to 17 per 1000 instructions) and are
therefore the longest (20 to 22 million instructions).

## Not included

- **MiBench, other programs.** Most need file input and an operating system. Of the small integer ones:
  `telecomm/CRC32` and `automotive/basicmath` are the sources that `embench.crc32` and `embench.cubic` were derived
  from; `automotive/qsort` only calls the C library's `qsort`; `office/stringsearch` has its data inside a `main`
  that prints, and one pass is far too short; `security/sha` has no licence statement beyond author credits.
- **Embench-IoT 2.0.** Release 1.0 was preferred and built without difficulty; 2.0 was not tried.

## Tests

| Test | What it shows |
| --- | --- |
| `test/recount.sh` | Instructions, conditional branches, taken branches, JAL, JALR, MUL and DIV counted from the raw Spike log by the mnemonics that Spike's disassembler prints (not by decoding instruction words, as rvtrace does), with the `.text` range and the address of the store to `tohost` taken from binutils, equal the totals stored in `prog.evt`. |
| `test/coremark_crc.sh` | Reads the CRCs and CoreMark's verdict from the simulated memory (Spike debug mode). |
| `test/mibench_host.sh` | The MiBench drivers give the same checksums on x86-64 as on RV32IM under Spike; dijkstra's distances are shortest. |
| `test/check_xmath.sh` | `sqrt` is exactly rounded (4 million arguments, all exponents); sin, cos, atan, acos, exp, log are within 2 ulp of the host library and `pow(x, 1/3)` within 3 ulp (2 million arguments each). General `pow` is only as good as `exp(y·log x)`: its error grows with the size of `y·log x` (141 ulp seen for x up to 100 and y within ±20). |
