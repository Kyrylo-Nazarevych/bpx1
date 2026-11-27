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
from cocotb.utils import get_sim_time

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
        # per-predictor mispredictions counted from the prediction pins, as the FPGA does (the chip counts only
        # resolved branches and wrong answers since mosaic step 5): own copy of the in-flight queue, entries
        # [answer, DBG0, PBIM, PGSH, DBG1]; meaningful while DBG has the standard selections (events 10 and 13)
        self.hq, self.hmis, self.hres = [], [0, 0, 0, 0], 0
        self._drive()

    def _h_resolve(self, taken):
        if not self.hq:
            return
        e = self.hq.pop(0)
        self.hres += 1
        for k in range(4):
            self.hmis[k] += e[1 + k] != taken
        if e[0] != taken:
            self.hq.clear()

    def _h_count(self, op, ident, pred):
        if op == bpx1.OP_SPECIAL:
            if ident & 3 == bpx1.SUB_FLUSH:
                self.hq.clear()
            elif ident & 2:
                self._h_resolve(ident & 1)
            return
        e = [pred] + self.pins
        if op == bpx1.OP_PREDICT:
            if len(self.hq) < 4:
                self.hq.append(e)
        elif len(self.hq) >= 4:
            self._h_resolve(op & 1)
            self.hq.append(e)
        else:
            self.hq.append(e)
            self._h_resolve(op & 1)

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
        self._h_count(op, ident, (v >> 1) & 1)
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
    assert f == [m.fctl, m.fstat_reg], f"FCTL, FSTAT {f} != model {[m.fctl, m.fstat_reg]}"


async def compare_stats(h, m, pins=False):
    """pins=True: also the host's per-predictor counts from the pins (only where no clear or reset happened since the
    Host was created and DBG kept the standard selections)."""
    if pins:
        s = m.stats()
        exp = [s["mis_default"], s["mis_bimodal"], s["mis_gshare"], s["mis_tournament"]]
        assert (h.hres, h.hmis) == (s["resolved"], exp), f"pin counts {h.hres} {h.hmis} != model {s['resolved']} {exp}"
    await h.spi(0x02, [0x0008])                      # snapshot
    await ClockCycles(h.dut.clk, 8)
    w = await h.spi(0x03, n=12)
    s = m.stats()
    # the chip counts resolved branches and the mispredictions of the answers it gave; the per-predictor counts are
    # the host's (step 5): 0x09-0x0E read 0
    got = [w[2 + 2 * k] | (w[3 + 2 * k] << 16) for k in range(2)]
    exp = [s["resolved"], s["mis_drive"]]
    assert got == exp, f"statistics {got} != {exp}"
    assert w[6:12] == [0] * 6, f"0x09-0x0E must read 0: {w[6:12]}"
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
            m.mask = rng.random() < 0.3                  # CTRL[15] MASK (the twin is off here: no visible effect)
            await h.spi(0x01, [m.ctrl_word()])
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
            await compare_stats(h, m, pins=True)
    await compare_stats(h, m, pins=True)
    while m.queue:                                   # resolve what is left so the tables are quiescent
        tk = outcome(m.queue[0]["id"], hist)
        m.resolve(tk)
        await h.request(bpx1.OP_SPECIAL, bpx1.SUB_RESOLVE_T if tk else bpx1.SUB_RESOLVE_NT)
    await ClockCycles(dut.clk, 8)
    await compare_state(h, m)
    await compare_stats(h, m, pins=True)
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
    assert (await h.spi(0x01, n=1))[0] == 0xFDA0, "CTRL[15] (MASK) must read back as written"
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
    await compare_stats(h, m, pins=True)
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
            # one clock after the ACK: the canary's event (docs/mosaic.md 7.5) and the levels (RTL only: at gate level
            # the unit cell delays move the clock in which the host sees ACK)
            if not GATES:
                await RisingEdge(h.dut.clk)
                await Timer(1, unit="ns")
                v = int(h.dut.uo_out.value)
                for k, e in enumerate(m.dbg_pins(post=True)):
                    if e is not None:
                        assert (v >> (5 + k)) & 1 == e, f"{what}: DBG{k} one clock after ACK {(v >> (5 + k)) & 1}, model {e}"
    return exp


