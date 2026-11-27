// bpx_chip - BPX-1 at the Tiny Tapeout pin frame (docs/SPEC.md section 2): synchronisers, operation sequencer,
// control port.  Every input passes through flops clocked by clk before it is used; every output is a flop.
//
//   ui_in[7:0]  ID[7:0]         uio[3:2] OP      uio[4] CSn  uio[5] SCK  uio[6] MOSI  uio[7] REQ
//   uo_out[0] ACK  [1] PRED  [2] CONF  [3] FULL  [4] READY  [5] DBG0  [6] DBG1  [7] MISO
//   uio[0] PBIM, uio[1] PGSH (outputs): the last predictions of the bimodal and the gshare predictor.  After
//   reset DBG0 and DBG1 show those of the default predictor and the tournament: all four are on pins.
//
// Clock-domain crossings (RR-01), all into the single domain clk:
//   rst_n            2 flops, then used as a synchronous reset (hold rst_n low for at least 4 clk cycles)
//   REQ              2 flops (request detection)
//   ID, OP           1 flop; used only in cycles where the synchronised REQ shows a pending request, by which
//                    time the pins have been stable for more than a clock period (the host holds them until ACK)
//   CSn, SCK, MOSI   2 flops each; the control port samples them, SCK is not used as a clock
//   OP, CSn, MOSI    also form the reset strap, taken by the core at the last clock edge of rst: the pins must be
//                    stable from 4 clocks before rst_n rises until 4 clocks after (docs/SPEC.md)
`default_nettype none

module bpx_chip #(
    parameter W   = 8,
    parameter LGB = 6,
    parameter LGG = 7,
    parameter HG  = 4,
    parameter NT  = 4,
    parameter LGT = 5,
    parameter CB  = 4,
    parameter [NT*6-1:0] LV = {6'd20, 6'd9, 6'd8, 6'd0},
    parameter HL  = 21,
    parameter PL  = 8,
    parameter UBW = 2,
    parameter UGW = 4,
    parameter UTW = 4,
    parameter SK  = 0,
    parameter THETA = 10,
    parameter SB  = 8,
    parameter FEAT = 3'b111,       // features of the default predictor enabled after reset (CTRL[14:12])
    parameter STATW = 16,
    parameter [7:0] TWMASK = 8'hF0 // code mask of the leaky twin: with CTRL[15] MASK it stores code word ^ TWMASK (docs/mosaic.md 7.7;
                                   // the golden model's bpx1.TWMASK must be the same value)
) (
    input  wire [7:0] ui_in,
    output wire [7:0] uo_out,
    input  wire [7:0] uio_in,
    output wire [7:0] uio_out,
    output wire [7:0] uio_oe,
    input  wire       ena,
    input  wire       clk,
    input  wire       rst_n
);
  wire [3:0] lastp;
  assign uio_out = {6'd0, lastp[2], lastp[1]};
  assign uio_oe  = 8'h03;           // uio[1:0] are outputs, the others inputs, always

  reg        rst_m, rst;
  reg        req_m, req_s;
  reg  [7:0] id_d;
  reg  [1:0] op_d;
  reg        csn_m, csn_s, sck_m, sck_s, mosi_m, mosi_s;
  always @(posedge clk) begin
    rst_m  <= ~rst_n;
    rst    <= rst_m;
    req_m  <= uio_in[7];
    req_s  <= req_m;
    id_d   <= ui_in;
    op_d   <= uio_in[3:2];
    csn_m  <= uio_in[4];
    csn_s  <= csn_m;
    sck_m  <= uio_in[5];
    sck_s  <= sck_m;
    mosi_m <= uio_in[6];
    mosi_s <= mosi_m;
  end

  wire        ack, pred, conf, full, ready, miso;
  wire [1:0]  dbg;
  wire [5:0]  reg_addr;
  wire        reg_we, reg_re;
  wire [15:0] reg_wdata, reg_rdata;

  bpx_spi u_spi (
      .clk(clk), .rst(rst), .sck(sck_s), .mosi(mosi_s), .csn(csn_s), .miso(miso),
      .addr(reg_addr), .we(reg_we), .re(reg_re), .wdata(reg_wdata), .rdata(reg_rdata));

  // reset strap (docs/mosaic.md 7.7): OP = 3, CSn = 0 and MOSI = 1 at the last clock edge of reset keep the tables
  // (OP alone is an ordinary request code that a host may leave on the pins)
  wire strap = (op_d == 2'b11) & ~csn_s & mosi_s;

  // ring oscillators, divider and random bit (docs/mosaic.md 7.7)
  wire [2:0] ro_ctl;
  wire       ro_div, ro_rnd;
  bpx_ro u_ro (.clk(clk), .run_a(ro_ctl[0]), .run_b(ro_ctl[1]), .sel(ro_ctl[2]), .div(ro_div), .rnd(ro_rnd));

  bpx_core #(.W(W), .LGB(LGB), .LGG(LGG), .HG(HG), .NT(NT), .LGT(LGT), .CB(CB), .LV(LV), .HL(HL), .PL(PL), .UBW(UBW), .UGW(UGW), .UTW(UTW), .SK(SK), .THETA(THETA), .SB(SB), .FEAT(FEAT), .STATW(STATW), .TWMASK(TWMASK)) u_core (
      .clk(clk), .rst(rst), .strap(strap), .ro_ctl(ro_ctl), .ro_div(ro_div), .ro_rnd(ro_rnd),
      .req_s(req_s), .id_d(id_d[W-1:0]), .op_d(op_d),
      .ack(ack), .pred(pred), .conf(conf), .full(full), .ready(ready), .dbg(dbg), .lastp(lastp),
      .reg_addr(reg_addr), .reg_we(reg_we), .reg_re(reg_re), .reg_wdata(reg_wdata), .reg_rdata(reg_rdata));

  assign uo_out = {miso, dbg[1], dbg[0], ready, full, conf, pred, ack};

  wire _unused = &{1'b0, ena, uio_in[1:0], lastp[3], lastp[0]};
endmodule
