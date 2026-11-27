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
# switchable parts of the default predictor (CTRL[14:12]); all off = the plain hashed perceptron ("safe mode").
# CTRL[15] (binarised inference until the mosaic round) is MASK since step 5: the leaky twin's code mask (TWMASK).
FEAT_REF, FEAT_SX, FEAT_DYN = 1, 2, 4
FEAT_MASK = 7
# mosaic experiments (register FCTL, docs/mosaic.md 7.2); FCTL = 0 is the plain chip
FCTL_TWIN_EN, FCTL_TWIN_USE, FCTL_CODE_SHIFT, FCTL_RESTORE, FCTL_DBGF = 1, 2, 2, 16, 0x4000
# write-pulse setting of W0-W2 (FCTL[7:5], no effect on the model: a silicon experiment), demotion, canary self-test
FCTL_PULSE_SHIFT, FCTL_DEMOTE, FCTL_CTEST = 5, 0x2000, 0x8000
# step 5 (docs/mosaic.md 7.7): gshare probabilistic hysteresis and its random source (0 the LFSR, 1 the oscillator
# bit), the two ring oscillators' run bits and the divider's selection (no effect on the model: in every simulation
# the rings stand still and the oscillator bit is 0)
FCTL_GRAND, FCTL_GSRC, FCTL_ROA, FCTL_ROB, FCTL_ROSEL = 0x100, 0x200, 0x400, 0x800, 0x1000
LFSR_INIT = 0x55                      # the gshare LFSR (x^7 + x^6 + 1) after reset and clear; bit 6 is the random bit
OSC_BIT = 0                           # the oscillator's random bit as every simulation sees it
# code mask of the leaky twin: with CTRL[15] MASK = 1 the nodes hold code word ^ TWMASK (the RTL parameter TWMASK of bpx_chip; one value
# for RTL, model, tests and harness).  Raw space 12 reads and writes the nodes as they are.
TWMASK = 0xF0
CTRL_MASK = 0x8000
CODE_NONE, CODE_PARITY, CODE_SECDED, CODE_DETECT = 0, 1, 2, 3
# sticky fault flags (register FSTAT) and fault events 0-3 on the DBG pins
TW_ERR, TW_COR, TW_DET, TW_SDC = 1, 2, 4, 8
# FSTAT 4-10 (docs/mosaic.md 7.5, 7.6): canary, the four invariant checkers, watchdog (sticky); DEMOTED (live).
# The checkers and the watchdog never fire without a physical fault (proven), so the model never sets them.
CANARY, CHK_QUEUE, CHK_STAGE, CHK_WSEQ, CHK_HANDSHAKE, WDOG, DEMOTED = 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400
KEPT = 0x800                          # FSTAT[11] (live): the last reset had the strap and kept the tables
# fault events on DBG (FCTL[14]): 0-3 twin flags, 4 canary, 5 any checker, 6 watchdog, 7 DEMOTED, 8 the divided
# oscillator (0 here), 9 the random bit of the last training before the last predict, 10 default with W3
EV_CANARY, EV_CHECKER, EV_WDOG, EV_DEMOTED, EV_DIV, EV_RAND, EV_DEF_W3 = 4, 5, 6, 7, 8, 9, 10
SPACE_TWIN = 12
# decode outcomes of one twin read (coverage and tests): see twin_outcome()
TWO_CLEAN, TWO_COR_DATA, TWO_COR_CHECK, TWO_ABSTAIN, TWO_MISCOR, TWO_SILENT, TWO_MASKED = range(7)
TWO_NAMES = ["clean", "corrected data bit", "corrected check bit", "detected: abstain", "miscorrected (silent)",
             "silent wrong weight", "wrong word, right weight"]


def tw_check(d: int) -> int:
    """Check bits of the twin code: c_k = XOR of the three data bits other than d_k (extended Hamming (8,4))."""
    b = [(d >> k) & 1 for k in range(4)]
    return sum((b[0] ^ b[1] ^ b[2] ^ b[3] ^ b[k]) << k for k in range(4))


def tw_encode(d: int) -> int:
    """Code word {check[3:0], data[3:0]} of a 4-bit weight (two's complement bits)."""
    d &= 15
    return (tw_check(d) << 4) | d


