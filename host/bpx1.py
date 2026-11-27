# BPX-1 host for the Tiny Tapeout demo board (MicroPython on the board's RP2040/RP2350), use case U2.
#
#   from bpx1 import Bpx1Host, demo
#   demo()                         # select the project, check the ID, replay a small trace, print the statistics
#
# The protocol code (class Bpx1Host) only needs four functions for the pins, so it also runs under CPython or
# cocotb for testing; class TTPins binds it to the demo board through the Tiny Tapeout MicroPython SDK.
# The same sequences are exercised against the RTL by test/test.py (class Host there); this file itself has not
# been run on hardware yet (docs/bringup.md).
#
# Pins (docs/SPEC.md section 2):
#   ui_in[7:0] ID[7:0]   uio[3:2] OP   uio[4] CSn   uio[5] SCK   uio[6] MOSI   uio[7] REQ
#   uio[0] PBIM, uio[1] PGSH: outputs of the chip (never drive them)
#   uo_out[0] ACK  [1] PRED  [2] CONF  [3] FULL  [4] READY  [5] DBG0  [6] DBG1  [7] MISO

OP_SPECIAL, OP_PREDICT, OP_PR_NT, OP_PR_T = 0, 1, 2, 3
SUB_NOP, SUB_FLUSH, SUB_RESOLVE_NT, SUB_RESOLVE_T = 0, 1, 2, 3
REG_ID, REG_CTRL, REG_CMD, REG_STATUS, REG_ERRCNT, REG_STATS, REG_RAW_ADDR, REG_RAW_DATA = 0, 1, 2, 3, 4, 5, 0x0F, 0x10
REG_FCTL, REG_FSTAT = 0x11, 0x12
CMD_SOFT_RESET, CMD_CLEAR_PRED, CMD_CLEAR_STATS, CMD_SNAPSHOT, CMD_SNAPSHOT_CLEAR = 1, 2, 4, 8, 16
# mosaic experiments (docs/mosaic.md 7.2): FCTL bits; FCTL = 0 is the plain chip
FCTL_TWIN_EN, FCTL_TWIN_USE, FCTL_RESTORE, FCTL_DBGF = 1, 2, 16, 0x4000
FCTL_PULSE_SHIFT, FCTL_DEMOTE, FCTL_CTEST = 5, 0x2000, 0x8000
# gshare randomness (GRAND; GSRC 0 the LFSR, 1 the oscillator bit), ring oscillators A and B run, divider takes B
FCTL_GRAND, FCTL_GSRC, FCTL_ROA, FCTL_ROB, FCTL_ROSEL = 0x100, 0x200, 0x400, 0x800, 0x1000
CODE_NONE, CODE_PARITY, CODE_SECDED, CODE_DETECT = 0, 1, 2, 3
# FSTAT (sticky, cleared by CMD_CLEAR_STATS and reset; DEMOTED and KEPT are live) and fault events on DBG with
# FCTL_DBGF: 0-3 twin flags, 4 canary (one clock after the ACK), 5 any checker, 6 watchdog, 7 DEMOTED, 8 the selected
# ring oscillator / 256, 9 the random bit of the last training before the last predict, 10 default with W3
TW_ERR, TW_COR, TW_DET, TW_SDC = 1, 2, 4, 8
CANARY, CHK_QUEUE, CHK_STAGE, CHK_WSEQ, CHK_HANDSHAKE, WDOG, DEMOTED, KEPT = 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800
# with CTRL[15] MASK = 1 the leaky twin stores code word ^ TWMASK (the chip's parameter; model/bpx1.py TWMASK is the
# same value); after switching MASK clear the predictors (CMD_CLEAR_PRED): older words read as faulty otherwise
TWMASK = 0xF0
CTRL_MASK = 0x8000
# reset strap: these uio pins while reset ends keep the tables (OP = 3, CSn = 0, MOSI = 1, SCK low, REQ unchanged)
STRAP_UIO = 0x4C
# write-pulse setting of W0-W2 (FCTL[7:5]): 0 standard (half a clock); 1-7 the tap after 1, 2, 3, 4, 6, 8, 12
# delay cells, typically about 0.6 ns each (docs/mosaic.md 7.4 has the widths per corner)
PULSE_CELLS = (0, 1, 2, 3, 4, 6, 8, 12)
SPACE_BC, SPACE_G, SPACE_SMALL, SPACE_W0, SPACE_W3, SPACE_TWIN = 0, 1, 7, 8, 11, 12
DRIVE = ("default", "bimodal", "gshare", "tournament", "static not-taken")


def fold_id(pc):
    """Branch address -> 8-bit identifier (IR-05): XOR of the 8-bit slices of pc[31:2]."""
    v, r = (pc >> 2) & 0x3FFFFFFF, 0
    while v:
        r ^= v & 0xFF
        v >>= 8
    return r


