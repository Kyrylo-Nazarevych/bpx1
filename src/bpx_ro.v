// bpx_ro - two ring oscillators of named standard cells, a ripple divider by 256 and an entropy bit
// (docs/mosaic.md 7.7).
//
// Each ring is one enable gate (sky130_fd_sc_hd__nand2_1: A = run, B = feedback) followed by an even number of
// inverting stages and one or two non-inverting stages, all named cells marked (* keep *), of the cell types that
// sit on the main design's critical path, so that the frequency tracks that path:
//   a222oi_1 / a22oi_1 / nand3_1 with all inputs tied together (inverters),
//   xor2_1 with B = run (an inverter while run = 1),
//   xnor2_1 with B = run (a non-inverting stage while run = 1; the simulation's delay stage, see below),
//   inv_1.
//   Ring A: nand2 + 4 x (a222oi, xor2, a22oi, nand3) + xnor2                 = 18 cells, 17 inverting, 137.6 um2
//   Ring B: nand2 + 3 x (a222oi, xor2, a22oi, nand3) + a222oi + inv + 2 xnor2 = 17 cells, 15 inverting, 128.9 um2
// Estimated from the liberty tables (NLDM, each stage loaded by the next stage's tied inputs plus 1.5 fF of wire,
// slowest arc of the tied inputs, not measured): ring A 124 MHz, ring B 137 MHz typical (tt, 25 C, 1.80 V);
// 65 / 71 MHz slow (ss, 100 C, 1.60 V); 222 / 244 MHz fast (ff, -40 C, 1.95 V).  Routing adds delay: lower in silicon.
//
// run = 0: the enable gate's output is constant 1 and every stage after it is static (no switching current).
//
// Divider: 8 toggle flops in ripple, the first clocked by the selected ring (a plain mux on the ring outputs: it can
// glitch when sel changes, harmless here), each next one by the previous flop's output.  The flops are held at 0
// (asynchronous reset) while the selected ring is stopped.  div = bit 7 = ring frequency / 256.
// rnd = bit 0 (ring frequency / 2) sampled by two flops in the clk domain: an entropy bit.
//
// Simulation.  No zero-delay loop exists in any simulation:
//   - cells-level RTL simulation (test/sky130_cells_sim.v): the model of sky130_fd_sc_hd__xnor2_1 carries an
//     inertial delay of 4 ns (it stands for the whole ring; the other models have no delay).  Simulated
//     frequencies: ring A (one xnor2) 125 MHz, ring B (two) 62.5 MHz.  These are not the silicon's frequencies.
//   - gate level with the PDK's models (FUNCTIONAL, combinational cells without delay): a running ring would be a
//     zero-delay loop, so the gate-level test keeps both run bits at 0.
//   - BPX_BEHAV or FORMAL: no ring at all, div = 0, rnd = 0.
`default_nettype none

module bpx_ro (
    input  wire clk,
    input  wire run_a,
    input  wire run_b,
    input  wire sel,      // 0: ring A drives the divider, 1: ring B
    output wire div,      // selected ring / 256
    output wire rnd       // divider bit 0 sampled in the clk domain
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
  bpx_ring_a u_ra (.run(run_a), .osc(osc_a));
  bpx_ring_b u_rb (.run(run_b), .osc(osc_b));

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
  assign div = q[7];

  reg r_m, r_s;
  always @(posedge clk) begin
    r_m <= q[0];
    r_s <= r_m;
  end
  assign rnd = r_s;
`endif
endmodule

`ifndef FORMAL
`ifndef BPX_BEHAV
// Ring A: nand2 + 4 x (a222oi, xor2, a22oi, nand3) + xnor2.
module bpx_ring_a (
    input  wire run,
    output wire osc
);
  wire [17:0] n;
  (* keep *) sky130_fd_sc_hd__nand2_1 u_en (.A(run), .B(n[17]), .Y(n[0]));
  genvar g;
  generate
    for (g = 0; g < 4; g = g + 1) begin : g_st
      (* keep *) sky130_fd_sc_hd__a222oi_1 u0 (.A1(n[4*g]), .A2(n[4*g]), .B1(n[4*g]), .B2(n[4*g]), .C1(n[4*g]), .C2(n[4*g]), .Y(n[4*g+1]));
      (* keep *) sky130_fd_sc_hd__xor2_1   u1 (.A(n[4*g+1]), .B(run), .X(n[4*g+2]));
      (* keep *) sky130_fd_sc_hd__a22oi_1  u2 (.A1(n[4*g+2]), .A2(n[4*g+2]), .B1(n[4*g+2]), .B2(n[4*g+2]), .Y(n[4*g+3]));
      (* keep *) sky130_fd_sc_hd__nand3_1  u3 (.A(n[4*g+3]), .B(n[4*g+3]), .C(n[4*g+3]), .Y(n[4*g+4]));
    end
  endgenerate
  (* keep *) sky130_fd_sc_hd__xnor2_1 u_dl (.A(n[16]), .B(run), .Y(n[17]));
  assign osc = n[17];
endmodule

// Ring B: nand2 + 3 x (a222oi, xor2, a22oi, nand3) + a222oi + inv + 2 x xnor2.
module bpx_ring_b (
    input  wire run,
    output wire osc
);
  wire [16:0] n;
  (* keep *) sky130_fd_sc_hd__nand2_1 u_en (.A(run), .B(n[16]), .Y(n[0]));
  genvar g;
  generate
    for (g = 0; g < 3; g = g + 1) begin : g_st
      (* keep *) sky130_fd_sc_hd__a222oi_1 u0 (.A1(n[4*g]), .A2(n[4*g]), .B1(n[4*g]), .B2(n[4*g]), .C1(n[4*g]), .C2(n[4*g]), .Y(n[4*g+1]));
      (* keep *) sky130_fd_sc_hd__xor2_1   u1 (.A(n[4*g+1]), .B(run), .X(n[4*g+2]));
      (* keep *) sky130_fd_sc_hd__a22oi_1  u2 (.A1(n[4*g+2]), .A2(n[4*g+2]), .B1(n[4*g+2]), .B2(n[4*g+2]), .Y(n[4*g+3]));
      (* keep *) sky130_fd_sc_hd__nand3_1  u3 (.A(n[4*g+3]), .B(n[4*g+3]), .C(n[4*g+3]), .Y(n[4*g+4]));
    end
  endgenerate
  (* keep *) sky130_fd_sc_hd__a222oi_1 u_x0 (.A1(n[12]), .A2(n[12]), .B1(n[12]), .B2(n[12]), .C1(n[12]), .C2(n[12]), .Y(n[13]));
  (* keep *) sky130_fd_sc_hd__inv_1    u_x1 (.A(n[13]), .Y(n[14]));
  (* keep *) sky130_fd_sc_hd__xnor2_1  u_dl0 (.A(n[14]), .B(run), .Y(n[15]));
  (* keep *) sky130_fd_sc_hd__xnor2_1  u_dl1 (.A(n[15]), .B(run), .Y(n[16]));
  assign osc = n[16];
endmodule
`endif
`endif
