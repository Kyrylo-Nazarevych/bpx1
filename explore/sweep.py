#!/usr/bin/env python3
"""BPX-1 design-space exploration driver (spec section 6).

  sweep.py run <stage> [opt=val ...]   generate the stage's candidate list, run bpsim on every benchmark, aggregate
  sweep.py show <stage> [top=N sort=key]   print the aggregate table of a finished stage
  sweep.py chips <tag>                 whole-chip report of a finished "chips" run (bits and area of the whole chip)
  sweep.py pareto <stage>...           Pareto chart(s) -> out/explore/pareto_<stage>.png
  sweep.py list <stage> <file>         only write the stage's candidate list (to run bpsim elsewhere)

Aggregates (one value per candidate; every program has equal weight):
  acc_seq / acc_cpu   arithmetic mean over programs of prediction accuracy (one in flight / in the U1 CPU model), %
  mpki                arithmetic mean over programs of mispredictions per 1000 instructions (U1 CPU model)
  speedup             geometric mean over programs of cycles(static not-taken) / cycles(candidate driving)
  worst               lowest per-program accuracy (U1 CPU model) and the program it occurs on
Everything runs inside the Linux mirror: ~/work/bpx1, benchmark traces in out/bench/<name>/prog.evt.
"""
import csv, math, os, subprocess, sys, glob, itertools

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "out", "explore")
BIN = os.path.join(OUT, "bpsim")


def build():
    os.makedirs(OUT, exist_ok=True)
    src = os.path.join(ROOT, "explore", "bpsim.cc")
    if not os.path.exists(BIN) or os.path.getmtime(BIN) < os.path.getmtime(src):
        subprocess.check_call(["g++", "-O2", "-std=c++17", "-pthread", "-o", BIN, src])


def traces():
    t = sorted(glob.glob(os.path.join(ROOT, "out", "bench", "*", "prog.evt")))
    if not t:
        sys.exit("no benchmark traces in out/bench: run bench/build.sh first")
    return t


# ------------------------------------------------------------------------------------------- candidate lists
def tage(name, bn, te, tt, th, **kw):
    s = f"{name} tage bn={bn} te={','.join(map(str, te))} tt={','.join(map(str, tt))} th={','.join(map(str, th))}"
    return s + "".join(f" {k}={v}" for k, v in kw.items())


def geo(lmin, lmax, n):
    if n == 1:
        return [lmin]
    return [max(1, round(lmin * (lmax / lmin) ** (i / (n - 1)))) for i in range(n)]


def stage_refs():
    """Size curves of the classic predictors (FR-07: reference sizes are chosen at the knee of these curves)."""
    c = ["snt snt", "st st", "btfn btfn"]
    for n in (16, 32, 64, 128, 256, 512, 1024, 4096):
        c.append(f"bim{n} bimodal n={n}")
        c.append(f"bim{n}i2 bimodal n={n} init=2")
    for n in (32, 64, 128, 256, 512, 1024, 4096):
        lg = n.bit_length() - 1
        for h in sorted({0, 1, 2, 3, 4, 5, 6, 8, 10, 12} & set(range(0, lg + 5))):
            c.append(f"gsh{n}h{h} gshare n={n} h={h}")
    for nb, ng, nc in ((32, 64, 32), (64, 64, 64), (64, 128, 64), (64, 256, 64), (128, 128, 64), (128, 128, 128), (128, 256, 64),
                       (128, 256, 128), (256, 256, 128), (256, 512, 256), (128, 512, 128), (512, 1024, 512)):
        lg = ng.bit_length() - 1
        for h in sorted({2, 3, 4, 5, 6, 8, 10} & set(range(0, lg + 3))):
            c.append(f"tn{nb}_{ng}h{h}_{nc} tourn nb={nb} ng={ng} h={h} nc={nc}")
            c.append(f"tn{nb}_{ng}h{h}_{nc}ch tourn nb={nb} ng={ng} h={h} nc={nc} chist=1")
    return c


