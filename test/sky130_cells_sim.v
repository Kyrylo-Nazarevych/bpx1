// Behavioural models of the few SKY130 cells that src/ instantiates by name (bpx_ltab.v, bpx_pred.v, bpx_core.v,
// bpx_ro.v).
// Used only for RTL simulation without the PDK (the cocotb RTL test, Verilator).  The gate-level test and the
// chip flow use the PDK's own models and never read this file.
`default_nettype none
`timescale 1ns / 1ps

module sky130_fd_sc_hd__buf_2 (
    input  wire A,
    output wire X
);
  assign X = A;
endmodule

module sky130_fd_sc_hd__and2_1 (
    input  wire A,
    input  wire B,
    output wire X
);
  assign X = A & B;
endmodule

// D latch, transparent while GATE_N is low.
module sky130_fd_sc_hd__dlxtn_1 (
    input  wire D,
    input  wire GATE_N,
    output reg  Q
);
  always @(D or GATE_N) if (!GATE_N) Q <= D;
endmodule

module sky130_fd_sc_hd__nand2_1 (
    input  wire A,
    input  wire B,
    output wire Y
);
  assign Y = ~(A & B);
endmodule

module sky130_fd_sc_hd__nand3_1 (
    input  wire A,
    input  wire B,
    input  wire C,
    output wire Y
);
  assign Y = ~(A & B & C);
endmodule

module sky130_fd_sc_hd__or2_1 (
    input  wire A,
    input  wire B,
    output wire X
);
  assign X = A | B;
endmodule

module sky130_fd_sc_hd__a22o_1 (
    input  wire A1,
    input  wire A2,
    input  wire B1,
    input  wire B2,
    output wire X
);
  assign X = (A1 & A2) | (B1 & B2);
endmodule

module sky130_fd_sc_hd__a22oi_1 (
    input  wire A1,
    input  wire A2,
    input  wire B1,
    input  wire B2,
    output wire Y
);
  assign Y = ~((A1 & A2) | (B1 & B2));
endmodule

module sky130_fd_sc_hd__a222oi_1 (
    input  wire A1,
    input  wire A2,
    input  wire B1,
    input  wire B2,
    input  wire C1,
    input  wire C2,
    output wire Y
);
  assign Y = ~((A1 & A2) | (B1 & B2) | (C1 & C2));
endmodule

module sky130_fd_sc_hd__and3b_1 (
    input  wire A_N,
    input  wire B,
    input  wire C,
    output wire X
);
  assign X = ~A_N & B & C;
endmodule

module sky130_fd_sc_hd__xor2_1 (
    input  wire A,
    input  wire B,
    output wire X
);
  assign X = A ^ B;
endmodule

module sky130_fd_sc_hd__dfxtp_1 (
    input  wire CLK,
    input  wire D,
    output reg  Q
);
  always @(posedge CLK) Q <= D;
endmodule

// Delay cell (write-pulse selector, canaries) with a real delay: the typical delay of the liberty file
// (tt_025C_1v80, about 0.57 ns with one cell's load), so that the latch-cell test sees shortened write pulses of
// the right widths (docs/mosaic.md 7.4).  Inertial, like the cell.
/* verilator lint_off ASSIGNDLY */
module sky130_fd_sc_hd__dlygate4sd3_1 (
    input  wire A,
    output wire X
);
  assign #0.57 X = A;
endmodule

// The canaries' delay cell (bpx_canary): about 0.24 ns typical.
module sky130_fd_sc_hd__dlygate4sd1_1 (
    input  wire A,
    output wire X
);
  assign #0.24 X = A;
endmodule

// The ring oscillators' delay stage (src/bpx_ro.v): an inertial delay of 4 ns that stands for a whole ring (the
// other cell models have none), so that a running ring is never a zero-delay loop: each ring (one such stage)
// simulates at 125 MHz.  Use xnor2_1 by name nowhere else, or it inherits this delay.
module sky130_fd_sc_hd__xnor2_1 (
    input  wire A,
    input  wire B,
    output wire Y
);
  assign #4 Y = ~(A ^ B);
endmodule
/* verilator lint_on ASSIGNDLY */

// Tri-state inverter used as a dynamic storage node (bpx_ltab, DYN = 1): this model holds the last driven value
// for ever (an ideal capacitor).  Real silicon leaks; the PDK's gate-level model shows the floating node as unknown.
module sky130_fd_sc_hd__einvp_1 (
    input  wire A,
    input  wire TE,
    output wire Z
);
  reg q;
  always @* if (TE) q = ~A;
  assign Z = q;
endmodule
