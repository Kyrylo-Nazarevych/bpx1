#!/usr/bin/env python3
"""BPX-1 pre-silicon fault-injection campaign (docs/fault_campaign.md), on explore/bpsim (kind gehlr = the chip).

  fault_campaign.py run <part> [seeds=1,2,3] [progs=a,b] [tag=name]   run bpsim on the traces -> out/<tag>.tsv
  fault_campaign.py checks                                           equivalence and sanity checks (needs run plain, chk)
  fault_campaign.py report                                           aggregate -> explore/fault_results/*.tsv, tables on stdout

Parts: plain (fault-free chip, reference predictors, twin on without decay in every mode, 3-table predictor),
A (Poisson bit flips into one static table), AT (flips into the twin), B (decay of the twin), chk (TWIN_USE driving
in every mode, and TWIN_USE off).  Environment: BPX_FI_ROOT (default ~/work/bpx1_fi), BPX_TRACES (glob of the traces,
default ~/work/bpx1/out/bench/*/prog.evt), BPX_THREADS (4).  Every run is niced.
"""
import csv
import glob
import math
import os
import random
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sweep import EXCLUDE  # noqa: E402  (the two programs left out of every aggregate)

ROOT = os.environ.get("BPX_FI_ROOT", os.path.expanduser("~/work/bpx1_fi"))
OUT = os.path.join(ROOT, "out")
BIN = os.path.join(OUT, "bpsim")
RES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fault_results")
TRACES = os.environ.get("BPX_TRACES", os.path.expanduser("~/work/bpx1/out/bench/*/prog.evt"))
THREADS = os.environ.get("BPX_THREADS", "4")

CHIP = "gehlr te=32 th=0,8,9,20 cb=4 ub=2 ug=4 ut=4 theta=10 dyn=1 sx=1 sk=0 pp=8"     # cosim/bpsim.cfg, def
CHIP3 = "gehlr te=32 th=0,8,9 cb=4 ub=2 ug=4 ut=4 theta=10 dyn=1 sx=1 sk=0 pp=8"       # the 3-table predictor
TARGETS = ["bim", "cho", "gsh", "w0", "w1", "w2", "w3"]
RATES = ["1e-5", "1e-4", "1e-3", "1e-2"]
TAUS = ["1e2", "3e2", "1e3", "3e3", "1e4", "3e4", "1e5", "1e6"]
PREFS = ["0", "1", "m"]
DISTS = ["e", "l"]
MASKS = ["00", "03", "30", "21", "01", "0F", "F0", "3C"]
MTAUS = ["1e3", "1e4", "1e5"]
PRED = ["default", "bimodal", "gshare", "tournament"]
MODES = [(c, r) for c in range(4) for r in range(2)]


def lines(part, seeds, rates=None):
    rates = rates or RATES
    if part == "plain":
        out = [f"def {CHIP}", "bim bimodal n=64", "gsh gshare n=128 h=4", "trn tourn nb=64 ng=128 h=4 nc=64", "snt snt", f"t3 {CHIP3}"]
        out += [f"twx_c{c}r{r} {CHIP} tws=x twuse=1 code={c} rest={r}" for c, r in MODES]
        return out
    if part == "A":
        return [f"A_{t}_{r}_{s} {CHIP} fi_tgt={t} fi_rate={r} fi_seed={s}" + (" lt=1" if float(r) <= 1e-4 else "")
                for t in TARGETS for r in rates for s in seeds]
    if part == "AT":
        return [f"AT_{r}_{s} {CHIP} tws=x fi_tgt=tw fi_rate={r} fi_seed={s}" for r in rates for s in seeds]
    if part == "B":
        return [f"B_{d}{p}_{s} {CHIP} tws={','.join(f'{d}{t}p{p}' for t in TAUS)} dseed={s} dsig=0.5" for d in DISTS for p in PREFS for s in seeds]
    # coset masks for the twin (docs/fault_campaign.md section 8)
    if part == "M":
        return [f"M_e{p}_{s} {CHIP} tws={','.join(f'e{t}p{p}k{m}' for m in MASKS for t in MTAUS)} dseed={s} dsig=0.5" for p in PREFS for s in seeds]
    if part == "ML":       # log-normal family and the extra tau values, for the masks named in BPX_MASKS_L
        ml = os.environ.get("BPX_MASKS_L", "00").split(",")
        return [f"ML_{d}{p}_{s} {CHIP} tws={','.join(f'{d}{t}p{p}k{m}' for m in ml for t in taus)} dseed={s} dsig=0.5"
                for d, taus in (("l", MTAUS + ["1e2", "1e6"]), ("e", ["1e2", "1e6"])) for p in PREFS for s in seeds]
    if part == "MX":       # no decay: every mask must leave every count unchanged
        out = [f"MX_all {CHIP} tws={','.join('xk' + m for m in MASKS)}"]
        out += [f"MX_c{c}r{r} {CHIP} tws=xk{m} twuse=1 code={c} rest={r}" for c, r in MODES for m in ("21",)]
        return out
    if part == "MAT":      # Part A's flips into the twin, with the mask in BPX_MASK
        m = os.environ.get("BPX_MASK", "00")
        return [f"AT_{r}_{s} {CHIP} tws=xk{m} fi_tgt=tw fi_rate={r} fi_seed={s}" for r in rates for s in seeds]
    if part == "chk":
        out = [f"D_c{c}r{r} {CHIP} tws=e1e3pm dseed=1 twuse=1 code={c} rest={r}" for c, r in MODES]
        out += [f"U_off {CHIP} tws=e1e3pm dseed=1", f"U_off2 {CHIP} tws=l1e2p0,e1e4p1 dseed=2"]
        return out
    sys.exit(f"unknown part {part}")


