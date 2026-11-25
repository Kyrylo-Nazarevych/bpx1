#!/usr/bin/env python3
"""Checks every out/bench/<name>/{prog.evt, prog.text, summary.json} against bench/README.md with code that
shares nothing with rvtrace: header, record layout, the instruction-count identity, totals against
summary.json, END record, and that every conditional-branch / JAL target equals pc + imm decoded
from prog.text (all records; plus the 1000-random-record sample the brief asks for)."""
import json, os, random, struct, subprocess, sys

ROOT = os.path.expanduser("~/work/bpx1")
OUT = os.path.join(ROOT, "out/bench")
names = []
for line in open(os.path.join(ROOT, "bench/programs.txt")):
    p = line.split()
    if len(p) >= 4 and not p[0].startswith("#"):
        names.append(p[0])

def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v

def imm_b(w):
    return sext(((w >> 31) & 1) << 12 | ((w >> 7) & 1) << 11 | ((w >> 25) & 0x3f) << 5 | ((w >> 8) & 0xf) << 1, 13)

def imm_j(w):
    return sext(((w >> 31) & 1) << 20 | ((w >> 12) & 0xff) << 12 | ((w >> 20) & 1) << 11 | ((w >> 21) & 0x3ff) << 1, 21)

bad = 0
tot = {"records": 0, "instructions": 0, "cond": 0}
sample_report = None
for name in names:
    d = os.path.join(OUT, name)
    errs = []
    s = json.load(open(os.path.join(d, "summary.json")))
    text = open(os.path.join(d, "prog.text"), "rb").read()
    base, nwords = struct.unpack_from("<II", text, 0)
    if len(text) != 8 + 4 * nwords: errs.append("prog.text size")
    words = struct.unpack_from("<%dI" % nwords, text, 8)
    if s["text_bytes"] != 4 * nwords: errs.append("text_bytes")
    # prog.text against an independent tool
    raw = subprocess.run(["riscv64-unknown-elf-objcopy", "-O", "binary", "-j", ".text", os.path.join(d, "prog.elf"), "/dev/stdout"],
                         capture_output=True, check=True).stdout
    if raw != text[8:]: errs.append("prog.text differs from objcopy -j .text")
    nm = subprocess.run(["riscv64-unknown-elf-nm", os.path.join(d, "prog.elf")], capture_output=True, text=True, check=True).stdout
    syms = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
    if base != syms["_start"]: errs.append("base != _start")

    evt = open(os.path.join(d, "prog.evt"), "rb").read()
    magic, version, nrec, reserved = struct.unpack_from("<4sIII", evt, 0)
    if magic != b"BPXE" or version != 1 or reserved != 0: errs.append("header")
    if len(evt) != 16 + 16 * nrec: errs.append("file size vs n_records")
    instr = cond = taken = jal = jalr = lu = mul = div = ends = sat = 0
    sites = set()
    bad_target = bad_kind = bad_range = 0
    cond_idx = []
    prev_target = None
    flow_bad = 0
    recs = struct.iter_unpack("<IIII", evt[16:])
    last = None
    for i, (pc, target, info, extra) in enumerate(recs):
        kind, tk, nb = info & 3, (info >> 2) & 1, info >> 8
        if info & 0xf8: errs.append("info[7:3] not zero"); break
        l, dv, ml = extra & 0xffff, (extra >> 16) & 0xff, extra >> 24
        if l == 0xffff or dv == 0xff or ml == 0xff: sat += 1
        lu += l; div += dv; mul += ml
        off = pc - base
        if off < 0 or off >= 4 * nwords or pc & 3:
            bad_range += 1
            continue
        w = words[off >> 2]
        # straight-line consistency: this record's pc = where the previous transfer went + 4 * n_before
        if prev_target is not None and kind != 3 and pc != prev_target + 4 * nb: flow_bad += 1
        if prev_target is not None and kind == 3 and pc != prev_target + 4 * (nb - 1): flow_bad += 1
        if kind == 3:
            ends += 1
            instr += nb
            if tk or target != 0: errs.append("END fields")
            if i != nrec - 1: errs.append("END not last")
            if pc != syms["__tohost_store"]: errs.append("END pc is not the tohost store")
            continue
        instr += nb + 1
        if kind == 0:
            cond += 1; taken += tk; sites.add(pc)
            if w & 0x7f != 0x63: bad_kind += 1
            elif target != (pc + imm_b(w)) & 0xffffffff: bad_target += 1
            cond_idx.append(i)
            prev_target = target if tk else pc + 4
        elif kind == 1:
            jal += 1
            if not tk: errs.append("JAL not taken")
            if w & 0x7f != 0x6f: bad_kind += 1
            elif target != (pc + imm_j(w)) & 0xffffffff: bad_target += 1
            prev_target = target
        else:
            jalr += 1
            if not tk: errs.append("JALR not taken")
            if w & 0x7f != 0x67: bad_kind += 1
            prev_target = target
    if ends != 1: errs.append("%d END records" % ends)
    if bad_target: errs.append("%d wrong targets" % bad_target)
    if bad_kind: errs.append("%d records whose word in prog.text is another kind" % bad_kind)
    if bad_range: errs.append("%d pcs outside prog.text" % bad_range)
    if flow_bad: errs.append("%d records not where straight-line flow puts them" % flow_bad)
    exp = (("instructions", instr), ("cond_branches", cond), ("cond_taken", taken), ("static_cond_sites", len(sites)),
           ("jal", jal), ("jalr", jalr))
    for k, v in exp:
        if s[k] != v: errs.append("%s: summary %d, evt %d" % (k, s[k], v))
    if sat == 0:
        for k, v in (("loaduse", lu), ("mul", mul), ("div", div)):
            if s[k] != v: errs.append("%s: summary %d, evt %d" % (k, s[k], v))
    if s["spike_status"] != 0: errs.append("spike_status %d" % s["spike_status"])
    if s["name"] != name: errs.append("name")
    # the brief's sample: 1000 random conditional-branch records, target == pc + imm from prog.text
    rnd = random.Random(12345)
    pick = [cond_idx[rnd.randrange(len(cond_idx))] for _ in range(1000)]
    ok = 0
    for i in pick:
        pc, target, info, extra = struct.unpack_from("<IIII", evt, 16 + 16 * i)
        w = words[(pc - base) >> 2]
        if info & 3 == 0 and w & 0x7f == 0x63 and target == (pc + imm_b(w)) & 0xffffffff: ok += 1
    if ok != 1000: errs.append("sample: %d of 1000" % ok)
    tot["records"] += nrec; tot["instructions"] += instr; tot["cond"] += cond
    print("%-22s %s  records %8d  identity %9d == %9d  sample %d/1000  saturated %d%s" % (
        name, "ok  " if not errs else "FAIL", nrec, instr, s["instructions"], ok, sat, "  " + "; ".join(errs) if errs else ""))
    bad += bool(errs)
print("programs %d, bad %d, records %d, instructions %d, conditional branches %d" % (len(names), bad, tot["records"], tot["instructions"], tot["cond"]))
sys.exit(1 if bad else 0)