@cocotb.test()
async def mosaic_registers(dut):
    """FCTL and FSTAT (docs/mosaic.md 7.2), also at gate level: reset values, FCTL storage, soft reset, and with the
    twin's read data kept off (FCTL[0] = 0) the chip behaves plainly whatever the other bits say."""
    await start(dut)
    h = Host(dut, random.Random(SEED + 3000))
    m = bpx1.Bpx1()
    # at gate level the write-pulse setting (FCTL[7:5]) stays 0 (short pulses are a silicon experiment) and no ring
    # oscillator runs (FCTL[11:10]: a zero-delay loop with the PDK's models); in the RTL run against the model the
    # rings stay off too, so that the oscillator bit is 0 as in the model (they run in ring_oscillators)
    pm = (~0x0CE0 & 0xFFFF) if GATES else 0xFFFF
    assert await h.spi(0x11, n=2) == [0, 0], "FCTL, FSTAT after reset"
    for v in (0xBEEE, 0x4000, 0xFFFE, 0x0000, 0x5552):
        v &= pm
        await h.spi(0x11, [v])
        assert (await h.spi(0x11, n=1))[0] == v, f"FCTL does not hold {v:#06x}"
    pm &= ~(bpx1.FCTL_ROA | bpx1.FCTL_ROB)
    await h.spi(0x02, [0x0001])                      # soft reset clears FCTL like CTRL
    await ClockCycles(dut.clk, 30)
    assert await h.spi(0x11, n=2) == [0, 0], "FCTL, FSTAT after soft reset"
    # everything but TWIN_EN set: TWIN_USE needs TWIN_EN, so the lookups are plain; DBG shows the fault events
    # (event 10: the default prediction with W3 = the plain default prediction).  CTEST makes the canary fire at
    # every predict, so with DEMOTE the fourth one demotes the chip: from then on the tournament drives.
    await set_fctl(h, m, 0xFFFE & pm)
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
    assert m.dem and m.fstat == bpx1.CANARY, f"model: canary and demotion expected, FSTAT {m.fstat_reg:#x}"
    await compare_state(h, m)                        # includes the twin, read as 0, and FSTAT = CANARY | DEMOTED
    await compare_stats(h, m)
    dut._log.info("mosaic registers: FCTL storage and resets; with the twin's data off and CTEST, DEMOTE: canary "
                  "at every predict, demoted after the fourth; replies, pins, state and FSTAT equal the model")


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
    assert await raw_rd(h, bpx1.SPACE_TWIN, 3) == 0xFF, "twin entry after reset (MASK = 0): the code word of -1"
    ids = [0x15, 0x2A, 0x3C, 0x47, 0x5E, 0x63, 0x79, 0x8B, 0x91, 0xA4, 0xB2, 0xCD]
    flips = [0x00, 0x01, 0x04, 0x10, 0x80, 0x22, 0x30, 0x03, 0x07, 0x51, None]     # None: W3 changed, twin not
    seen = set()
    dsel_pairs = [(0, 1), (2, 3), (10, 2), (1, 3)]
    n = 0
    for mk in (0, 1):                                    # CTRL[15] MASK: the code mask off and on
        m.mask = bool(mk)
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
                    await raw_wr(h, m, bpx1.SPACE_TWIN, a, m.node_word(w3) ^ (fl or 0))
                    if fl is None:
                        await raw_wr(h, m, 11, a, w3 ^ 0x5)
                    await checked_request(h, m, bpx1.OP_PREDICT, ident, f"mask {mk} code {code} use {use} flips {fl}")
                    assert m.last_tw is not None
                    seen.add((mk, code, m.last_tw["outcome"], use))
                    assert (await h.spi(0x12, n=1))[0] == m.fstat_reg, f"FSTAT, mask {mk} code {code} flips {fl}: model {m.fstat_reg:#x}"
                    await checked_request(h, m, bpx1.OP_SPECIAL, bpx1.SUB_FLUSH, "flush")
    want = {(mk, cd, o, u) for mk in (0, 1) for cd in range(4)
            for o in {0: (0, 5, 6), 1: (0, 3, 5, 6), 2: (0, 1, 2, 3, 4, 5), 3: (0, 3, 5)}[cd] for u in (0, 1)}
    assert want <= seen, f"decode outcomes not reached: {sorted(want - seen)}"

    # the clear writes the initial word under the present MASK (here 1: 0xFF ^ TWMASK)
    await h.spi(0x02, [0x0002])
    await ClockCycles(dut.clk, 30)
    m.clear_predictors()
    assert await raw_rd(h, bpx1.SPACE_TWIN, 7) == 0xFF ^ bpx1.TWMASK == m.raw(bpx1.SPACE_TWIN, 7), "clear under MASK = 1"

    # code mask (docs/mosaic.md 7.7): nodes that have all decayed the same way (all 0, all 1).  MASK = 0: both are
    # code words (of 0 and -1), so no code detects them; MASK = 1: they are no code word once the mask is taken off,
    # and SEC-DED and detect-all abstain (TW_DET)
    decay = []
    for mk in (0, 1):
        m.mask = bool(mk)
        await h.spi(0x01, [m.ctrl_word()])
        for code in (0, 1, 2, 3):
            for nodes in (0x00, 0xFF):
                ident = ids[(2 * code + (nodes & 1) + mk) % len(ids)]
                await set_fctl(h, m, bpx1.FCTL_TWIN_EN | (code << 2) | bpx1.FCTL_DBGF)
                a = m._lookup(ident, m.sh, m.psh).idx[-1]
                await raw_wr(h, m, bpx1.SPACE_TWIN, a, nodes)
                await checked_request(h, m, bpx1.OP_PREDICT, ident, f"decayed nodes {nodes:#04x}, mask {mk}, code {code}")
                f = m.last_tw["flags"]
                if mk and code in (2, 3):
                    assert f & bpx1.TW_DET, f"decayed nodes {nodes:#04x}, MASK = 1, code {code}: not detected ({f:#x})"
                if not mk:
                    assert not f & (bpx1.TW_DET | bpx1.TW_COR), f"decayed nodes {nodes:#04x}, MASK = 0: not a valid word ({f:#x})"
                decay.append((mk, code, nodes, bpx1.TWO_NAMES[m.last_tw["outcome"]]))
                await checked_request(h, m, bpx1.OP_SPECIAL, bpx1.SUB_FLUSH, "flush")
    dut._log.info(f"decayed words, TWMASK {bpx1.TWMASK:#04x}: (MASK, code, nodes, outcome) {decay}")
    # restore-on-use: make a branch confidently right (weights at their limits), flip one bit of its twin word,
    # resolve it without a learning step: the entry is rewritten corrected (SEC-DED) or as read (parity)
    m.dsel = [10, 13]
    await h.spi(0x01, [m.ctrl_word()])                 # MASK stays 1 for the restore checks
    rest = []
    for code, fl in ((2, 0x02), (2, 0x40), (1, 0x08), (0, 0x01)):
        await set_fctl(h, m, bpx1.FCTL_TWIN_EN | (code << 2) | bpx1.FCTL_RESTORE)
        ident = 0x33
        L = m._lookup(ident, m.sh, m.psh)
        for i in range(len(c.weights)):
            await raw_wr(h, m, 8 + i, L.idx[i], 0x8 if L.neg[i] else 0x7)
        a = L.idx[-1]
        await raw_wr(h, m, bpx1.SPACE_TWIN, a, m.node_word(m.wt[-1][a]) ^ fl)
        tk = 1 if m._lookup(ident, m.sh, m.psh).p_def else 0
        await checked_request(h, m, bpx1.OP_PREDICT_RESOLVE_T if tk else bpx1.OP_PREDICT_RESOLVE_NT, ident, f"restore code {code}")
        rest.append(m.last_train["restore"])
        await ClockCycles(dut.clk, 8)
        assert await raw_rd(h, bpx1.SPACE_TWIN, a) == m.tw[a], f"restore code {code}: twin entry differs from the model"
    assert rest == [1, 1, 2, 2], f"restore kinds {rest}"
    await ClockCycles(dut.clk, 8)
    await compare_state(h, m)
    dut._log.info(f"twin: {len(seen)} (code, outcome, TWIN_USE) combinations, restore-on-use {rest}; state equals the model")


