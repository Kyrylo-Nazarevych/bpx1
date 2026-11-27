#!/usr/bin/env python3
"""Constrained-random operations with expected results from the golden model (spec VR-01, VR-02).

  randgen.py <operations> <seed> <out.ops> <out.meta> [conc]

out.ops is played against the RTL by `cosim --rand` (cosim/cosim.cpp), one line per step:
  R op id expected gap setup     a request; expected = PRED | CONF<<1 [| the four prediction pins <<2 | 64 for an
                                 accepted predict [| DBG0, DBG1 one clock after the ACK: check mask <<7, values
                                 <<9]]; gap = idle clocks before it; setup = clocks ID/OP are placed before REQ
                                 toggles
  W addr value                   a control-port write (CTRL, CMD, FCTL, or RAW_ADDR and RAW_DATA for a raw write)
  S / C  resolved m0 m1 m2 m3 errors flags queue     snapshot (S) or snapshot-and-clear (C), then compare
  D space count w...             read `count` raw words of a state space and compare (FR-10)
  G addr value                   read one register (FCTL, FSTAT) and compare
  Z s                            a pin reset; s bit 0: OP = 3 (else OP = s >> 3), bit 1: CSn = 0, bit 2: MOSI = 1 while
                                 reset ends (all three: the reset strap, the tables are kept; docs/mosaic.md 7.7)
Mosaic (docs/mosaic.md 7.2, 7.3): FCTL changes in mid-stream (twin on/off, TWIN_USE, CODE, RESTORE, the fault-event
bank on DBG with random event selections, random storage bits), fault injection by raw writes of single, double
and triple bit flips into twin words (space 12) and of new weights into W3 (space 11), FSTAT reads, and clears of
FSTAT by CMD bit 2 (not in `conc` runs: it clears the statistics too).  Steps 4 (docs/mosaic.md 7.4-7.6): random
write-pulse settings (no effect here), the canary self-test CTEST and DEMOTE in random FCTL values, and demotion
sequences: the default predictor drives, the twin is on with SEC-DED or detect-all and DEMOTE, then double-bit
errors are injected into the twin entries that coming predicts read (or CTEST is set) until the fourth detected
fault demotes the chip; the tournament then drives; later CMD bit 2 (or, in `conc` runs and in the stretch without
clears, DEMOTE = 0) ends it.  Step 5 (docs/mosaic.md 7.7): GRAND and GSRC in the random FCTL values (the LFSR exact,
the oscillator bit 0), fault events 8 and 9 among the DBG selections, pin resets with the strap or with a wrong
strap condition (not in `conc` runs); CTRL[15] MASK (the twin's code mask) in random CTRL writes and switched in
mid-stream with and without a following clear, and fully decayed twin words (all nodes 0 or 1).  Environment RG_PLAIN=1: none of this (FCTL stays 0; the steps are those of the
generator before the mosaic, plus reads of space 12, FCTL and FSTAT).
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
import os
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
    plain = os.environ.get("RG_PLAIN", "") == "1"
    rng = random.Random(seed)
    ops = open(sys.argv[3], "w")
    meta = open(sys.argv[4], "w")
    m = bpx1.Bpx1()
    cfg = m.cfg
    NID = 1 << cfg.W
    hist = {}
    ids = [rng.randrange(NID) for _ in range(48)]
    spaces = [(0, cfg.NB), (1, cfg.NG)] + [(8 + i, e) for i, (e, _) in enumerate(cfg.weights)]
    N3 = cfg.weights[-1][0]
    fclr = [0]                                           # FSTAT bits cleared since the last request (coverage)
    dev = [0]                                            # demotion events since the last request (coverage):
                                                         # 1 suspended (DEMOTE 1 -> 0 while DEMOTED), 2 resumed
    dem_end = [None]                                     # request count at which a demotion sequence is ended
    mev = [0]                                            # MASK switched since the last request: 1 without, 2 with a clear
    pres = [0]                                           # resets since the last request (coverage): 1 strap (kept),
                                                         # 2/4/8 OP/CSn/MOSI alone wrong, 16 two or three wrong,
                                                         # 32 KEPT cleared by a pin reset, 64 by a soft reset,
                                                         # 128 a strapped reset while KEPT

    def clear_stats():
        fclr[0] |= m.fstat_reg
        ops.write("W 2 4\n")
        m.clear_stats()

    def set_fctl(f):
        """A random FCTL change; the DBG selections follow the fault-event bank (FCTL[14])."""
        if m.dem and (m.fctl ^ f) & bpx1.FCTL_DEMOTE:
            dev[0] |= 2 if f & bpx1.FCTL_DEMOTE else 1
        ops.write(f"W 17 {f}\n")
        m.write_fctl(f)
        if f & bpx1.FCTL_DBGF:
            ev = [0, 1, 2, 3, 4, 7, 8, 9, 9, 10]
            want = [rng.choice(ev) if rng.random() < 0.85 else rng.randrange(16) for _ in range(2)]
        else:
            want = [10, 13]                              # the standard events the model tracks
        if want != m.dsel:
            m.dsel = want
            ops.write(f"W 1 {m.ctrl_word()}\n")

    def new_fctl():
        if rng.random() < 0.15:
            return 0
        # random: GRAND, GSRC [9:8] (the gshare randomness: LFSR exact, oscillator bit 0), the ring bits [12:10] and the
        # write-pulse setting [7:5] (no effect in this model: no ring runs, every pulse writes)
        f = rng.getrandbits(16) & ~(0x401F | bpx1.FCTL_DEMOTE | bpx1.FCTL_CTEST)
        f |= (1 if rng.random() < 0.85 else 0) | (2 if rng.random() < 0.5 else 0) | (rng.randrange(4) << 2)
        f |= (16 if rng.random() < 0.5 else 0) | (bpx1.FCTL_DBGF if rng.random() < 0.4 else 0)
        f |= (bpx1.FCTL_DEMOTE if rng.random() < 0.25 else 0) | (bpx1.FCTL_CTEST if rng.random() < 0.05 else 0)
        return f

    def demote_seq():
        """Demotion: the default predictor drives, the twin is on with SEC-DED or detect-all and DEMOTE set; double
        errors injected into the twin entries that coming predicts read (or CTEST) until DEMOTED; then the
        tournament drives (checked in req()); the sequence is ended some requests later (dem_end)."""
        if m.drive != bpx1.DRV_DEFAULT:
            m.drive = bpx1.DRV_DEFAULT
            ops.write(f"W 1 {m.ctrl_word()}\n")
        canary = rng.random() < 0.3
        f = bpx1.FCTL_TWIN_EN | (rng.choice([2, 3]) << 2) | bpx1.FCTL_DEMOTE | (rng.getrandbits(3) << bpx1.FCTL_PULSE_SHIFT)
        f |= (bpx1.FCTL_DBGF if rng.random() < 0.5 else 0) | (bpx1.FCTL_RESTORE if rng.random() < 0.3 else 0)
        f |= (bpx1.FCTL_TWIN_USE if rng.random() < 0.3 else 0) | (bpx1.FCTL_CTEST if canary else 0)
        set_fctl(f)
        k = 0
        while not m.dem and k < 12:
            ident = rng.choice(ids)
            if not canary:
                a = m._lookup(ident, m.sh, m.psh).idx[-1]
                raw_wr(bpx1.SPACE_TWIN, a, m.tw[a] ^ sum(1 << b for b in rng.sample(range(8), 2)))
            if len(m.queue) < cfg.DEPTH:
                req(bpx1.OP_PREDICT, ident, rng.random() < 0.3)
            else:
                tk = outcome(m.queue[0]["id"], hist)
                req(bpx1.OP_PREDICT_RESOLVE_T if tk else bpx1.OP_PREDICT_RESOLVE_NT, ident, rng.random() < 0.3)
            k += 1
        if canary:
            set_fctl(m.fctl & ~bpx1.FCTL_CTEST)
        return k

    def end_demotion(clear_ok):
        dem_end[0] = None
        if not conc and clear_ok and rng.random() < 0.7:
            clear_stats()                                # CMD bit 2: count and DEMOTED cleared
        else:
            set_fctl(m.fctl & ~bpx1.FCTL_DEMOTE)         # suspended: DEMOTED stays, the default predictor drives

    def pin_reset():
        """A pin reset (cosim step Z) with the strap (OP = 3, CSn = 0, MOSI = 1 at its last clock edge: the tables are
        kept) or with one, two or three of the conditions wrong (an ordinary clearing reset)."""
        k = rng.random()
        s = 7 if k < 0.5 else rng.choice([6, 5, 3]) if k < 0.9 else rng.choice([0, 1, 2, 4])
        ops.write(f"Z {s | (rng.randrange(3) << 3)}\n")   # bits 4:3: the OP value when bit 0 says OP is wrong
        strap = s == 7
        b = 1 if strap else {6: 2, 5: 4, 3: 8}.get(s, 16)
        if m.kept:
            b |= 128 if strap else 32
        pres[0] |= b
        fclr[0] |= m.fstat_reg
        m.reset(strap=strap)
        if rng.random() < 0.5:
            dump()

    def raw_wr(space, a, v):
        ops.write(f"W 15 {0x3000 | (space << 8) | a}\nW 16 {v}\n")
        m.raw_write(space, a, v)

    def inject():
        """A fault by raw write: 1, 2 or 3 flipped bits of a twin word, a fully decayed twin word (all nodes 0 or all
        1), or a new weight in W3 (twin unchanged)."""
        if rng.random() < 0.7:                           # an entry a coming predict is likely to read
            a = m._lookup(rng.choice(ids), m.sh, m.psh).idx[-1]
        else:
            a = rng.randrange(N3)
        k = rng.random()
        if k < 0.12:
            raw_wr(bpx1.SPACE_TWIN, a, rng.choice([0x00, 0xFF]))
        elif k < 0.8:
            nb = rng.choice([1, 1, 1, 2, 2, 3])
            mask = sum(1 << b for b in rng.sample(range(8), nb))
            raw_wr(bpx1.SPACE_TWIN, a, m.tw[a] ^ mask)
        else:
            raw_wr(11, a, rng.randrange(1 << cfg.CB))

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
        # the twin (read as 0 while TWIN_EN = 0; sometimes switched on just for the dump), FCTL and FSTAT
        f0 = m.fctl
        tmp = not plain and not m.twin_en and rng.random() < 0.5
        if tmp:
            ops.write(f"W 17 {f0 | 1}\n")
            m.write_fctl(f0 | 1)
        ops.write(f"D {bpx1.SPACE_TWIN} {N3} " + " ".join(str(m.raw(bpx1.SPACE_TWIN, a)) for a in range(N3)) + "\n")
        if tmp:
            ops.write(f"W 17 {f0}\n")
            m.write_fctl(f0)
        ops.write(f"G 17 {m.fctl}\nG 18 {m.fstat_reg}\n")

    def req(op, arg, fast):
        depth = len(m.queue)
        head = m.queue[0] if m.queue else None
        Lp = m._lookup(arg, m.sh, m.psh) if op != bpx1.OP_SPECIAL else None
        m.last_train = None
        frozen, drive, feat = m.frozen, m.drive, m.feat
        fs0 = m.fstat_reg
        deff = m.demoted and m.drive == bpx1.DRV_DEFAULT             # the demotion applies to this predict
        dno = m.demoted and m.drive != bpx1.DRV_DEFAULT              # demoted, but CTRL selects another predictor
        m.last_tw = None
        m.fault_log = []
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
        pexec = op != bpx1.OP_SPECIAL and not rep.refused & bpx1.ERR_PREDICT_FULL
        dseen, tchg = 0, 0
        if pexec and deff:
            assert rep.pred == m.last_preds[3], "a demoted chip answers with the tournament's prediction"
        if pexec:
            d0, d1 = m.dbg_pins()
            exp |= 64 | (d0 << 2) | (m.last_preds[1] << 3) | (m.last_preds[2] << 4) | (d1 << 5)   # DBG0, PBIM, PGSH, DBG1
            # one clock after the ACK (the canary's event; untracked standard events are not checked)
            for j, v in enumerate(m.dbg_pins(post=True)):
                if v is not None:
                    exp |= (1 << (7 + j)) | (v << (9 + j))
            if m.fctl & bpx1.FCTL_DBGF:
                p0, p1 = m.dbg_pins(post=True)
                for k, v, pv in zip(m.dsel, (d0, d1), (p0, p1)):
                    if v and k in (0, 1, 2, 3, 8, 9, 10):
                        dseen |= 1 << {10: 4, 9: 7, 8: 8}.get(k, k)
                    if (v or pv) and k == bpx1.EV_DEMOTED:
                        dseen |= 1 << 5
                    if pv and k == bpx1.EV_CANARY:
                        dseen |= 1 << 6
            tchg = 1 if m.last_preds[0] != m.last_pdef3 else 0
        ops.write(f"R {op} {arg} {exp} {gap} {setup}\n")
        two = m.last_tw["outcome"] if (pexec and m.last_tw) else -1
        decay = (1 if m.last_tw["raw"] == 0xFF else 0) if (two >= 0 and m.last_tw["raw"] in (0x00, 0xFF)) else -1
        rest = t["restore"] if t is not None else -1
        # demotion and canary (coverage): fault outcomes of this request, bit 0-2 count steps 0->1, 1->2, 2->3, bit 3
        # DEMOTED set, bit 4 fourth fault with DEMOTE = 0 (blocked), bit 5 a fault from the twin, bit 6 from the canary
        fev = 0
        for src, what in m.fault_log:
            fev |= (1 << what) if isinstance(what, int) else 8 if what == "set" else 16 if what == "blocked" else 0
            fev |= 32 if src == "twin" else 64
        dchg = 1 if (pexec and deff and m.last_preds[0] != rep.pred) else 0     # the demotion changed PRED
        meta.write(f"{cls} {depth} {misp} {rep.refused} {drive} {1 if frozen else 0} {rep.pred} {rep.conf} {feat} {lowp} {squash} "
                   f"{rfirst} {gap} {setup} {tr[0]} {tr[1]} {tr[2]} {tr[3]} {tr[4]} "
                   f"{two} {m.code} {1 if m.twin_use else 0} {rest} {dseen} {m.fstat_reg & ~fs0} {fclr[0]} {tchg} "
                   f"{1 if m.last_canary else 0} {1 if (pexec and deff) else 0} {fev} {dev[0]} {dchg} "
                   f"{1 if (pexec and dno) else 0} {1 if (op != bpx1.OP_SPECIAL and rep.refused & bpx1.ERR_PREDICT_FULL) else 0} "
                   f"{t.get('grand', -1) if t is not None else -1} {pres[0]} {1 if m.mask else 0} {decay} {mev[0]}\n")
        fclr[0] = 0
        dev[0] = 0
        pres[0] = 0
        mev[0] = 0

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
        if not plain:                                    # mosaic steps (a separate draw: RG_PLAIN keeps the old stream)
            if dem_end[0] is not None and n >= dem_end[0]:
                end_demotion(clear_ok)
                continue
            r = rng.random()
            if r < 0.0012:
                set_fctl(new_fctl())
                continue
            if r < 0.0052:
                inject()
                continue
            if r < 0.0060:
                ops.write(f"G 18 {m.fstat_reg}\n")
                continue
            if 0.0072 <= r < 0.0078 and dem_end[0] is None:
                n += demote_seq()
                dem_end[0] = n + rng.randint(4, 80)
                continue
            if 0.0078 <= r < 0.0090 and not conc and clear_ok:
                pin_reset()                              # resets the statistics: not in `conc` runs
                continue
            if 0.0090 <= r < 0.0100:
                # MASK (CTRL[15]) switched in mid-stream: words stored under the old setting read as faulty until they
                # are rewritten; half the time the predictors are cleared after it, as an experimenter would
                m.mask = not m.mask
                ops.write(f"W 1 {m.ctrl_word()}\n")
                if rng.random() < 0.5:
                    ops.write("W 2 2\n")
                    m.clear_predictors()
                    mev[0] |= 2
                else:
                    mev[0] |= 1
                continue
            if r < 0.0062:
                ops.write(f"G 17 {m.fctl}\n")
                continue
            if r < 0.0072 and m.queue:
                # staging probe: a flipped bit in the twin entry of the oldest in-flight branch (staged again by the
                # chip under the present FCTL), then a change of TWIN_EN or CODE with RESTORE on before it resolves:
                # restore-on-use must write what a lookup under the new setting reads (the FCTL staging rule)
                a = m._lookup(m.queue[0]["id"], m.ch, m.pch).idx[-1]
                raw_wr(bpx1.SPACE_TWIN, a, m.tw[a] ^ (1 << rng.randrange(8)))
                f = m.fctl | bpx1.FCTL_RESTORE
                if rng.random() < 0.5:                   # or MASK (CTRL[15]), which follows the same staging rule
                    set_fctl((f & ~(3 << 2)) | bpx1.FCTL_TWIN_EN | (bpx1.CODE_SECDED << 2))
                    m.mask = not m.mask
                    mev[0] |= 1
                    ops.write(f"W 1 {m.ctrl_word()}\n")
                    continue
                f ^= rng.choice([bpx1.FCTL_TWIN_EN, 2 << 2, 1 << 2, 3 << 2])
                set_fctl(f | (bpx1.FCTL_TWIN_EN if rng.random() < 0.5 else 0))
                continue
            if r < 0.0066 and not conc and clear_ok:
                clear_stats()                            # CMD bit 2 clears FSTAT (and the statistics)
                continue
        r = rng.random()
        if r < 0.0015:
            m.drive = rng.choice([0, 0, 0, 1, 2, 3, 4])
            m.frozen = rng.random() < 0.15
            m.feat = rng.choice([cfg.FEAT] * 6 + list(range(8)))
            # CTRL[15] (removed binarised inference) is set now and then: the chip must ignore it
            # CTRL[15] is MASK (the twin's code mask); RG_PLAIN keeps it 0 (the draw stays, so the stream is the same)
            mk = rng.random() < 0.3
            if not plain:
                if mk != m.mask:
                    mev[0] |= 1
                m.mask = mk
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
                sp, size = rng.choice(spaces + [(0, cfg.NB), (7, 9)] + ([] if plain else [(bpx1.SPACE_TWIN, N3)]))
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
            clear_stats()                                # clear statistics
            continue
        if r < 0.00298 and clear_ok:
            ops.write("W 2 1\n")                         # soft reset
            fclr[0] |= m.fstat_reg
            if m.kept:
                pres[0] |= 64                            # a soft reset is a reset without the strap: KEPT goes
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