def stage_families():
    """Breadth (EX-02): distinct families at several storage budgets, each with a small parameter sweep."""
    c = ["snt snt", "btfn btfn"]
    for n in (64, 128, 256, 512, 1024):
        c.append(f"bimodal_{n} bimodal n={n}")
        c.append(f"bimodal_agree_{n} bimodal n={n} agree=1")
    for n in (128, 256, 512, 1024):
        for h in (2, 4, 6, 8):
            c.append(f"gshare_{n}h{h} gshare n={n} h={h}")
            c.append(f"gshare_agree_{n}h{h} gshare n={n} h={h} agree=1")
    for nb, ng, nc in ((64, 128, 64), (128, 256, 64), (128, 256, 128), (256, 512, 128), (256, 512, 256)):
        for h in (4, 6, 8):
            c.append(f"tourn_{nb}_{ng}h{h}_{nc} tourn nb={nb} ng={ng} h={h} nc={nc}")
    for a in (4, 5, 6, 7, 8):
        for h in (1, 2, 3, 4):
            if a + h <= 10:
                c.append(f"gselect_a{a}h{h} gselect a={a} h={h}")
    for g, pl, a in ((1, 2, 5), (2, 2, 4), (3, 2, 4), (2, 3, 4), (3, 2, 5), (2, 2, 5), (3, 3, 3), (2, 4, 3), (4, 2, 3)):
        for nl in (32, 64, 128):
            c.append(f"alloy_g{g}p{pl}a{a}_{nl} alloy g={g} p={pl} a={a} nl={nl}")
    for n in (128, 256, 512, 1024):
        for sh in (2, 4):
            c.append(f"bimhyst_{n}s{sh} bimhyst n={n} sh={sh}")
    for nc, nd in ((64, 64), (64, 128), (128, 128), (128, 256), (256, 256)):
        for h in (3, 5, 7):
            c.append(f"bimode_{nc}_{nd}h{h} bimode nc={nc} nd={nd} h={h}")
    for nc, n in ((64, 16), (128, 16), (128, 32), (256, 32), (256, 64), (128, 64)):
        for h in (3, 5, 8):
            for tb in (5, 7):
                c.append(f"yags_{nc}_{n}h{h}t{tb} yags nc={nc} n={n} h={h} tb={tb}")
    for n in (64, 128, 256):
        for h0, h1 in ((2, 5), (3, 8), (4, 10)):
            c.append(f"gskew_{n}h{h0}_{h1} gskew n={n} h0={h0} h1={h1}")
    for nl in (16, 32, 64):
        for hl in (4, 6, 8):
            c.append(f"local_{nl}x{hl} local nl={nl} hl={hl}")
            for ng in (64, 128):
                c.append(f"alpha_{nl}x{hl}_{ng} alpha nl={nl} hl={hl} ng={ng}")
    for np_ in (4, 8, 16):
        for h in (8, 12, 16):
            for wb in (5, 6):
                c.append(f"perceptron_{np_}h{h}w{wb} perceptron np={np_} h={h} wb={wb}")
    for n in (32, 64, 128):
        for cb in (3, 4):
            for hl in ("0,2,5", "0,3,8", "0,2,5,11", "0,3,7,14", "0,2,4,8,16"):
                c.append(f"ogehl_{n}c{cb}_{hl.replace(',', '-')} ogehl n={n} cb={cb} hl={hl}")
    for bn in (64, 128, 256):
        for nt in (1, 2, 3, 4):
            for e in (8, 16, 32, 64):
                for tb in (5, 7):
                    for lmax in (8, 16, 28):
                        if nt == 1 and lmax != 8:
                            continue
                        th = geo(3 if nt > 1 else 6, lmax, nt)
                        c.append(tage(f"tage_b{bn}_{nt}x{e}t{tb}L{lmax}", bn, [e] * nt, [tb] * nt, th, cb=3, ub=1))
    return c