# ---------------------------------------------------------------------------------------------------- mosaic step 4
def core_of(dut):
    """bpx_core inside the testbench (repository top: tt_um -> bpx_chip; Makefile.bpx: bpx_chip)."""
    u = dut.user_project
    return u.u_chip.u_core if hasattr(u, "u_chip") else u.u_core


async def resolve_all(h, m, hist):
    while m.queue:
        tk = outcome(m.queue[0]["id"], hist)
        await checked_request(h, m, bpx1.OP_SPECIAL, bpx1.SUB_RESOLVE_T if tk else bpx1.SUB_RESOLVE_NT, "drain")


@cocotb.test(skip=GATES)
async def pulse_settings(dut):
    """Write-pulse selector (docs/mosaic.md 7.4).  With the latch cells and the delay-cell model (0.57 ns per cell,
    test/sky130_cells_sim.v): the pulse of W0-W2 has the width of its tap at every setting while the standard pulse
    keeps half a clock; at every setting raw writes and training of W0-W3 land exactly as in the golden model (in
    simulation every setting writes; failures are a silicon experiment).  Flip-flop model: no effect."""
    await start(dut)
    h = Host(dut, random.Random(SEED + 5000))
    m = bpx1.Bpx1()
    rng = random.Random(SEED + 5001)
    core = core_of(dut)
    ws, pr = core.u_wseq, core.u_pred
    structural = hasattr(ws, "u_wpx")
    # the pulse as each table receives it: W0-W2 the selected one, the others (BC, G, W3, twin) the standard one
    tabs = {"W0": pr.g_t[0].u_t, "W1": pr.g_t[1].u_t, "W2": pr.g_t[2].u_t, "W3": pr.g_t[3].u_t, "BC": pr.u_bc,
            "G": pr.u_g, "twin": pr.u_tw}
    widths = {"x": [], "s": []}
    widths.update({k: [] for k in tabs})

    async def watch(sig, key):
        while True:
            await RisingEdge(sig)
            t0 = get_sim_time(unit="ps")
            await FallingEdge(sig)
            widths[key].append(get_sim_time(unit="ps") - t0)

    tasks = [cocotb.start_soon(watch(ws.wpulse_x, "x")), cocotb.start_soon(watch(ws.wpulse, "s"))]
    tasks += [cocotb.start_soon(watch(t.wpulse, k)) for k, t in tabs.items()]
    taps = [0, 1, 2, 3, 4, 6, 8, 12]
    hist, ids = {}, [rng.randrange(256) for _ in range(16)]
    seen = {}
    for s in range(8):
        await set_fctl(h, m, s << bpx1.FCTL_PULSE_SHIFT)
        await ClockCycles(dut.clk, 4)
        for w in widths.values():
            w.clear()
        for t in range(8, 12):                       # raw writes of W0-W3, read back
            a, v = rng.randrange(32), rng.randrange(16)
            await raw_wr(h, m, t, a, v)
            got = await raw_rd(h, t, a)
            assert got == m.raw(t, a), f"setting {s}: raw write of W{t - 8}[{a}] = {v}, read {got}"
        for n in range(60):                          # training at this setting
            ident = rng.choice(ids)
            if rng.random() < 0.6:
                await checked_request(h, m, bpx1.OP_PREDICT_RESOLVE_T if outcome(ident, hist) else bpx1.OP_PREDICT_RESOLVE_NT, ident, f"setting {s} op {n}")
            else:
                await checked_request(h, m, bpx1.OP_PREDICT, ident, f"setting {s} op {n}")
                await resolve_all(h, m, hist)
        await ClockCycles(dut.clk, 8)
        for i in range(4):
            tab = await h.dump(8 + i, 32)
            assert tab == [m.raw(8 + i, k) for k in range(32)], f"setting {s}: weight table {i} differs from the model"
        wx, wsd = sorted(set(widths["x"])), sorted(set(widths["s"]))
        seen[s] = (wx, wsd, len(widths["x"]))
        assert widths["x"] and len(widths["x"]) == len(widths["s"]), f"setting {s}: pulses {widths}"
        exp = CLK_NS * 500 if (s == 0 or not structural) else round(taps[s] * 570)
        assert wsd == [CLK_NS * 500], f"setting {s}: standard pulse widths {wsd} ps"
        assert wx == [exp], f"setting {s}: W0-W2 pulse widths {wx} ps, expected {exp}"
        for k in tabs:
            want = exp if k in ("W0", "W1", "W2") else CLK_NS * 500
            assert sorted(set(widths[k])) == [want] and len(widths[k]) == len(widths["s"]), \
                f"setting {s}: table {k} received pulses {sorted(set(widths[k]))} ps ({len(widths[k])}), expected {want}"
    for tk in tasks:
        tk.cancel()
    await set_fctl(h, m, 0)
    await compare_state(h, m)
    dut._log.info(f"pulse settings ({'latch cells' if structural else 'flip-flop model'}): widths in ps by setting "
                  f"(W0-W2, standard, pulses) {seen}; tables equal the model at every setting")


