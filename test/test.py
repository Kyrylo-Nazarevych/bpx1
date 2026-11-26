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
        self.uio = (self.uio & 0x70) | ((ident >> 8) & 3) | ((op & 3) << 2)
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
    for i, (e, tb, _) in enumerate(c.tagged):
        t = await h.dump(2 + i, e)
        exp = [(u << (tb + 3)) | (tag << 3) | ctr for tag, ctr, u in st["tagged"][i]]
        assert t == exp, f"tagged table {i} differs: first mismatch at {next(k for k in range(e) if t[k] != exp[k])}"
    r = await h.dump(7, 5)
    ch, sh, misc = r[0] | (r[1] << 12), r[2] | (r[3] << 12), r[4]
    assert ch == st["ch"], f"committed history {ch:x} != {st['ch']:x}"
    assert sh == st["sh"], f"speculative history {sh:x} != {st['sh']:x}"
    assert misc & 15 == st["ua"], "use-alternate counter differs"
    assert misc >> 4 == st["lfsr"], "allocation LFSR differs"


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
    ids = [rng.randrange(1024) for _ in range(40)]
    drive = 0
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
            op, arg = bpx1.OP_SPECIAL, (bpx1.SUB_RESOLVE_T if outcome(head, hist) else bpx1.SUB_RESOLVE_NT) | (rng.randrange(256) << 2)
        elif r < 0.96:
            op, arg = bpx1.OP_SPECIAL, bpx1.SUB_FLUSH | (rng.randrange(256) << 2)
        elif r < 0.98:
            op, arg = bpx1.OP_SPECIAL, bpx1.SUB_NOP | (rng.randrange(256) << 2)
        else:
            drive = rng.choice([0, 0, 1, 2, 3, 4])
            freeze = 1 if rng.random() < 0.2 else 0
            await h.spi(0x01, [drive | (freeze << 3)])
            m.drive, m.frozen = drive, bool(freeze)
            continue
        exp = m.request(op, arg)
        pred, conf, cyc = await h.request(op, arg)
        lat[cyc] = lat.get(cyc, 0) + 1
        if op != bpx1.OP_SPECIAL:
            assert (pred, conf) == (exp.pred, exp.conf), f"op {n}: op={op} id={arg:#x} got pred={pred} conf={conf}, model pred={exp.pred} conf={exp.conf} refused={exp.refused}"
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
        exp = m.predict_resolve(ident, taken)
        pred, conf, _ = await h.request(bpx1.OP_PREDICT_RESOLVE_T if taken else bpx1.OP_PREDICT_RESOLVE_NT, ident)
        assert (pred, conf) == (exp.pred, exp.conf), f"record {n}: id={ident:#x} got pred={pred} conf={conf}, model pred={exp.pred} conf={exp.conf}"
        wrong += pred != taken
    await ClockCycles(dut.clk, 8)
    await compare_stats(h, m)
    await compare_state(h, m)
    dut._log.info(f"trace replay: {min(OPS, len(trace))} records, default predictor mispredicted {wrong}; statistics and state equal the model")
