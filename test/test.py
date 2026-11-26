# Probe test (branch probe-ro): the two ring oscillators of src/bpx_ro.v.
# RTL (test/sky130_cells_sim.v, whose xnor2_1 carries a 4 ns delay): each ring is enabled in turn, div toggles at the
# simulated ring frequency / 256 (ring A 125 MHz, ring B 62.5 MHz in this model), rnd changes, and with run = 0 or the
# other ring selected everything is static at 0.
# Gate level (the PDK's models have no delay: a running ring would be a zero-delay loop): both rings stay off and
# only the static state is checked.
import os
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles

GL = os.environ.get("GATES") == "yes"
PERIOD_NS = 30


async def watch(dut, cycles):
    """Sample uo_out every clock for `cycles` clocks; return (changes of div, changes of rnd, other bits seen)."""
    last = None
    ndiv = nrnd = 0
    other = 0
    for _ in range(cycles):
        await ClockCycles(dut.clk, 1)
        v = int(dut.uo_out.value)
        other |= v & 0xFC
        if last is not None:
            ndiv += (v ^ last) & 1
            nrnd += ((v ^ last) >> 1) & 1
        last = v
    return ndiv, nrnd, other


@cocotb.test()
async def ring_oscillators(dut):
    cocotb.start_soon(Clock(dut.clk, PERIOD_NS, unit="ns").start())
    dut.ena.value = 1
    dut.ui_in.value = 0
    dut.uio_in.value = 0
    dut.rst_n.value = 0
    await ClockCycles(dut.clk, 8)
    dut.rst_n.value = 1
    await ClockCycles(dut.clk, 8)

    # both rings off: static
    ndiv, nrnd, other = await watch(dut, 200)
    assert int(dut.uo_out.value) == 0 and ndiv == 0 and nrnd == 0 and other == 0, "outputs not static at 0 with both rings off"
    if GL:
        dut._log.info("ring oscillator probe: ok (gate level: rings kept off, static state only)")
        return

    cycles = 600  # 18 us
    for name, ui, sim_mhz in (("A", 0b001, 125.0), ("B", 0b110, 62.5)):
        dut.ui_in.value = ui
        await ClockCycles(dut.clk, 10)
        ndiv, nrnd, other = await watch(dut, cycles)
        mhz = ndiv / 2 * 256 / (cycles * PERIOD_NS * 1e-3)
        dut._log.info(f"ring {name}: div changed {ndiv} times in {cycles} clocks (ring ~{mhz:.1f} MHz), rnd changed {nrnd} times")
        assert other == 0, "unused outputs not 0"
        assert abs(mhz - sim_mhz) < 0.15 * sim_mhz, f"ring {name}: {mhz:.1f} MHz, expected about {sim_mhz} MHz"
        assert nrnd > 10, f"ring {name}: rnd changed only {nrnd} times"
        # stop it: the divider returns to 0 and stays there
        dut.ui_in.value = ui & 0b100
        await ClockCycles(dut.clk, 6)
        ndiv, nrnd, other = await watch(dut, 100)
        assert int(dut.uo_out.value) == 0 and ndiv == 0 and nrnd == 0, f"ring {name}: not static after run = 0"

    # ring A running but ring B selected (stopped): static
    dut.ui_in.value = 0b101
    await ClockCycles(dut.clk, 6)
    ndiv, nrnd, other = await watch(dut, 200)
    assert int(dut.uo_out.value) == 0 and ndiv == 0 and nrnd == 0, "ring A leaks through while ring B is selected"
    dut.ui_in.value = 0
    await ClockCycles(dut.clk, 6)
    dut._log.info("ring oscillator probe: ok")