def chip_line(nb, ng, gh, te, tt, th, base="trn", cb=3, ub=2, ua=4, alloc=0, tag="", **kw):
    """One whole-chip candidate: the reference tables (bimodal nb, gshare ng with gh history bits, chooser nb) and a
    default predictor of tagged tables (entries te, tag bits tt, history lengths th) over base = 'bim' (the
    reference bimodal table) or 'trn' (the reference tournament).  rng/rgh record the reference gshare even when
    the default predictor does not use it, so that the chip cost can be computed from the line."""
    j = lambda v: "-".join(map(str, v))
    name = f"c_b{nb}_g{ng}h{gh}_E{j(te)}_t{j(tt)}_H{j(th)}_c{cb}u{ub}a{ua}{alloc}_{base}{tag}"
    k = dict(cb=cb, ub=ub, ua=ua, alloc=alloc, rng=ng, rgh=gh)
    if base == "trn":
        k.update(gn=ng, gh=gh, cn=nb)
    k.update(kw)
    return tage(name, nb, te, tt, th, **k)


LMAX = {1: 8, 2: 14, 3: 20, 4: 24}
SHAPES = ([16, 16], [32, 32], [16, 32], [32, 16], [64, 64], [32, 64], [16, 16, 16], [32, 32, 32], [16, 32, 32], [16, 16, 32],
          [64], [32], [16, 16, 16, 16])


def stage_chips():
    """Whole-chip candidates (FR-07 references + a default predictor sharing their tables)."""
    seen, c = set(), []

    def add(line):
        n = line.split()[0]
        if n not in seen:
            seen.add(n)
            c.append(line)

    # A. table shapes, tag widths and reference sizes
    for nb in (32, 64, 128):
        for ng, gh in ((64, 4), (64, 6), (128, 5), (128, 6), (128, 7), (256, 6), (256, 8)):
            for te in SHAPES:
                for tb in (5, 6, 7, 8):
                    add(chip_line(nb, ng, gh, te, [tb] * len(te), geo(4, LMAX[len(te)], len(te))))
    # B. history lengths
    for te in ([32, 32], [16, 16], [32, 32, 32], [16, 16, 16], [16, 32]):
        for lmin in (2, 3, 4, 5, 6, 8):
            for lmax in (8, 10, 12, 14, 16, 18, 20, 24, 28, 32):
                if lmax > lmin + 1:
                    add(chip_line(64, 128, 6, te, [7] * len(te), geo(lmin, lmax, len(te))))
    # C. policies
    for te in ([32, 32], [16, 16, 16]):
        th = geo(4, LMAX[len(te)], len(te))
        for base in ("bim", "trn"):
            for cb in (2, 3):
                for ub in (1, 2):
                    for ua in (0, 3, 4, 5):
                        for alloc in (0, 1):
                            for altw in (0, 1):
                                add(chip_line(64, 128, 6, te, [7] * len(te), th, base=base, cb=cb, ub=ub, ua=ua, alloc=alloc,
                                              tag="" if altw else "_noaltw", altw=altw))
        # D. loop predictor on top
        for ln in (2, 4, 8):
            for lcnt in (6, 8):
                for ltag in (6, 8):
                    add(chip_line(64, 128, 6, te, [7] * len(te), th, tag=f"_loop{ln}c{lcnt}t{ltag}", ln=ln, lcnt=lcnt, ltag=ltag))
        # E. agree-coded private base (the host sends a static hint bit)
        add(chip_line(64, 128, 6, te, [7] * len(te), th, base="bim", tag="_agree", bagree=1))
    return c


