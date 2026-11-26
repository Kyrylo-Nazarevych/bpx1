# BPX-1 against the golden model model/bpx1.py (spec VR-01, VR-04), through the real pins, in RTL and at gate level:
#   trace_replay              a branch trace replayed one record per request (predict + resolve of the same branch)
#   random_ops_against_model  random operations with random host timing
# Every reply is compared; then the statistics and every state bit are read back through the control port and
# compared.  OPS=<n> SEED=<n> select the length and the seed; the gate-level run (GATES=yes) uses a shorter default.
import os
import random
import sys

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles, FallingEdge, RisingEdge, Timer

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "model"))
import bpx1  # noqa: E402

CLK_NS = 20
GATES = os.environ.get("GATES", "") == "yes"
OPS = int(os.environ.get("OPS", "700" if GATES else "3000"))
SEED = int(os.environ.get("SEED", "1"))


class Host:
    """Bit-level host: the two-phase predict port and the SPI control port, with random timing."""

    def __init__(self, dut, rng):
        self.dut, self.rng = dut, rng
        self.ui, self.uio = 0, 0x10          # CSn high
        self.req = 0
        self._drive()

    def _drive(self):
        self.dut.ui_in.value = self.ui
        self.dut.uio_in.value = self.uio | (self.req << 7)

    def ack(self):
        return int(self.dut.uo_out.value) & 1

    async def request(self, op, ident):
        await Timer(self.rng.randint(100, 3000 * CLK_NS), unit="ps")           # anywhere within three clock periods
        self.ui = ident & 0xFF
        self.uio = (self.uio & 0x70) | ((op & 3) << 2)
        self._drive()
        if self.rng.random() < 0.5:
            await Timer(self.rng.randint(100, 1000 * CLK_NS), unit="ps")
        a = self.ack()
        self.req ^= 1
        self._drive()
        n = 0
        while self.ack() == a:
            await RisingEdge(self.dut.clk)
            await Timer(1, unit="ns")
            n += 1
            assert n < 64, "no acknowledge within 64 clocks"
        v = int(self.dut.uo_out.value)
        u = int(self.dut.uio_out.value)
        self.pins = [(v >> 5) & 1, u & 1, (u >> 1) & 1, (v >> 6) & 1]       # DBG0, PBIM, PGSH, DBG1
        return (v >> 1) & 1, (v >> 2) & 1, n

    async def _spi_bits(self, value, nbits):
        got = 0
        for i in reversed(range(nbits)):
            self.uio = (self.uio & ~0x40) | (((value >> i) & 1) << 6)
            self._drive()
            await ClockCycles(self.dut.clk, 5)
            self.uio |= 0x20
            self._drive()
            got = (got << 1) | ((int(self.dut.uo_out.value) >> 7) & 1)
            await ClockCycles(self.dut.clk, 5)
            self.uio &= ~0x20
            self._drive()
        return got

    async def spi(self, addr, words=None, n=1):
        """Write the list `words` starting at addr, or read n words."""
        await FallingEdge(self.dut.clk)
        self.uio &= ~0x10
        self._drive()
        await ClockCycles(self.dut.clk, 4)
        await self._spi_bits((0x80 if words is not None else 0) | (addr & 0x3F), 8)
        out = []
        if words is not None:
            for w in words:
                await self._spi_bits(w & 0xFFFF, 16)
        else:
            for _ in range(n):
                out.append(await self._spi_bits(0, 16))
        await ClockCycles(self.dut.clk, 4)
        self.uio |= 0x10
        self._drive()
        await ClockCycles(self.dut.clk, 4)
        return out

    async def dump(self, space, count):
        """Read `count` consecutive raw entries of a table or of the small-register space."""
        await self.spi(0x0F, [0x8000 | (space << 8)])
        await ClockCycles(self.dut.clk, 8)
        out = []
        for _ in range(count):
            out += await self.spi(0x10, n=1)
        return out


