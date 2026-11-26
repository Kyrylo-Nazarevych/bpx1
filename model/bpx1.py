"""BPX-1 golden model (spec VR-01): the executable specification of docs/SPEC.md, sections 3 and 7.

Transaction level: one call per host request or control action.  No clocks, no timing: by FR-06 nothing the chip
returns may depend on them, so the RTL must match this model bit for bit on any request sequence.

    m = Bpx1()
    r = m.request(OP_PREDICT, ident)            # -> Reply(pred, conf, refused)
    r = m.request(OP_PREDICT_RESOLVE_T, ident)  # predict ident, then resolve the oldest in-flight branch as taken
    m.resolve(taken); m.flush()

The predictor configuration (table sizes, history lengths) is a parameter object so that the exploration can swap
the algorithm late (EX-05); `Config()` is the configuration of the chip.
"""
from dataclasses import dataclass, field
from typing import List, Tuple

OP_SPECIAL, OP_PREDICT, OP_PREDICT_RESOLVE_NT, OP_PREDICT_RESOLVE_T = 0, 1, 2, 3
SUB_NOP, SUB_FLUSH, SUB_RESOLVE_NT, SUB_RESOLVE_T = 0, 1, 2, 3
DRV_DEFAULT, DRV_BIMODAL, DRV_GSHARE, DRV_TOURNAMENT, DRV_NOT_TAKEN = 0, 1, 2, 3, 4
ERR_RESOLVE_EMPTY, ERR_PREDICT_FULL, ERR_WITHDRAWN = 1, 2, 4


def fold(v: int, inbits: int, outbits: int) -> int:
    """XOR of the consecutive outbits-wide slices of the low inbits of v."""
    if outbits <= 0:
        return 0
    v &= (1 << inbits) - 1
    r = 0
    while v:
        r ^= v & ((1 << outbits) - 1)
        v >>= outbits
    return r


@dataclass
class Config:
    W: int = 10                       # identifier bits
    NB: int = 64                      # bimodal entries (2-bit), also the chooser's size
    NG: int = 128                     # gshare entries (2-bit)
    HG: int = 4                       # gshare history bits
    # tagged tables of the default predictor: (entries, tag bits, history length), shortest history first
    tagged: List[Tuple[int, int, int]] = field(default_factory=lambda: [(16, 7, 4), (16, 7, 9), (16, 7, 20)])
    CB: int = 3                       # tagged counter bits
    UB: int = 2                       # tagged useful bits
    UA: int = 4                       # use-alternate counter bits (0 = off)
    base_tournament: bool = True      # default predictor's base: True = the tournament, False = bimodal
    alloc_lfsr: bool = True           # allocation choice: True = 8-bit LFSR among the free tables, False = shortest
    DEPTH: int = 4                    # in-flight capacity
    STATW: int = 16                   # statistics counter width
    ERRW: int = 8                     # error counter width

    @property
    def HL(self) -> int:              # history register length
        return max([self.HG] + [t[2] for t in self.tagged])


@dataclass
class Reply:
    pred: int = 0
    conf: int = 0
    refused: int = 0                  # error bits of this request (0 = accepted)


@dataclass
class _Look:                          # one table lookup for one branch (what the staging register holds)
    bi: int = 0
    gi: int = 0
    idx: List[int] = field(default_factory=list)
    tag: List[int] = field(default_factory=list)
    hit: List[bool] = field(default_factory=list)
    prov: int = -1
    alt: int = -1
    p_bim: int = 0
    p_gsh: int = 0
    p_trn: int = 0
    p_base: int = 0
    p_prov: int = 0
    p_alt: int = 0
    p_def: int = 0
    weak: bool = False
    conf: int = 0


