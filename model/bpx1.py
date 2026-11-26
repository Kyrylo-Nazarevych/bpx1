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
# switchable parts of the default predictor (CTRL[15:12]); all off = the plain hashed perceptron ("safe mode")
# FEAT_BIN (off after reset) is an experiment: only the signs of the weights vote (a majority, as in a binarised
# neural network); the weights still train in full
FEAT_REF, FEAT_SX, FEAT_DYN, FEAT_BIN = 1, 2, 4, 8


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
    W: int = 8                    # identifier bits
    NB: int = 64                      # bimodal entries (2-bit), also the chooser's size
    NG: int = 128                     # gshare entries (2-bit)
    HG: int = 4                       # gshare history bits
    # weight tables of the default predictor (a hashed perceptron): (entries, history length), shortest history first
    weights: List[Tuple[int, int]] = field(default_factory=lambda: [(32, 0), (32, 8), (32, 9), (32, 20)])
    CB: int = 5                  # weight bits (two's complement)
    UBW: int = 2                # the bimodal counter enters the sum as (2c-3) * UBW   (FEAT_REF)
    UGW: int = 4                # the gshare counter enters the sum as (2c-3) * UGW    (FEAT_REF)
    UTW: int = 4                # the tournament prediction enters the sum as +-UTW     (FEAT_REF)
    PL: int = 16                      # path history: one identifier bit of each of the last PL branches (0 = none)
    SK: int = 0                  # table i rotates the upper identifier bits by i * SK in its index hash
    THETA: int = 12           # training threshold after a clear
    THMIN: int = 1
    THMAX: int = 63
    TCB: int = 4                      # bits of the threshold-adaptation counter (FEAT_DYN)
    FEAT: int = 7              # features enabled after reset
    DEPTH: int = 4                    # in-flight capacity
    STATW: int = 16                   # statistics counter width
    ERRW: int = 8                     # error counter width

    @property
    def HL(self) -> int:              # history register length (one more than the longest history: FEAT_SX)
        return max([self.HG] + [h + 1 for _, h in self.weights])


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
    neg: List[int] = field(default_factory=list)      # FEAT_SX: this table's weight counts negated
    s: int = 0                        # the perceptron sum
    p_bim: int = 0
    p_gsh: int = 0
    p_trn: int = 0
    p_def: int = 0
    conf: List[int] = field(default_factory=list)     # confidence of default, bimodal, gshare, tournament