async def start(dut):
    cocotb.start_soon(Clock(dut.clk, CLK_NS, unit="ns").start())
    dut.ena.value = 1
    dut.ui_in.value = 0
    dut.uio_in.value = 0x10
    dut.rst_n.value = 0
    await ClockCycles(dut.clk, 8)
    dut.rst_n.value = 1
    for _ in range(40):
        await RisingEdge(dut.clk)
        if (int(dut.uo_out.value) >> 4) & 1:
            return
    assert False, "READY did not rise"


def outcome(ident, hist):
    """Synthetic branch behaviour: biased, loop-like and history-correlated branches by identifier."""
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


async def compare_state(h, m):
    c = m.cfg
    st = m.state()
    bc = await h.dump(0, c.NB)
    assert [x & 3 for x in bc] == st["bim"], "bimodal table differs"
    assert [(x >> 2) & 3 for x in bc] == st["cho"], "chooser table differs"
    g = await h.dump(1, c.NG)
    assert [x & 3 for x in g] == st["gsh"], "gshare table differs"
    for i, (e, _) in enumerate(c.weights):
        t = await h.dump(8 + i, e)
        exp = [m.raw(8 + i, k) for k in range(e)]
        assert t == exp, f"weight table {i} differs: first mismatch at {next(k for k in range(e) if t[k] != exp[k])}"
    r = await h.dump(7, 9)
    ch, sh, misc = r[0] | (r[1] << 12), r[2] | (r[3] << 12), r[4]
    assert r[5:9] == [m.raw(7, a) for a in range(5, 9)], f"path histories {r[5:9]} differ from the model"
    assert ch == st["ch"], f"committed history {ch:x} != {st['ch']:x}"
    assert sh == st["sh"], f"speculative history {sh:x} != {st['sh']:x}"
    assert misc == m.raw(7, 4), f"threshold and adaptation counter {misc:x} != {m.raw(7, 4):x}"
    # mosaic: the leaky twin (reads 0 while TWIN_EN = 0, also at gate level where its nodes are unknown), FCTL, FSTAT
    n3 = c.weights[-1][0]
    tw = await h.dump(bpx1.SPACE_TWIN, n3)
    assert tw == [m.raw(bpx1.SPACE_TWIN, k) for k in range(n3)], f"twin (space 12) {tw} differs from the model"
    f = await h.spi(0x11, n=2)
    assert f == [m.fctl, m.fstat], f"FCTL, FSTAT {f} != model {[m.fctl, m.fstat]}"


async def compare_stats(h, m):
    await h.spi(0x02, [0x0008])                      # snapshot
    await ClockCycles(h.dut.clk, 8)
    w = await h.spi(0x03, n=12)
    s = m.stats()
    got = [w[2 + 2 * k] | (w[3 + 2 * k] << 16) for k in range(5)]
    exp = [s["resolved"], s["mis_default"], s["mis_bimodal"], s["mis_gshare"], s["mis_tournament"]]
    assert got == exp, f"statistics {got} != {exp}"
    assert w[1] & 0xFF == s["errors"], f"error counter {w[1] & 0xFF} != {s['errors']}"
    assert (w[0] >> 8) & 3 == s["err_flags"] & 3, "error flags differ"
    assert w[0] & 7 == len(m.queue), "queue count differs"