@cocotb.test(skip=GATES)
async def checkers_watchdog(dut):
    """The four invariant checkers and the hang watchdog (docs/mosaic.md 7.5), RTL only: each fired once by
    depositing an upset into an internal register; FSTAT shows exactly its flag, DBG its fault event (5 any checker,
    6 watchdog), CMD bit 2 clears it; afterwards the chip still equals the golden model."""
    await start(dut)
    h = Host(dut, random.Random(SEED + 6000))
    m = bpx1.Bpx1()
    core = core_of(dut)
    m.dsel = [bpx1.EV_CHECKER, bpx1.EV_WDOG]
    await h.spi(0x01, [m.ctrl_word()])
    await set_fctl(h, m, bpx1.FCTL_DBGF)
    cnt = [0, 0]

    async def mon():
        while True:
            await RisingEdge(dut.clk)
            await Timer(1, unit="ns")
            v = int(dut.uo_out.value)
            cnt[0] += (v >> 5) & 1
            cnt[1] += (v >> 6) & 1

    mt = cocotb.start_soon(mon())

    async def check(flags, what, ev):
        await ClockCycles(dut.clk, 4)
        f = (await h.spi(0x12, n=1))[0]
        assert f == flags, f"{what}: FSTAT {f:#06x}, expected {flags:#06x}"
        assert cnt[ev] > 0, f"{what}: fault event {5 + ev} not seen on DBG{ev}"
        res = (what, f, cnt[0], cnt[1])
        await h.spi(0x02, [0x0004])                  # CMD bit 2
        m.clear_stats()
        await ClockCycles(dut.clk, 4)
        f = (await h.spi(0x12, n=1))[0]
        assert f == 0, f"{what}: FSTAT {f:#06x} after CMD bit 2"
        cnt[0] = cnt[1] = 0
        return res

    log = []
    assert (await h.spi(0x12, n=1))[0] == 0 and cnt == [0, 0]
    # queue: FULL set while the queue is empty (FULL is rewritten every clock: one firing)
    await FallingEdge(dut.clk)
    core.full.value = 1
    log.append(await check(bpx1.CHK_QUEUE, "queue checker", 0))
    # staging: the staging-valid bit set with an empty queue (fires until a FLUSH clears it)
    await FallingEdge(dut.clk)
    core.stg_v.value = 1
    await ClockCycles(dut.clk, 3)
    await checked_request(h, m, bpx1.OP_SPECIAL, bpx1.SUB_FLUSH, "flush")
    log.append(await check(bpx1.CHK_STAGE, "staging checker", 0))
    # write sequencer: both state flops set; the extra pulse rewrites the entry the last write wrote, with its data
    a3 = 5
    await raw_wr(h, m, 8, a3, m.raw(8, a3))
    await FallingEdge(dut.clk)
    core.u_wseq.s1.value = 1
    core.u_wseq.a.value = 1
    log.append(await check(bpx1.CHK_WSEQ, "write-sequencer checker", 0))
    # handshake: ACK flipped with nothing pending (OP/ID = NOP, so the phantom request is a NOP that flips it back)
    h.ui, h.uio = 0, h.uio & 0x70
    h._drive()
    await ClockCycles(dut.clk, 4)
    await FallingEdge(dut.clk)
    core.ack.value = 1 - int(core.ack.value)
    log.append(await check(bpx1.CHK_HANDSHAKE, "handshake checker", 0))
    # watchdog: a pending predict blocked for 70 clocks (resolve-pending set with an empty queue, which the staging
    # checker also reports), then released
    assert not m.queue
    await FallingEdge(dut.clk)
    core.rpend.value = 1
    ident = 0x5A
    h.ui, h.uio = ident, (h.uio & 0x70) | (bpx1.OP_PREDICT << 2)
    h._drive()
    a0 = h.ack()
    h.req ^= 1
    h._drive()
    await ClockCycles(dut.clk, 70)
    assert h.ack() == a0, "the blocked predict was answered"
    await FallingEdge(dut.clk)
    core.rpend.value = 0
    for _ in range(20):
        await RisingEdge(dut.clk)
        if h.ack() != a0:
            break
    assert h.ack() != a0, "no answer after the release"
    exp = m.request(bpx1.OP_PREDICT, ident)
    await Timer(1, unit="ns")
    assert (int(dut.uo_out.value) >> 1) & 1 == exp.pred, "released predict: PRED differs from the model"
    log.append(await check(bpx1.CHK_STAGE | bpx1.WDOG, "watchdog", 1))
    mt.cancel()
    await resolve_all(h, m, {})
    await ClockCycles(dut.clk, 8)
    await compare_state(h, m)
    await compare_stats(h, m)
    dut._log.info(f"checkers and watchdog: (what, FSTAT, DBG0 clocks, DBG1 clocks) {log}; cleared; state equals the model")


