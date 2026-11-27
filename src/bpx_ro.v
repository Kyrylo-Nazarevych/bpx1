// bpx_ro - two identical ring oscillators of named standard cells, a ripple divider by 256 and an entropy bit
// (docs/mosaic.md 7.7).  The probe (branch probe-ro) built ring B differently; here both rings are ring A, so that
// the ageing experiment compares a ring kept running (A) with an identical one that is rested (B).
//
// Each ring is one enable gate (sky130_fd_sc_hd__nand2_1: A = run, B = feedback) followed by 16 inverting stages and
// one non-inverting stage, all named cells marked (* keep *), of the cell types that sit on the main design's
// critical path (the read trees and the sum), so that the frequency tracks that path:
//   a222oi_1 / a22oi_1 / nand3_1 with all inputs tied together (inverters),
//   xor2_1 with B = run (an inverter while run = 1),
//   xnor2_1 with B = run (a non-inverting stage while run = 1; the simulation's delay stage, see below).
//   Ring: nand2 + 4 x (a222oi, xor2, a22oi, nand3) + xnor2 = 18 cells, 17 inverting, 137.6 um2, plus a buffer
//   (buf_2) that drives the run pins.
// Estimated from the liberty tables in the probe (NLDM, each stage loaded by the next stage's tied inputs plus 1.5 fF
// of wire, not measured): 124 MHz typical (tt, 25 C, 1.80 V), 65 MHz slow (ss, 100 C, 1.60 V), 222 MHz fast (ff,
// -40 C, 1.95 V).  Routing adds delay: lower in silicon.  The flow may not resize any ring cell
// (RSZ_DONT_TOUCH_RX in src/config.json), so the two rings stay cell for cell identical.
//
// run = 0: the enable gate's output is constant 1 and every stage after it is static (no switching current).
//
// Divider: 8 toggle flops in ripple, the first clocked by the selected ring (a plain mux on the ring outputs: it can
// glitch when sel changes, harmless here), each next one by the previous flop's output.  The flops are held at 0
// (asynchronous reset) while the selected ring is stopped.  div = bit 7 = ring frequency / 256, taken into the clk
// domain by one flop (the DBG flop behind it is the second).  rnd = bit 0 (ring frequency / 2) sampled by two flops
// in the clk domain: the oscillator's random bit (gshare, FCTL[9]).
//
// Simulation.  No zero-delay loop exists in any simulation:
//   - cells-level RTL simulation (test/sky130_cells_sim.v): the model of sky130_fd_sc_hd__xnor2_1 carries an
//     inertial delay of 4 ns (it stands for the whole ring; the other models have no delay): both rings simulate at
//     125 MHz.  These are not the silicon's frequencies.
//   - gate level with the PDK's models (FUNCTIONAL, combinational cells without delay): a running ring would be a
//     zero-delay loop, so the gate-level tests keep both run bits at 0.
//   - BPX_BEHAV or FORMAL: no ring at all, div = 0, rnd = 0.
`default_nettype none

module bpx_ro (
    input  wire clk,
    input  wire run_a,
    input  wire run_b,
    input  wire sel,      // 0: ring A drives the divider, 1: ring B
    output wire div,      // selected ring / 256, one flop in the clk domain
    output wire rnd       // divider bit 0 sampled by two flops in the clk domain
);
`ifdef FORMAL
  assign div = 1'b0;
  assign rnd = 1'b0;
  wire _unused_ro = &{1'b0, clk, run_a, run_b, sel};
`elsif BPX_BEHAV
  assign div = 1'b0;
  assign rnd = 1'b0;
  wire _unused_ro = &{1'b0, clk, run_a, run_b, sel};
`else
  wire osc_a, osc_b;
  bpx_ring u_ra (.run(run_a), .osc(osc_a));
  bpx_ring u_rb (.run(run_b), .osc(osc_b));

  wire       osc   = sel ? osc_b : osc_a;
  wire       run_s = sel ? run_b : run_a;
  reg  [7:0] q;
  always @(posedge osc or negedge run_s) begin
    if (!run_s) q[0] <= 1'b0;
    else        q[0] <= ~q[0];
  end
  genvar i;
  generate
    for (i = 1; i < 8; i = i + 1) begin : g_div
      always @(posedge q[i-1] or negedge run_s) begin
        if (!run_s) q[i] <= 1'b0;
        else        q[i] <= ~q[i];
      end
    end
  endgenerate

  reg r_m, r_s, d_m;
  always @(posedge clk) begin
    r_m <= q[0];
    r_s <= r_m;
    d_m <= q[7];
  end
  assign rnd = r_s;
  assign div = d_m;
`endif
endmodule

`ifndef FORMAL
`ifndef BPX_BEHAV
// One ring: nand2 + 4 x (a222oi, xor2, a22oi, nand3) + xnor2 (the probe's ring A).
module bpx_ring (
    input  wire run,
    output wire osc
);
  wire [17:0] n;
  // run reaches the six run pins through a named buffer u_bf.  The flow may not touch the ring's cells and nets
  // (RSZ_DONT_TOUCH_RX matches u_en, g_st*, u_dl and the nets n*, rn, not u_bf), and the resizer stops with an error
  // on a net it wants to repair whose loads are all untouchable (the first build: RSZ-3006 on the FCTL[11] net); so
  // the FCTL flop drives only u_bf (and the divider's mux), which the resizer may resize or buffer in front of, and
  // the untouchable net rn behind it is left alone.
  wire rn;
  (* keep *) sky130_fd_sc_hd__buf_2 u_bf (.A(run), .X(rn));
  (* keep *) sky130_fd_sc_hd__nand2_1 u_en (.A(rn), .B(n[17]), .Y(n[0]));
  genvar g;
  generate
    for (g = 0; g < 4; g = g + 1) begin : g_st
      (* keep *) sky130_fd_sc_hd__a222oi_1 u0 (.A1(n[4*g]), .A2(n[4*g]), .B1(n[4*g]), .B2(n[4*g]), .C1(n[4*g]), .C2(n[4*g]), .Y(n[4*g+1]));
      (* keep *) sky130_fd_sc_hd__xor2_1   u1 (.A(n[4*g+1]), .B(rn), .X(n[4*g+2]));
      (* keep *) sky130_fd_sc_hd__a22oi_1  u2 (.A1(n[4*g+2]), .A2(n[4*g+2]), .B1(n[4*g+2]), .B2(n[4*g+2]), .Y(n[4*g+3]));
      (* keep *) sky130_fd_sc_hd__nand3_1  u3 (.A(n[4*g+3]), .B(n[4*g+3]), .C(n[4*g+3]), .Y(n[4*g+4]));
    end
  endgenerate
  (* keep *) sky130_fd_sc_hd__xnor2_1 u_dl (.A(n[16]), .B(rn), .Y(n[17]));
  // the ring's output leaves through a buffer of its own (u_ob, unprotected), so that the protected net n[17] has
  // only ring cells and this buffer as loads and the divider's mux sits on an ordinary net
  (* keep *) sky130_fd_sc_hd__buf_2 u_ob (.A(n[17]), .X(osc));
endmodule
`endif
`endif
