#!/usr/bin/env python3
"""Tables and charts for docs/exploration.md from the sweep results in out/explore/ (spec EX-03).

  report.py refs | families | chips | sens | programs <chip-config-name> | charts
Prints Markdown tables; `charts` writes docs/img/*.png.
"""
import csv, glob, json, math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sweep

OUT = sweep.OUT
ROOT = sweep.ROOT

FAMILIES = ("bimodal_agree", "bimodal", "gshare_agree", "gshare", "gselect", "alloy", "bimhyst", "tourn", "bimode", "yags", "gskew", "local",
            "alpha", "perceptron", "ogehl", "tage")


def fam(name):
    for p in FAMILIES:
        if name.startswith(p + "_") or name == p:
            return p
    return name


def md(rows, head):
    print("| " + " | ".join(head) + " |")
    print("|" + "|".join(" --- " for _ in head) + "|")
    for r in rows:
        print("| " + " | ".join(str(x) for x in r) + " |")
    print()


def refs():
    agg = {x["config"]: x for x in sweep.aggregate(os.path.join(OUT, "refs.tsv"))}
    print(f"Programs: {len(next(iter(agg.values()))['per'])}\n")
    md([[n, f"{agg[f'bim{n}']['acc_cpu']:.2f}", f"{agg[f'bim{n}']['speedup']:.4f}", f"{agg[f'bim{n}i2']['acc_cpu']:.2f}"] for n in (16, 32, 64, 128, 256, 512, 1024, 4096)],
       ["bimodal entries", "accuracy %", "speedup", "accuracy %, initialised weakly taken"])
    hs = (0, 1, 2, 3, 4, 5, 6, 8, 10, 12)
    rows = []
    for n in (32, 64, 128, 256, 512, 1024, 4096):
        rows.append([n] + [f"{agg[f'gsh{n}h{h}']['acc_cpu']:.2f}" if f"gsh{n}h{h}" in agg else "" for h in hs])
    md(rows, ["gshare entries \\ history bits"] + [str(h) for h in hs])
    t = sorted([x for x in agg.values() if x["kind"] == "tourn"], key=lambda x: -x["acc_cpu"])
    best = {}
    for x in t:
        best.setdefault(x["bits"], x)
    md([[f"{b:.0f}", x["config"], f"{x['acc_cpu']:.2f}", f"{x['speedup']:.4f}"] for b, x in sorted(best.items())], ["tournament bits", "best configuration", "accuracy %", "speedup"])
    for k in ("snt", "st", "btfn"):
        print(f"{k}: accuracy {agg[k]['acc_cpu']:.2f} %, speedup {agg[k]['speedup']:.4f}")


def families(tag="families"):
    agg = sweep.aggregate(os.path.join(OUT, tag + ".tsv"))
    caps = (300, 600, 900, 1200, 1600, 10 ** 9)
    best = {}
    for x in agg:
        for cap in caps:
            if x["bits"] <= cap:
                k = (fam(x["config"]), cap)
                if k not in best or x["speedup"] > best[k]["speedup"]:
                    best[k] = x
    fams = sorted({k[0] for k in best}, key=lambda f: -max(best[(f, c)]["speedup"] for c in caps if (f, c) in best))
    rows = []
    for f in fams:
        r = [f]
        for c in caps:
            b = best.get((f, c))
            r.append(f"{b['speedup']:.4f} / {b['acc_cpu']:.2f} ({b['bits']:.0f} b)" if b else "")
        rows.append(r)
    print(f"{len(agg)} candidates on {len(agg[0]['per'])} programs. Cell: speedup / accuracy % (storage bits) of the family's best candidate within the budget.\n")
    md(rows, ["family"] + [f"≤ {c} bits" if c < 10 ** 9 else "any size" for c in caps])
    d = max(abs(x["acc_seq"] - x["acc_cpu"]) for x in agg)
    print(f"Largest |accuracy one-in-flight − accuracy in the CPU model| over all candidates: {d:.4f} points\n")


def chips(tag="chips"):
    sweep.report_chips(tag)


def sens():
    def load(t):
        p = os.path.join(OUT, t + ".tsv")
        return {x["config"]: x for x in sweep.aggregate(p)} if os.path.exists(p) else None
    base = load("sens_W10")
    names = list(base)
    ws = [w for w in (6, 8, 10, 12, 16) if load(f"sens_W{w}")]
    print("Identifier width W (accuracy %, U1 contract):\n")
    md([[n] + [f"{load(f'sens_W{w}')[n]['acc_cpu']:.3f}" for w in ws] for n in names], ["candidate"] + [f"W={w}" for w in ws])
    var = [("contract (BTB 256, EX)", "sens_W10"), ("no BTB", "sens_btb0"), ("BTB 64", "sens_btb64"), ("BTB 1024", "sens_btb1024"), ("resolve +1 stage", "sens_R2"),
           ("resolve +2 stages", "sens_R3"), ("resolve before predict", "sens_rf1")]
    print("Speedup over static not-taken under pipeline variants:\n")
    md([[n] + [f"{load(t)[n]['speedup']:.4f}" if load(t) else "" for _, t in var] for n in names], ["candidate"] + [v for v, _ in var])
    print("Accuracy % under the same variants:\n")
    md([[n] + [f"{load(t)[n]['acc_cpu']:.3f}" if load(t) else "" for _, t in var] for n in names], ["candidate"] + [v for v, _ in var])