@cocotb.test(skip=GATES)
async def canary_demotion(dut):
    """Canary self-test and demotion (docs/mosaic.md 7.5, 7.6) against the golden model: CTEST makes the canary fire
    one clock after every answered predict (FSTAT bit 4, fault event 4); four detected twin errors (double errors,
    SEC-DED) with DEMOTE set DEMOTED (bit 10, event 7): the tournament then drives instead of the default predictor;
    DEMOTE = 0 suspends it, CMD bit 2 ends it."""
    await start(dut)
    h = Host(dut, random.Random(SEED + 7000))
    m = bpx1.Bpx1()
    rng = random.Random(SEED + 7001)
    hist = {}
    m.dsel = [bpx1.EV_CANARY, bpx1.EV_DEMOTED]
    await h.spi(0x01, [m.ctrl_word()])

    async def fstat_is(what):
        f = (await h.spi(0x12, n=1))[0]
        assert f == m.fstat_reg, f"{what}: FSTAT {f:#06x}, model {m.fstat_reg:#06x}"
        return f

    # canary self-test (the post-ACK DBG check of checked_request sees event 4)
    await set_fctl(h, m, bpx1.FCTL_CTEST | bpx1.FCTL_DBGF)
    for n in range(3):
        await checked_request(h, m, bpx1.OP_PREDICT, 0x10 + n, f"CTEST predict {n}")
    await resolve_all(h, m, hist)
    assert m.fstat == bpx1.CANARY and m.dcnt == 3 and not m.dem
    f_can = await fstat_is("canary")
    await h.spi(0x02, [0x0004])
    m.clear_stats()
    await fstat_is("after CMD bit 2")
    await set_fctl(h, m, bpx1.FCTL_DBGF)
    await checked_request(h, m, bpx1.OP_PREDICT_RESOLVE_T, 0x21, "no CTEST")
    # demotion by twin errors: default drives, TWIN_EN, SEC-DED, DEMOTE; a double error in each read entry
    await set_fctl(h, m, bpx1.FCTL_TWIN_EN | (bpx1.CODE_SECDED << 2) | bpx1.FCTL_DEMOTE | bpx1.FCTL_DBGF)
    k = 0
    while not m.dem:
        ident = rng.randrange(256)
        a = m._lookup(ident, m.sh, m.psh).idx[-1]
        await raw_wr(h, m, bpx1.SPACE_TWIN, a, m.tw[a] ^ 0x41)
        await checked_request(h, m, bpx1.OP_PREDICT, ident, f"twin error {k}")
        await resolve_all(h, m, hist)
        k += 1
        assert k <= 8, "not demoted after 8 injected double errors"
    f_dem = await fstat_is("demoted")
    assert f_dem & bpx1.DEMOTED
    # a branch whose tournament prediction differs from the default prediction: its bimodal counter strongly taken
    # (chooser 1: the tournament follows it), its four weights at the not-taken limit (the twin entry rewritten too)
    target = 0x6B
    L = m._lookup(target, m.sh, m.psh)
    await raw_wr(h, m, 0, L.bi, 3 | (1 << 2))
    for i in range(len(m.cfg.weights)):
        await raw_wr(h, m, 8 + i, L.idx[i], 0x7 if L.neg[i] else 0x8)
    await raw_wr(h, m, bpx1.SPACE_TWIN, L.idx[-1], m.node_word(m.wt[-1][L.idx[-1]]))
    L = m._lookup(target, m.sh, m.psh)
    assert L.p_def != L.p_trn, f"target branch: default {L.p_def}, tournament {L.p_trn}"
    # demoted: the tournament answers; the target branch shows it
    r = await checked_request(h, m, bpx1.OP_PREDICT, target, "demoted, target")
    assert r.pred == m.last_preds[3] != m.last_preds[0], "demoted: PRED must be the tournament's, unlike the default's"
    await checked_request(h, m, bpx1.OP_SPECIAL, bpx1.SUB_FLUSH, "flush")
    # suspended: DEMOTE = 0 (DEMOTED stays set), the default predictor answers again
    await set_fctl(h, m, m.fctl & ~bpx1.FCTL_DEMOTE)
    r = await checked_request(h, m, bpx1.OP_PREDICT, target, "suspended, target")
    assert r.pred == m.last_preds[0], "suspended: PRED must be the default predictor's"
    await checked_request(h, m, bpx1.OP_SPECIAL, bpx1.SUB_FLUSH, "flush")
    await fstat_is("suspended")
    # resumed, then ended by CMD bit 2
    await set_fctl(h, m, m.fctl | bpx1.FCTL_DEMOTE)
    r = await checked_request(h, m, bpx1.OP_PREDICT, target, "resumed, target")
    assert r.pred == m.last_preds[3]
    await checked_request(h, m, bpx1.OP_SPECIAL, bpx1.SUB_FLUSH, "flush")
    await h.spi(0x02, [0x0004])
    m.clear_stats()
    await fstat_is("cleared")
    r = await checked_request(h, m, bpx1.OP_PREDICT, target, "cleared, target")
    assert r.pred == m.last_preds[0] and not m.dem
    await resolve_all(h, m, hist)
    await ClockCycles(dut.clk, 8)
    await compare_state(h, m)
    dut._log.info(f"canary: FSTAT {f_can:#06x} after 3 CTEST predicts; demotion after {k} injected double errors "
                  f"(FSTAT {f_dem:#06x}); tournament drives, suspended, resumed, cleared; state equals the model")


@cocotb.test(skip=GATES)
async def canary_halves(dut):
    """Each half of the canary on its own (review fix-up): CTEST inverts both shadow samples at once, so it cannot show
    that the PRED half and the CONF half each work.  With CTEST off, the shadow flop of one half (`u_s` of g_b[0]
    PRED or g_b[1] CONF; in the flip-flop model the matching bit of `rs`) is forced to the opposite of the value its
    real flop takes at an acknowledged predict: FSTAT CANARY and fault event 4 must follow, for each half alone; with
    nothing forced, neither fires.  RTL only (forces internal nets)."""
    from cocotb.handle import Force, Release
    await start(dut)
    h = Host(dut, random.Random(SEED + 7500))
    m = bpx1.Bpx1()
    core = core_of(dut)
    can = core.u_can
    structural = hasattr(can, "g_b")
    m.dsel = [bpx1.EV_CANARY, bpx1.EV_DEMOTED]
    await h.spi(0x01, [m.ctrl_word()])
    await set_fctl(h, m, bpx1.FCTL_DBGF)
    cnt = [0]

    async def mon():
        while True:
            await RisingEdge(dut.clk)
            await Timer(1, unit="ns")
            cnt[0] += (int(dut.uo_out.value) >> 5) & 1          # DBG0 = fault event 4

    mt = cocotb.start_soon(mon())
    res = {}
    for n, half in enumerate((None, 0, 1, None)):            # nothing, PRED, CONF, nothing
        ident = 0x30 + 7 * n
        exp = m.request(bpx1.OP_PREDICT, ident)
        bits = [exp.pred, exp.conf]                          # what PRED (bit 0) and CONF (bit 1) take at the answer
        sig = None
        if half is not None:
            await FallingEdge(dut.clk)
            if structural:
                sig = can.g_b[half].sq
                sig.value = Force(1 - bits[half])
            else:
                sig = can.rs
                sig.value = Force((bits[0] | (bits[1] << 1)) ^ (1 << half))
        cnt[0] = 0
        pred, conf, _ = await h.request(bpx1.OP_PREDICT, ident)
        assert (pred, conf) == (exp.pred, exp.conf), f"half {half}: pred/conf {(pred, conf)}, model {(exp.pred, exp.conf)}"
        await ClockCycles(dut.clk, 3)                        # the comparison is in the clock after the answer
        if sig is not None:
            await FallingEdge(dut.clk)
            sig.value = Release()
        await ClockCycles(dut.clk, 4)
        ev = cnt[0]
        f = (await h.spi(0x12, n=1))[0]
        name = {None: "none", 0: "PRED", 1: "CONF"}[half]
        if half is None:
            assert f == 0 and ev == 0, f"nothing forced: FSTAT {f:#06x}, event 4 seen {ev} clocks"
        else:
            assert f == bpx1.CANARY, f"{name} half forced: FSTAT {f:#06x}, expected CANARY only"
            assert ev == 1, f"{name} half forced: fault event 4 high for {ev} clocks, expected 1"
        res[f"{n}:{name}"] = (f, ev)
        await checked_request(h, m, bpx1.OP_SPECIAL, bpx1.SUB_RESOLVE_T if exp.pred else bpx1.SUB_RESOLVE_NT, "resolve")
        await h.spi(0x02, [0x0004])                          # CMD bit 2: FSTAT and the fault count
        m.clear_stats()
    mt.cancel()
    await ClockCycles(dut.clk, 8)
    await compare_state(h, m)
    dut._log.info(f"canary halves ({'latch cells' if structural else 'flip-flop model'}): (FSTAT, event-4 clocks) {res}")