@cocotb.test()
async def random_ops_against_model(dut):
    rng = random.Random(SEED)
    await start(dut)
    h = Host(dut, rng)
    m = bpx1.Bpx1()
    assert (await h.spi(0x00, n=1))[0] == 0xB101
    hist, lat = {}, {}
    NID = 1 << m.cfg.W
    ids = [rng.randrange(NID) for _ in range(40)]
    for n in range(OPS):
        r = rng.random()
        ident = rng.choice(ids)
        head = m.queue[0]["id"] if m.queue else rng.choice(ids)
        if r < 0.36:
            op, arg = bpx1.OP_PREDICT, ident
        elif r < 0.68:
            # the resolve of a combined request applies to the oldest entry after the predict (or before, if full)
            tgt = head if m.queue else ident
            op, arg = (bpx1.OP_PREDICT_RESOLVE_T if outcome(tgt, hist) else bpx1.OP_PREDICT_RESOLVE_NT), ident
        elif r < 0.93:
            op, arg = bpx1.OP_SPECIAL, (bpx1.SUB_RESOLVE_T if outcome(head, hist) else bpx1.SUB_RESOLVE_NT) | (rng.randrange(NID >> 2) << 2)
        elif r < 0.96:
            op, arg = bpx1.OP_SPECIAL, bpx1.SUB_FLUSH | (rng.randrange(NID >> 2) << 2)
        elif r < 0.98:
            op, arg = bpx1.OP_SPECIAL, bpx1.SUB_NOP | (rng.randrange(NID >> 2) << 2)
        else:
            m.drive = rng.choice([0, 0, 1, 2, 3, 4])
            m.frozen = rng.random() < 0.2
            m.feat = rng.choice([m.cfg.FEAT] * 4 + list(range(8)))
            # CTRL[15] (removed binarised inference) is set now and then: the chip must ignore it
            await h.spi(0x01, [m.ctrl_word() | (0x8000 if rng.random() < 0.3 else 0)])
            continue
        exp = m.request(op, arg)
        pred, conf, cyc = await h.request(op, arg)
        lat[cyc] = lat.get(cyc, 0) + 1
        if op != bpx1.OP_SPECIAL:
            assert (pred, conf) == (exp.pred, exp.conf), f"op {n}: op={op} id={arg:#x} got pred={pred} conf={conf}, model pred={exp.pred} conf={exp.conf} refused={exp.refused}"
            if not exp.refused & bpx1.ERR_PREDICT_FULL:      # the four predictions on the pins
                d0, d1 = m.dbg_pins()
                assert h.pins == [d0, m.last_preds[1], m.last_preds[2], d1], f"op {n}: prediction pins {h.pins}, model {m.last_preds}"
        if n % 1000 == 999:
            await compare_stats(h, m)
    await compare_stats(h, m)
    while m.queue:                                   # resolve what is left so the tables are quiescent
        tk = outcome(m.queue[0]["id"], hist)
        m.resolve(tk)
        await h.request(bpx1.OP_SPECIAL, bpx1.SUB_RESOLVE_T if tk else bpx1.SUB_RESOLVE_NT)
    await ClockCycles(dut.clk, 8)
    await compare_state(h, m)
    await compare_stats(h, m)
    dut._log.info(f"{OPS} operations, seed {SEED}: replies, statistics and state equal the model; acknowledge latency (clocks: count) {dict(sorted(lat.items()))}; {m.stats()}")