def traces(progs=None):
    t = sorted(glob.glob(TRACES))
    if progs:
        t = [x for x in t if x.split("/")[-2] in progs]
    return t


def run(part, seeds, progs, tag, seq=1, rates=None):
    os.makedirs(OUT, exist_ok=True)
    cfg, tsv = os.path.join(OUT, tag + ".cfg"), os.path.join(OUT, tag + ".tsv")
    ls = lines(part, seeds, rates)
    open(cfg, "w").write("\n".join(ls) + "\n")
    tr = traces(progs)
    t0 = time.time()
    r0 = os.times()
    subprocess.check_call(["nice", "-n", "10", BIN, "W=8", f"threads={THREADS}", f"seq={seq}", cfg, tsv] + tr)
    r1 = os.times()
    cpu = (r1.children_user + r1.children_system) - (r0.children_user + r0.children_system)
    print(f"{tag}: {len(ls)} configs x {len(tr)} programs, {time.time() - t0:.1f} s elapsed, {cpu:.1f} s CPU")
    with open(os.path.join(OUT, "cpu_time.log"), "a") as f:
        f.write(f"{tag}\t{part}\t{len(ls)}\t{len(tr)}\t{time.time() - t0:.1f}\t{cpu:.1f}\n")


# ------------------------------------------------------------------------------------------- loading
def parse_x(s):
    if s == "-":
        return None
    d = {}
    for kv in s.split(";"):
        k, v = kv.split("=")
        d[k] = [int(x) for x in v.split(",")]
    return d


PREFIX = os.environ.get("BPX_FI_PREFIX", "")     # report on runs tagged <prefix>A, <prefix>AT, <prefix>B (pilot)


def have(tag):
    return os.path.exists(os.path.join(OUT, PREFIX + tag + ".tsv"))


def load(tag):
    rows = []
    if tag in ("A", "AT", "B"):
        tag = PREFIX + tag
    for r in csv.DictReader(open(os.path.join(OUT, tag + ".tsv")), delimiter="\t"):
        r["xs"], r["xc"] = parse_x(r["x_seq"]), parse_x(r["x_cpu"])
        rows.append(r)
    return rows


def plain_ref():
    """program -> dict(n=branches, cpu=[4 mispredictions], seq=[4], t3cpu, t3seq) of the fault-free chip."""
    ref = {}
    for r in load("plain"):
        p = r["program"]
        e = ref.setdefault(p, {"n": int(r["branches"])})
        if r["config"] == "def":
            e["cpu"], e["seq"] = r["xc"]["m4"], r["xs"]["m4"]
            e["ab_cpu"], e["ab_seq"] = r["xc"]["ab"][0], r["xs"]["ab"][0]
            e["def_cpu"], e["def_seq"] = int(r["mis_cpu"]), int(r["mis_seq"])
        elif r["config"] == "t3":
            e["t3cpu"], e["t3seq"] = int(r["mis_cpu"]), int(r["mis_seq"])
        elif r["config"] in ("bim", "gsh", "trn"):
            e[r["config"] + "_cpu"], e[r["config"] + "_seq"] = int(r["mis_cpu"]), int(r["mis_seq"])
        elif r["config"].startswith("twx_"):
            e[r["config"]] = (int(r["mis_cpu"]), int(r["mis_seq"]), r["xc"]["m4"], r["xs"]["m4"], r["xc"], r["xs"])
    return ref