def stage_chips2():
    """Refinement around the best whole-chip shapes of stage 'chips' (tournament base, 3-bit counters)."""
    seen, c = set(), []

    def add(line):
        n = line.split()[0]
        if n not in seen:
            seen.add(n)
            c.append(line)

    def var(nb, ng, gh, te, tb, th, ub=2, alloc=1, chist=0, cn=None, lw=16, **kw):
        tag = ("_lfsr%d" % lw if alloc and lw != 16 else "") + ("_ch%d" % (cn or nb) if chist else "")
        k = dict(kw)
        if alloc:
            k["lw"] = lw
        if chist:
            k.update(chist=1, cn=cn or nb)
        l = chip_line(nb, ng, gh, te, [tb] * len(te), th, ub=ub, alloc=alloc, tag=tag, **k)
        if chist:   # chip_line sets cn=nb for the tournament base; override
            l = l.replace(f" cn={nb} ", f" cn={cn or nb} ", 1) if f" cn={nb} " in l else l
        add(l)

    h3 = [(a, b, d) for a in (3, 4, 5, 6) for b in (7, 9, 11, 13) for d in (14, 17, 20, 24, 28, 32) if b > a + 1 and d > b + 2]
    h2 = [(a, b) for a in (4, 5, 6, 8) for b in (10, 12, 14, 17, 20, 24)]
    h4 = [tuple(geo(a, b, 4)) for a in (3, 4, 5) for b in (20, 24, 28, 32)]
    # (a) history lengths, per shape, with and without LFSR allocation, 1 or 2 useful bits, chooser by id or history
    for te, hs in (([16, 16, 16], h3), ([32, 32, 32], h3), ([16, 16, 32], h3), ([16, 16, 16, 16], h4), ([32, 32], h2), ([16, 32], h2), ([16, 16], h2)):
        for th in hs:
            for alloc in (0, 1):
                for ub in (1, 2):
                    for chist in (0, 1):
                        var(64, 128, 6, te, 7, list(th), ub=ub, alloc=alloc, chist=chist)
    # (b) reference sizes and tag width at the centre histories
    for te, th in (([16, 16, 16], [4, 9, 20]), ([32, 32], [4, 14]), ([16, 16, 16, 16], [4, 7, 13, 24])):
        for nb in (32, 64, 128):
            for ng, gh in ((64, 3), (64, 4), (128, 4), (128, 5), (128, 6), (128, 7), (256, 5), (256, 6)):
                for tb in (5, 6, 7, 8, 9):
                    for chist in (0, 1):
                        var(nb, ng, gh, te, tb, th, chist=chist)
    # (c) chooser size when indexed by history; 8-bit allocation LFSR
    for te, th in (([16, 16, 16], [4, 9, 20]), ([16, 16, 16, 16], [4, 7, 13, 24])):
        for cn in (16, 32, 64, 128):
            var(64, 128, 6, te, 7, th, chist=1, cn=cn)
        var(64, 128, 6, te, 7, th, lw=8)
        var(64, 128, 6, te, 7, th, lw=8, chist=1)
    return c


def parse_line(line):
    t = line.split()
    return t[0], t[1], dict(x.split("=", 1) for x in t[2:])


def chip_cost(params):
    """Storage bits and estimated area (tables by the synthesis fit, logic in drive-strength-2 cells) of a whole-chip
    candidate.  Bimodal and chooser share one two-field table; an agree-coded base needs a private bimodal table."""
    ints = lambda k: [int(x) for x in params[k].split(",")]
    nb, ng, gh = int(params["bn"]), int(params["rng"]), int(params["rgh"])
    te, tt, th = ints("te"), ints("tt"), ints("th")
    cb, ub = int(params.get("cb", 3)), int(params.get("ub", 1))
    tab = lambda n, w: 550 + n * (20.5 * w + 6.5)
    priv = int(params.get("bagree", 0))
    bits = 4 * nb + 2 * ng + sum(e * (t + cb + ub) for e, t in zip(te, tt)) + (2 * nb if priv else 0)
    area = tab(nb, 4) + tab(ng, 2) + sum(tab(e, t + cb + ub) for e, t in zip(te, tt)) + (tab(nb, 2) if priv else 0)
    if int(params.get("chist", 0)):       # a chooser indexed by history cannot share the bimodal table's address
        nc = int(params.get("cn", nb))
        bits += 2 * nc - 2 * nb
        area += tab(nb, 2) + tab(nc, 2) - tab(nb, 4)
    if int(params.get("alloc", 0)):
        area += int(params.get("lw", 16)) * 21.3 + 50
    for e, t, h in zip(te, tt, th):
        lg = e.bit_length() - 1
        area += 16.3 * (max(h - lg, 0) + lg + max(h - t, 0) + t) + 20 * t + 300 + 21.3 * (cb + ub + 1)
    hm = max(th + [gh])
    area += 16.3 * gh + 2 * 21.3 * hm + 11.3 * hm
    ln = int(params.get("ln", 0))
    if ln:
        eb = int(params.get("ltag", 8)) + 2 * int(params.get("lcnt", 7)) + 6
        bits += ln * eb
        area += ln * eb * 22.5 + 800
    return bits, area


