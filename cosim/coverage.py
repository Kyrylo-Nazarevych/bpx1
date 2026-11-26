#!/usr/bin/env python3
"""Functional coverage of the random-operation runs (spec VR-02).

  coverage.py <run-prefix> [<run-prefix> ...]      each run has <prefix>.meta (model/randgen.py) and <prefix>.lat (cosim --rand)

Bins are defined here; docs/coverage.md explains them.  Writes docs/coverage_bins.md and prints the summary.
"""
import sys
from collections import Counter

OPN = ["PREDICT", "PREDICT+RESOLVE", "RESOLVE", "FLUSH", "NOP"]
TCASE = ["confident hit: weights unchanged", "low-margin hit", "miss", "miss at low margin"]
# bins of the required cross that cannot occur while the control port is silent, with the reason; each is an
# assertion of the formal task "nowait" (formal/bpx.sby, docs/coverage.md)
UNREACH = {(0, 4, -1, 1): "a refused predict is acknowledged at once"}
UNREACH.update({(1, 4, m, 0): "queue full: the resolve goes first, the predict a clock later" for m in (0, 1)})
UNREACH[(2, 0, -1, 1)] = "a refused resolve is acknowledged at once"
UNREACH.update({(2, 4, m, 1): "with the queue full the oldest entry is always staged and no write is in progress" for m in (0, 1)})
UNREACH.update({(c, d, -1, 1): "FLUSH and NOP do not wait for the tables" for c in (3, 4) for d in range(5)})


