#!/usr/bin/env python3
"""Independent re-implementation of bench/README.md's event trace in Python: raw Spike -l log on stdin ->
prog.evt bytes on the given path. Compared byte for byte with rvtrace's prog.evt by the caller.
Trace = first instruction at pc >= 0x80000000 ... instruction before the first jump-to-itself."""
import re, struct, sys

LINE = re.compile(r"^core\s+\d+: 0x([0-9a-f]+) \(0x([0-9a-f]+)\)")

def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v

seq = []
for line in sys.stdin:
    m = LINE.match(line)
    if not m:
        continue
    pc, w = int(m.group(1), 16) & 0xffffffff, int(m.group(2), 16)
    if not seq and pc < 0x80000000:
        continue
    seq.append((pc, w))
    if w == 0x6f:
        break
assert seq[-1][1] == 0x6f, "no jump-to-itself found"
out = []
n_before = lu = mul = div = 0
prev = None
for i in range(len(seq) - 1):           # the last element is the spin jump: only used as "next pc"
    pc, w = seq[i]
    opc = w & 0x7f
    rd, rs1, rs2 = (w >> 7) & 31, (w >> 15) & 31, (w >> 20) & 31
    if prev is not None and prev[1] & 0x7f == 0x03 and (prev[1] >> 7) & 31 != 0:
        prd = (prev[1] >> 7) & 31
        if opc in (0x33, 0x63, 0x23):
            lu += prd in (rs1, rs2)
        elif opc in (0x13, 0x03, 0x67):
            lu += prd == rs1
    if opc == 0x33 and w >> 25 == 1:
        if (w >> 12) & 7 < 4: mul += 1
        else: div += 1
    nxt = seq[i + 1][0]
    kind = {0x63: 0, 0x6f: 1, 0x67: 2}.get(opc)
    if kind is None:
        n_before += 1
    else:
        if kind == 0:
            imm = sext(((w >> 31) & 1) << 12 | ((w >> 7) & 1) << 11 | ((w >> 25) & 0x3f) << 5 | ((w >> 8) & 0xf) << 1, 13)
            target, taken = (pc + imm) & 0xffffffff, int(nxt != pc + 4)
        elif kind == 1:
            imm = sext(((w >> 31) & 1) << 20 | ((w >> 12) & 0xff) << 12 | ((w >> 20) & 1) << 11 | ((w >> 21) & 0x3ff) << 1, 21)
            target, taken = (pc + imm) & 0xffffffff, 1
        else:
            target, taken = nxt, 1
        out.append(struct.pack("<IIII", pc, target, kind | taken << 2 | n_before << 8,
                               min(lu, 0xffff) | min(div, 255) << 16 | min(mul, 255) << 24))
        n_before = lu = mul = div = 0
    prev = (pc, w)
out.append(struct.pack("<IIII", seq[-2][0], 0, 3 | n_before << 8, min(lu, 0xffff) | min(div, 255) << 16 | min(mul, 255) << 24))
with open(sys.argv[1], "wb") as f:
    f.write(struct.pack("<4sIII", b"BPXE", 1, len(out), 0))
    f.write(b"".join(out))
print("%s: %d records regenerated" % (sys.argv[1], len(out)))