def load_chips(tag="chips"):
    """Aggregate of a chips run with chip bits and area attached (cbits, carea)."""
    agg = aggregate(os.path.join(OUT, tag + ".tsv"))
    par = {}
    for line in open(os.path.join(OUT, tag + ".cfg")):
        if line.strip():
            n, _, p = parse_line(line)
            par[n] = p
    for x in agg:
        x["params"] = par[x["config"]]
        x["cbits"], x["carea"] = chip_cost(par[x["config"]])
    return agg


def report_chips(tag="chips", caps=(900, 1000, 1100, 1200, 1300, 1400, 1500, 1700)):
    agg = load_chips(tag)
    print(f"{len(agg)} whole-chip candidates on {len(agg[0]['per'])} programs")
    row = lambda x: (f"  {x['config']:58s} bits={x['cbits']:5.0f} area~{x['carea']:6.0f} acc={x['acc_cpu']:.3f} (seq {x['acc_seq']:.3f}) "
                     f"mpki={x['mpki']:.3f} speedup={x['speedup']:.4f} worst={x['worst']:.2f} {x['worst_prog']}")
    for cap in caps:
        r = sorted([x for x in agg if x["cbits"] <= cap], key=lambda x: -x["speedup"])
        print(f"=== best 6 by speedup with chip bits <= {cap}")
        for x in r[:6]:
            print(row(x))
    print("=== Pareto frontier: estimated area against speedup")
    for x in pareto_front([dict(x, area=x["carea"]) for x in agg], "area", "speedup"):
        print(row(x))


STAGES = {"refs": stage_refs, "families": stage_families, "chips": stage_chips, "chips2": stage_chips2}


# ------------------------------------------------------------------------------------------- run / aggregate
def run(stage, cands, opts):
    build()
    os.makedirs(OUT, exist_ok=True)
    cfg = os.path.join(OUT, stage + ".cfg")
    tsv = os.path.join(OUT, stage + ".tsv")
    with open(cfg, "w") as f:
        f.write("\n".join(cands) + "\n")
    subprocess.check_call([BIN] + [f"{k}={v}" for k, v in opts.items()] + [cfg, tsv] + traces())
    return tsv


def aggregate(tsv):
    rows = list(csv.DictReader(open(tsv), delimiter="\t"))
    by = {}
    for r in rows:
        by.setdefault(r["config"], []).append(r)
    agg = []
    for name, rs in by.items():
        acc_s = [100 * (1 - int(r["mis_seq"]) / max(int(r["branches"]), 1)) for r in rs]
        acc_c = [100 * (1 - int(r["mis_cpu"]) / max(int(r["branches"]), 1)) for r in rs]
        mpki = [1000 * int(r["mis_cpu"]) / max(int(r["instr"]), 1) for r in rs]
        sp = [int(r["snt_cycles"]) / max(int(r["cycles"]), 1) for r in rs]
        wi = min(range(len(rs)), key=lambda i: acc_c[i])
        agg.append(dict(config=name, kind=rs[0]["kind"], bits=float(rs[0]["bits"]), sbits=float(rs[0]["sbits"]), area=float(rs[0]["area"]),
                        sarea=float(rs[0]["sarea"]), acc_seq=sum(acc_s) / len(rs), acc_cpu=sum(acc_c) / len(rs), mpki=sum(mpki) / len(rs),
                        speedup=math.exp(sum(math.log(x) for x in sp) / len(rs)), worst=acc_c[wi], worst_prog=rs[wi]["program"],
                        per={r["program"]: (acc_c[i], sp[i], acc_s[i]) for i, r in enumerate(rs)}))
    return agg


