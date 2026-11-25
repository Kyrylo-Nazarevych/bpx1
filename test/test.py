# Gate G0 area probe test (replaced by the BPX-1 test suite at gate G2).
#
# The probe has no meaningful function; what must be shown is that the latch tables behave identically in RTL
# simulation (behavioural cell models) and in gate-level simulation of the routed netlist (the PDK's models):
# reset, the one-pulse clear of every table, then 4000 cycles of pseudo-random writes and reads.  Every output
# sample is folded into a signature; both simulations must produce SIGNATURE, and no output may ever be X or Z
# after reset (RR-02).
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles, FallingEdge

SIGNATURE = 0x062C87D7   # from the RTL run; the gate-level run must reproduce it


def lfsr32(x):
    x ^= (x << 13) & 0xFFFFFFFF
    x ^= x >> 17
    x ^= (x << 5) & 0xFFFFFFFF
    return x & 0xFFFFFFFF


@cocotb.test()
async def test_probe_signature(dut):
    cocotb.start_soon(Clock(dut.clk, 20, unit="ns").start())
    dut.ena.value = 1
    dut.ui_in.value = 0
    dut.uio_in.value = 0
    dut.rst_n.value = 0
    await ClockCycles(dut.clk, 10)
    await FallingEdge(dut.clk)
    dut.rst_n.value = 1
    await ClockCycles(dut.clk, 12)          # synchronised reset release + table clear

    assert int(dut.uio_oe.value) == 0, "all bidirectional pins must be inputs"

    x, sig = 0x1234ABCD, 0xFFFFFFFF
    for cycle in range(4000):
        await FallingEdge(dut.clk)
        v = dut.uo_out.value
        assert v.is_resolvable, f"uo_out has X/Z at cycle {cycle}: {v}"
        sig = lfsr32(sig ^ int(v) ^ (cycle << 8))
        x = lfsr32(x)
        dut.ui_in.value = x & 0xFF
        # shift (bit 7) and write-request (bit 5) on most cycles so that every table is written and re-read
        dut.uio_in.value = ((x >> 8) & 0xFF) | (0x80 if (x >> 20) & 3 else 0) | (0x20 if (x >> 22) & 1 else 0)

    dut._log.info(f"signature = 0x{sig:08X}")
    if SIGNATURE is not None:
        assert sig == SIGNATURE, f"signature 0x{sig:08X} differs from the RTL reference 0x{SIGNATURE:08X}"
