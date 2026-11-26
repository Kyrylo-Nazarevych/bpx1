#!/usr/bin/env python3
"""Validation of the fault injection in explore/bpsim against the golden model model/bpx1.py (docs/fault_campaign.md).

  fault_validate.py <bpsim binary> <workdir> <trace.evt>... [n=30000]

For each trace, its first n conditional branches are written to a short trace; then, one branch in flight (bpsim's
sequential instance, the model's PREDICT then RESOLVE):
  1. tables: a fixed random list of (branch, table, entry, bit) flips over the bimodal, chooser, gshare and W0-W3
     tables is applied in bpsim (flist=) and in the model (raw_write just before the resolve of that branch);
  2. twin flips: the same with flips into the twin, for every CODE x RESTORE with TWIN_EN = TWIN_USE = 1;
  3. twin decay: bpsim logs the decay events of the copy the mode uses (dlog=); the model applies each as a raw write
     of the twin word with that bit at its preferred level, just before the resolve at which it takes effect.
Compared exactly: the misprediction counts of all four predictors (model statistics, 32-bit) and the twin's flag
counts (sum over predicts of FSTAT bits 0-3 as set by that predict).
"""
import os
import random
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "model"))
import bpx1  # noqa: E402

CHIP = "gehlr te=32 th=0,8,9,20 cb=4 ub=2 ug=4 ut=4 theta=10 dyn=1 sx=1 sk=0 pp=8"
TBITS = [2, 2, 2, 4, 4, 4, 4, 8]
TSIZE = [64, 64, 128, 32, 32, 32, 32, 32]


def short_trace(src, dst, n):
    """First n conditional branches of src (with the records before them) as a trace of its own."""
    data = open(src, "rb").read()
    hdr = list(struct.unpack_from("<4I", data, 0))
    cnt, keep = 0, 0
    for i in range(hdr[2]):
        info = struct.unpack_from("<I", data, 16 + 16 * i + 8)[0]
        if info & 3 == 3:
            break
        keep = i + 1
        if info & 3 == 0:
            cnt += 1
            if cnt == n:
                break
    hdr[2] = keep
    with open(dst, "wb") as f:
        f.write(struct.pack("<4I", *hdr) + data[16:16 + 16 * keep])
    return cnt


def branches(path):
    data = open(path, "rb").read()
    n = struct.unpack_from("<I", data, 8)[0]
    out = []
    for i in range(n):
        pc, target, info, extra = struct.unpack_from("<IIII", data, 16 + 16 * i)
        if info & 3 == 3:
            break
        if info & 3 == 0:
            out.append((bpx1.fold_id(pc, 8), (info >> 2) & 1))
    return out


def bpsim(binary, work, trace, line, tag):
    cfg, tsv = os.path.join(work, tag + ".cfg"), os.path.join(work, tag + ".tsv")
    open(cfg, "w").write(f"v gehlr {line}\n")
    subprocess.check_call([binary, "W=8", "threads=1", "seq=1", cfg, tsv, trace], stderr=subprocess.DEVNULL)
    rows = open(tsv).read().splitlines()
    head, vals = rows[0].split("\t"), rows[1].split("\t")
    r = dict(zip(head, vals))
    x = dict(kv.split("=") for kv in r["x_seq"].split(";"))
    return int(r["mis_seq"]), x


MUT = int(os.environ.get("FV_MUT", "0"))     # mutation check: 1 = the model applies each event one branch late


def model_run(brs, events, fctl):
    """events: dict branch index (1-based) -> list of ('flip', table, entry, bit) or ('decay', entry, bit, level)."""
    m = bpx1.Bpx1(bpx1.Config(STATW=32))
    m.write_fctl(fctl)
    flags = [0, 0, 0, 0]
    for k, (ident, taken) in enumerate(brs, 1):
        m.predict(ident)
        for b in range(4):
            flags[b] += (m.last_twf >> b) & 1
        for ev in events.get(k - MUT, ()):
            if ev[0] == "flip":
                _, t, e, b = ev
                if t == 0:
                    m.raw_write(0, e, m.bim[e] ^ (1 << b), mask=1)
                elif t == 1:
                    m.raw_write(0, e, (m.cho[e] ^ (1 << b)) << 2, mask=2)
                elif t == 2:
                    m.raw_write(1, e, m.gsh[e] ^ (1 << b))
                elif t < 7:
                    m.raw_write(8 + t - 3, e, (m.wt[t - 3][e] & 15) ^ (1 << b))
                else:
                    m.raw_write(bpx1.SPACE_TWIN, e, m.tw[e] ^ (1 << b))
            else:
                _, e, b, lv = ev
                m.raw_write(bpx1.SPACE_TWIN, e, (m.tw[e] & ~(1 << b)) | (lv << b))
        m.resolve(taken)
    return list(m.mis), flags


