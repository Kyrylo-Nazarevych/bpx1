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
CMD_SOFT_RESET, CMD_CLEAR_PRED, CMD_CLEAR_STATS, CMD_SNAPSHOT, CMD_SNAPSHOT_CLEAR = 1, 2, 4, 8, 16
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

    def __init__(self, set_ui, set_uio, get_uo, idle):
        self.set_ui, self.set_uio, self.get_uo, self.idle = set_ui, set_uio, get_uo, idle
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

    def set_drive(self, drive=0, freeze=False, features=7, dbg0=10, dbg1=13):
        """Which predictor answers the host: 0 default, 1 bimodal, 2 gshare, 3 tournament, 4 static not-taken.
        features (CTRL[15:12]) of the default predictor: 1 reference inputs, 2 negation in memory, 4 adaptive
        threshold (7 = as after reset), 8 binarised inference (experiment); 0 = safe mode, the plain perceptron.
        dbg0, dbg1: events on the DBG pins (10 and 13: the last predictions of the default predictor and the
        tournament, as after reset)."""
        self.write(REG_CTRL, (drive & 7) | (8 if freeze else 0) | ((dbg0 & 15) << 4) | ((dbg1 & 15) << 8) | ((features & 15) << 12))

    def command(self, bits):
        self.write(REG_CMD, bits)
        for _ in range(16):
            self.idle()

    def stats(self, clear=False):
        """Snapshot the counters (optionally restarting them) and read them."""
        self.command(CMD_SNAPSHOT_CLEAR if clear else CMD_SNAPSHOT)
        w = self.read(REG_STATUS, 12)
        c = [w[2 + 2 * k] | (w[3 + 2 * k] << 16) for k in range(5)]
        return dict(resolved=c[0], mis_default=c[1], mis_bimodal=c[2], mis_gshare=c[3], mis_tournament=c[4],
                    errors=w[1] & 0xFF, err_flags=(w[0] >> 8) & 0xF, in_flight=w[0] & 7)

    def dump(self, space, count):
        """Raw state (FR-10): space 0 bimodal+chooser, 1 gshare, 7 small registers, 8.. weight tables."""
        self.write(REG_RAW_ADDR, 0x8000 | (space << 8))
        for _ in range(8):
            self.idle()
        return [self.read(REG_RAW_DATA)[0] for _ in range(count)]

    # ---------------------------------------------------------------- trace replay
    def replay(self, trace):
        """trace: iterable of (identifier, taken).  Returns the number of mispredictions of the driving predictor."""
        wrong = 0
        for ident, taken in trace:
            pred, _ = self.predict_resolve(ident, 1 if taken else 0)
            wrong += pred != (1 if taken else 0)
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
        tt.reset_project(True)
        time.sleep_ms(2)
        tt.reset_project(False)
        self.tt = tt
        self.wait_us = max(1, 2_000_000 // clock_hz)

    def set_ui(self, v):
        self.tt.ui_in.value = v

    def set_uio(self, v):
        self.tt.uio_in.value = v

    def get_uo(self):
        return int(self.tt.uo_out.value)

    def idle(self):
        self.time.sleep_us(self.wait_us)


def demo(clock_hz=1_000_000):
    p = TTPins(clock_hz)
    h = Bpx1Host(p.set_ui, p.set_uio, p.get_uo, p.idle)
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
    for k in ("mis_default", "mis_bimodal", "mis_gshare", "mis_tournament"):
        print("  %-15s %6d  accuracy %.1f %%" % (k, s[k], 100.0 * (1 - s[k] / max(s["resolved"], 1))))
    print("errors:", s["errors"])
    return h