class Bpx1:
    def __init__(self, cfg: Config = None):
        self.cfg = cfg or Config()
        c = self.cfg
        self.lgb = c.NB.bit_length() - 1
        self.lgg = c.NG.bit_length() - 1
        self.lgt = [e.bit_length() - 1 for e, _, _ in c.tagged]
        self.cmax = (1 << c.CB) - 1
        self.half = 1 << (c.CB - 1)
        self.umax = (1 << c.UB) - 1
        self.ua_max = (1 << max(c.UA, 1)) - 1
        self.ua_mid = (self.ua_max + 1) // 2
        self.drive = DRV_DEFAULT
        self.frozen = False
        self.reset()

    # ------------------------------------------------------------------ state
    def clear_predictors(self):
        """FR-09: every predictor table and register back to its initial value.  In-flight work is dropped."""
        c = self.cfg
        self.bim = [1] * c.NB                    # weakly not-taken
        self.cho = [1] * c.NB                    # weakly "use bimodal"
        self.gsh = [1] * c.NG
        self.tt = [[[0, self.half - 1, 0] for _ in range(e)] for e, _, _ in c.tagged]   # [tag, ctr, u]
        self.ua = self.ua_mid
        self.lfsr = 0xE1
        self.ch = 0                              # committed history, bit 0 = most recent outcome
        self.sh = 0                              # speculative history
        self.queue = []                          # in-flight: dicts id, preds(4), pdrv

    def clear_stats(self):
        self.n_res = 0
        self.mis = [0, 0, 0, 0]                  # default, bimodal, gshare, tournament
        self.n_err = 0
        self.err_flags = 0
        self.stat_ovf = False

    def reset(self):
        """Pin reset or soft reset: predictor state, statistics and settings."""
        self.clear_predictors()
        self.clear_stats()
        self.drive = DRV_DEFAULT
        self.frozen = False

    # ------------------------------------------------------------------ lookup (pure function of state)
    def _lookup(self, ident: int, hist: int) -> _Look:
        c = self.cfg
        L = _Look()
        ident &= (1 << c.W) - 1
        L.bi = fold(ident, c.W, self.lgb)
        hg = hist & ((1 << c.HG) - 1)
        L.gi = (fold(ident, c.W, self.lgg) ^ ((hg << (self.lgg - c.HG)) if c.HG <= self.lgg else fold(hg, c.HG, self.lgg))) & (c.NG - 1)
        L.p_bim = self.bim[L.bi] >> 1
        L.p_gsh = self.gsh[L.gi] >> 1
        L.p_trn = L.p_gsh if self.cho[L.bi] >= 2 else L.p_bim
        L.p_base = L.p_trn if c.base_tournament else L.p_bim
        for i, (e, tb, hl) in enumerate(c.tagged):
            lg = self.lgt[i]
            h = hist & ((1 << hl) - 1)
            idx = (fold(ident, c.W, lg) ^ fold(h, hl, lg)) & (e - 1)
            tag = (fold(ident >> lg, max(c.W - lg, 1), tb) ^ fold(h, hl, tb) ^ (fold(h, hl, tb - 1) << 1)) & ((1 << tb) - 1)
            L.idx.append(idx)
            L.tag.append(tag)
            L.hit.append(self.tt[i][idx][0] == tag)
        for i in reversed(range(len(c.tagged))):
            if L.hit[i]:
                if L.prov < 0:
                    L.prov = i
                else:
                    L.alt = i
                    break
        if L.prov < 0:
            L.p_prov = L.p_alt = L.p_def = L.p_base
            base_ctr = self.bim[L.bi] if not (c.base_tournament and self.cho[L.bi] >= 2) else self.gsh[L.gi]
            L.conf = 1 if base_ctr in (0, 3) else 0
        else:
            _, ctr, u = self.tt[L.prov][L.idx[L.prov]]
            L.p_prov = 1 if ctr >= self.half else 0
            L.p_alt = (1 if self.tt[L.alt][L.idx[L.alt]][1] >= self.half else 0) if L.alt >= 0 else L.p_base
            L.weak = ctr in (self.half, self.half - 1) and u == 0
            use_alt = L.weak and c.UA > 0 and self.ua >= self.ua_mid
            L.p_def = L.p_alt if use_alt else L.p_prov
            L.conf = 1 if (ctr in (0, self.cmax) and not use_alt) else 0
        return L

    # ------------------------------------------------------------------ training (resolve)
    @staticmethod
    def _sat(v, up, mx):
        return min(v + 1, mx) if up else max(v - 1, 0)

    def _train(self, ident: int, taken: int):
        c = self.cfg
        L = self._lookup(ident, self.ch)
        nt = len(c.tagged)
        if L.prov >= 0:
            e = self.tt[L.prov][L.idx[L.prov]]
            if L.p_prov != L.p_alt:
                if L.weak and c.UA > 0:
                    self.ua = self._sat(self.ua, L.p_alt == taken, self.ua_max)
                e[2] = self._sat(e[2], L.p_prov == taken, self.umax)
            e[1] = self._sat(e[1], taken, self.cmax)
            if L.weak and L.alt >= 0:
                a = self.tt[L.alt][L.idx[L.alt]]
                a[1] = self._sat(a[1], taken, self.cmax)
        if L.p_def != taken and L.prov < nt - 1:
            cand = [j for j in range(L.prov + 1, nt) if self.tt[j][L.idx[j]][2] == 0]
            if cand:
                pick = 0
                if c.alloc_lfsr:
                    self.lfsr = (self.lfsr >> 1) ^ (0xB8 if self.lfsr & 1 else 0)
                    while pick < len(cand) - 1 and (self.lfsr >> pick) & 1:
                        pick += 1
                j = cand[pick]
                self.tt[j][L.idx[j]] = [L.tag[j], self.half if taken else self.half - 1, 0]
            else:
                for j in range(L.prov + 1, nt):
                    n = self.tt[j][L.idx[j]]
                    if n[2] > 0:
                        n[2] -= 1
        # classic reference predictors, every branch (FR-07)
        if L.p_bim != L.p_gsh:
            self.cho[L.bi] = self._sat(self.cho[L.bi], L.p_gsh == taken, 3)
        self.bim[L.bi] = self._sat(self.bim[L.bi], taken, 3)
        self.gsh[L.gi] = self._sat(self.gsh[L.gi], taken, 3)

    # ------------------------------------------------------------------ operations
    def _hmask(self):
        return (1 << self.cfg.HL) - 1

    def _error(self, bit, rep=None):
        self.err_flags |= bit
        self.n_err = min(self.n_err + 1, (1 << self.cfg.ERRW) - 1)
        if rep is not None:
            rep.refused |= bit

    def _do_predict(self, ident, rep):
        L = self._lookup(ident, self.sh)
        preds = [L.p_def, L.p_bim, L.p_gsh, L.p_trn]
        pdrv = 0 if self.drive == DRV_NOT_TAKEN else preds[self.drive]
        self.queue.append(dict(id=ident & ((1 << self.cfg.W) - 1), preds=preds, pdrv=pdrv))
        self.sh = ((self.sh << 1) | pdrv) & self._hmask()
        rep.pred, rep.conf = pdrv, L.conf

    def _do_resolve(self, taken, rep):
        e = self.queue.pop(0)
        if not self.frozen:                       # freeze stops learning; the history is context, not learning
            self._train(e["id"], taken)
        self.ch = ((self.ch << 1) | taken) & self._hmask()
        mx = (1 << self.cfg.STATW) - 1
        if self.n_res >= mx:
            self.stat_ovf = True
        self.n_res = min(self.n_res + 1, mx)
        for k in range(4):
            if e["preds"][k] != taken:
                self.mis[k] = min(self.mis[k] + 1, mx)
        if e["pdrv"] != taken:                    # the host followed a wrong prediction: repair (FR-04)
            self.queue.clear()
            self.sh = self.ch

    def request(self, op: int, ident: int = 0) -> Reply:
        rep = Reply()
        full = len(self.queue) >= self.cfg.DEPTH
        if op == OP_SPECIAL:
            sub = ident & 3
            if sub == SUB_FLUSH:
                self.queue.clear()
                self.sh = self.ch
            elif sub in (SUB_RESOLVE_NT, SUB_RESOLVE_T):
                if not self.queue:
                    self._error(ERR_RESOLVE_EMPTY, rep)
                else:
                    self._do_resolve(sub & 1, rep)
        elif op == OP_PREDICT:
            if full:
                self._error(ERR_PREDICT_FULL, rep)
            else:
                self._do_predict(ident, rep)
        else:
            taken = op & 1
            if full:                              # resolve first: it frees the slot
                self._do_resolve(taken, rep)
                self._do_predict(ident, rep)
            else:                                 # predict, then resolve the oldest (possibly this very branch)
                self._do_predict(ident, rep)
                self._do_resolve(taken, rep)
        return rep

    # convenience wrappers
    def predict(self, ident):
        return self.request(OP_PREDICT, ident)

    def resolve(self, taken):
        return self.request(OP_SPECIAL, SUB_RESOLVE_T if taken else SUB_RESOLVE_NT)

    def predict_resolve(self, ident, taken):
        return self.request(OP_PREDICT_RESOLVE_T if taken else OP_PREDICT_RESOLVE_NT, ident)

    def flush(self):
        return self.request(OP_SPECIAL, SUB_FLUSH)

    # ------------------------------------------------------------------ observation
    def stats(self):
        return dict(resolved=self.n_res, mis_default=self.mis[0], mis_bimodal=self.mis[1], mis_gshare=self.mis[2],
                    mis_tournament=self.mis[3], errors=self.n_err, err_flags=self.err_flags, overflow=self.stat_ovf)

    def state(self):
        """Every bit of predictor state (FR-10), in a canonical form the testbench compares with the chip."""
        return dict(bim=list(self.bim), cho=list(self.cho), gsh=list(self.gsh), tagged=[[list(e) for e in t] for t in self.tt],
                    ua=self.ua, lfsr=self.lfsr, ch=self.ch, sh=self.sh, queue=[dict(q) for q in self.queue],
                    drive=self.drive, frozen=self.frozen)

    def raw(self, space: int, addr: int) -> int:
        """The word the control port's raw access reads at (space, addr) (docs/SPEC.md section 4)."""
        c = self.cfg
        if space == 0:
            return self.bim[addr] | (self.cho[addr] << 2)
        if space == 1:
            return self.gsh[addr]
        if space == 7:
            return [self.ch & 0xfff, self.ch >> 12, self.sh & 0xfff, self.sh >> 12, self.ua | (self.lfsr << 4)][addr]
        tag, ctr, u = self.tt[space - 2][addr]
        return (u << (c.tagged[space - 2][1] + 3)) | (tag << 3) | ctr


def fold_id(pc: int, W: int = 10) -> int:
    """Host-side fold of a branch address into the identifier (IR-05)."""
    return fold(pc >> 2, 30, W)


if __name__ == "__main__":
    # Self-check against the exploration engine: replay an event trace one branch at a time and print the
    # misprediction counts; `explore/bpsim` must report the same numbers for the same configuration (mis_seq).
    import struct, sys
    path = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else None
    data = open(path, "rb").read()
    n = struct.unpack_from("<I", data, 8)[0]
    m = Bpx1()
    done = 0
    for i in range(n):
        pc, target, info, extra = struct.unpack_from("<IIII", data, 16 + 16 * i)
        if info & 3 == 0:
            m.predict_resolve(fold_id(pc, m.cfg.W), (info >> 2) & 1)
            done += 1
            if limit and done >= limit:
                break
    print(m.stats())
