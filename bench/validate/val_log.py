#!/usr/bin/env python3
"""Independent count straight from a raw Spike -l log on stdin (not using rvtrace or the ELF).
Classification by Spike's disassembly text (mnemonics); load-use by the README rule on the hex word.
Trace = from the first instruction at pc >= 0x80000000 to the instruction before the first
"jump to itself" (0x0000006f), i.e. up to and including the store to tohost.
usage: spike --isa=rv32im -l --log=/dev/stdout prog.elf | python3 val_log.py <name>"""
import json, re, sys

LINE = re.compile(r"^core\s+\d+: 0x([0-9a-f]+) \(0x([0-9a-f]+)\)\s+(\S+)")
MUL = {"mul", "mulh", "mulhsu", "mulhu"}
DIV = {"div", "divu", "rem", "remu"}
JAL = {"j", "jal"}
JALR = {"jr", "jalr", "ret"}
RS12 = {0x33, 0x63, 0x23}
RS1 = {0x13, 0x03, 0x67}

n = cond = taken = jal = jalr = mul = div = loaduse = 0
sites = set()
prev_branch_pc = None
prev_load_rd = 0
started = False
other_b = set()
for line in sys.stdin:
    m = LINE.match(line)
    if not m:
        continue
    pc = int(m.group(1), 16) & 0xffffffff
    word = int(m.group(2), 16)
    mn = m.group(3)
    if not started:
        if pc < 0x80000000:
            continue
        started = True
    if prev_branch_pc is not None:
        if pc != prev_branch_pc + 4:
            taken += 1
        prev_branch_pc = None
    if word == 0x0000006f:
        break
    n += 1
    opc = word & 0x7f
    if prev_load_rd:
        rs1, rs2 = (word >> 15) & 31, (word >> 20) & 31
        if (opc in RS12 and prev_load_rd in (rs1, rs2)) or (opc in RS1 and prev_load_rd == rs1):
            loaduse += 1
    prev_load_rd = (word >> 7) & 31 if opc == 0x03 else 0
    if mn[0] == "b":
        cond += 1
        sites.add(pc)
        prev_branch_pc = pc
        other_b.add(mn)
    elif mn in JAL:
        jal += 1
    elif mn in JALR:
        jalr += 1
    elif mn in MUL:
        mul += 1
    elif mn in DIV:
        div += 1
print(json.dumps({"name": sys.argv[1], "instructions": n, "cond_branches": cond, "cond_taken": taken,
                  "static_cond_sites": len(sites), "jal": jal, "jalr": jalr, "loaduse": loaduse, "mul": mul,
                  "div": div, "branch_mnemonics": sorted(other_b)}))
