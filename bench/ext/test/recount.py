#!/usr/bin/env python3
"""BPX-1 bench/ext test: recount a program's totals from a raw Spike log, independently of rvtrace.

    spike --isa=rv32im -l --log=/dev/stdout prog.elf | recount.py <prog.evt> <text_lo> <text_hi> <store_pc>

rvtrace decodes the 32-bit instruction words. This script does not look at them: it classifies each
executed instruction by the mnemonic that Spike's disassembler printed in the log line

    core   0: 0x80000058 (0x00b50863) beq     a0, a1, pc + 16

  conditional branch   mnemonic starts with "b" (beq bne blt bge bltu bgeu beqz bnez bltz bgez blez bgtz;
                       RV32IM has no other mnemonic starting with "b")
  taken                the next executed pc is not pc + 4
  JAL / JALR           j, jal / jr, jalr, ret
  MUL / DIV            mul mulh mulhsu mulhu / div divu rem remu

It counts the instructions whose pc lies in [text_lo, text_hi), up to and including the first
execution of store_pc (the store to HTIF tohost; the program only spins after it). The addresses
are given in hex by recount.sh, which takes them from binutils (objdump, nm).
Then it reads the totals out of prog.evt (format: bench/README.md) and compares.
Exit status 0 when every total is equal."""
import struct
import sys


def evt_totals(path):
    data = open(path, "rb").read()
    magic, version, n, _ = struct.unpack_from("<4sIII", data, 0)
    if magic != b"BPXE" or version != 1 or len(data) != 16 + 16 * n:
        sys.exit("recount: %s is not a version 1 event trace" % path)
    t = dict(instructions=0, cond=0, taken=0, jal=0, jalr=0, mul=0, div=0)
    for i in range(n):
        _, _, info, extra = struct.unpack_from("<IIII", data, 16 + 16 * i)
        kind = info & 3
        t["instructions"] += (info >> 8) + (0 if kind == 3 else 1)
        if kind == 0:
            t["cond"] += 1
            t["taken"] += (info >> 2) & 1
        elif kind == 1:
            t["jal"] += 1
        elif kind == 2:
            t["jalr"] += 1
        t["div"] += (extra >> 16) & 0xff        # exact unless a span saturated at 255 (rvtrace notes it)
        t["mul"] += (extra >> 24) & 0xff
    return t


def log_totals(stream, lo, hi, store_pc):
    t = dict(instructions=0, cond=0, taken=0, jal=0, jalr=0, mul=0, div=0)
    muls = {"mul", "mulh", "mulhsu", "mulhu"}
    divs = {"div", "divu", "rem", "remu"}
    branch_pc = None                # pc of the previous instruction when it was a conditional branch
    for line in stream:
        if not line.startswith("core"):
            continue
        f = line.split(None, 5)
        if len(f) < 5 or not f[2].startswith("0x") or not f[3].startswith("(0x"):
            continue
        pc = int(f[2], 16)
        if branch_pc is not None:
            if pc != branch_pc + 4:
                t["taken"] += 1
            branch_pc = None
        if not lo <= pc < hi:
            continue
        m = f[4]
        t["instructions"] += 1
        if m[0] == "b":
            t["cond"] += 1
            branch_pc = pc
        elif m in ("j", "jal"):
            t["jal"] += 1
        elif m in ("jr", "jalr", "ret"):
            t["jalr"] += 1
        elif m in muls:
            t["mul"] += 1
        elif m in divs:
            t["div"] += 1
        if pc == store_pc:
            return t
    sys.exit("recount: the log ends before the store to tohost")


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    evt = evt_totals(sys.argv[1])
    lo, hi, store_pc = (int(a, 16) for a in sys.argv[2:5])
    log = log_totals(sys.stdin, lo, hi, store_pc)
    ok = True
    print("%-14s %12s %12s" % ("", "raw log", "prog.evt"))
    for k in ("instructions", "cond", "taken", "jal", "jalr", "mul", "div"):
        same = log[k] == evt[k]
        ok = ok and same
        print("%-14s %12d %12d  %s" % (k, log[k], evt[k], "equal" if same else "DIFFERENT"))
    print("recount: " + ("MATCH" if ok else "MISMATCH"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