class Bpx1:
    def __init__(self, cfg: Config = None):
        self.cfg = cfg or Config()
        c = self.cfg
        self.lgb = c.NB.bit_length() - 1
        self.lgg = c.NG.bit_length() - 1
        self.lgt = [e.bit_length() - 1 for e, _ in c.weights]
        self.wmax = (1 << (c.CB - 1)) - 1
        self.wmin = -(1 << (c.CB - 1))
        self.tcmax = (1 << (c.TCB - 1)) - 1
        self.tcmin = -(1 << (c.TCB - 1))
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
        nt = len(c.weights)
        # tables start alternately at 0 and -1 (contributions +1 and -1): the sum starts near zero
        self.wt = [[0 if (i & 1) == (nt & 1) else -1] * e for i, (e, _) in enumerate(c.weights)]
        self.theta = c.THETA
        self.tc = 0
        self.pch = 0                             # committed path history
        self.psh = 0                             # speculative path history
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
        self.feat = self.cfg.FEAT

    # ------------------------------------------------------------------ lookup (pure function of state)
    def pbit(self, ident: int) -> int:
        return (ident ^ (ident >> 3)) & 1

    def _lookup(self, ident: int, hist: int, phist: int = 0) -> _Look:
        c = self.cfg
        L = _Look()
        ident &= (1 << c.W) - 1
        L.bi = fold(ident, c.W, self.lgb)
        hg = hist & ((1 << c.HG) - 1)
        L.gi = (fold(ident, c.W, self.lgg) ^ ((hg << (self.lgg - c.HG)) if c.HG <= self.lgg else fold(hg, c.HG, self.lgg))) & (c.NG - 1)
        bim, gsh, cho = self.bim[L.bi], self.gsh[L.gi], self.cho[L.bi]
        L.p_bim = bim >> 1
        L.p_gsh = gsh >> 1
        L.p_trn = L.p_gsh if cho >= 2 else L.p_bim
        s = sb = 0
        for i, (e, hl) in enumerate(c.weights):
            lg = self.lgt[i]
            lo, hi = ident & (e - 1), fold(ident >> lg, max(c.W - lg, 1), lg)
            r = (i * c.SK) % lg
            ih = lo ^ (((hi << r) | (hi >> (lg - r))) & (e - 1))
            h = hist & ((1 << hl) - 1)
            hh = 0 if hl == 0 else (h << (lg - hl)) if hl <= lg else fold(h, hl, lg) ^ (fold(h, hl, lg - 1) << 1)
            if c.PL and hl > 0:                  # path hash, rotated by two against the outcome hash
                n = min(hl, c.PL)
                x = fold(phist & ((1 << n) - 1), n, lg)
                hh ^= ((x << 2) | (x >> (lg - 2))) & (e - 1)
            idx = (ih ^ hh) & (e - 1)
            neg = 1 if (self.feat & FEAT_SX) and hl > 0 and (hist >> hl) & 1 else 0
            L.idx.append(idx)
            L.neg.append(neg)
            w = self.wt[i][idx]
            s += -(2 * w + 1) if neg else 2 * w + 1
            sb += 1 if (w >= 0) != bool(neg) else -1
        if self.feat & FEAT_REF:
            r = c.UBW * (2 * bim - 3) + c.UGW * (2 * gsh - 3) + (c.UTW if L.p_trn else -c.UTW)
            s += r
            sb += r
        L.s = s                                  # the margin (training rule, confidence) is always the full sum
        L.p_def = 1 if (sb if self.feat & FEAT_BIN else s) >= 0 else 0
        L.conf = [1 if abs(s) > self.theta else 0, 1 if bim in (0, 3) else 0, 1 if gsh in (0, 3) else 0,
                  1 if (gsh if cho >= 2 else bim) in (0, 3) else 0]
        return L

    # ------------------------------------------------------------------ training (resolve)
    @staticmethod
    def _sat(v, up, mx):
        return min(v + 1, mx) if up else max(v - 1, 0)

    def _train(self, ident: int, taken: int):
        c = self.cfg
        L = self._lookup(ident, self.ch, self.pch)
        miss = L.p_def != taken
        low = abs(L.s) <= self.theta
        tr = dict(miss=miss, low=low, s=L.s, neg=sum(L.neg), sat_hi=0, sat_lo=0, th=0)
        if miss or low:                           # perceptron rule: learn from a miss or from a low margin
            for i in range(len(c.weights)):
                t = self.wt[i]
                w = t[L.idx[i]]
                if taken != L.neg[i]:
                    tr["sat_hi"] += w == self.wmax
                    t[L.idx[i]] = min(w + 1, self.wmax)
                else:
                    tr["sat_lo"] += w == self.wmin
                    t[L.idx[i]] = max(w - 1, self.wmin)
        if self.feat & FEAT_DYN:                  # adaptive threshold: balance misses against low-margin hits
            if miss:
                if self.tc == self.tcmax:
                    self.tc = 0
                    if self.theta < c.THMAX:
                        self.theta += 1
                        tr["th"] = 1
                else:
                    self.tc += 1
            elif low:
                if self.tc == self.tcmin:
                    self.tc = 0
                    if self.theta > c.THMIN:
                        self.theta -= 1
                        tr["th"] = -1
                else:
                    self.tc -= 1
        self.last_train = tr
        # classic reference predictors, every branch (FR-07)
        if L.p_bim != L.p_gsh:
            self.cho[L.bi] = self._sat(self.cho[L.bi], L.p_gsh == taken, 3)
        self.bim[L.bi] = self._sat(self.bim[L.bi], taken, 3)
        self.gsh[L.gi] = self._sat(self.gsh[L.gi], taken, 3)

    # ------------------------------------------------------------------ operations
    def _hmask(self):
        return (1 << self.cfg.HL) - 1

    def _pmask(self):
        return (1 << self.cfg.PL) - 1

    def _error(self, bit, rep=None):
        self.err_flags |= bit
        self.n_err = min(self.n_err + 1, (1 << self.cfg.ERRW) - 1)
        if rep is not None:
            rep.refused |= bit

    def _do_predict(self, ident, rep):
        L = self._lookup(ident, self.sh, self.psh)
        preds = [L.p_def, L.p_bim, L.p_gsh, L.p_trn]
        pdrv = 0 if self.drive == DRV_NOT_TAKEN else preds[self.drive]
        self.queue.append(dict(id=ident & ((1 << self.cfg.W) - 1), preds=preds, pdrv=pdrv))
        self.last_preds = preds                    # on the pins: DBG0, PBIM, PGSH, DBG1 after reset
        self.sh = ((self.sh << 1) | pdrv) & self._hmask()
        self.psh = ((self.psh << 1) | self.pbit(ident)) & self._pmask()
        rep.pred = pdrv
        rep.conf = 0 if self.drive >= DRV_NOT_TAKEN else L.conf[self.drive]     # CONF qualifies PRED

    def _do_resolve(self, taken, rep):
        e = self.queue.pop(0)
        if not self.frozen:                       # freeze stops learning; the history is context, not learning
            self._train(e["id"], taken)
        self.ch = ((self.ch << 1) | taken) & self._hmask()
        self.pch = ((self.pch << 1) | self.pbit(e["id"])) & self._pmask()
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
            self.psh = self.pch

    def request(self, op: int, ident: int = 0) -> Reply:
        rep = Reply()
        full = len(self.queue) >= self.cfg.DEPTH
        if op == OP_SPECIAL:
            sub = ident & 3
            if sub == SUB_FLUSH:
                self.queue.clear()
                self.sh = self.ch
                self.psh = self.pch
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

    def ctrl_word(self, dbg0=10, dbg1=13):
        """The CTRL register for the present settings (DBG selections as after reset)."""
        return self.drive | (8 if self.frozen else 0) | (dbg0 << 4) | (dbg1 << 8) | (self.feat << 12)

    # ------------------------------------------------------------------ observation
    def stats(self):
        return dict(resolved=self.n_res, mis_default=self.mis[0], mis_bimodal=self.mis[1], mis_gshare=self.mis[2],
                    mis_tournament=self.mis[3], errors=self.n_err, err_flags=self.err_flags, overflow=self.stat_ovf)

    def state(self):
        """Every bit of predictor state (FR-10), in a canonical form the testbench compares with the chip."""
        return dict(bim=list(self.bim), cho=list(self.cho), gsh=list(self.gsh), weights=[list(t) for t in self.wt],
                    theta=self.theta, tc=self.tc, ch=self.ch, sh=self.sh, pch=self.pch, psh=self.psh, queue=[dict(q) for q in self.queue],
                    drive=self.drive, frozen=self.frozen, feat=self.feat)

    def raw(self, space: int, addr: int) -> int:
        """The word the control port's raw access reads at (space, addr) (docs/SPEC.md section 4)."""
        c = self.cfg
        if space == 0:
            return self.bim[addr] | (self.cho[addr] << 2)
        if space == 1:
            return self.gsh[addr]
        if space == 7:
            return [self.ch & 0xfff, self.ch >> 12, self.sh & 0xfff, self.sh >> 12, (self.tc & 0xf) | (self.theta << 4),
                    self.pch & 0xfff, self.pch >> 12, self.psh & 0xfff, self.psh >> 12][addr]
        return self.wt[space - 8][addr] & ((1 << c.CB) - 1)

    def raw_write(self, space: int, addr: int, value: int, mask: int = 3):
        """Raw write (mask bit 0: first field, bit 1: second field of bimodal+chooser; ignored elsewhere)."""
        c = self.cfg
        if space == 0:
            if mask & 1:
                self.bim[addr] = value & 3
            if mask & 2:
                self.cho[addr] = (value >> 2) & 3
        elif space == 1:
            self.gsh[addr] = value & 3
        elif space == 7:
            if addr == 4:
                self.tc = (value & 0xf) - (16 if value & 8 else 0)
                self.theta = (value >> 4) & 0x3f
        else:
            v = value & ((1 << c.CB) - 1)
            self.wt[space - 8][addr] = v - (1 << c.CB if v >> (c.CB - 1) else 0)

def fold_id(pc: int, W: int = 8) -> int:
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