def tw_decode(word: int, code: int):
    """Decode a twin word by CODE: returns (data, abstain, corrected).  0: data as stored; 1: odd parity abstains;
    2 (SEC-DED): odd parity -> correct the single bit the syndrome names (a check-bit error leaves the data), even
    parity with a non-zero syndrome abstains; 3: any non-zero syndrome abstains."""
    d = word & 15
    syn = tw_check(d) ^ (word >> 4)
    par = bin(word & 0xFF).count("1") & 1
    if code == CODE_PARITY:
        return d, par == 1, False
    if code == CODE_SECDED:
        if par:
            for k in range(4):
                if syn == 15 ^ (1 << k):
                    d ^= 1 << k
            return d, False, True
        return d, syn != 0, False
    if code == CODE_DETECT:
        return d, syn != 0, False
    return d, False, False


def tw_store(d: int, mask: bool) -> int:
    """The node values of the twin entry that holds weight d: its code word, ^ TWMASK with MASK."""
    return tw_encode(d) ^ (TWMASK if mask else 0)


def tw_flip_kind(word: int) -> int:
    """Syndrome weight of a word read back: 0 code word, 1 a check bit, 3 a data bit, 2 or 4 even errors."""
    return bin(tw_check(word & 15) ^ (word >> 4)).count("1")


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
    CB: int = 4                  # weight bits (two's complement)
    UBW: int = 2                # the bimodal counter enters the sum as (2c-3) * UBW   (FEAT_REF)
    UGW: int = 4                # the gshare counter enters the sum as (2c-3) * UGW    (FEAT_REF)
    UTW: int = 4                # the tournament prediction enters the sum as +-UTW     (FEAT_REF)
    PL: int = 8                       # path history: one identifier bit of each of the last PL branches (0 = none)
    SK: int = 0                  # table i rotates the upper identifier bits by i * SK in its index hash
    THETA: int = 10           # training threshold after a clear
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
    p_def3: int = 0                   # default prediction with W3 (differs from p_def only with TWIN_USE)
    tw: dict = None                   # the twin's read (TWIN_EN = 1 only): raw, data, abstain, cor, flags, outcome


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
        # leaky twin of the last weight table (W3L): the node values, code word of each entry's weight ^ TWMASK;
        # never leaks here
        self.tw = [self.node_word(self.wt[-1][0])] * c.weights[-1][0]   # under the present MASK
        self._clear_registers()

    def _clear_registers(self):
        """The predictor registers that every reset and every clear set to their initial values (the reset strap
        keeps only the tables)."""
        c = self.cfg
        self.theta = c.THETA
        self.tc = 0
        self.lfsr = LFSR_INIT                    # gshare randomness (docs/mosaic.md 7.7): one step per training
        self.last_rbit = 0                       # the random bit at the last training
        self.pch = 0                             # committed path history
        self.psh = 0                             # speculative path history
        self.ch = 0                              # committed history, bit 0 = most recent outcome
        self.sh = 0                              # speculative history
        self.queue = []                          # in-flight: dicts id, preds(4), pdrv

    def clear_counters(self):
        """The statistics and error counters only (cosim/conc_check.py accumulates totals with this)."""
        self.n_res = 0
        self.mis = [0, 0, 0, 0]                  # default, bimodal, gshare, tournament: counted by the FPGA from the pins
        self.mis_drv = 0                         # the chip's counter: mispredictions of the answer given on PRED
        # default and tournament counted only over the branches whose predict showed them on DBG0/DBG1 (standard
        # events 10 and 13, FCTL[14] DBGF = 0): what a host can count from the pins (cosim/cosim.cpp checks it)
        self.mis_shown = [0, 0]
        self.n_err = 0
        self.err_flags = 0
        self.stat_ovf = False

    def clear_stats(self):
        """CMD bit 2: statistics, error flags, the sticky fault flags, the fault count and DEMOTED."""
        self.clear_counters()
        self.fstat = 0                           # sticky FSTAT bits 0-9
        self.dcnt = 0                            # detected faults, saturating at 3 (docs/mosaic.md 7.6)
        self.dem = False                         # DEMOTED (FSTAT bit 10)

    def reset(self, strap: bool = False):
        """Pin reset or soft reset: predictor state, statistics and settings.  strap = True: a pin reset with the
        reset strap (OP = 3, CSn = 0, MOSI = 1 at its last clock edge; docs/mosaic.md 7.7): the tables (bimodal and
        chooser, gshare, W0-W3, the twin) keep their contents, everything else resets, and KEPT is set until the
        next reset without the strap.  A soft reset (CMD bit 0) is a reset without the strap."""
        self.mask = False                        # CTRL[15] MASK resets first: the reset's clear writes unmasked words
        if strap:
            self._clear_registers()
        else:
            self.clear_predictors()
        self.kept = strap
        self.clear_stats()
        self.drive = DRV_DEFAULT
        self.frozen = False
        self.feat = self.cfg.FEAT
        self.dsel = [10, 13]                     # DBG0, DBG1 event selections (CTRL[7:4], CTRL[11:8])
        self.fctl = 0
        self.last_preds = [0, 0, 0, 0]
        self.last_pdef3 = 0                      # fault event 10
        self.last_ev9 = 0                        # fault event 9: last_rbit as it was at the last executed predict
        self.last_twf = 0                       # twin flags of the last executed predict (fault events 0-3)
        self.last_tw = None                      # the twin's read at the last executed predict (None: TWIN_EN = 0)
        self.last_canary = False                 # the canary fired at the last predict request (fault event 4)
        self.last_dem0 = self.last_dem1 = False  # DEMOTED at the last executed predict: before it, after its TW_DET
        self.fault_log = []                      # what each detected fault did (coverage): see _fault()

    # ------------------------------------------------------------------ mosaic settings
    @property
    def twin_en(self):
        return self.fctl & FCTL_TWIN_EN != 0

    @property
    def twin_use(self):
        return self.fctl & (FCTL_TWIN_EN | FCTL_TWIN_USE) == (FCTL_TWIN_EN | FCTL_TWIN_USE)

    @property
    def code(self):
        return (self.fctl >> FCTL_CODE_SHIFT) & 3

    def write_fctl(self, value: int):
        self.fctl = value & 0xFFFF

    @property
    def tmk(self):
        """The code mask in effect (CTRL[15] MASK): TWMASK or 0."""
        return TWMASK if self.mask else 0

    def node_word(self, w: int) -> int:
        """The twin's node values for weight w under the present MASK (what training and the clear write)."""
        return tw_store(w, self.mask)

    @property
    def fstat_reg(self):
        """FSTAT as the chip reads it: the sticky bits, DEMOTED (bit 10) and KEPT (bit 11)."""
        return self.fstat | (DEMOTED if self.dem else 0) | (KEPT if self.kept else 0)

    @property
    def demoted(self):
        """The demotion is in effect: DEMOTED and DEMOTE (FCTL[13])."""
        return self.dem and self.fctl & FCTL_DEMOTE != 0

    def drive_eff(self):
        """The driving predictor: the tournament replaces the default predictor while demoted."""
        return DRV_TOURNAMENT if self.demoted and self.drive == DRV_DEFAULT else self.drive

    def _fault(self, source: str):
        """A detected fault (TW_DET of a predict, canary, a checker, the watchdog): a 2-bit saturating count; the
        fourth fault with DEMOTE = 1 sets DEMOTED."""
        if self.dcnt == 3:
            if self.fctl & FCTL_DEMOTE:
                self.fault_log.append((source, "set" if not self.dem else "again"))
                self.dem = True
            else:
                self.fault_log.append((source, "blocked"))
        else:
            self.fault_log.append((source, self.dcnt))
            self.dcnt += 1

    def twin_outcome(self, raw: int, w3: int):
        """What the decoder makes of a twin word against W3's weight w3 (4-bit pattern) under the present CODE:
        (data, abstain, corrected, flags, outcome).  raw: the node values (the mask is taken off here)."""
        cw = raw ^ self.tmk
        d, ab, cor = tw_decode(cw, self.code)
        flags = (TW_ERR if cw != tw_encode(w3) else 0) | (TW_COR if cor else 0) | (TW_DET if ab else 0) | \
                (TW_SDC if not ab and d != w3 else 0)
        if cw == tw_encode(w3):
            o = TWO_CLEAN
        elif ab:
            o = TWO_ABSTAIN
        elif cor:
            o = (TWO_COR_DATA if tw_flip_kind(cw) == 3 else TWO_COR_CHECK) if d == w3 else TWO_MISCOR
        elif d != w3:
            o = TWO_SILENT
        else:
            o = TWO_MASKED
        return d, ab, cor, flags, o

    # ------------------------------------------------------------------ lookup (pure function of state)
    def pbit(self, ident: int) -> int:
        return (ident ^ (ident >> 3)) & 1

    def _lookup(self, ident: int, hist: int, phist: int = 0, twin: bool = False) -> _Look:
        """One lookup.  twin = True for a predict: the twin is read (TWIN_EN), and with TWIN_USE its decoded weight
        replaces W3's in the sum (an abstaining twin leaves the term out).  Training lookups use the static tables."""
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
        s = 0
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
        if self.feat & FEAT_REF:
            s += c.UBW * (2 * bim - 3) + c.UGW * (2 * gsh - 3) + (c.UTW if L.p_trn else -c.UTW)
        L.p_def3 = 1 if s >= 0 else 0
        if twin and self.twin_en:
            w3 = self.wt[-1][L.idx[-1]]
            raw = self.tw[L.idx[-1]]
            d, ab, cor, flags, o = self.twin_outcome(raw, w3 & ((1 << c.CB) - 1))
            L.tw = dict(raw=raw, data=d, abstain=ab, cor=cor, flags=flags, outcome=o)
            if self.twin_use:
                wd = d - 16 if d & 8 else d
                t3, td = 2 * w3 + 1, (0 if ab else 2 * wd + 1)
                s += (td - t3) * (-1 if L.neg[-1] else 1)
        L.s = s
        L.p_def = 1 if s >= 0 else 0
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
        tr["restore"] = -1
        i3 = L.idx[-1]
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
            self.tw[i3] = self.node_word(self.wt[-1][i3])  # the twin is written with W3, whatever FCTL says
        elif self.twin_en and self.fctl & FCTL_RESTORE:
            # restore-on-use: rewrite the twin entry with the word read at the commit lookup, corrected if SEC-DED
            # corrected it (odd parity), otherwise as read (the nodes as they were)
            raw = self.tw[i3]
            d, ab, cor = tw_decode(raw ^ self.tmk, self.code)
            new = self.node_word(d) if cor else raw
            tr["restore"] = 0 if raw == self.node_word(self.wt[-1][i3]) else 1 if cor else 2
            self.tw[i3] = new
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
        # classic reference predictors, every branch (FR-07)
        if L.p_bim != L.p_gsh:
            self.cho[L.bi] = self._sat(self.cho[L.bi], L.p_gsh == taken, 3)
        self.bim[L.bi] = self._sat(self.bim[L.bi], taken, 3)
        # gshare probabilistic hysteresis (docs/mosaic.md 7.7): with GRAND a counter at a limit that training would
        # move away from it moves only if the random bit is 1; the bit is the LFSR's bit 6 (GSRC = 0) or the
        # oscillator bit (GSRC = 1, 0 here).  The LFSR steps once per training, whatever FCTL says.
        rb = OSC_BIT if self.fctl & FCTL_GSRC else (self.lfsr >> 6) & 1
        g = self.gsh[L.gi]
        away = (g == 0 and taken) or (g == 3 and not taken)
        if self.fctl & FCTL_GRAND:
            tr["grand"] = (1 + (1 if self.fctl & FCTL_GSRC else 0) * 3 + (1 + rb if away else 0))
        else:
            tr["grand"] = 0
        if not (self.fctl & FCTL_GRAND and away and not rb):
            self.gsh[L.gi] = self._sat(g, taken, 3)
        self.lfsr = ((self.lfsr << 1) | (((self.lfsr >> 6) ^ (self.lfsr >> 5)) & 1)) & 0x7F
        self.last_rbit = rb
        self.last_train = tr

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
        L = self._lookup(ident, self.sh, self.psh, twin=True)
        preds = [L.p_def, L.p_bim, L.p_gsh, L.p_trn]
        drv = self.drive_eff()                     # demotion: the tournament instead of the default predictor
        pdrv = 0 if drv >= DRV_NOT_TAKEN else preds[drv]
        shown = self.dsel == [10, 13] and not self.fctl & FCTL_DBGF     # DBG0/DBG1 show the default and tournament
        self.queue.append(dict(id=ident & ((1 << self.cfg.W) - 1), preds=preds, pdrv=pdrv, shown=shown))
        self.last_preds = preds                    # on the pins: DBG0, PBIM, PGSH, DBG1 after reset
        self.last_pdef3 = L.p_def3
        self.last_ev9 = self.last_rbit             # before the resolve of a combined request (predict first)
        self.last_tw = L.tw
        self.last_twf = L.tw["flags"] if L.tw else 0
        self.fstat |= self.last_twf
        self.last_dem0 = self.dem
        if self.last_twf & TW_DET:                 # a detected (uncorrected) twin error counts towards demotion
            self._fault("twin")
        self.last_dem1 = self.dem
        self.sh = ((self.sh << 1) | pdrv) & self._hmask()
        self.psh = ((self.psh << 1) | self.pbit(ident)) & self._pmask()
        rep.pred = pdrv
        rep.conf = 0 if drv >= DRV_NOT_TAKEN else L.conf[drv]     # CONF qualifies PRED

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
        if e["shown"]:
            for j, k in enumerate((0, 3)):
                if e["preds"][k] != taken:
                    self.mis_shown[j] = min(self.mis_shown[j] + 1, mx)
        if e["pdrv"] != taken:                    # the chip's counter (0x07/0x08): the answer given was wrong
            self.mis_drv = min(self.mis_drv + 1, mx)
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
        self.last_canary = False
        if op != OP_SPECIAL and self.fctl & FCTL_CTEST:
            # canary self-test: the shadow samples of PRED and CONF are inverted, so the canary fires in the clock
            # after every answered predict request (executed or refused)
            self.last_canary = True
            self.fstat |= CANARY
            self._fault("canary")
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

    def ctrl_word(self):
        """The CTRL register for the present settings."""
        return self.drive | (8 if self.frozen else 0) | (self.dsel[0] << 4) | (self.dsel[1] << 8) | ((self.feat & FEAT_MASK) << 12) | \
            (CTRL_MASK if self.mask else 0)

    def write_ctrl(self, value: int):
        """A write of CTRL as the chip takes it (CTRL[15] is MASK: words stored before a change read as faulty until
        they are rewritten; the experimenter clears the predictors after switching it)."""
        self.drive, self.frozen, self.feat = value & 7, bool(value & 8), (value >> 12) & FEAT_MASK
        self.dsel = [(value >> 4) & 15, (value >> 8) & 15]
        self.mask = bool(value & CTRL_MASK)

    def dbg_event(self, k: int, post: bool = False):
        """Value of DBG event k as the pins show it together with the ACK of an accepted predict (post = False) or
        one clock later (post = True).  Fault events (FCTL[14] = 1): 0-3 the twin flags of that predict (pulses with
        the ACK); 4 the canary (a pulse one clock after the ACK); 5 and 6, checker and watchdog, never fire here;
        7 DEMOTED (a level: with the ACK as it was before this predict, one clock later including the predict's own
        TW_DET, not yet its canary); 8 the divided oscillator (0: no ring runs in simulation); 9 the random bit of
        the last training before this predict (a level); 10 its default prediction with W3 (a level); the others 0.
        Standard events: 10-13 the four predictions (levels); None for a standard event the model does not track."""
        if self.fctl & FCTL_DBGF:
            if k < 4:
                return 0 if post else (self.last_twf >> k) & 1
            if k == EV_CANARY:
                return 1 if (post and self.last_canary) else 0
            if k == EV_DEMOTED:
                return 1 if (self.last_dem1 if post else self.last_dem0) else 0
            if k == EV_RAND:
                return self.last_ev9
            return self.last_pdef3 if k == EV_DEF_W3 else 0
        return self.last_preds[k - 10] if 10 <= k <= 13 else None

    def dbg_pins(self, post: bool = False):
        """(DBG0, DBG1) with the ACK of the last accepted predict, or one clock later (post)."""
        return self.dbg_event(self.dsel[0], post), self.dbg_event(self.dsel[1], post)

    # ------------------------------------------------------------------ observation
    def stats(self):
        return dict(resolved=self.n_res, mis_drive=self.mis_drv, mis_default=self.mis[0], mis_bimodal=self.mis[1], mis_gshare=self.mis[2],
                    mis_tournament=self.mis[3], mis_default_shown=self.mis_shown[0], mis_tournament_shown=self.mis_shown[1], errors=self.n_err, err_flags=self.err_flags, overflow=self.stat_ovf)

    def state(self):
        """Every bit of predictor state (FR-10), in a canonical form the testbench compares with the chip."""
        return dict(bim=list(self.bim), cho=list(self.cho), gsh=list(self.gsh), weights=[list(t) for t in self.wt],
                    theta=self.theta, tc=self.tc, ch=self.ch, sh=self.sh, pch=self.pch, psh=self.psh, queue=[dict(q) for q in self.queue],
                    drive=self.drive, frozen=self.frozen, feat=self.feat, twin=list(self.tw), fctl=self.fctl, fstat=self.fstat_reg,
                    dcnt=self.dcnt)

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
        if space == SPACE_TWIN:                   # the twin's 8 nodes (code word ^ TWMASK), read as 0 unless TWIN_EN
            return self.tw[addr] if self.twin_en else 0
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
        elif space == SPACE_TWIN:                 # the nodes as given (no mask), whatever FCTL says; a write of W3
                                                  # (space 11) leaves the twin
            if mask & 1:
                self.tw[addr] = value & 0xFF
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
