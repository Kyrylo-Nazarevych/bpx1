#!/usr/bin/env python3
"""Constrained-random operations with expected results from the golden model (spec VR-01, VR-02).

  randgen.py <operations> <seed> <out.ops> <out.meta> [conc]

out.ops is played against the RTL by `cosim --rand` (cosim/cosim.cpp), one line per step:
  R op id expected gap setup     a request; expected = PRED | CONF<<1 [| the four prediction pins <<2 | 64 for an
                                 accepted predict]; gap = idle clocks before it; setup = clocks
                                 ID/OP are placed before REQ toggles
  W addr value                   a control-port write (CTRL, CMD, or RAW_ADDR and RAW_DATA for a raw write)
  S / C  resolved m0 m1 m2 m3 errors flags queue     snapshot (S) or snapshot-and-clear (C), then compare
  D space count w...             read `count` raw words of a state space and compare (FR-10)
With `conc` the control port works while the requests run (FR-10 during a live run): no foreground statistics
steps; instead background jobs that `cosim --rand` starts and does not wait for (dropped if one is still running):
  X S / X C                      snapshot / snapshot-and-clear, then read STATUS, ERRCNT and the ten statistics words
  X D space addr count           raw read of `count` entries
  X I                            read ID and CTRL
  X W addr value                 raw write of a bimodal+chooser entry that no branch in use maps to (never dropped)
  J                              wait for the background job to finish
What the jobs read is checked by cosim/conc_check.py.
out.meta has one line per R with what the model did, for the functional coverage (cosim/coverage.py).
"""
import random
import sys

import bpx1


