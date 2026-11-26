// Probe (branch probe-leaky): one dynamic-storage table of the BPX-1 mosaic (docs/mosaic.md) alone on the tile, to
// see what the flow, the precheck and the gate-level simulation make of floating storage nodes.
//   ui_in[3:0] address   ui_in[7:4] data nibble (stored as {~nibble, nibble})
//   uio_in[0] write (rising edge)   uio_in[1] read enable (0: outputs forced to 0)   uio_in[2] clear (rising edge)
//   uo_out[7:0] the addressed entry (while read enable is 1)
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

  reg       rst_m, rst;
  reg [7:0] in_m, in_s;
  reg [2:0] c_m, c_s, c_q;
  reg [7:0] out_q;
  reg       en_q;
  wire      wcap, wact, wpulse, busy;
  wire [7:0] rdata;
  wire      wr  = c_s[0] & ~c_q[0];
  wire      clr = c_s[2] & ~c_q[2];

  always @(posedge clk) begin
    rst_m <= ~rst_n;
    rst   <= rst_m;
    in_m  <= ui_in;
    in_s  <= in_m;
    c_m   <= uio_in[2:0];
    c_s   <= c_m;
    c_q   <= c_s;
    en_q  <= c_s[1] & ~busy;                  // never show a read taken during a write
    out_q <= rdata;
  end

  bpx_wseq u_wseq (.clk(clk), .rst(rst), .req(wr | clr), .wcap(wcap), .wact(wact), .wpulse(wpulse), .busy(busy));

  bpx_ltab #(.AW(4), .RW(2), .EW0(8), .EW1(0), .INIT(8'h00), .DYN(1)) u_dyn (
      .clk(clk), .wcap(wcap), .wact(wact), .wpulse(wpulse), .clr(clr),
      .we0(wr), .we1(1'b0), .waddr(in_s[3:0]), .wdata({~in_s[7:4], in_s[7:4]}),
      .raddr(in_s[3:0]), .rdata(rdata));

  assign uo_out = out_q & {8{en_q}};

  wire _unused = &{1'b0, ena, uio_in[7:3]};
endmodule