def programs(name, tag="chips", refs=("bim64", "gsh128h6", "trn64_128h6_64")):
    x = next(a for a in sweep.aggregate(os.path.join(OUT, tag + ".tsv")) if a["config"] == name)
    s = {a["config"]: a for a in sweep.aggregate(os.path.join(OUT, "sens_W10.tsv"))}
    rows = []
    for p in sorted(x["per"]):
        r = [p, f"{x['per'][p][0]:.2f}", f"{x['per'][p][1]:.4f}"]
        bestref = max(refs, key=lambda k: s[k]["per"][p][0])
        for k in refs:
            r.append(f"{s[k]['per'][p][0]:.2f}")
        r.append(f"{x['per'][p][0] - s[bestref]['per'][p][0]:+.2f}")
        rows.append(r)
    md(rows, ["program", "default acc %", "default speedup"] + [f"{k} acc %" for k in refs] + ["default − best reference"])


def charts():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    os.makedirs(os.path.join(ROOT, "docs", "img"), exist_ok=True)
    agg = sweep.aggregate(os.path.join(OUT, "families.tsv"))
    ch = sweep.load_chips("chips")
    fig, axes = plt.subplots(1, 2, figsize=(15, 6.5))
    cmap = plt.get_cmap("tab20")
    fl = sorted({fam(x["config"]) for x in agg if x["area"] > 0})
    for ax, (yk, yl) in zip(axes, (("speedup", "O1: speedup over static not-taken (geometric mean)"), ("acc_cpu", "O2: accuracy in the U1 CPU model, % (mean)"))):
        for i, f in enumerate(fl):
            pts = [x for x in agg if fam(x["config"]) == f and 0 < x["area"] < 60000]
            ax.scatter([p["area"] for p in pts], [p[yk] for p in pts], s=12, color=cmap(i % 20), label=f, alpha=0.75)
        fr = sweep.pareto_front([x for x in agg if 0 < x["area"] < 60000], "area", yk)
        ax.plot([p["area"] for p in fr], [p[yk] for p in fr], "k-", lw=1, label="Pareto frontier")
        ax.set_xlabel("estimated area of the predictor alone, µm² (table fit + logic estimate)")
        ax.set_ylabel(yl)
        ax.grid(alpha=0.3)
        ax.legend(fontsize=7, ncol=2)
    fig.suptitle(f"Predictor families: {len(agg)} candidates, {len(agg[0]['per'])} programs, U1 CPU contract")
    fig.tight_layout()
    fig.savefig(os.path.join(ROOT, "docs", "img", "pareto_families.png"), dpi=100)
    fig, axes = plt.subplots(1, 2, figsize=(15, 6.5))
    for ax, (yk, yl) in zip(axes, (("speedup", "O1: speedup over static not-taken (geometric mean)"), ("acc_cpu", "O2: accuracy in the U1 CPU model, % (mean)"))):
        for lab, sel, col in (("tagged tables over the tournament", lambda x: "_trn" in x["config"] and "loop" not in x["config"], "tab:blue"),
                              ("tagged tables over the bimodal table", lambda x: "_bim" in x["config"] and "agree" not in x["config"], "tab:orange"),
                              ("with loop predictor", lambda x: "loop" in x["config"], "tab:green"), ("agree-coded private base", lambda x: "agree" in x["config"], "tab:red")):
            pts = [x for x in ch if sel(x)]
            ax.scatter([p["carea"] for p in pts], [p[yk] for p in pts], s=10, color=col, label=lab, alpha=0.6)
        fr = sweep.pareto_front([dict(x, area=x["carea"]) for x in ch], "area", yk)
        ax.plot([p["area"] for p in fr], [p[yk] for p in fr], "k-", lw=1, label="Pareto frontier")
        ax.axvspan(0, 31000, color="green", alpha=0.06)
        ax.set_xlabel("estimated area of all predictor tables and their logic (whole chip), µm²; shaded: fits the budget")
        ax.set_ylabel(yl)
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
    fig.suptitle(f"Whole-chip candidates (references + default sharing their tables): {len(ch)} candidates, {len(ch[0]['per'])} programs")
    fig.tight_layout()
    fig.savefig(os.path.join(ROOT, "docs", "img", "pareto_chips.png"), dpi=100)
    print("wrote docs/img/pareto_families.png, docs/img/pareto_chips.png")


