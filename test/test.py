# Probe test (branch probe-leaky): writes and reads the dynamic table.  At gate level the floating nodes are unknown
# in the PDK's models, so there only the masked output (read enable 0) is checked.
import os
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles

GL = os.environ.get("GATES") == "yes"


@cocotb.test()
async def dyn_table(dut):
    cocotb.start_soon(Clock(dut.clk, 30, unit="ns").start())
    dut.ena.value = 1
    dut.ui_in.value = 0
    dut.uio_in.value = 0
    dut.rst_n.value = 0
    await ClockCycles(dut.clk, 8)
    dut.rst_n.value = 1
    await ClockCycles(dut.clk, 4)
    dut.uio_in.value = 4                      # clear
    await ClockCycles(dut.clk, 8)
    dut.uio_in.value = 0
    await ClockCycles(dut.clk, 4)
    for a in range(16):
        dut.ui_in.value = a | ((a ^ 5) << 4)
        await ClockCycles(dut.clk, 3)
        dut.uio_in.value = 1                  # write
        await ClockCycles(dut.clk, 8)
        dut.uio_in.value = 0
        await ClockCycles(dut.clk, 2)
    for a in range(16):
        dut.ui_in.value = a
        dut.uio_in.value = 0
        await ClockCycles(dut.clk, 6)
        assert int(dut.uo_out.value) == 0, "masked output is not 0"
        if not GL:
            dut.uio_in.value = 2              # read enable
            await ClockCycles(dut.clk, 6)
            n = a ^ 5
            assert int(dut.uo_out.value) == ((~n & 15) << 4 | n), f"entry {a} reads {int(dut.uo_out.value):02x}"
    dut._log.info("dynamic table probe: ok" + (" (gate level: masked output only)" if GL else ""))