@cocotb.test(skip=GATES)
async def mask_staging(dut):
    """The MASK staging rule (review fix-up), in a plain run: a change of CTRL[15] MASK must invalidate the staging
    register, because the staged twin word depends on it.  Each round: a branch in flight (staged under the present
    MASK), a flipped bit in its twin entry, TWIN_EN with SEC-DED and RESTORE, then MASK switched by a CTRL write and
    the branch resolved: restore-on-use and training must write what the golden model (no staging) writes."""
    await start(dut)
    h = Host(dut, random.Random(SEED + 7600))
    m = bpx1.Bpx1()
    rng = random.Random(SEED + 7601)
    hist = {}
    await set_fctl(h, m, bpx1.FCTL_TWIN_EN | (bpx1.CODE_SECDED << 2) | bpx1.FCTL_RESTORE)
    n3 = m.cfg.weights[-1][0]
    for n in range(24):
        ident = rng.randrange(256)
        await checked_request(h, m, bpx1.OP_PREDICT, ident, f"round {n} predict")
        await ClockCycles(dut.clk, 8)                        # the chip stages the oldest branch
        a = m._lookup(m.queue[0]["id"], m.ch, m.pch).idx[-1]
        if rng.random() < 0.5:
            await raw_wr(h, m, bpx1.SPACE_TWIN, a, m.tw[a] ^ (1 << rng.randrange(8)))
        m.mask = not m.mask
        await h.spi(0x01, [m.ctrl_word()])
        tk = outcome(ident, hist)
        await checked_request(h, m, bpx1.OP_SPECIAL, bpx1.SUB_RESOLVE_T if tk else bpx1.SUB_RESOLVE_NT, f"round {n} resolve")
        await ClockCycles(dut.clk, 8)
        tw = await h.dump(bpx1.SPACE_TWIN, n3)
        exp = [m.raw(bpx1.SPACE_TWIN, k) for k in range(n3)]
        assert tw == exp, f"round {n}: twin differs from the model at {[k for k in range(n3) if tw[k] != exp[k]]}"
    await compare_state(h, m)
    dut._log.info("MASK staging: 24 rounds of MASK switched with a staged branch in flight; twin and state equal the model")


# ---------------------------------------------------------------------------------------------------- mosaic step 5
@cocotb.test()
async def gshare_random(dut):
    """gshare probabilistic hysteresis (docs/mosaic.md 7.7), also at gate level: a counter at its limit, trained away
    from it, always moves with GRAND = 0, moves only on a 1 of the LFSR with GSRC = 0 (some held, some moved, exactly
    as the golden model's LFSR says), and never with GSRC = 1 (the oscillator bit is 0 while no ring runs).  Fault
    event 9 (the random bit of the last training) and event 8 (the divided oscillator, 0) on DBG against the model."""
    await start(dut)
    h = Host(dut, random.Random(SEED + 8000))
    m = bpx1.Bpx1()
    m.dsel = [bpx1.EV_RAND, bpx1.EV_DIV]
    await h.spi(0x01, [m.ctrl_word()])
    rng = random.Random(SEED + 8001)
    trials = 16 if GATES else 40
    res = {}
    for name, f in (("GRAND off", 0), ("LFSR", bpx1.FCTL_GRAND), ("oscillator", bpx1.FCTL_GRAND | bpx1.FCTL_GSRC)):
        await set_fctl(h, m, f | bpx1.FCTL_DBGF)
        held = moved = 0
        for n in range(trials):
            ident = rng.randrange(256)
            top = rng.random() < 0.5
            gi = m._lookup(ident, m.sh, m.psh).gi          # queue empty: the commit lookup is the predict lookup
            await raw_wr(h, m, 1, gi, 3 if top else 0)
            tk = 0 if top else 1                            # away from the limit
            await checked_request(h, m, bpx1.OP_PREDICT_RESOLVE_T if tk else bpx1.OP_PREDICT_RESOLVE_NT, ident,
                                  f"{name} trial {n}")
            got = await raw_rd(h, 1, gi)
            assert got == m.gsh[gi], f"{name} trial {n}: gshare entry {gi} is {got}, model {m.gsh[gi]}"
            if got == (3 if top else 0):
                held += 1
            else:
                moved += 1
        res[name] = (held, moved)
    assert res["GRAND off"] == (0, trials), f"GRAND = 0 must always move: {res}"
    assert res["LFSR"][0] > 0 and res["LFSR"][1] > 0, f"the LFSR must hold some and move some: {res}"
    assert res["oscillator"] == (trials, 0), f"GSRC = 1 without a running ring must always hold: {res}"
    await set_fctl(h, m, 0)
    await ClockCycles(dut.clk, 8)
    await compare_state(h, m)
    dut._log.info(f"gshare randomness: (held, moved) at a limit {res}; DBG events 8, 9 and state equal the model")