# ------------------------------------------------------------------------------------------- statistics
def boot_ratio(per, reps=1000, seed=1):
    """per: {program: [(extra, faults) per seed]}.  Estimate: mean over programs of sum(extra)/sum(faults); CI: 95 %
    percentile bootstrap resampling programs and, within each, seeds."""
    progs = [p for p, v in per.items() if sum(f for _, f in v) > 0]
    if not progs:
        return float("nan"), float("nan"), float("nan"), []
    pr = {p: sum(e for e, _ in per[p]) / sum(f for _, f in per[p]) for p in progs}
    est = sum(pr.values()) / len(pr)
    rng = random.Random(seed)
    bs = []
    for _ in range(reps):
        acc, n = 0.0, 0
        for _ in progs:
            v = per[progs[rng.randrange(len(progs))]]
            sv = [v[rng.randrange(len(v))] for _ in v]
            f = sum(x[1] for x in sv)
            if f:
                acc += sum(x[0] for x in sv) / f
                n += 1
        bs.append(acc / max(n, 1))
    bs.sort()
    return est, bs[int(0.025 * reps)], bs[int(0.975 * reps) - 1], sorted(pr.values())


def boot_mean(per, reps=1000, seed=1):
    """per: {program: [value per seed]}; mean over programs of the mean over seeds, bootstrap as above."""
    progs = list(per)
    pm = {p: sum(v) / len(v) for p, v in per.items()}
    est = sum(pm.values()) / len(pm)
    rng = random.Random(seed)
    bs = []
    for _ in range(reps):
        acc = 0.0
        for _ in progs:
            v = per[progs[rng.randrange(len(progs))]]
            acc += sum(v[rng.randrange(len(v))] for _ in v) / len(v)
        bs.append(acc / len(progs))
    bs.sort()
    return est, bs[int(0.025 * reps)], bs[int(0.975 * reps) - 1], sorted(pm.values())


def q(xs, f):
    return xs[min(len(xs) - 1, int(f * len(xs)))] if xs else float("nan")


# ------------------------------------------------------------------------------------------- checks
def checks():
    ref = plain_ref()
    progs = sorted(p for p in ref if p not in EXCLUDE)
    print(f"plain run: {len(ref)} programs, {len(progs)} in the aggregates")
    acc_c = sum(100 * (1 - ref[p]["def_cpu"] / ref[p]["n"]) for p in progs) / len(progs)
    acc_s = sum(100 * (1 - ref[p]["def_seq"] / ref[p]["n"]) for p in progs) / len(progs)
    print(f"default predictor, mean accuracy over the {len(progs)} programs: {acc_c:.4f} % (CPU model), {acc_s:.4f} % (one in flight)")
    for k, name in ((1, "bim"), (2, "gsh"), (3, "trn")):
        a = sum(100 * (1 - ref[p][name + "_cpu"] / ref[p]["n"]) for p in progs) / len(progs)
        print(f"  {PRED[k]:10s} {a:.4f} % (stand-alone kind)")
    bad = [(p, k) for p in ref for k, name in ((1, "bim"), (2, "gsh"), (3, "trn")) if ref[p]["seq"][k] != ref[p][name + "_seq"]]
    bad += [(p, 0) for p in ref if ref[p]["cpu"][0] != ref[p]["def_cpu"] or ref[p]["seq"][0] != ref[p]["def_seq"]]
    print(f"one in flight: the four predictions from the chip's one state equal the stand-alone predictors on every program: {not bad} {bad[:5]}")
    for k, name in ((1, "bim"), (2, "gsh"), (3, "trn")):
        d = [ref[p]["cpu"][k] - ref[p][name + "_cpu"] for p in progs]
        a = sum(100 * (1 - ref[p]["cpu"][k] / ref[p]["n"]) for p in progs) / len(progs)
        print(f"  CPU model, {PRED[k]} observed beside the driving default: {a:.4f} %; minus stand-alone (driving) per program: "
              f"min {min(d)} max {max(d)} sum {sum(d)} (it trains at the default's timing)")
    # twin on (TWIN_EN = TWIN_USE = 1), no decay: every predictor's count must equal the plain run exactly
    bad = []
    for p in ref:
        for c, r in MODES:
            mc, msq, m4c, m4s, xc, xs = ref[p][f"twx_c{c}r{r}"]
            if mc != ref[p]["def_cpu"] or msq != ref[p]["def_seq"] or m4c[1:] != ref[p]["cpu"][1:] or m4s[1:] != ref[p]["seq"][1:]:
                bad.append((p, c, r))
            for x in (xc, xs):
                for cc, rr in MODES:
                    if x[f"s0c{cc}r{rr}"] != [x["m4"][0], 0, 0, 0, 0]:
                        bad.append((p, c, r, cc, rr))
    print(f"twin on without decay, 8 modes driving x 71 programs: all counts equal the plain run, all flags 0: {not bad} {bad[:5]}")
    if os.path.exists(os.path.join(OUT, "chk.tsv")):
        rows = load("chk")
        # U_off (tws=e1e3pm dseed=1, TWIN_USE off) carries the shadow count of every mode in a run the static
        # default drives; D_c?r? is the same scenario and seed with that mode's twin driving
        off = {r["program"]: r for r in rows if r["config"] == "U_off"}
        bad, diffs = [], []
        for r in rows:
            p = r["program"]
            if r["config"].startswith("U_off"):
                if int(r["mis_cpu"]) != ref[p]["def_cpu"] or int(r["mis_seq"]) != ref[p]["def_seq"] or r["xc"]["m4"] != ref[p]["cpu"] or r["xs"]["m4"] != ref[p]["seq"]:
                    bad.append((r["config"], p))
            else:
                c, rr = int(r["config"][3]), int(r["config"][5])
                sh = off[p]
                if int(r["mis_seq"]) != sh["xs"][f"s0c{c}r{rr}"][0] or r["xs"]["m4"][1:] != ref[p]["seq"][1:] or r["xs"][f"s0c{c}r{rr}"][1:] != sh["xs"][f"s0c{c}r{rr}"][1:]:
                    bad.append((r["config"], p, "seq"))
                diffs.append((int(r["mis_cpu"]) - sh["xc"][f"s0c{c}r{rr}"][0], int(r["mis_cpu"]) - ref[p]["def_cpu"], p, r["config"], ref[p]["n"]))
        print(f"TWIN_USE off with decay (e1e3pm, l1e2p0+e1e4p1): all counts equal the plain run; twin driving vs its shadow in a "
              f"run the static default drives, one in flight: mispredictions, flags and the other predictors equal: {not bad} {bad[:5]}")
        if diffs:
            ad = [abs(d[0]) for d in diffs]
            tot = sum(abs(d[1]) for d in diffs)
            net = sum(d[0] for d in diffs)
            print(f"twin driving in the CPU model, (driving - shadow) default mispredictions over {len(diffs)} runs (8 modes x 71): "
                  f"sum |.| {sum(ad)}, net {net:+d}, max |.| {max(ad)}; sum of |driving - plain| {tot}: the timing feedback the shadow leaves out")
            for c, rr in MODES:
                dd = [d for d in diffs if d[3] == f"D_c{c}r{rr}" and d[2] not in EXCLUDE]
                print(f"  C{c} R{rr}: mean over 69 programs of (driving - shadow) per 1000 branches {sum(1000 * d[0] / d[4] for d in dd) / len(dd):+.4f}, "
                      f"of (driving - plain) {sum(1000 * d[1] / d[4] for d in dd) / len(dd):+.4f}")