@cocotb.test()
async def raw_access_write(dut):
    """Raw writes through the control port (FR-10): whole entries, single fields, neighbours untouched, and the
    written values are what the predictors then use."""
    await start(dut)
    h = Host(dut, random.Random(SEED + 2000))

    async def wr(space, addr, mask, val):
        await h.spi(0x0F, [(mask << 12) | (space << 8) | addr])
        await ClockCycles(dut.clk, 8)
        await h.spi(0x10, [val])
        await ClockCycles(dut.clk, 12)

    async def rd(space, addr):
        await h.spi(0x0F, [(space << 8) | addr])
        await ClockCycles(dut.clk, 8)
        return (await h.spi(0x10, n=1))[0]

    assert await rd(0, 5) == 0x5, "bimodal/chooser entry is not at its initial value"
    await wr(0, 5, 3, 0xA)
    assert await rd(0, 5) == 0xA, "both fields written"
    await wr(0, 5, 1, 0x7)
    assert await rd(0, 5) == 0xB, "field 0 only (bimodal counter)"
    await wr(0, 5, 2, 0x4)
    assert await rd(0, 5) == 0x7, "field 1 only (chooser)"
    assert await rd(0, 4) == 0x5 and await rd(0, 6) == 0x5, "neighbours changed"
    await wr(1, 100, 1, 0x2)
    assert await rd(1, 100) == 0x2 and await rd(1, 99) == 0x1 and await rd(1, 101) == 0x1, "gshare entry"
    m0 = bpx1.Bpx1()
    wm = (1 << m0.cfg.CB) - 1
    for i in range(len(m0.cfg.weights)):
        t = 8 + i
        await wr(t, 9, 1, 0xABC & wm)
        assert await rd(t, 9) == 0xABC & wm, f"weight table {i}: entry"
        await wr(t, 9, 1, 0x155 & wm)
        assert await rd(t, 9) == 0x155 & wm, f"weight table {i}: entry rewritten"
        await wr(t, 9, 2, 0x000)
        assert await rd(t, 9) == 0x155 & wm, f"weight table {i}: a write with the field disabled changed the entry"
        assert await rd(t, 8) == m0.raw(t, 8) and await rd(t, 10) == m0.raw(t, 10), f"weight table {i}: neighbours changed"
    await wr(7, 4, 0, 0x2A3)
    assert await rd(7, 4) == 0x2A3, "threshold and adaptation counter"
    assert (await h.spi(0x01, n=1))[0] == m0.ctrl_word() == 0x7DA0, "CTRL after reset"
    await h.spi(0x01, [0xFDA0])
    assert (await h.spi(0x01, n=1))[0] == 0x7DA0, "CTRL[15] must read 0 and ignore writes"
    # the bimodal predictor uses the written counter: entry 5 is now strongly taken, entry 4 still weakly not-taken
    await h.spi(0x01, [0x0DA1])
    pred, _, _ = await h.request(bpx1.OP_PREDICT, 5)
    assert pred == 1, "written bimodal counter not used"
    pred, _, _ = await h.request(bpx1.OP_PREDICT, 4)
    assert pred == 0
    dut._log.info("raw access: entries, single fields and small registers written and read back")