def show(agg, top=40, sort="speedup", flt=None):
    a = [x for x in agg if not flt or flt in x["config"]]
    a.sort(key=lambda x: -x[sort] if sort not in ("mpki", "bits", "area") else x[sort])
    print(f"{'config':34s} {'bits':>6s} {'area':>7s} {'acc_seq':>8s} {'acc_cpu':>8s} {'mpki':>7s} {'speedup':>8s} {'worst':>7s}  worst_prog")
    for x in a[:top]:
        print(f"{x['config']:34s} {x['bits']:6.0f} {x['area']:7.0f} {x['acc_seq']:8.3f} {x['acc_cpu']:8.3f} {x['mpki']:7.3f} {x['speedup']:8.4f} {x['worst']:7.2f}  {x['worst_prog']}")


def pareto_front(agg, xkey="area", ykey="speedup"):
    pts = sorted(agg, key=lambda x: (x[xkey], -x[ykey]))
    front, best = [], -1
    for p in pts:
        if p[ykey] > best:
            front.append(p)
            best = p[ykey]
    return front


def plot(stage, agg):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, 2, figsize=(15, 6.5))
    kinds = sorted({x["kind"] for x in agg})
    cmap = plt.get_cmap("tab10")
    for ax, (ykey, ylabel) in zip(axes, (("speedup", "O1: speedup over static not-taken (geomean)"), ("acc_cpu", "O2: accuracy in the U1 CPU model, % (mean)"))):
        for i, k in enumerate(kinds):
            pts = [x for x in agg if x["kind"] == k and x["area"] > 0]
            ax.scatter([p["area"] for p in pts], [p[ykey] for p in pts], s=14, color=cmap(i % 10), label=k, alpha=0.7)
        fr = pareto_front([x for x in agg if x["area"] > 0], "area", ykey)
        ax.plot([p["area"] for p in fr], [p[ykey] for p in fr], "k-", lw=1, label="Pareto frontier")
        ax.set_xlabel("estimated area, um^2 (table fit + logic estimate)")
        ax.set_ylabel(ylabel)
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
    fig.suptitle(f"BPX-1 exploration, stage '{stage}': {len(agg)} candidates")
    fig.tight_layout()
    p = os.path.join(OUT, f"pareto_{stage}.png")
    fig.savefig(p, dpi=110)
    print("wrote", p)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, stage = sys.argv[1], sys.argv[2]
    opts = dict(a.split("=", 1) for a in sys.argv[3:] if "=" in a)
    if cmd == "run":
        tag = opts.pop("tag", stage)
        show_opts = {k: opts.pop(k) for k in ("top", "sort", "flt") if k in opts}
        tsv = run(tag, STAGES[stage](), opts)
        agg = aggregate(tsv)
        show(agg, int(show_opts.get("top", 40)), show_opts.get("sort", "speedup"), show_opts.get("flt"))
    elif cmd == "show":
        agg = aggregate(os.path.join(OUT, stage + ".tsv"))
        show(agg, int(opts.get("top", 40)), opts.get("sort", "speedup"), opts.get("flt"))
    elif cmd == "chips":
        report_chips(stage)
    elif cmd == "pareto":
        for st in sys.argv[2:]:
            plot(st, aggregate(os.path.join(OUT, st + ".tsv")))
    elif cmd == "list":
        cands = STAGES[stage]()
        with open(sys.argv[3], "w") as f:
            for line in cands:
                print(line, file=f)
        print(len(cands), "candidates ->", sys.argv[3])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
