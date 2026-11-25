# BPX-1 benchmark set

Programs that the U1 CPU model runs (spec EX-01, VR-06). Every benchmark is a bare-metal **RV32IM** C program,
compiled with `riscv64-unknown-elf-gcc -march=rv32im -mabi=ilp32 -O2`, executed by Spike, and converted into an
**event trace** that the cycle-level CPU model replays. Program behaviour does not depend on the branch predictor
(the CPU is in-order and precise), so the trace is recorded once and is exact for every predictor.

## Layout

| Path | Content |
| --- | --- |
| `programs.txt` | Manifest: one line per program: `name  source.c  SEL  origin`. |
| `src/*.c` | Sources. One file holds several programs; `-DSEL=<n>` picks one. |
| `rt/` | Runtime: `crt0.S`, `link.ld`, `rt.c` (exit through Spike's HTIF `tohost`, small libc). |
| `rvtrace.cc` | Converts a Spike instruction log (stdin) into `.evt`; dumps `.text`. |
| `build.sh` | Builds, runs and traces every program in the manifest into `out/bench/<name>/`. |

Outputs are not committed (size): `out/bench/<name>/{prog.elf, prog.evt, prog.text, summary.json}` and
`out/bench/summary.tsv`.

## Event trace `prog.evt`

Little-endian. A 16-byte header, then 16-byte records, one per executed **control-transfer instruction**, in order.

```
header:  char magic[4] = "BPXE";  u32 version = 1;  u32 n_records;  u32 reserved = 0
record:  u32 pc;  u32 target;  u32 info;  u32 extra
```

| Field | Meaning |
| --- | --- |
| `pc` | Address of the control-transfer instruction. |
| `target` | Conditional branch: `pc + imm` (the taken target, whatever the outcome). JAL: its target. JALR: the address actually jumped to. END: 0. |
| `info[1:0]` | Kind: 0 conditional branch, 1 JAL, 2 JALR, 3 END (last record, written once when the program exits; `pc` = last executed pc). |
| `info[2]` | Taken. Conditional branch: 1 if taken. JAL, JALR: 1. END: 0. |
| `info[31:8]` | `n_before`: instructions executed after the previous record's instruction and before this one. |
| `extra[15:0]` | `n_loaduse`: load-use hazards in this span (the `n_before` instructions plus this instruction). |
| `extra[23:16]` | `n_div`: DIV, DIVU, REM, REMU executed in this span (saturating at 255). |
| `extra[31:24]` | `n_mul`: MUL, MULH, MULHSU, MULHU executed in this span (saturating at 255). |

A **load-use hazard** is an instruction that reads, through a source register its format really uses, the
destination register (not `x0`) of the immediately preceding instruction, when that preceding instruction is a load
(opcode `0000011`). Source registers by opcode: `0110011` (OP), `1100011` (BRANCH), `0100011` (STORE): rs1 and rs2;
`0010011` (OP-IMM), `0000011` (LOAD), `1100111` (JALR): rs1; all others: none.

Total executed instructions = Σ (`n_before` + 1) over all records except END, + END's `n_before`.

## Text image `prog.text`

Little-endian: `u32 base; u32 n_words;` then `n_words` raw 32-bit instruction words of the executable sections,
from address `base`. The CPU model decodes it to follow wrong paths (which it never executes).