def outcome(ident, hist):
    """Synthetic branch behaviour by identifier: biased, loop-like, alternating, patterned and hashed branches."""
    k = ident % 7
    c = hist.setdefault(ident, 0)
    hist[ident] = c + 1
    if k == 0:
        return 1
    if k == 1:
        return 0
    if k == 2:
        return 1 if c % 5 != 4 else 0
    if k == 3:
        return c & 1
    if k == 4:
        return 1 if (c // 3) & 1 else 0
    return (ident * 2654435761 >> (c % 11)) & 1


def main():
    N, seed = int(sys.argv[1]), int(sys.argv[2])
    conc = len(sys.argv) > 5 and sys.argv[5] == "conc"
    rng = random.Random(seed)
    ops = open(sys.argv[3], "w")
    meta = open(sys.argv[4], "w")
    m = bpx1.Bpx1()
    cfg = m.cfg
    NID = 1 << cfg.W
    hist = {}
    ids = [rng.randrange(NID) for _ in range(48)]
    spaces = [(0, cfg.NB), (1, cfg.NG)] + [(8 + i, e) for i, (e, _) in enumerate(cfg.weights)]

    def stat_line(kind):
        s = m.stats()
        ops.write(f"{kind} {s['resolved']} {s['mis_default']} {s['mis_bimodal']} {s['mis_gshare']} {s['mis_tournament']} "
                  f"{s['errors']} {(s['err_flags'] & 3) | (8 if s['overflow'] else 0)} {len(m.queue)}\n")
        if kind == "C":
            m.n_res, m.mis = 0, [0, 0, 0, 0]

    def dump():
        for sp, n in spaces:
            ops.write(f"D {sp} {n} " + " ".join(str(m.raw(sp, a)) for a in range(n)) + "\n")
        ops.write("D 7 9 " + " ".join(str(m.raw(7, a)) for a in range(9)) + "\n")

    def req(op, arg, fast):
        depth = len(m.queue)
        head = m.queue[0] if m.queue else None
        Lp = m._lookup(arg, m.sh, m.psh) if op != bpx1.OP_SPECIAL else None
        m.last_train = None
        frozen, drive, feat = m.frozen, m.drive, m.feat
        rep = m.request(op, arg)
        sub = arg & 3
        cls = 0 if op == bpx1.OP_PREDICT else 1 if op != bpx1.OP_SPECIAL else (4 if sub == 0 else 3 if sub == 1 else 2)
        misp, squash, rfirst = -1, -1, 0
        if cls in (1, 2) and not rep.refused:
            tk = (op & 1) if cls == 1 else (sub & 1)
            if cls == 1 and depth < cfg.DEPTH and depth == 0:
                pd = rep.pred                      # resolves the branch it has just predicted
            else:
                pd = head["pdrv"]
            misp = 1 if pd != tk else 0
            rfirst = 1 if cls == 1 and depth >= cfg.DEPTH else 0
            if misp:
                squash = (depth - 1) if (cls == 2 or rfirst) else depth
                if cls == 1 and depth == 0:
                    squash = 0
        lowp = -1
        if Lp is not None and not (rep.refused & bpx1.ERR_PREDICT_FULL) and not rfirst:
            lowp = 0 if Lp.conf[0] else 1          # the default predictor's margin at the predict
        t = m.last_train
        if t is None:
            tr = (-1, 0, 0, 0, 0)
        else:
            tr = ((2 if t["miss"] else 0) + (1 if t["low"] else 0), 1 if t["neg"] else 0, 1 if t["sat_hi"] else 0, 1 if t["sat_lo"] else 0, t["th"])
        gap = 0 if fast else rng.choice([0, 0, 0, 1, 1, 2, 3, 5, 9])
        setup = rng.choice([0, 0, 1, 2])
        exp = rep.pred | (rep.conf << 1)
        if op != bpx1.OP_SPECIAL and not rep.refused & bpx1.ERR_PREDICT_FULL:
            exp |= 64 | sum(b << (2 + k) for k, b in enumerate(m.last_preds))   # DBG0, PBIM, PGSH, DBG1
        ops.write(f"R {op} {arg} {exp} {gap} {setup}\n")
        meta.write(f"{cls} {depth} {misp} {rep.refused} {drive} {1 if frozen else 0} {rep.pred} {rep.conf} {feat} {lowp} {squash} "
                   f"{rfirst} {gap} {setup} {tr[0]} {tr[1]} {tr[2]} {tr[3]} {tr[4]}\n")

    def drain():
        while m.queue:
            tk = outcome(m.queue[0]["id"], hist)
            req(bpx1.OP_SPECIAL, bpx1.SUB_RESOLVE_T if tk else bpx1.SUB_RESOLVE_NT, False)

    n = 0
    mode, left = 0, 0
    while n < N:
        clear_ok = n < 0.35 * N or n > 0.9 * N           # a long stretch without clears lets the counters saturate
        if left == 0:
            mode = rng.choice([0, 0, 0, 1, 2, 3])        # 0 mixed, 1 predict-heavy (fills the queue), 2 trace-like, 3 fast mixed
            left = rng.randint(20, 400)
            if rng.random() < 0.02:
                if conc:
                    ops.write("J\n")
                ids = [rng.randrange(NID) for _ in range(rng.choice([8, 48, 200]))]
        left -= 1
        r = rng.random()
        if r < 0.0015:
            m.drive = rng.choice([0, 0, 0, 1, 2, 3, 4])
            m.frozen = rng.random() < 0.15
            m.feat = rng.choice([cfg.FEAT] * 6 + list(range(16)))
            ops.write(f"W 1 {m.ctrl_word()}\n")
            continue
        if conc and r < 0.0025:
            continue
        if r < 0.0025:
            stat_line("C" if (clear_ok and rng.random() < 0.3) else "S")
            continue
        if r < 0.0027:
            if rng.random() < 0.5:
                drain()
            dump()
            continue
        if r < 0.00275:
            ops.write("W 2 2\n")                         # clear predictors
            m.clear_predictors()
            continue
        if r < 0.0029:                                   # raw write of the threshold and its adaptation counter
            v = rng.choice([0, 1, 2, 5, 12, 30, 62, 63]) << 4 | rng.choice([0, 6, 7, 8, 9, 15])
            m.raw_write(7, 4, v)
            ops.write(f"W 15 {0x3704}\nW 16 {v}\n")
            continue
        if conc and r < 0.00305:
            continue
        if conc and r < 0.025:
            k = rng.choice("SSCCDDDDDWWI")
            if k == "D":
                sp, size = rng.choice(spaces + [(0, cfg.NB), (7, 9)])
                if sp == 7:
                    a, cnt = rng.choice([(0, 1), (0, 2), (1, 1), (4, 1), (4, 3), (5, 2), (6, 1)])
                else:
                    cnt = rng.randint(1, 6)
                    a = m._lookup(rng.choice(ids), m.ch).bi if (sp == 0 and rng.random() < 0.7) else rng.randrange(size)
                    a = min(a, size - cnt)
                ops.write(f"X D {sp} {a} {cnt}\n")
            elif k == "W":
                used = {m._lookup(i, 0).bi for i in ids} | {m._lookup(q["id"], 0).bi for q in m.queue}
                free = [a for a in range(cfg.NB) if a not in used]
                if free:
                    a, v = rng.choice(free), rng.randrange(16)
                    m.raw_write(0, a, v)
                    ops.write(f"X W {a} {v}\n")
            else:
                ops.write(f"X {k}\n")
            continue
        if r < 0.00295 and clear_ok:
            ops.write("W 2 4\n")                         # clear statistics
            m.clear_stats()
            continue
        if r < 0.00298 and clear_ok:
            ops.write("W 2 1\n")                         # soft reset
            m.reset()
            continue
        n += 1
        ident = rng.choice(ids)
        head = m.queue[0]["id"] if m.queue else None
        r = rng.random()
        pw, cw, rw = [(0.36, 0.32, 0.25), (0.62, 0.15, 0.16), (0.03, 0.90, 0.05), (0.36, 0.32, 0.25)][mode]
        fast = mode == 3 or rng.random() < 0.2
        hi = rng.randrange(NID >> 2) << 2
        if r < pw:
            req(bpx1.OP_PREDICT, ident, fast)
        elif r < pw + cw:
            tgt = head if head is not None else ident
            req(bpx1.OP_PREDICT_RESOLVE_T if outcome(tgt, hist) else bpx1.OP_PREDICT_RESOLVE_NT, ident, fast)
        elif r < pw + cw + rw:
            tk = outcome(head if head is not None else ident, hist)
            req(bpx1.OP_SPECIAL, (bpx1.SUB_RESOLVE_T if tk else bpx1.SUB_RESOLVE_NT) | hi, fast)
        elif r < pw + cw + rw + 0.6 * (1 - pw - cw - rw):
            req(bpx1.OP_SPECIAL, bpx1.SUB_FLUSH | hi, fast)
        else:
            req(bpx1.OP_SPECIAL, bpx1.SUB_NOP | hi, fast)
    if conc:
        ops.write("J\n")
        drain()
        dump()
        ops.write("X S\nJ\n")
    else:
        stat_line("S")
        drain()
        dump()
        stat_line("S")
    print(f"{n} operations, seed {seed}: {m.stats()}")


if __name__ == "__main__":
    main()