# ------------------------------------------------------------------------------------------- report
def fmt(x, d=3):
    return "nan" if x != x else f"{x:.{d}f}"


def report():
    os.makedirs(RES, exist_ok=True)
    ref = plain_ref()
    keep = lambda p: p not in EXCLUDE  # noqa: E731
    # ---- Part A, static tables
    rows = load("A") if have("A") else []
    for extra in sorted(glob.glob(os.path.join(OUT, PREFIX + "Amore*.tsv"))):    # more seeds / rates, same configuration names
        rows += load(os.path.basename(extra)[:-4])
    rates_in = sorted({r["config"].split("_")[2] for r in rows}, key=float)
    data, lt, nfault = {}, {}, {}
    for r in rows:
        _, t, rate, s = r["config"].split("_")
        p = r["program"]
        nfault.setdefault((t, rate), []).append((r["xc"]["nf"][0], ref[p]["n"]))
        if not keep(p):
            continue
        for inst, xk in (("cpu", "xc"), ("seq", "xs")):
            x = r[xk]
            for k in range(4):
                data.setdefault((t, rate, k, inst), {}).setdefault(p, []).append((x["m4"][k] - ref[p][inst][k], x["nf"][0]))
        if "lt" in r["xc"]:
            a = lt.setdefault((t, rate), [0] * 7)
            for i, v in enumerate(r["xc"]["lt"]):
                a[i] += v
    out = ["target\trate\tpredictor\tinstance\textra_per_fault\tci_lo\tci_hi\tpooled\tfaults\tprog_p10\tprog_median\tprog_p90\tprog_min\tprog_max"]
    tab = {}
    for (t, rate, k, inst), per in sorted(data.items()):
        est, lo, hi, pv = boot_ratio(per)
        pooled = sum(e for v in per.values() for e, _ in v) / max(1, sum(f for v in per.values() for _, f in v))
        nf = sum(f for v in per.values() for _, f in v)
        tab[(t, rate, k, inst)] = (est, lo, hi, pooled, nf, pv)
        out.append(f"{t}\t{rate}\t{PRED[k]}\t{inst}\t{est:.4f}\t{lo:.4f}\t{hi:.4f}\t{pooled:.4f}\t{nf}\t{q(pv, .1):.4f}\t{q(pv, .5):.4f}\t{q(pv, .9):.4f}\t{pv[0]:.4f}\t{pv[-1]:.4f}")
    if rows:
        open(os.path.join(RES, "partA.tsv"), "w").write("\n".join(out) + "\n")
        for inst, iname in (("cpu", "CPU model"), ("seq", "one in flight")):
            print(f"\n### Part A summary ({iname}): extra mispredictions per fault [95 % CI], rate {rates_in}; seeds per rate\n")
            for t in TARGETS:
                for k in range(4):
                    cells = []
                    for rate in rates_in:
                        e = tab.get((t, rate, k, inst))
                        ns = len(next(iter(data[(t, rate, k, inst)].values())))
                        cells.append(f"{e[0]:.3f} [{e[1]:.3f}, {e[2]:.3f}] s{ns}")
                    print(f"| {t} | {PRED[k]} | " + " | ".join(cells) + " |")
        print("\n### Part A: extra mispredictions per fault (CPU model, mean over programs, 95 % CI)\n")
        for rate in rates_in:
            print(f"\nrate {rate} per resolved branch\n")
            print("| Target | " + " | ".join(PRED) + " | faults |")
            print("| --- | " + " | ".join("---" for _ in PRED) + " | --- |")
            for t in TARGETS:
                cells = []
                for k in range(4):
                    e = tab.get((t, rate, k, "cpu"))
                    cells.append("–" if not e else f"{e[0]:.3f} [{e[1]:.3f}, {e[2]:.3f}]")
                nf = tab.get((t, rate, 0, "cpu"), (0, 0, 0, 0, 0))[4]
                print(f"| {t} | " + " | ".join(cells) + f" | {nf} |")
        # fault counts against the rate (all 71 programs)
        print("\nfaults injected / expected (rate x branches), all 71 programs, all seeds:")
        for (t, rate), v in sorted(nfault.items()):
            if t == "w0":
                got, exp_ = sum(a for a, _ in v), sum(float(rate) * n for _, n in v)
                print(f"  {t} {rate}: {got} / {exp_:.1f} (ratio {got / exp_:.4f}, {(got - exp_) / math.sqrt(exp_):+.2f} sd)")
        lo_ = ["target\trate\tfaults\tentry_recovered\tmean_entry_life\tstate_recovered\tmean_state_life\tcensored\tmean_censored_age"]
        print("\nfault lifetime (resolved branches; CPU instance, all 71 programs, all seeds)\n")
        print("| Target | rate | faults | entry back to fault-free value: share, mean | whole state back: share, mean | never (end of trace or overlap) |")
        print("| --- | --- | --- | --- | --- | --- |")
        for (t, rate), a in sorted(lt.items(), key=lambda kv: (TARGETS.index(kv[0][0]), kv[0][1])):
            n, er, es, gr, gs, cn, csum = a
            lo_.append(f"{t}\t{rate}\t{n}\t{er}\t{es / max(er, 1):.1f}\t{gr}\t{gs / max(gr, 1):.1f}\t{cn}\t{csum / max(cn, 1):.1f}")
            print(f"| {t} | {rate} | {n} | {100 * er / max(n, 1):.1f} %, {es / max(er, 1):.0f} | {100 * gr / max(n, 1):.1f} %, {gs / max(gr, 1):.0f} | {100 * cn / max(n, 1):.1f} % |")
        open(os.path.join(RES, "partA_lifetime.tsv"), "w").write("\n".join(lo_) + "\n")
    # ---- Part A on the twin
    if have("AT"):
        data = {}
        for r in load("AT"):
            p = r["program"]
            if not keep(p):
                continue
            _, rate, s = r["config"].split("_")
            x = r["xc"]
            nf = x["nftw"][0]
            for c, rr in MODES:
                v = x[f"s0c{c}r{rr}"]
                d = data.setdefault((rate, c, rr), {})
                d.setdefault(p, []).append((v[0] - ref[p]["cpu"][0], nf, v[1:]))
        out = ["rate\tcode\trestore\textra_per_flip\tci_lo\tci_hi\tflips\terr_per_flip\tcor_per_flip\tdet_per_flip\tsdc_per_flip\textra_per_err"]
        print("\n### Part A on the twin W3L (TWIN_EN = TWIN_USE = 1): extra mispredictions of the default per flip (CPU model)\n")
        print("| rate | CODE | RESTORE | extra / flip [95 % CI] | TW_ERR / flip | TW_COR / flip | TW_DET / flip | TW_SDC / flip | extra / TW_ERR |")
        print("| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
        for (rate, c, rr), per in sorted(data.items(), key=lambda kv: (float(kv[0][0]), kv[0][1], kv[0][2])):
            est, lo, hi, _ = boot_ratio({p: [(e, f) for e, f, _ in v] for p, v in per.items()})
            nf = sum(f for v in per.values() for _, f, _ in v)
            ev = [sum(x[2][i] for v in per.values() for x in v) / max(nf, 1) for i in range(4)]
            ex = sum(x[0] for v in per.values() for x in v)
            epe = ex / max(1, sum(x[2][0] for v in per.values() for x in v))
            out.append(f"{rate}\t{c}\t{rr}\t{est:.4f}\t{lo:.4f}\t{hi:.4f}\t{nf}\t" + "\t".join(f"{e:.2f}" for e in ev) + f"\t{epe:.5f}")
            print(f"| {rate} | {c} | {rr} | {est:.3f} [{lo:.3f}, {hi:.3f}] | " + " | ".join(f"{e:.1f}" for e in ev) + f" | {epe:.4f} |")
        open(os.path.join(RES, "partA_twin.tsv"), "w").write("\n".join(out) + "\n")
    # ---- Part B
    if have("B"):
        data = {}
        abst, t3 = {}, {}
        for p in ref:
            if keep(p):
                abst[p] = [1000 * (ref[p]["ab_cpu"] - ref[p]["cpu"][0]) / ref[p]["n"]]
                t3[p] = [1000 * (ref[p]["t3cpu"] - ref[p]["cpu"][0]) / ref[p]["n"]]
        for r in load("B"):
            p = r["program"]
            if not keep(p):
                continue
            grp, s = r["config"].split("_")[1:]
            x, n, base = r["xc"], ref[p]["n"], ref[p]["cpu"][0]
            assert x["m4"] == ref[p]["cpu"], (p, r["config"])
            for j, tau in enumerate(TAUS):
                for c, rr in MODES:
                    v = x[f"s{j}c{c}r{rr}"]
                    d = data.setdefault((grp[0], grp[1], tau, c, rr), {})
                    d.setdefault(p, []).append((1000 * (v[0] - base) / n, v[0] - base, [1000 * e / n for e in v[1:]], v[1]))
        out = ["dist\tpref\ttau\tcode\trestore\textra_per_1k\tci_lo\tci_hi\terr_per_1k\tcor_per_1k\tdet_per_1k\tsdc_per_1k\textra_per_err\tprog_max"]
        summary = {}
        for key, per in sorted(data.items()):
            est, lo, hi, pv = boot_mean({p: [x[0] for x in v] for p, v in per.items()})
            ev = [sum(sum(x[2][i] for x in v) / len(v) for v in per.values()) / len(per) for i in range(4)]
            ex = sum(x[1] for v in per.values() for x in v)
            er = sum(x[3] for v in per.values() for x in v)
            summary[key] = (est, lo, hi, ev, ex / max(er, 1), pv[-1])
            out.append("\t".join(map(str, key)) + f"\t{est:.4f}\t{lo:.4f}\t{hi:.4f}\t" + "\t".join(f"{e:.3f}" for e in ev) + f"\t{ex / max(er, 1):.5f}\t{pv[-1]:.3f}")
        open(os.path.join(RES, "partB.tsv"), "w").write("\n".join(out) + "\n")
        ea, la, ha, _ = boot_mean(abst)
        e3, l3, h3, _ = boot_mean(t3)
        print(f"\nalways abstain on W3 (W3's term left out of the sum): {ea:.3f} [{la:.3f}, {ha:.3f}] extra mispredictions per 1000 branches")
        print(f"3-table predictor (W3 not built, trained without it): {e3:.3f} [{l3:.3f}, {h3:.3f}] per 1000 branches")
        for d, dn in (("e", "exponential lifetime"), ("l", "log-normal per-cell lifetime, sigma 0.5")):
            for pf, pn in (("m", "mixed preferred level"), ("0", "all decay to 0"), ("1", "all decay to 1")):
                print(f"\n### Part B, {dn}, {pn}: extra mispredictions of the default per 1000 branches [95 % CI] (CPU model)\n")
                print("| tau (branches) | " + " | ".join(f"C{c} R{r}" for c, r in MODES) + " |")
                print("| --- | " + " | ".join("---" for _ in MODES) + " |")
                for tau in TAUS:
                    cells = []
                    for c, r in MODES:
                        e = summary.get((d, pf, tau, c, r))
                        cells.append("–" if not e else f"{e[0]:.2f} [{e[1]:.2f}, {e[2]:.2f}]")
                    print(f"| {tau} | " + " | ".join(cells) + " |")
        # paired comparisons (same programs, seeds and decay draws): difference of extra mispredictions per 1000 branches
        def paired(key_a, key_b=None, ref_b=None):
            pa = data[key_a]
            per = {p: [a[0] - (data[key_b][p][i][0] if key_b else ref_b[p][0]) for i, a in enumerate(v)] for p, v in pa.items()}
            return boot_mean(per)
        cols = [("C2R1 − C3R0", (2, 1), (3, 0), None), ("C2R0 − C3R0", (2, 0), (3, 0), None), ("C3R1 − C3R0", (3, 1), (3, 0), None),
                ("C2R1 − C2R0", (2, 1), (2, 0), None), ("C0R0 − abstain", (0, 0), None, abst), ("C3R0 − abstain", (3, 0), None, abst),
                ("C2R1 − abstain", (2, 1), None, abst), ("C2R1 − 3-table", (2, 1), None, t3), ("C3R0 − 3-table", (3, 0), None, t3)]
        pout = ["dist\tpref\ttau\tcomparison\tdiff_per_1k\tci_lo\tci_hi"]
        for d in DISTS:
            for pf in PREFS:
                print(f"\n### Part B paired differences, {d} {pf}: extra mispredictions per 1000 branches, A − B [95 % CI]; negative = A better\n")
                print("| tau | " + " | ".join(c[0] for c in cols) + " |")
                print("| --- | " + " | ".join("---" for _ in cols) + " |")
                for tau in TAUS:
                    cells = []
                    for name, ma, mb, rb in cols:
                        e, lo, hi, _ = paired((d, pf, tau) + ma, (d, pf, tau) + mb if mb else None, rb)
                        cells.append(f"{e:+.2f} [{lo:+.2f}, {hi:+.2f}]")
                        pout.append(f"{d}\t{pf}\t{tau}\t{name}\t{e:.4f}\t{lo:.4f}\t{hi:.4f}")
                    print(f"| {tau} | " + " | ".join(cells) + " |")
        open(os.path.join(RES, "partB_paired.tsv"), "w").write("\n".join(pout) + "\n")
        print("\n### Part B events, exponential, mixed: TW_ERR / TW_COR / TW_DET / TW_SDC per 1000 branches; extra mispredictions per faulty read\n")
        print("| tau | " + " | ".join(f"C{c} R{r}" for c, r in MODES) + " |")
        print("| --- | " + " | ".join("---" for _ in MODES) + " |")
        for tau in TAUS:
            cells = []
            for c, r in MODES:
                e = summary.get(("e", "m", tau, c, r))
                cells.append("–" if not e else f"{e[3][0]:.0f}/{e[3][1]:.0f}/{e[3][2]:.0f}/{e[3][3]:.0f}; {e[4]:.4f}")
            print(f"| {tau} | " + " | ".join(cells) + " |")


def mask_analysis():
    """Algebra of each mask against the twin's (8,4) code (model/bpx1.py tw_*): code word or not, syndrome, distance
    to the code, and what a fully decayed word (nodes all 0 or all 1) decodes to under each CODE."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "model"))
    import bpx1
    code = [bpx1.tw_encode(d) for d in range(16)]
    print("code words:", " ".join(f"{c:02X}" for c in code))
    for ms in MASKS:
        m = int(ms, 16)
        dist = min(bin(m ^ c).count("1") for c in code)
        syn = bpx1.tw_check(m & 15) ^ (m >> 4)
        cells = []
        for node in (0x00, 0xFF):
            w = node ^ m
            outs = []
            for cd in range(4):
                d, ab, cor = bpx1.tw_decode(w, cd)
                outs.append("abstain" if ab else (f"w={d - 16 if d & 8 else d}" + (" (corrected)" if cor else "")))
            cells.append(f"nodes {node:02X} -> word {w:02X}: " + ", ".join(f"C{cd} {o}" for cd, o in enumerate(outs)))
        print(f"mask {ms}: weight {bin(m).count('1')}, code word {m in code}, syndrome {syn:X}, distance to code {dist}; " + "; ".join(cells))


def mask_checks():
    ref = plain_ref()
    bad = []
    for r in load("MX"):
        p = r["program"]
        if int(r["mis_cpu"]) != ref[p]["def_cpu"] or int(r["mis_seq"]) != ref[p]["def_seq"] or r["xc"]["m4"] != ref[p]["cpu"] or r["xs"]["m4"] != ref[p]["seq"]:
            bad.append((r["config"], p))
        for x in (r["xc"], r["xs"]):
            for k, v in x.items():
                if k.startswith("s") and v != [x["m4"][0], 0, 0, 0, 0]:
                    bad.append((r["config"], p, k))
    print(f"no decay, masks {MASKS} (shadow, all modes) and mask 21 driving in all 8 modes, 71 programs: every count equals the plain run, all flags 0: {not bad} {bad[:5]}")
    # mask 00 in the M runs must reproduce Part B exactly (same seeds, same scenario seeds)
    if have("B") and os.path.exists(os.path.join(OUT, "M.tsv")):
        b = {(r["config"], r["program"]): r for r in load("B")}
        nb = neq = 0
        for r in load("M"):
            _, fam, s = r["config"].split("_")
            ob = b.get((f"B_{fam}_{s}", r["program"]))
            if not ob:
                continue
            for jt, tau in enumerate(MTAUS):
                jm = MASKS.index("00") * len(MTAUS) + jt
                jb = TAUS.index(tau)
                for c, rr in MODES:
                    for inst in ("xc", "xs"):
                        nb += 1
                        neq += r[inst][f"s{jm}c{c}r{rr}"] != ob[inst][f"s{jb}c{c}r{rr}"]
        print(f"mask 00 in the mask runs against Part B (same seeds): {nb - neq} of {nb} mode counts identical")


def mask_report():
    ref = plain_ref()
    keep = lambda p: p not in EXCLUDE  # noqa: E731
    data = {}
    for tag in ("M", "ML"):
        if not os.path.exists(os.path.join(OUT, tag + ".tsv")):
            continue
        cfgs = {l.split()[0]: l.split() for l in open(os.path.join(OUT, tag + ".cfg")) if l.strip()}
        for r in load(tag):
            p = r["program"]
            if not keep(p):
                continue
            specs = next(t for t in cfgs[r["config"]] if t.startswith("tws=")).split("=")[1].split(",")
            x, n, base = r["xc"], ref[p]["n"], ref[p]["cpu"][0]
            for j, sp in enumerate(specs):
                d, tau, pf, m = sp[0], sp[1:sp.index("p")], sp[sp.index("p") + 1], sp.split("k")[1]
                for c, rr in MODES:
                    v = x[f"s{j}c{c}r{rr}"]
                    data.setdefault((m, d, pf, tau, c, rr), {}).setdefault(p, []).append((1000 * (v[0] - base) / n, 1000 * v[4] / n))
    out = ["mask\tdist\tpref\ttau\tcode\trestore\textra_per_1k\tci_lo\tci_hi\tsdc_per_1k\tdiff_vs_mask00\tdiff_lo\tdiff_hi"]
    summ = {}
    for key, per in sorted(data.items()):
        est, lo, hi, _ = boot_mean({p: [a for a, _ in v] for p, v in per.items()})
        sdc = sum(sum(s for _, s in v) / len(v) for v in per.values()) / len(per)
        k0 = ("00",) + key[1:]
        dd = (float("nan"),) * 3
        if k0 in data and key[0] != "00":
            z = data[k0]
            dd = boot_mean({p: [a - z[p][i][0] for i, (a, _) in enumerate(v)] for p, v in per.items()})[:3]
        summ[key] = (est, lo, hi, sdc, dd)
        out.append("\t".join(map(str, key)) + f"\t{est:.4f}\t{lo:.4f}\t{hi:.4f}\t{sdc:.3f}\t{dd[0]:.4f}\t{dd[1]:.4f}\t{dd[2]:.4f}")
    os.makedirs(RES, exist_ok=True)
    open(os.path.join(RES, "masks.tsv"), "w").write("\n".join(out) + "\n")
    fams = sorted({k[1:3] for k in summ})
    taus = sorted({k[3] for k in summ}, key=float)
    masks = [m for m in MASKS if any(k[0] == m for k in summ)]
    for d, pf in fams:
        for tau in taus:
            if not any(k[1:4] == (d, pf, tau) for k in summ):
                continue
            print(f"\n### masks, {d} {pf}, tau {tau}: extra per 1000 branches (SDC per 1000) [paired diff vs mask 00]\n")
            print("| mask | " + " | ".join(f"C{c} R{r}" for c, r in MODES) + " |")
            print("| --- | " + " | ".join("---" for _ in MODES) + " |")
            for m in masks:
                cells = []
                for c, r in MODES:
                    e = summ.get((m, d, pf, tau, c, r))
                    if not e:
                        cells.append("–")
                        continue
                    t = f"{e[0]:.2f} ({e[3]:.0f})"
                    if m != "00" and e[4][0] == e[4][0]:
                        t += f" [{e[4][0]:+.2f}{'*' if e[4][1] > 0 or e[4][2] < 0 else ''}]"
                    cells.append(t)
                print(f"| {m} | " + " | ".join(cells) + " |")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    opts = dict(a.split("=", 1) for a in sys.argv[2:] if "=" in a)
    args = [a for a in sys.argv[2:] if "=" not in a]
    if cmd == "run":
        part = args[0]
        seeds = [int(s) for s in opts.get("seeds", "1,2,3").split(",")]
        progs = set(opts["progs"].split(",")) if "progs" in opts else None
        rates = opts["rates"].split(",") if "rates" in opts else None
        run(part, seeds, progs, opts.get("tag", part), int(opts.get("seq", "1")), rates)
    elif cmd == "checks":
        checks()
    elif cmd == "report":
        report()
    elif cmd == "masks":
        mask_analysis()
    elif cmd == "maskchecks":
        mask_checks()
    elif cmd == "maskreport":
        mask_report()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
