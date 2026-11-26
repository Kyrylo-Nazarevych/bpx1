# BPX-1 timing constraints: the flow's own constraints (LibreLane base.sdc: clock, I/O delays, loads, derating)
# plus one exception.
#
# The leaky twin W3L (src/bpx_pred.v, u_tw: bpx_ltab with DYN = 1) stores each bit on the output node of a
# tri-state inverter sky130_fd_sc_hd__einvp_1 whose enable TE is the table's write gate, i.e. the write pulse of
# bpx_wseq.  Without an exception, static timing follows TE -> Z as an ordinary combinational arc: from the pulse
# flops `a` (rising edge, start of the pulse cycle) and `b` (falling edge, middle of it) through the twin's read
# tree and decoder into the sum.  Those paths are false: the node changes only while the pulse is high, inside the
# pulse cycle, and no flop takes table read data in that cycle (every capture enable - predict, staging, raw read,
# resolve - requires the write sequencer to be idle; docs/SPEC.md 6.2, the same argument as for the latch tables).
# The data arc A -> Z (from the captured write data, a full cycle before the pulse) and the read paths from the
# address flops stay timed.  Measured on the step-3 build (docs/mosaic.md section 9): worst setup at the slow
# corner -2.60 ns with the arc, +3.71 ns without it, on the true path address flop -> twin read -> decoder -> CONF.
# The cell is used nowhere else in the design.
source $::env(FALLBACK_SDC)
set_disable_timing [get_lib_cells */sky130_fd_sc_hd__einvp_1] -from TE -to Z