def main():
    binary, work = sys.argv[1], sys.argv[2]
    n = int(next((a.split("=")[1] for a in sys.argv[3:] if a.startswith("n=")), 30000))
    traces = [a for a in sys.argv[3:] if not a.startswith("n=")]
    os.makedirs(work, exist_ok=True)
    rng = random.Random(20261126)
    fails, checks = 0, 0

    def cmp(label, bm4, bev, mm, mf):
        nonlocal fails, checks
        checks += 1
        ok = bm4 == mm and (bev is None or bev == mf)
        fails += not ok
        print(f"  {'OK  ' if ok else 'FAIL'} {label:34s} bpsim mis {bm4} model {mm}" + ("" if bev is None else f" | flags bpsim {bev} model {mf}"))

    for tr in traces:
        name = tr.split("/")[-2]
        st = os.path.join(work, name + ".evt")
        nb = short_trace(tr, st, n)
        brs = branches(st)
        print(f"{name}: {nb} branches")
        # 1. table flips: 400 flips over tables 0-6, a few at the same branch
        fl = sorted((rng.randint(1, nb), t, rng.randrange(TSIZE[t]), rng.randrange(TBITS[t]))
                    for t in (rng.randrange(7) for _ in range(400)))
        fl += [(fl[10][0], 3, 5, 0), (fl[10][0], 6, 5, 3)]
        fl.sort()
        fp = os.path.join(work, name + ".flips")
        open(fp, "w").write("".join(f"{k} {t} {e} {b}\n" for k, t, e, b in fl))
        ev = {}
        for k, t, e, b in fl:
            ev.setdefault(k, []).append(("flip", t, e, b))
        ms, x = bpsim(binary, work, st, f"{CHIP} flist={fp}", name + "_tab")
        bm4 = [int(v) for v in x["m4"].split(",")]
        mm, _ = model_run(brs, ev, 0)
        cmp(f"table flips ({len(fl)}), FCTL=0", bm4, None, mm, None)
        assert ms == bm4[0] and int(x["nf"]) == len(fl), (ms, bm4, x["nf"])
        ms0, x0 = bpsim(binary, work, st, CHIP, name + "_plain")
        print(f"    plain default {ms0} -> with flips {ms}; flips injected {x['nf']}")
        # 2. twin flips, every CODE x RESTORE; 300 flips (some double and triple in one word)
        tf = sorted((rng.randint(1, nb), 7, rng.randrange(32), rng.randrange(8)) for _ in range(300))
        k0 = tf[50][0]
        tf += [(k0, 7, 9, 1), (k0, 7, 9, 6), (k0 + 1, 7, 12, 0), (k0 + 1, 7, 12, 2), (k0 + 1, 7, 12, 5)]
        tf.sort()
        fp = os.path.join(work, name + ".twflips")
        open(fp, "w").write("".join(f"{k} {t} {e} {b}\n" for k, t, e, b in tf))
        ev = {}
        for k, t, e, b in tf:
            ev.setdefault(k, []).append(("flip", t, e, b))
        for code in range(4):
            for rest in range(2):
                ms, x = bpsim(binary, work, st, f"{CHIP} tws=x flist={fp} twuse=1 code={code} rest={rest}", f"{name}_tw{code}{rest}")
                bm4 = [int(v) for v in x["m4"].split(",")]
                sh = [int(v) for v in x[f"s0c{code}r{rest}"].split(",")]
                mm, mf = model_run(brs, ev, 1 | 2 | (code << 2) | (16 if rest else 0))
                cmp(f"twin flips CODE {code} RESTORE {rest}", [ms] + bm4[1:], sh[1:], mm, mf)
                assert sh[0] == ms, (sh, ms)
        # 3. twin decay, exponential and log-normal, mixed preferred level, every CODE x RESTORE
        for spec in ("e300pm", "l1000p0", "e3000p1"):
            for code in range(4):
                for rest in range(2):
                    lg = os.path.join(work, f"{name}_{spec}_{code}{rest}.dlog")
                    ms, x = bpsim(binary, work, st, f"{CHIP} tws={spec} dseed=7 twuse=1 code={code} rest={rest} dlog={lg}", f"{name}_d{code}{rest}")
                    bm4 = [int(v) for v in x["m4"].split(",")]
                    sh = [int(v) for v in x[f"s0c{code}r{rest}"].split(",")]
                    ev = {}
                    nd = 0
                    for ln in open(lg):
                        D, e, b, lv = map(int, ln.split())
                        if D <= nb:
                            ev.setdefault(D, []).append(("decay", e, b, lv))
                            nd += 1
                    mm, mf = model_run(brs, ev, 1 | 2 | (code << 2) | (16 if rest else 0))
                    cmp(f"decay {spec} CODE {code} RESTORE {rest} ({nd} ev)", [ms] + bm4[1:], sh[1:], mm, mf)
    print(f"{checks - fails} of {checks} comparisons equal")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