class Bpx1Host:
    """Protocol over four pin functions: set_ui(byte), set_uio(byte), get_uo() -> byte, idle() (lets the chip clock
    run for at least one period; with a free-running clock of 1 MHz or more it can be an empty function)."""

    def __init__(self, set_ui, set_uio, get_uo, idle, get_uio=None):
        self.set_ui, self.set_uio, self.get_uo, self.idle = set_ui, set_uio, get_uo, idle
        self.get_uio = get_uio              # optional: reads uio (PBIM, PGSH) for the per-predictor counts
        self.uio = 0x10                      # CSn high, REQ low
        self.set_ui(0)
        self.set_uio(self.uio)

    # ---------------------------------------------------------------- predict port (two-phase handshake)
    def sync(self):
        """After reset: wait for READY and align REQ with ACK, so that no request is pending."""
        for _ in range(1000):
            if self.get_uo() & 0x10:
                break
            self.idle()
        else:
            raise RuntimeError("READY did not rise")
        self.uio = (self.uio & 0x7F) | ((self.get_uo() & 1) << 7)
        self.set_uio(self.uio)

    def request(self, op, ident=0):
        """One request; returns (pred, conf)."""
        ack = self.get_uo() & 1
        self.set_ui(ident & 0xFF)
        self.uio = (self.uio & 0xF0) | ((op & 3) << 2)
        self.set_uio(self.uio)               # ID and OP first
        self.uio ^= 0x80
        self.set_uio(self.uio)               # then toggle REQ
        for _ in range(1000):
            v = self.get_uo()
            if (v & 1) != ack:
                return (v >> 1) & 1, (v >> 2) & 1
            self.idle()
        raise RuntimeError("no acknowledge")

    def predict(self, ident):
        return self.request(OP_PREDICT, ident)

    def resolve(self, taken):
        self.request(OP_SPECIAL, SUB_RESOLVE_T if taken else SUB_RESOLVE_NT)

    def predict_resolve(self, ident, taken):
        """Predict branch `ident` and train on its outcome in one request (trace replay, one branch in flight)."""
        return self.request(OP_PR_T if taken else OP_PR_NT, ident)

    def flush(self):
        self.request(OP_SPECIAL, SUB_FLUSH)

    def full(self):
        return (self.get_uo() >> 3) & 1

    # ---------------------------------------------------------------- control port (SPI mode 0, bit-banged)
    def _spi_bits(self, value, nbits):
        got = 0
        for i in range(nbits - 1, -1, -1):
            self.uio = (self.uio & ~0x60) | (((value >> i) & 1) << 6)
            self.set_uio(self.uio)           # MOSI, SCK low
            for _ in range(4):
                self.idle()
            self.uio |= 0x20
            self.set_uio(self.uio)           # SCK high: the chip samples MOSI; MISO is valid
            got = (got << 1) | ((self.get_uo() >> 7) & 1)
            for _ in range(4):
                self.idle()
        self.uio &= ~0x20
        self.set_uio(self.uio)
        return got

    def _spi(self, addr, words, n):
        self.uio &= ~0x10
        self.set_uio(self.uio)               # CSn low
        for _ in range(4):
            self.idle()
        self._spi_bits((0x80 if words is not None else 0) | (addr & 0x3F), 8)
        out = []
        if words is not None:
            for w in words:
                self._spi_bits(w & 0xFFFF, 16)
        else:
            for _ in range(n):
                out.append(self._spi_bits(0, 16))
        for _ in range(4):
            self.idle()
        self.uio |= 0x10
        self.set_uio(self.uio)               # CSn high
        for _ in range(4):
            self.idle()
        return out

    def read(self, addr, n=1):
        """Read n consecutive 16-bit registers."""
        return self._spi(addr, None, n)

    def write(self, addr, *words):
        self._spi(addr, words, 0)

    def chip_id(self):
        return self.read(REG_ID)[0]          # 0xB101

    def set_drive(self, drive=0, freeze=False, features=7, dbg0=10, dbg1=13, mask=False):
        """Which predictor answers the host: 0 default, 1 bimodal, 2 gshare, 3 tournament, 4 static not-taken.
        features (CTRL[14:12]) of the default predictor: 1 reference inputs, 2 negation in memory, 4 adaptive
        threshold (7 = as after reset); 0 = safe mode, the plain perceptron.
        dbg0, dbg1: events on the DBG pins (10 and 13: the last predictions of the default predictor and the
        tournament, as after reset).  mask: CTRL[15] MASK, the leaky twin's code mask (TWMASK); clear the predictors
        after changing it."""
        self.write(REG_CTRL, (drive & 7) | (8 if freeze else 0) | ((dbg0 & 15) << 4) | ((dbg1 & 15) << 8) | ((features & 7) << 12)
                   | (CTRL_MASK if mask else 0))

    def command(self, bits):
        self.write(REG_CMD, bits)
        for _ in range(16):
            self.idle()

    def stats(self, clear=False):
        """Snapshot the chip's two counters (optionally restarting them) and read them: resolved branches and the
        mispredictions of the answers given on PRED (the driving predictor).  Per-predictor counts are the host's job
        since mosaic step 5: replay() counts them from the prediction pins."""
        self.command(CMD_SNAPSHOT_CLEAR if clear else CMD_SNAPSHOT)
        w = self.read(REG_STATUS, 12)
        c = [w[2 + 2 * k] | (w[3 + 2 * k] << 16) for k in range(2)]
        return dict(resolved=c[0], mis_drive=c[1],
                    errors=w[1] & 0xFF, err_flags=(w[0] >> 8) & 0xF, in_flight=w[0] & 7)

    def dump(self, space, count):
        """Raw state (FR-10): space 0 bimodal+chooser, 1 gshare, 7 small registers, 8.. weight tables,
        12 the leaky twin of W3 (its nodes: {check[3:0], data[3:0]}, ^ TWMASK with MASK; reads 0 unless FCTL_TWIN_EN)."""
        self.write(REG_RAW_ADDR, 0x8000 | (space << 8))
        for _ in range(8):
            self.idle()
        return [self.read(REG_RAW_DATA)[0] for _ in range(count)]

    def raw_write(self, space, addr, value, fields=3):
        """Raw write of one entry (fields: write enables, bit 0 first field, bit 1 second field).  Fault injection:
        a twin word (space 12) or W3 (space 11, which leaves the twin as it is)."""
        self.write(REG_RAW_ADDR, (fields << 12) | (space << 8) | (addr & 0x7F))
        for _ in range(8):
            self.idle()
        self.write(REG_RAW_DATA, value)
        for _ in range(12):
            self.idle()

    # ---------------------------------------------------------------- mosaic (docs/mosaic.md 7.2, 7.3)
    def set_fctl(self, twin=False, use=False, code=CODE_NONE, restore=False, dbgf=False, pulse=0, demote=False,
                 ctest=False, grand=False, gsrc=0, ro_a=False, ro_b=False, ro_sel=0):
        """twin: let the leaky twin's data through (compare, flags, events); use: the default predictor sums the
        twin's weight instead of W3's; code: protection of the twin (0 none, 1 parity -> abstain, 2 SEC-DED,
        3 detect all -> abstain); restore: restore-on-use; dbgf: the DBG selections index the fault events;
        pulse: write-pulse setting of W0-W2 (0 standard, 1-7 shorter: PULSE_CELLS); demote: after four detected
        faults the tournament drives instead of the default predictor; ctest: canary self-test (fires at every
        predict); grand: gshare counters leave a limit only on a random 1, from the LFSR (gsrc 0) or the
        oscillator (gsrc 1); ro_a, ro_b: ring oscillator A / B runs (A is meant to be kept running, B rested);
        ro_sel: the divider (event 8) and the oscillator bit take ring A (0) or B (1)."""
        v = (FCTL_TWIN_EN if twin else 0) | (FCTL_TWIN_USE if use else 0) | ((code & 3) << 2)
        v |= (FCTL_RESTORE if restore else 0) | (FCTL_DBGF if dbgf else 0) | ((pulse & 7) << FCTL_PULSE_SHIFT)
        v |= (FCTL_DEMOTE if demote else 0) | (FCTL_CTEST if ctest else 0)
        v |= (FCTL_GRAND if grand else 0) | (FCTL_GSRC if gsrc else 0) | (FCTL_ROA if ro_a else 0)
        v |= (FCTL_ROB if ro_b else 0) | (FCTL_ROSEL if ro_sel else 0)
        self.write(REG_FCTL, v)
        return v

    def fstat(self, clear=False):
        """Fault flags: TW_ERR, TW_COR, TW_DET, TW_SDC, CANARY, the four checkers, WDOG (sticky), DEMOTED and KEPT
        (live); clear=True clears the sticky ones, the fault count and DEMOTED, with the statistics (CMD bit 2)."""
        v = self.read(REG_FSTAT)[0]
        if clear:
            self.command(CMD_CLEAR_STATS)
        return v

    @staticmethod
    def twin_word(weight, mask=False):
        """The node values (raw space 12) of a twin entry holding a 4-bit weight: the code word {check, data}, with
        c_k = XOR of the three data bits other than d_k, XOR TWMASK if MASK (CTRL[15]) is set."""
        d = weight & 15
        b = [(d >> k) & 1 for k in range(4)]
        return (sum((b[0] ^ b[1] ^ b[2] ^ b[3] ^ b[k]) << (4 + k) for k in range(4)) | d) ^ (TWMASK if mask else 0)

    # ---------------------------------------------------------------- trace replay
    def replay(self, trace):
        """trace: iterable of (identifier, taken).  Returns the number of mispredictions of the driving predictor and
        counts the per-predictor mispredictions from the pins in self.counts (the chip keeps only resolved branches
        and wrong answers since mosaic step 5): default and tournament from DBG0/DBG1, bimodal and gshare from
        PBIM/PGSH when a get_uio function was given.  DBG0/DBG1 show the default and tournament predictions only with
        the standard selections (CTRL events 10 and 13, as after reset) and FCTL[14] DBGF = 0: with the fault-event
        bank selected they show fault events, and those two counts are then meaningless."""
        wrong = 0
        self.counts = dict(mis_default=0, mis_bimodal=0, mis_gshare=0, mis_tournament=0, branches=0)
        for ident, taken in trace:
            t = 1 if taken else 0
            pred, _ = self.predict_resolve(ident, t)
            v = self.get_uo()
            wrong += pred != t
            c = self.counts
            c["branches"] += 1
            c["mis_default"] += ((v >> 5) & 1) != t
            c["mis_tournament"] += ((v >> 6) & 1) != t
            if self.get_uio:
                u = self.get_uio()
                c["mis_bimodal"] += (u & 1) != t
                c["mis_gshare"] += ((u >> 1) & 1) != t
        return wrong


