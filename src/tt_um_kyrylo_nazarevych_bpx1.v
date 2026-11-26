// Probe (branch probe-ro): the two ring oscillators of the BPX-1 mosaic (docs/mosaic.md 7.7, src/bpx_ro.v) alone on
// the tile, to see what the flow, the precheck and the gate-level simulation make of combinational loops of named
// standard cells.
//   ui_in[0] run A   ui_in[1] run B   ui_in[2] sel (0: A, 1: B)      (synchronised by two flops)
//   uo_out[0] div (selected ring / 256)   uo_out[1] rnd (entropy bit)   uo_out[7:2] 0
`default_nettype none

module tt_um_kyrylo_nazarevych_bpx1 (
    input  wire [7:0] ui_in,
    output wire [7:0] uo_out,
    input  wire [7:0] uio_in,
    output wire [7:0] uio_out,
    output wire [7:0] uio_oe,
    input  wire       ena,
    input  wire       clk,
    input  wire       rst_n
);
  assign uio_out = 8'h00;
  assign uio_oe  = 8'h00;

  reg [2:0] in_m, in_s;
  always @(posedge clk) begin
    in_m <= ui_in[2:0];
    in_s <= in_m;
  end

  wire div, rnd;
  bpx_ro u_ro (.clk(clk), .run_a(in_s[0]), .run_b(in_s[1]), .sel(in_s[2]), .div(div), .rnd(rnd));

  assign uo_out = {6'b0, rnd, div};

  wire _unused = &{1'b0, ena, rst_n, ui_in[7:3], uio_in};
endmodule