def main():
    cross, other, lat_h = Counter(), Counter(), Counter()
    total = 0
    for pre in sys.argv[1:]:
        lat = open(pre + ".lat", "rb").read()
        prev = None
        for i, ln in enumerate(open(pre + ".meta")):
            v = list(map(int, ln.split()))
            cls, depth, misp, refused, drive, frozen, pred, conf, feat, lowp, squash, rfirst, gap, setup, tcase, tneg, tsh, tsl, tth = v
            la = lat[i]
            bp = 1 if la > 3 else 0
            total += 1
            cross[(cls, depth, misp, bp)] += 1
            lat_h[la] += 1
            o = other
            o[("latency", min(la, 7))] += 1
            o[("gap", "0" if gap == 0 else "1-2" if gap <= 2 else "3+")] += 1
            o[("setup", setup)] += 1
            if cls in (0, 1) and not refused & 2:
                o[("drive x pred", drive, pred)] += 1
                o[("pred x conf", pred, conf)] += 1
                if drive < 4:
                    o[("drive x conf", drive, conf)] += 1
                o[("predict with features", feat)] += 1
                if lowp >= 0:
                    o[("default predictor: margin at most the threshold", lowp)] += 1
            if refused & 1:
                o[("error", "resolve on empty")] += 1
            if refused & 2:
                o[("error", "predict on full")] += 1
            if rfirst:
                o[("combined at full: resolve first", misp)] += 1
            if cls == 3:
                o[("flush at depth", depth)] += 1
            if squash >= 0:
                o[("repair discards", squash)] += 1
            if misp >= 0:
                o[("resolve x frozen x mispredicted", frozen, misp)] += 1
                o[("resolve x drive", drive)] += 1
            if tcase >= 0:
                o[("train", TCASE[tcase])] += 1
                o[("train with features", feat)] += 1
                o[("train: a table counts negated", tneg)] += 1
                o[("train: a weight already at its maximum", tsh)] += 1
                o[("train: a weight already at its minimum", tsl)] += 1
                o[("train: threshold moves", tth)] += 1
            if prev is not None:
                o[("sequence", OPN[prev], OPN[cls])] += 1
            prev = cls
    # ---- defined bins
    cb = []
    for cls in range(5):
        for depth in range(5):
            if cls in (1, 2):
                ms = [-1] if (cls == 2 and depth == 0) else [0, 1]
            else:
                ms = [-1]
            for m in ms:
                for bp in (0, 1):
                    cb.append((cls, depth, m, bp))
    ob = [("latency", k) for k in (3, 4, 5, 6)] + [("gap", g) for g in ("0", "1-2", "3+")] + [("setup", s) for s in (0, 1, 2)]
    ob += [("drive x pred", d, p) for d in range(5) for p in (0, 1) if not (d == 4 and p == 1)]
    ob += [("pred x conf", p, c) for p in (0, 1) for c in (0, 1)]
    ob += [("drive x conf", d, c) for d in range(4) for c in (0, 1)] + [("predict with features", f) for f in range(8)]
    ob += [("default predictor: margin at most the threshold", x) for x in (0, 1)]
    ob += [("error", "resolve on empty"), ("error", "predict on full")]
    ob += [("combined at full: resolve first", m) for m in (0, 1)]
    ob += [("flush at depth", d) for d in range(5)] + [("repair discards", s) for s in range(4)]
    ob += [("resolve x frozen x mispredicted", f, m) for f in (0, 1) for m in (0, 1)] + [("resolve x drive", d) for d in range(5)]
    ob += [("train", t) for t in TCASE] + [("train with features", f) for f in range(8)]
    ob += [("train: a table counts negated", x) for x in (0, 1)] + [("train: a weight already at its maximum", x) for x in (0, 1)]
    ob += [("train: a weight already at its minimum", x) for x in (0, 1)] + [("train: threshold moves", x) for x in (-1, 0, 1)]
    ob += [("sequence", a, b) for a in OPN for b in OPN]
    L = ["# BPX-1 functional coverage: bins and hits (VR-02)\n",
         f"Generated by `cosim/coverage.py` from {len(sys.argv) - 1} random runs, {total:,} requests. Definitions: docs/coverage.md.\n",
         "## Required cross: operation × in-flight depth × misprediction × back-pressure\n",
         "Back-pressure = the request was acknowledged later than the 3 clocks of an idle chip.\n",
         "| Operation | Depth before | Mispredicted | Back-pressure | Hits |", "| --- | --- | --- | --- | --- |"]
    hit = unreach = 0
    for b in cb:
        n = cross[b]
        u = b in UNREACH
        hit += n > 0
        unreach += u and n == 0
        L.append(f"| {OPN[b[0]]} | {b[1]} | {'-' if b[2] < 0 else ['no', 'yes'][b[2]]} | {['no', 'yes'][b[3]]} | {n:,}{'' if n or not u else ' (cannot occur: ' + UNREACH[b] + ')'} |")
    extra = [b for b in cross if b not in cb]
    L.append(f"\nCross bins hit: **{hit} of {len(cb) - unreach}** reachable ({len(cb)} defined, {unreach} cannot occur). Undefined combinations seen: {len(extra)} {extra if extra else ''}.\n")
    L += ["## Other bins\n", "| Bin | Hits |", "| --- | --- |"]
    oh = 0
    for b in ob:
        n = other[b]
        oh += n > 0
        L.append(f"| {' / '.join(map(str, b))} | {n:,} |")
    tot_def = len(cb) - unreach + len(ob)
    L.append(f"\nOther bins hit: **{oh} of {len(ob)}**. All bins: **{hit + oh} of {tot_def} = {100 * (hit + oh) / tot_def:.1f} %**.\n")
    L.append("Acknowledge latency, clocks: " + ", ".join(f"{k}: {v:,}" for k, v in sorted(lat_h.items())) + ".\n")
    open("docs/coverage_bins.md", "w", encoding="utf-8", newline="\n").write("\n".join(L))
    print(f"cross {hit}/{len(cb) - unreach} (unreachable {unreach}), other {oh}/{len(ob)}, all {100 * (hit + oh) / tot_def:.1f} %")
    print("cross bins not hit:", [(OPN[b[0]],) + b[1:] for b in cb if not cross[b]])
    print("other bins not hit:", [b for b in ob if not other[b]])
    print("latency:", sorted(lat_h.items()))


if __name__ == "__main__":
    main()