def pick(budget=33000, top=14):
    """Design choice: every whole-chip candidate of the refinement, grid and random stages whose estimated area
    fits the budget, ranked by speedup on the TUNING programs only; the held-out programs are shown beside it."""
    budget = float(budget)
    pool = {}
    for tag in ("chips", "chips2", "rand"):
        if not os.path.exists(os.path.join(OUT, tag + ".tsv")):
            continue
        tune = {x["config"]: x for x in sweep.load_chips(tag, "tune")}
        hold = {x["config"]: x for x in sweep.load_chips(tag, "hold")}
        for n, x in tune.items():
            pool[n] = (tag, x, hold[n])
    fit = [v for v in pool.values() if v[1]["carea"] <= budget]
    fit.sort(key=lambda v: -v[1]["speedup"])
    nt, nh = len(fit[0][1]["per"]), len(fit[0][2]["per"])
    print(f"{len(pool)} whole-chip candidates, {len(fit)} within {budget:.0f} um2; tuning set {nt} programs, held-out set {nh} programs\n")
    rows = []
    for tag, x, h in fit[:int(top)]:
        rows.append([x["config"], tag, f"{x['cbits']:.0f}", f"{x['carea']:.0f}", f"{x['speedup']:.4f}", f"{x['acc_cpu']:.3f}", f"{h['speedup']:.4f}", f"{h['acc_cpu']:.3f}"])
    md(rows, ["candidate", "stage", "bits", "area", "speedup (tuning)", "accuracy % (tuning)", "speedup (held-out)", "accuracy % (held-out)"])
    for tag in ("chips2", "chips", "rand"):
        b = [v for v in fit if v[0] == tag]
        if b:
            print(f"best of stage {tag}: {b[0][1]['config']}  speedup {b[0][1]['speedup']:.4f} (tuning), {b[0][2]['speedup']:.4f} (held-out); {len(b)} candidates within budget")
    r = [v for v in pool.values() if v[0] == "rand"]
    if r:
        r.sort(key=lambda v: -v[1]["speedup"])
        print(f"best random candidate at any size: {r[0][1]['config']} bits {r[0][1]['cbits']:.0f} area {r[0][1]['carea']:.0f} speedup {r[0][1]['speedup']:.4f}")
    # rank agreement between the two sets among the fitting candidates (Spearman)
    a = sorted(fit, key=lambda v: -v[1]["speedup"]); ra = {id(v): i for i, v in enumerate(a)}
    b = sorted(fit, key=lambda v: -v[2]["speedup"]); rb = {id(v): i for i, v in enumerate(b)}
    n = len(fit)
    rho = 1 - 6 * sum((ra[id(v)] - rb[id(v)]) ** 2 for v in fit) / (n * (n * n - 1))
    print(f"rank correlation of speedup between tuning and held-out programs over the {n} fitting candidates: {rho:.3f}")


def one(name, tag="chips2"):
    """Per-program results of one whole-chip candidate against the reference predictors of the sensitivity run."""
    x = next(a for a in sweep.aggregate(os.path.join(OUT, tag + ".tsv"), "all") if a["config"] == name)
    s = {a["config"]: a for a in sweep.aggregate(os.path.join(OUT, "sens_W10.tsv"), "all")}
    refs = [k for k in ("bim64", "gsh128h6", "trn64_128h6_64") if k in s]
    tune, hold = sweep.split(set(x["per"]))
    rows = []
    for p in sorted(x["per"]):
        best = max(refs, key=lambda k: s[k]["per"][p][0])
        rows.append([p, "held-out" if p in hold else "tuning", f"{x['per'][p][0]:.2f}", f"{x['per'][p][1]:.4f}"] + [f"{s[k]['per'][p][0]:.2f}" for k in refs] +
                    [f"{x['per'][p][0] - s[best]['per'][p][0]:+.2f}"])
    md(rows, ["program", "set", "default acc %", "default speedup"] + [k + " acc %" for k in refs] + ["default - best reference"])
    worse = [r for r in rows if float(r[-1]) < 0]
    print(f"programs where the default is less accurate than the best reference: {len(worse)} of {len(rows)}: " + ", ".join(f"{r[0]} ({r[-1]})" for r in worse))


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "refs":
        refs()
    elif cmd == "families":
        families(*sys.argv[2:3])
    elif cmd == "chips":
        chips(*sys.argv[2:3])
    elif cmd == "sens":
        sens()
    elif cmd == "programs":
        programs(*sys.argv[2:])
    elif cmd == "pick":
        pick(*sys.argv[2:])
    elif cmd == "one":
        one(*sys.argv[2:])
    elif cmd == "charts":
        charts()
    else:
        sys.exit(__doc__)