class TTPins:
    """Demo-board binding (Tiny Tapeout MicroPython SDK).  The board drives ui_in and all eight uio pins."""

    def __init__(self, clock_hz=1_000_000, project="tt_um_kyrylo_nazarevych_bpx1"):
        from ttboard.demoboard import DemoBoard
        from ttboard.mode import RPMode
        import time
        self.time = time
        tt = DemoBoard.get()
        tt.mode = RPMode.ASIC_RP_CONTROL
        getattr(tt.shuttle, project).enable()
        tt.uio_oe_pico.value = 0xFC          # uio[1:0] are outputs of the chip: the board must not drive them
        tt.ui_in.value = 0
        tt.uio_in.value = 0x10
        tt.clock_project_PWM(clock_hz)
        self.tt = tt
        self.wait_us = max(1, 2_000_000 // clock_hz)
        self.reset()

    def reset(self, strap=False):
        """Pin reset.  strap=True: the reset strap (OP = 3, CSn = 0, MOSI = 1, SCK low while reset ends; docs/SPEC.md)
        keeps the predictor tables (warm reset, or the latches' power-up state after power-up); FSTAT KEPT shows it.
        The pins are held well beyond the 4 clocks before and after the release that the chip needs; afterwards
        CSn goes high (and stays high for more than 4 clocks before the first SPI word, as every word here does)."""
        self.tt.uio_in.value = STRAP_UIO if strap else 0x10
        self.time.sleep_ms(1)
        self.tt.reset_project(True)
        self.time.sleep_ms(2)
        self.tt.reset_project(False)
        self.time.sleep_ms(1)
        self.tt.uio_in.value = 0x10
        self.time.sleep_ms(1)

    def set_ui(self, v):
        self.tt.ui_in.value = v

    def set_uio(self, v):
        self.tt.uio_in.value = v

    def get_uo(self):
        return int(self.tt.uo_out.value)

    def get_uio(self):
        """uio as the RP sees it: bits 1:0 are the chip's PBIM and PGSH outputs (for the per-predictor counts).
        Uses the SDK's uio_out port (not yet run on hardware, like the rest of this file)."""
        return int(self.tt.uio_out.value)

    def idle(self):
        self.time.sleep_us(self.wait_us)


def demo(clock_hz=1_000_000):
    p = TTPins(clock_hz)
    h = Bpx1Host(p.set_ui, p.set_uio, p.get_uo, p.idle, p.get_uio)
    h.sync()
    print("chip id: 0x%04X (expect 0xB101)" % h.chip_id())
    h.command(CMD_CLEAR_PRED | CMD_CLEAR_STATS)
    # a loop of five iterations with an alternating branch inside and a rarely taken exit
    trace = []
    for outer in range(200):
        for inner in range(5):
            trace.append((0x040, inner < 4))
            trace.append((0x023, (outer + inner) & 1))
        trace.append((0x0F1, outer % 9 != 0))
    wrong = h.replay(trace)
    s = h.stats()
    print("branches:", len(trace), "mispredicted by the driving predictor:", wrong)
    print("chip: resolved %d, wrong answers %d" % (s["resolved"], s["mis_drive"]))
    for k in ("mis_default", "mis_bimodal", "mis_gshare", "mis_tournament"):
        n = h.counts[k]
        print("  %-15s %6d  accuracy %.1f %%  (counted from the pins)" % (k, n, 100.0 * (1 - n / max(h.counts["branches"], 1))))
    print("errors:", s["errors"])
    return h