async def pin_reset(h, op, csn, mosi, release=True):
    """A pin reset (rst_n low for 8 clocks) with OP, CSn, MOSI held from 4 clocks before until 6 clocks after it (SCK
    low, REQ unchanged), then, with release, CSn high and OP 0; waits for READY."""
    dut = h.dut
    await FallingEdge(dut.clk)
    h.ui, h.uio = 0, ((op & 3) << 2) | ((csn & 1) << 4) | ((mosi & 1) << 6)
    h._drive()
    await ClockCycles(dut.clk, 4)
    dut.rst_n.value = 0
    await ClockCycles(dut.clk, 8)
    dut.rst_n.value = 1
    await ClockCycles(dut.clk, 6)
    if release:
        h.uio = 0x10
        h._drive()
    for _ in range(60):
        await RisingEdge(dut.clk)
        if (int(dut.uo_out.value) >> 4) & 1:
            return
    assert False, "READY did not rise after the pin reset"


@cocotb.test()
async def strap_reset(dut):
    """Reset strap (docs/mosaic.md 7.7), also at gate level: train, then a pin reset with OP = 3, CSn = 0, MOSI = 1:
    the tables (bimodal/chooser, gshare, W0-W3, the twin) are unchanged, the small registers, histories, settings
    and statistics are reset, FSTAT KEPT is set (CMD bit 2 leaves it); right after it, with CSn still low and no SCK
    edge, the next SPI word works without raising CSn first.  Each single wrong strap condition, and a soft reset,
    give a normal clearing reset and clear KEPT."""
    await start(dut)
    h = Host(dut, random.Random(SEED + 9000))
    m = bpx1.Bpx1()
    rng = random.Random(SEED + 9001)
    hist = {}
    ids = [rng.randrange(256) for _ in range(24)]

    async def train(n, what):
        # TWIN_EN in RTL (the twin's nodes are unknown at gate level); GRAND with the LFSR moves the LFSR along
        await set_fctl(h, m, (bpx1.FCTL_GRAND if GATES else bpx1.FCTL_TWIN_EN | bpx1.FCTL_GRAND))
        for k in range(n):
            ident = rng.choice(ids)
            await checked_request(h, m, bpx1.OP_PREDICT_RESOLVE_T if outcome(ident, hist) else bpx1.OP_PREDICT_RESOLVE_NT,
                                  ident, f"{what} {k}")
        if not GATES:
            a = rng.randrange(32)
            await raw_wr(h, m, bpx1.SPACE_TWIN, a, m.tw[a] ^ 0x10)     # a twin word that training does not produce
        await raw_wr(h, m, 7, 4, 0x0C3)                    # threshold 12, adaptation counter 3: reset must undo it
        m.drive = 1
        await h.spi(0x01, [m.ctrl_word()])                 # CTRL: reset must undo it
        await checked_request(h, m, bpx1.OP_PREDICT, rng.choice(ids), f"{what} in flight")   # one branch in flight

    async def tables_differ_from_reset():
        c = bpx1.Bpx1()
        return m.bim != c.bim and m.gsh != c.gsh and m.wt[1] != c.wt[1]

    n_train = 60 if GATES else 200
    await train(n_train, "train")
    assert await tables_differ_from_reset(), "training did not change the tables"
    before = m.state()
    # the strap; CSn and MOSI stay as they were after reset, and the first SPI word starts without a CSn edge
    await pin_reset(h, 3, 0, 1, release=False)
    m.reset(strap=True)
    h.uio = (h.uio & ~0x0C) & 0xFF                         # OP back to 0; CSn still low, MOSI 1, SCK low
    h._drive()
    await ClockCycles(dut.clk, 4)
    ident = await h.spi(0x00, n=1)
    assert ident == [0xB101], f"first SPI word after a strapped reset with CSn still low: {ident}"
    assert (await h.spi(0x12, n=1))[0] == bpx1.KEPT == m.fstat_reg, "FSTAT after the strapped reset: KEPT only"
    st = m.state()
    for k in ("bim", "cho", "gsh", "weights", "twin"):
        assert st[k] == before[k], f"model: table {k} not kept"
    assert st["theta"] == m.cfg.THETA and st["tc"] == 0 and st["queue"] == [] and st["drive"] == 0
    if not GATES:
        await set_fctl(h, m, bpx1.FCTL_TWIN_EN)            # so that the dump reads the twin's nodes
    await compare_state(h, m)                              # tables as trained, small registers reset, FSTAT = KEPT
    await set_fctl(h, m, 0)
    await compare_stats(h, m)
    await h.spi(0x02, [0x0004])                            # CMD bit 2 leaves KEPT
    m.clear_stats()
    assert (await h.spi(0x12, n=1))[0] == bpx1.KEPT, "CMD bit 2 must leave KEPT"
    log = ["strap: tables kept, KEPT"]

    # each single wrong condition: a normal reset (tables cleared, KEPT cleared)
    for op, csn, mosi, what in ((2, 0, 1, "OP = 2"), (3, 1, 1, "CSn = 1"), (3, 0, 0, "MOSI = 0")):
        await train(20 if GATES else 60, what)
        await pin_reset(h, 3, 0, 1)                        # KEPT first, so that the wrong strap visibly clears it
        m.reset(strap=True)
        assert (await h.spi(0x12, n=1))[0] == bpx1.KEPT
        await pin_reset(h, op, csn, mosi)
        m.reset(strap=False)
        f = (await h.spi(0x12, n=1))[0]
        assert f == 0, f"{what}: FSTAT {f:#06x} after a reset without the strap"
        if GATES:                                          # a sample of each table (full dumps take long at gate level)
            for sp, a in ((0, 5), (1, 9), (8, 3), (11, 17)):
                assert await raw_rd(h, sp, a) == m.raw(sp, a), f"{what}: space {sp} entry {a} not cleared"
        else:
            await compare_state(h, m)
        log.append(f"{what}: cleared")

    # a soft reset is a reset without the strap
    await train(10, "soft")
    await pin_reset(h, 3, 0, 1)
    m.reset(strap=True)
    await h.spi(0x02, [0x0001])
    await ClockCycles(dut.clk, 30)
    m.reset()
    assert (await h.spi(0x12, n=1))[0] == 0, "soft reset must clear KEPT"
    if not GATES:
        await compare_state(h, m)
    log.append("soft reset: cleared")

    # a soft reset issued while the strap pins are true (review fix-up): a host that leaves OP = 3 on the pins sends
    # CMD 0x0001 with CSn low and MOSI = 1 (the word's last bit), so the strap condition holds during the soft reset;
    # it is still a reset without the strap: the tables are cleared and KEPT stays 0
    await train(10, "soft, strap pins")
    await pin_reset(h, 3, 0, 1)
    m.reset(strap=True)
    h.uio = (h.uio & ~0x0C) | (3 << 2)                     # OP = 3 on the pins, REQ unchanged
    h._drive()
    await h.spi(0x02, [0x0001])
    st = int(dut.uio_in.value)
    assert (st >> 2) & 3 == 3 and (st >> 6) & 1 == 1, f"strap pins not as intended after the CMD word: uio {st:#04x}"
    await ClockCycles(dut.clk, 30)
    m.reset()
    h.uio &= ~0x0C                                         # OP back to 0
    h._drive()
    assert (await h.spi(0x12, n=1))[0] == 0, "soft reset with the strap pins true must leave KEPT = 0"
    if GATES:
        for sp, a in ((0, 5), (1, 9), (8, 3), (11, 17)):
            assert await raw_rd(h, sp, a) == m.raw(sp, a), f"soft reset, strap pins: space {sp} entry {a} not cleared"
    else:
        await compare_state(h, m)
    log.append("soft reset with the strap pins true: cleared")
    dut._log.info(f"reset strap: {log}; replies and state equal the model")


