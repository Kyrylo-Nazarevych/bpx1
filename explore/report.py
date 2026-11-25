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
    ws = [w for w in (6, 7, 8, 9, 10, 11, 12, 14, 16) if load(f"sens_W{w}")]
    print("Identifier width W (accuracy %, U1 contract):\n")
    md([[n] + [f"{load(f'sens_W{w}')[n]['acc_cpu']:.3f}" for w in ws] for n in names], ["candidate"] + [f"W={w}" for w in ws])
    var = [("contract (BTB 256, EX)", "sens_W10"), ("no BTB", "sens_btb0"), ("BTB 64", "sens_btb64"), ("BTB 1024", "sens_btb1024"), ("resolve +1 stage", "sens_R2"),
           ("resolve +2 stages", "sens_R3"), ("resolve before predict", "sens_rf1"), ("hint bit = 0", "sens_nohint")]
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
    elif cmd == "charts":
        charts()
    else:
        sys.exit(__doc__)