@cocotb.test()
async def trace_replay(dut):
    """One request per trace record, as the demo board replays a recorded trace (U2)."""
    rng = random.Random(SEED + 1000)
    await start(dut)
    h = Host(dut, rng)
    m = bpx1.Bpx1()
    hist = {}
    # a small synthetic program: a loop nest with a data-dependent branch and a rarely taken exit
    trace = []
    for outer in range(OPS // 12 + 1):
        for inner in range(5):
            trace.append((0x040, 1 if inner < 4 else 0))
            trace.append((0x123, outcome(0x123, hist)))
        trace.append((0x2A7, (outer * 7 + 3) % 4 == 0))
        trace.append((0x3F1, 1 if outer % 9 else 0))
    wrong = 0
    for n, (ident, taken) in enumerate(trace[:OPS]):
        taken = 1 if taken else 0
        ident &= (1 << m.cfg.W) - 1
        exp = m.predict_resolve(ident, taken)
        pred, conf, _ = await h.request(bpx1.OP_PREDICT_RESOLVE_T if taken else bpx1.OP_PREDICT_RESOLVE_NT, ident)
        assert (pred, conf) == (exp.pred, exp.conf), f"record {n}: id={ident:#x} got pred={pred} conf={conf}, model pred={exp.pred} conf={exp.conf}"
        wrong += pred != taken
    await ClockCycles(dut.clk, 8)
    await compare_stats(h, m)
    await compare_state(h, m)
    dut._log.info(f"trace replay: {min(OPS, len(trace))} records, default predictor mispredicted {wrong}; statistics and state equal the model")


# ---------------------------------------------------------------------------------------------------- mosaic
async def raw_wr(h, m, space, addr, val):
    await h.spi(0x0F, [0x3000 | (space << 8) | addr])
    await ClockCycles(h.dut.clk, 8)
    await h.spi(0x10, [val])
    await ClockCycles(h.dut.clk, 12)
    m.raw_write(space, addr, val)


async def raw_rd(h, space, addr):
    await h.spi(0x0F, [(space << 8) | addr])
    await ClockCycles(h.dut.clk, 8)
    return (await h.spi(0x10, n=1))[0]


async def set_fctl(h, m, v):
    await h.spi(0x11, [v])
    m.write_fctl(v)


async def checked_request(h, m, op, ident, what):
    exp = m.request(op, ident)
    pred, conf, _ = await h.request(op, ident)
    if op != bpx1.OP_SPECIAL:
        assert (pred, conf) == (exp.pred, exp.conf), f"{what}: pred/conf {(pred, conf)}, model {(exp.pred, exp.conf)}"
        if not exp.refused & bpx1.ERR_PREDICT_FULL:
            d0, d1 = m.dbg_pins()
            assert h.pins == [d0, m.last_preds[1], m.last_preds[2], d1], f"{what}: pins {h.pins}, model {[d0, m.last_preds[1], m.last_preds[2], d1]}"


@cocotb.test()
async def mosaic_registers(dut):
    """FCTL and FSTAT (docs/mosaic.md 7.2), also at gate level: reset values, FCTL storage, soft reset, and with the
    twin's read data kept off (FCTL[0] = 0) the chip behaves plainly whatever the other bits say."""
    await start(dut)
    h = Host(dut, random.Random(SEED + 3000))
    m = bpx1.Bpx1()
    assert await h.spi(0x11, n=2) == [0, 0], "FCTL, FSTAT after reset"
    for v in (0xBEEE, 0x4000, 0xFFFE, 0x0000, 0x5552):
        await h.spi(0x11, [v])
        assert (await h.spi(0x11, n=1))[0] == v, f"FCTL does not hold {v:#06x}"
    await h.spi(0x02, [0x0001])                      # soft reset clears FCTL like CTRL
    await ClockCycles(dut.clk, 30)
    assert await h.spi(0x11, n=2) == [0, 0], "FCTL, FSTAT after soft reset"
    # everything but TWIN_EN set: TWIN_USE needs TWIN_EN, so the predictions are plain; DBG shows the fault events
    # (event 10: the default prediction with W3 = the plain default prediction)
    await set_fctl(h, m, 0xFFFE)
    rng = random.Random(SEED + 3001)
    hist = {}
    ids = [rng.randrange(256) for _ in range(12)]
    for n in range(120 if GATES else 300):
        ident = rng.choice(ids)
        if rng.random() < 0.5:
            await checked_request(h, m, bpx1.OP_PREDICT_RESOLVE_T if outcome(ident, hist) else bpx1.OP_PREDICT_RESOLVE_NT, ident, f"op {n}")
        elif m.queue and rng.random() < 0.6:
            tk = outcome(m.queue[0]["id"], hist)
            await checked_request(h, m, bpx1.OP_SPECIAL, bpx1.SUB_RESOLVE_T if tk else bpx1.SUB_RESOLVE_NT, f"op {n}")
        else:
            await checked_request(h, m, bpx1.OP_PREDICT, ident, f"op {n}")
    while m.queue:
        tk = outcome(m.queue[0]["id"], hist)
        await checked_request(h, m, bpx1.OP_SPECIAL, bpx1.SUB_RESOLVE_T if tk else bpx1.SUB_RESOLVE_NT, "drain")
    await ClockCycles(dut.clk, 8)
    await compare_state(h, m)                        # includes the twin, read as 0, and FSTAT = 0
    await compare_stats(h, m)
    dut._log.info("mosaic registers: FCTL storage and resets, FSTAT 0, plain behaviour with the twin's data off")


@cocotb.test(skip=GATES)
async def twin_directed(dut):
    """The leaky twin W3L (docs/mosaic.md 7.3) in RTL simulation, where its cells never leak: faults injected by raw
    writes into the twin (space 12) and into W3 (space 11); every decode outcome of every CODE, with and without
    TWIN_USE; FSTAT, the fault events on DBG, restore-on-use; all against the golden model."""
    await start(dut)
    h = Host(dut, random.Random(SEED + 4000))
    m = bpx1.Bpx1()
    c = m.cfg
    assert await raw_rd(h, bpx1.SPACE_TWIN, 3) == 0, "twin must read 0 while TWIN_EN = 0"
    await set_fctl(h, m, bpx1.FCTL_TWIN_EN)
    assert await raw_rd(h, bpx1.SPACE_TWIN, 3) == 0xFF, "twin entry after reset: the code word of -1"
    ids = [0x15, 0x2A, 0x3C, 0x47, 0x5E, 0x63, 0x79, 0x8B, 0x91, 0xA4, 0xB2, 0xCD]
    flips = [0x00, 0x01, 0x04, 0x10, 0x80, 0x22, 0x30, 0x03, 0x07, 0x51, None]     # None: W3 changed, twin not
    seen = set()
    dsel_pairs = [(0, 1), (2, 3), (10, 2), (1, 3)]
    n = 0
    for use in (0, 1):
        for code in range(4):
            for fl in flips:
                ident = ids[n % len(ids)]
                d0, d1 = dsel_pairs[n % len(dsel_pairs)]
                n += 1
                m.dsel = [d0, d1]
                await h.spi(0x01, [m.ctrl_word()])
                await set_fctl(h, m, bpx1.FCTL_TWIN_EN | (use << 1) | (code << 2) | bpx1.FCTL_DBGF)
                await h.spi(0x02, [0x0004])          # clear FSTAT (and the statistics)
                m.clear_stats()
                a = m._lookup(ident, m.sh, m.psh).idx[-1]
                w3 = m.raw(11, a)
                await raw_wr(h, m, bpx1.SPACE_TWIN, a, bpx1.tw_encode(w3) ^ (fl or 0))
                if fl is None:
                    await raw_wr(h, m, 11, a, w3 ^ 0x5)
                await checked_request(h, m, bpx1.OP_PREDICT, ident, f"code {code} use {use} flips {fl}")
                assert m.last_tw is not None
                seen.add((code, m.last_tw["outcome"], use))
                assert (await h.spi(0x12, n=1))[0] == m.fstat, f"FSTAT, code {code} flips {fl}: model {m.fstat:#x}"
                await checked_request(h, m, bpx1.OP_SPECIAL, bpx1.SUB_FLUSH, "flush")
    want = {(cd, o, u) for cd in range(4) for o in {0: (0, 5, 6), 1: (0, 3, 5, 6), 2: (0, 1, 2, 3, 4, 5), 3: (0, 3, 5)}[cd] for u in (0, 1)}
    assert want <= seen, f"decode outcomes not reached: {sorted(want - seen)}"

    # restore-on-use: make a branch confidently right (weights at their limits), flip one bit of its twin word,
    # resolve it without a learning step: the entry is rewritten corrected (SEC-DED) or as read (parity)
    await h.spi(0x01, [0x0DA0 | (m.feat << 12)])
    m.dsel = [10, 13]
    rest = []
    for code, fl in ((2, 0x02), (2, 0x40), (1, 0x08), (0, 0x01)):
        await set_fctl(h, m, bpx1.FCTL_TWIN_EN | (code << 2) | bpx1.FCTL_RESTORE)
        ident = 0x33
        L = m._lookup(ident, m.sh, m.psh)
        for i in range(len(c.weights)):
            await raw_wr(h, m, 8 + i, L.idx[i], 0x8 if L.neg[i] else 0x7)
        a = L.idx[-1]
        await raw_wr(h, m, bpx1.SPACE_TWIN, a, bpx1.tw_encode(m.wt[-1][a]) ^ fl)
        tk = 1 if m._lookup(ident, m.sh, m.psh).p_def else 0
        await checked_request(h, m, bpx1.OP_PREDICT_RESOLVE_T if tk else bpx1.OP_PREDICT_RESOLVE_NT, ident, f"restore code {code}")
        rest.append(m.last_train["restore"])
        await ClockCycles(dut.clk, 8)
        assert await raw_rd(h, bpx1.SPACE_TWIN, a) == m.tw[a], f"restore code {code}: twin entry differs from the model"
    assert rest == [1, 1, 2, 2], f"restore kinds {rest}"
    await ClockCycles(dut.clk, 8)
    await compare_state(h, m)
    dut._log.info(f"twin: {len(seen)} (code, outcome, TWIN_USE) combinations, restore-on-use {rest}; state equals the model")
