// Behavioural models of the few SKY130 cells that src/ instantiates by name (bpx_ltab.v).
// Used only for RTL simulation without the PDK (the cocotb RTL test, Verilator).  The gate-level test and the
// chip flow use the PDK's own models and never read this file.
`default_nettype none
`timescale 1ns / 1ps

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