@cocotb.test(skip=GATES)
async def ring_oscillators(dut):
    """The two ring oscillators (docs/mosaic.md 7.7), latch-cell RTL only (test/sky130_cells_sim.v: the xnor2_1 model
    carries 4 ns, one per ring, so both rings simulate at 125 MHz).  Each ring is run and selected in turn: fault event
    8 on DBG0 toggles at the ring frequency / 256; with the selected ring stopped it stays 0; with GRAND and GSRC = 1
    the oscillator bit reaches gshare (event 9 shows both values).  Flip-flop model: no rings (div = rnd = 0)."""
    await start(dut)
    h = Host(dut, random.Random(SEED + 10000))
    m = bpx1.Bpx1()
    u = dut.user_project
    chip = u.u_chip if hasattr(u, "u_chip") else u
    if not hasattr(chip.u_ro, "u_ra"):
        dut._log.info("ring oscillators: flip-flop model, no rings (div = rnd = 0); nothing to measure")
        return
    m.dsel = [bpx1.EV_DIV, bpx1.EV_RAND]
    await h.spi(0x01, [m.ctrl_word()])

    async def watch(cycles):
        n0 = n1 = 0
        last = int(dut.uo_out.value)
        for _ in range(cycles):
            await RisingEdge(dut.clk)
            await Timer(1, unit="ns")
            v = int(dut.uo_out.value)
            n0 += ((v ^ last) >> 5) & 1
            n1 += ((v ^ last) >> 6) & 1
            last = v
        return n0, n1, last

    res = []
    cycles = 1000
    for name, f in (("A", bpx1.FCTL_ROA), ("B", bpx1.FCTL_ROB | bpx1.FCTL_ROSEL)):
        await h.spi(0x11, [f | bpx1.FCTL_DBGF])
        await ClockCycles(dut.clk, 10)
        n0, _, _ = await watch(cycles)
        mhz = n0 / 2 * 256 / (cycles * CLK_NS * 1e-3)
        res.append((name, round(mhz, 1)))
        assert abs(mhz - 125.0) < 0.15 * 125.0, f"ring {name}: {mhz:.1f} MHz, expected about 125 MHz"
        # the other ring selected (stopped): static 0
        await h.spi(0x11, [(f ^ bpx1.FCTL_ROSEL) | bpx1.FCTL_DBGF])
        await ClockCycles(dut.clk, 6)
        n0, _, last = await watch(200)
        assert n0 == 0 and not (last >> 5) & 1, f"ring {name} running, the other selected: event 8 not static at 0"
    # the oscillator bit into gshare: GRAND, GSRC = 1, ring A; event 9 must show both values over the trainings
    await h.spi(0x11, [bpx1.FCTL_ROA | bpx1.FCTL_GRAND | bpx1.FCTL_GSRC | bpx1.FCTL_DBGF])
    seen = set()
    rng = random.Random(SEED + 10001)
    for n in range(60):
        await h.request(bpx1.OP_PREDICT_RESOLVE_T if rng.random() < 0.5 else bpx1.OP_PREDICT_RESOLVE_NT, rng.randrange(256))
        seen.add(h.pins[3])                               # DBG1 = event 9 with the ACK
    assert seen == {0, 1}, f"event 9 with the oscillator bit: values {seen}"
    # all stopped: static
    await h.spi(0x11, [bpx1.FCTL_DBGF])
    await ClockCycles(dut.clk, 6)
    n0, _, last = await watch(200)
    assert n0 == 0 and not (last >> 5) & 1, "rings stopped: event 8 not static at 0"
    dut._log.info(f"ring oscillators: simulated frequency from event 8 {res} MHz; oscillator bit reaches gshare (event 9 {sorted(seen)})")
