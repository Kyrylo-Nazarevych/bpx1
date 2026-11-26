#!/usr/bin/env python3
"""Perceptron-family exploration (owner requirement of 26 Nov 2026: the default predictor is a perceptron).

  percep.py rand [n] [seed]        random sample of the design space of `gehlr` (explore/bpsim.cc) on all programs
  percep.py list <n> <seed> <file>  write the candidate list only (for a cloud run)
  percep.py file <stage> <file>    run the candidates listed in a file (lines: name gehlr key=value ...)
  percep.py top <stage> [n] [maxbits]     best candidates (sorted on the tuning programs; held-out shown)
  percep.py effects <stage> [maxbits]     mean speedup and accuracy by level of every factor (best half)
Options after the command as key=value go to bpsim (W=8).  Results in out/explore/<stage>.tsv.
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sweep

REFS = ["def tage bn=64 te=16,16,16 tt=7,7,7 th=4,9,20 cb=3 ub=2 ua=4 alloc=1 rng=128 rgh=4 gn=128 gh=4 cn=64 lw=8",
        "bim bimodal n=64", "gsh gshare n=128 h=4", "trn tourn nb=64 ng=128 h=4 nc=64", "snt snt"]
FACT = ("nt", "ent", "cb", "zero", "lmin", "lmax", "ub", "ug", "ut", "thf", "dyn", "sx", "sk", "pr", "pp", "bin")


def hist(nt, zero, lmin, lmax):
    n = nt - zero
    if n <= 0:
        return [0] * nt
    if n == 1:
        hs = [lmin]
    else:
        hs = sorted({max(1, round(lmin * (lmax / lmin) ** (i / (n - 1)))) for i in range(n)})
        while len(hs) < n:                       # duplicates after rounding: fill the gaps
            for v in range(1, 64):
                if v not in hs:
                    hs.append(v)
                    break
            hs.sort()
    return [0] * zero + hs


def line(f):
    th = hist(f["nt"], f["zero"], f["lmin"], f["lmax"])
    theta = max(1, round(f["thf"] * f["nt"]))
    name = "g_" + "_".join(f"{k}{f[k]}" for k in FACT)
    return (f"{name} gehlr te={f['ent']} th={','.join(map(str, th))} cb={f['cb']} ub={f['ub']} ug={f['ug']} ut={f['ut']} "
            f"theta={theta} dyn={f['dyn']} sx={f['sx']} sk={f['sk']} pr={f.get('pr', 0)} pp={f.get('pp', 0)} bin={f.get('bin', 0)}")


def sample2(n, seed):
    """Second round: around the best region of the first, with the exotic options as factors: binarised weights
    (cb=1), probabilistic updates (pr), path history (pp)."""
    rng = random.Random(seed)
    out, seen = [], set()
    while len(out) < n:
        f = dict(nt=rng.choice([3, 4, 5, 6, 8]), ent=rng.choice([16, 32, 64, 128]), cb=rng.choice([1, 1, 2, 3, 4, 5]), zero=rng.choice([0, 1]),
                 lmin=rng.choice([1, 2, 3, 4]), lmax=rng.choice([8, 10, 12, 16, 20, 24, 32]), ub=rng.choice([0, 1, 2, 4]),
                 ug=rng.choice([0, 1, 2, 4]), ut=rng.choice([0, 1, 2, 4]), thf=rng.choice([0.5, 1, 2, 3, 4, 6]), dyn=rng.choice([0, 1]),
                 sx=rng.choice([0, 1]), sk=rng.choice([0, 1, 2]), pr=rng.choice([0, 1]), pp=rng.choice([0, 0, 8, 16]), bin=rng.choice([0, 0, 1]))
        if not 256 <= f["nt"] * f["ent"] * f["cb"] <= 1024 or f["lmax"] <= f["lmin"]:
            continue
        if f["cb"] == 1:
            f["pr"] = 0
            f["bin"] = 0
            f["thf"] = rng.choice([0.5, 1, 2])
        ln = line(f)
        if ln not in seen:
            seen.add(ln)
            out.append(ln)
    return out


def refine(n, seed):
    """Third list: explicit history sets around the best 4- and 5-table candidates of the first round."""
    rng = random.Random(seed)
    out, seen = [], set()
    while len(out) < n:
        nt = rng.choice([4, 4, 5])
        cb = rng.choice([4, 5])
        if nt == 4:
            th = [0, rng.choice([3, 4, 5, 6, 8]), rng.choice([9, 10, 12, 14, 16]), rng.choice([18, 20, 23])]
        else:
            th = [0, rng.choice([2, 3, 4]), rng.choice([5, 6, 8]), rng.choice([10, 12, 14]), rng.choice([18, 20, 23])]
        ln = (f"gehlr te=32 th={','.join(map(str, th))} cb={cb} ub={rng.choice([1, 2, 4])} ug={rng.choice([1, 2, 4])} ut={rng.choice([0, 2, 4])} "
              f"theta={rng.choice([4, 6, 8, 12, 16])} dyn=1 sx={rng.choice([1, 1, 1, 0])} sk={rng.choice([0, 1])} pr={rng.choice([0, 0, 1])} pp={rng.choice([0, 0, 8, 16])} bin={rng.choice([0, 0, 1])}")
        if ln not in seen:
            seen.add(ln)
            out.append(f"x_{len(out)} " + ln)
    return out


def final_grid(n=0, seed=0):
    """Fourth list: the full grid around the chosen structure (4 tables x 32 x 5 bits, negation, adaptive threshold)."""
    out = []
    for h1 in (4, 5, 6, 8):
        for h2 in (9, 10, 12, 14):
            for h3 in (18, 20, 23):
                for pp in (8, 12, 16):
                    for ub in (2, 4):
                        for ug in (2, 4):
                            for ut in (2, 4):
                                for th in (4, 8, 12):
                                    for sk in (0, 1):
                                        out.append(f"f_{len(out)} gehlr te=32 th=0,{h1},{h2},{h3} cb=5 ub={ub} ug={ug} ut={ut} theta={th} dyn=1 sx=1 sk={sk} pr=0 pp={pp} bin=0")
    return out


def sample(n, seed):
    rng = random.Random(seed)
    out, seen = [], set()
    while len(out) < n:
        f = dict(nt=rng.choice([2, 3, 4, 5, 6, 8]), ent=rng.choice([16, 32, 64, 128]), cb=rng.choice([2, 3, 4, 5]), zero=rng.choice([0, 1]),
                 lmin=rng.choice([1, 2, 3, 4, 6]), lmax=rng.choice([6, 8, 10, 12, 16, 20, 24, 32]), ub=rng.choice([0, 1, 2, 4]),
                 ug=rng.choice([0, 1, 2, 4]), ut=rng.choice([0, 1, 2, 4]), thf=rng.choice([0.5, 1, 2, 3, 4, 6]), dyn=rng.choice([0, 1]),
                 sx=rng.choice([0, 1]), sk=rng.choice([0, 1, 2]))
        if not 256 <= f["nt"] * f["ent"] * f["cb"] <= 1024 or f["lmax"] <= f["lmin"]:
            continue
        ln = line(f)
        if ln not in seen:
            seen.add(ln)
            out.append(ln)
    return out


def factors(name):
    f = {}
    for tok in name.split("_")[1:]:
        for k in sorted(FACT, key=len, reverse=True):
            if tok.startswith(k):
                f[k] = tok[len(k):]
                break
    return f


def load(stage):
    tsv = os.path.join(sweep.OUT, stage + ".tsv")
    t = {x["config"]: x for x in sweep.aggregate(tsv, "tune")}
    h = {x["config"]: x for x in sweep.aggregate(tsv, "hold")}
    a = {x["config"]: x for x in sweep.aggregate(tsv)}
    return t, h, a


def top(stage, n=25, maxbits=10 ** 9):
    t, h, a = load(stage)
    cfg = dict(l.split(None, 1) for l in open(os.path.join(sweep.OUT, stage + ".cfg")) if l.strip())
    rows = sorted((x for x in t.values() if x["bits"] - x["sbits"] <= maxbits), key=lambda x: -x["speedup"])
    names = [x["config"] for x in rows[:n]] + [k for k in ("def", "trn", "gsh", "bim") if k in t]
    print("own bits | tune speedup acc | hold speedup acc | all speedup acc worst | config")
    for k in names:
        print(f"{t[k]['bits'] - t[k]['sbits']:5.0f} | {t[k]['speedup']:.4f} {t[k]['acc_cpu']:.2f} | {h[k]['speedup']:.4f} {h[k]['acc_cpu']:.2f} | "
              f"{a[k]['speedup']:.4f} {a[k]['acc_cpu']:.2f} {a[k]['worst']:.1f} | {k if k in ('def', 'trn', 'gsh', 'bim') else cfg[k].strip()[6:]}")


def effects(stage, maxbits=10 ** 9):
    t, h, a = load(stage)
    xs = sorted((x for x in t.values() if x["config"].startswith("g_") and x["bits"] - x["sbits"] <= maxbits), key=lambda x: -x["speedup"])
    half = xs[:len(xs) // 2]
    print(f"{len(xs)} candidates; mean over the better half ({len(half)}) and count in the best 5 %, by level")
    best = xs[:max(1, len(xs) // 20)]
    for k in FACT:
        lv = {}
        for x in half:
            lv.setdefault(factors(x["config"])[k], []).append(x)
        nb = {}
        for x in best:
            v = factors(x["config"])[k]
            nb[v] = nb.get(v, 0) + 1
        print(f"{k:5s} " + "  ".join(f"{v}: {sum(y['speedup'] for y in ys) / len(ys):.4f}/{sum(y['acc_cpu'] for y in ys) / len(ys):.2f} n{len(ys)} top{nb.get(v, 0)}"
                                     for v, ys in sorted(lv.items(), key=lambda kv: float(kv[0]))))


def main():
    cmd = sys.argv[1]
    args = [a for a in sys.argv[2:] if "=" not in a]
    opts = dict(a.split("=", 1) for a in sys.argv[2:] if "=" in a)
    opts.setdefault("threads", str(os.cpu_count() or 8))
    if cmd == "rand":
        n = int(args[0]) if args else 2000
        seed = int(args[1]) if len(args) > 1 else 1
        sweep.run(opts.pop("stage", "prand"), REFS + sample(n, seed), opts)
    elif cmd == "list":
        fn = {"1": sample, "2": sample2, "3": refine, "4": final_grid}[opts.pop("round", "1")]
        open(args[2], "w").write("\n".join(REFS + fn(int(args[0]), int(args[1]))) + "\n")
    elif cmd == "file":
        sweep.run(args[0], REFS + [l.strip() for l in open(args[1]) if l.strip() and not l.startswith("#")], opts)
    elif cmd == "top":
        top(args[0], int(args[1]) if len(args) > 1 else 25, float(args[2]) if len(args) > 2 else 10 ** 9)
    elif cmd == "effects":
        effects(args[0], float(args[1]) if len(args) > 1 else 10 ** 9)


if __name__ == "__main__":
    main()
