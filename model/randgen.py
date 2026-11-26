#!/usr/bin/env python3
"""Constrained-random operations with expected results from the golden model (spec VR-01, VR-02).

  randgen.py <operations> <seed> <out.ops> <out.meta>

out.ops is played against the RTL by `cosim --rand` (cosim/cosim.cpp), one line per step:
  R op id expected gap setup     a request; expected = PRED | CONF<<1; gap = idle clocks before it; setup = clocks
                                 ID/OP are placed before REQ toggles
  W addr value                   a control-port write (CTRL or CMD)
  S / C  resolved m0 m1 m2 m3 errors flags queue     snapshot (S) or snapshot-and-clear (C), then compare
  D space count w...             read `count` raw words of a state space and compare (FR-10)
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


class Cov(bpx1.Bpx1):
    """The golden model, recording what each training step did."""

    def _train(self, ident, taken):
        L = self._lookup(ident, self.ch)
        nt = len(self.cfg.tagged)
        before = [list(self.tt[j][L.idx[j]]) for j in range(nt)]
        lf = self.lfsr
        super()._train(ident, taken)
        alloc, udec = -1, 0
        if L.p_def != taken and L.prov < nt - 1:
            for j in range(L.prov + 1, nt):
                if before[j][2] == 0 and self.tt[j][L.idx[j]] != before[j]:
                    alloc = j
            if alloc < 0 and self.lfsr == lf:
                udec = 1
        self.tr = (L.prov, alloc, udec, 1 if L.weak else 0, 1 if L.alt >= 0 else 0)


def main():
    N, seed = int(sys.argv[1]), int(sys.argv[2])
    rng = random.Random(seed)
    ops = open(sys.argv[3], "w")
    meta = open(sys.argv[4], "w")
    m = Cov()
    hist = {}
    ids = [rng.randrange(1024) for _ in range(48)]

    def stat_line(kind):
        s = m.stats()
        ops.write(f"{kind} {s['resolved']} {s['mis_default']} {s['mis_bimodal']} {s['mis_gshare']} {s['mis_tournament']} "
                  f"{s['errors']} {(s['err_flags'] & 3) | (8 if s['overflow'] else 0)} {len(m.queue)}\n")
        if kind == "C":
            m.n_res, m.mis = 0, [0, 0, 0, 0]

    def dump():
        c, st = m.cfg, m.state()
        ops.write(f"D 0 {c.NB} " + " ".join(str(b | (ch << 2)) for b, ch in zip(st["bim"], st["cho"])) + "\n")
        ops.write(f"D 1 {c.NG} " + " ".join(map(str, st["gsh"])) + "\n")
        for i, (e, tb, _) in enumerate(c.tagged):
            ops.write(f"D {2 + i} {e} " + " ".join(str((u << (tb + 3)) | (tag << 3) | ctr) for tag, ctr, u in st["tagged"][i]) + "\n")
        ops.write(f"D 7 5 {st['ch'] & 0xfff} {st['ch'] >> 12} {st['sh'] & 0xfff} {st['sh'] >> 12} {st['ua'] | (st['lfsr'] << 4)}\n")

    def req(op, arg, fast):
        depth = len(m.queue)
        head = m.queue[0] if m.queue else None
        ua_hi = m.ua >= m.ua_mid
        Lp = m._lookup(arg, m.sh) if op != bpx1.OP_SPECIAL else None
        m.tr = (-2, -1, 0, 0, 0)
        frozen, drive = m.frozen, m.drive
        rep = m.request(op, arg)
        sub = arg & 3
        cls = 0 if op == bpx1.OP_PREDICT else 1 if op != bpx1.OP_SPECIAL else (4 if sub == 0 else 3 if sub == 1 else 2)
        misp, squash, rfirst = -1, -1, 0
        if cls in (1, 2) and not rep.refused:
            tk = (op & 1) if cls == 1 else (sub & 1)
            if cls == 1 and depth < m.cfg.DEPTH and depth == 0:
                pd = rep.pred                      # resolves the branch it has just predicted
            else:
                pd = head["pdrv"]
            misp = 1 if pd != tk else 0
            rfirst = 1 if cls == 1 and depth >= m.cfg.DEPTH else 0
            if misp:
                squash = (depth - 1) if (cls == 2 or rfirst) else depth
                if cls == 1 and depth == 0:
                    squash = 0
        prov = Lp.prov if (Lp is not None and not (rep.refused & bpx1.ERR_PREDICT_FULL)) else -2
        usealt = 1 if (prov >= 0 and Lp.weak and ua_hi) else 0
        if rfirst:
            prov, usealt = -3, 0                   # the lookup ran after the resolve: not reconstructed here
        gap = 0 if fast else rng.choice([0, 0, 0, 1, 1, 2, 3, 5, 9])
        setup = rng.choice([0, 0, 1, 2])
        ops.write(f"R {op} {arg} {rep.pred | (rep.conf << 1)} {gap} {setup}\n")
        meta.write(f"{cls} {depth} {misp} {rep.refused} {drive} {1 if frozen else 0} {rep.pred} {rep.conf} {prov} {usealt} {squash} "
                   f"{m.tr[0]} {m.tr[1]} {m.tr[2]} {rfirst} {gap} {setup} {m.tr[3]} {m.tr[4]}\n")

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
                ids = [rng.randrange(1024) for _ in range(rng.choice([8, 48, 200]))]
        left -= 1
        r = rng.random()
        if r < 0.0015:
            m.drive = rng.choice([0, 0, 0, 1, 2, 3, 4])
            m.frozen = rng.random() < 0.15
            ops.write(f"W 1 {m.drive | (8 if m.frozen else 0)}\n")
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
        if r < 0.0028 and clear_ok:
            ops.write("W 2 4\n")                         # clear statistics
            m.clear_stats()
            continue
        if r < 0.00283 and clear_ok:
            ops.write("W 2 1\n")                         # soft reset
            m.reset()
            continue
        n += 1
        ident = rng.choice(ids)
        head = m.queue[0]["id"] if m.queue else None
        r = rng.random()
        pw, cw, rw = [(0.36, 0.32, 0.25), (0.62, 0.15, 0.16), (0.03, 0.90, 0.05), (0.36, 0.32, 0.25)][mode]
        fast = mode == 3 or rng.random() < 0.2
        if r < pw:
            req(bpx1.OP_PREDICT, ident, fast)
        elif r < pw + cw:
            full = len(m.queue) >= m.cfg.DEPTH
            tgt = head if head is not None else ident
            req(bpx1.OP_PREDICT_RESOLVE_T if outcome(tgt, hist) else bpx1.OP_PREDICT_RESOLVE_NT, ident, fast)
        elif r < pw + cw + rw:
            tk = outcome(head if head is not None else ident, hist)
            req(bpx1.OP_SPECIAL, (bpx1.SUB_RESOLVE_T if tk else bpx1.SUB_RESOLVE_NT) | (rng.randrange(256) << 2), fast)
        elif r < pw + cw + rw + 0.6 * (1 - pw - cw - rw):
            req(bpx1.OP_SPECIAL, bpx1.SUB_FLUSH | (rng.randrange(256) << 2), fast)
        else:
            req(bpx1.OP_SPECIAL, bpx1.SUB_NOP | (rng.randrange(256) << 2), fast)
    stat_line("S")
    drain()
    dump()
    stat_line("S")
    print(f"{n} operations, seed {seed}: {m.stats()}")


if __name__ == "__main__":
    main()
