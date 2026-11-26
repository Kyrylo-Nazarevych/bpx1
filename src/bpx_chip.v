// bpx_chip - BPX-1 at the Tiny Tapeout pin frame (docs/SPEC.md section 2): synchronisers, operation sequencer,
// control port.  Every input passes through flops clocked by clk before it is used; every output is a flop.
//
//   ui_in[7:0]  ID[7:0]         uio[1:0] ID[9:8]     uio[3:2] OP      uio[4] CSn  uio[5] SCK  uio[6] MOSI  uio[7] REQ
//   uo_out[0] ACK  [1] PRED  [2] CONF  [3] FULL  [4] READY  [5] DBG0  [6] DBG1  [7] MISO
//
// Clock-domain crossings (RR-01), all into the single domain clk:
//   rst_n            2 flops, then used as a synchronous reset (hold rst_n low for at least 4 clk cycles)
//   REQ              2 flops (request detection)
//   ID, OP           1 flop; used only in cycles where the synchronised REQ shows a pending request, by which
//                    time the pins have been stable for more than a clock period (the host holds them until ACK)
//   CSn, SCK, MOSI   2 flops each; the control port samples them, SCK is not used as a clock
`default_nettype none

module bpx_chip #(
    parameter W   = 10,
    parameter LGB = 6,
    parameter LGG = 7,
    parameter HG  = 6,
    parameter NT  = 3,
    parameter LGT = 4,
    parameter TB  = 7,
    parameter [NT*6-1:0] LV = {6'd20, 6'd9, 6'd4},
    parameter HL  = 20,
    parameter ALLOC_LFSR = 1,
    parameter STATW = 20
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
  assign uio_out = 8'h00;
  assign uio_oe  = 8'h00;           // all bidirectional pins are inputs, always

  reg        rst_m, rst;
  reg        req_m, req_s;
  reg  [9:0] id_d;
  reg  [1:0] op_d;
  reg        csn_m, csn_s, sck_m, sck_s, mosi_m, mosi_s;
  always @(posedge clk) begin
    rst_m  <= ~rst_n;
    rst    <= rst_m;
    req_m  <= uio_in[7];
    req_s  <= req_m;
    id_d   <= {uio_in[1:0], ui_in};
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

  bpx_core #(.W(W), .LGB(LGB), .LGG(LGG), .HG(HG), .NT(NT), .LGT(LGT), .TB(TB), .LV(LV), .HL(HL), .ALLOC_LFSR(ALLOC_LFSR), .STATW(STATW)) u_core (
      .clk(clk), .rst(rst), .req_s(req_s), .id_d(id_d[W-1:0]), .op_d(op_d),
      .ack(ack), .pred(pred), .conf(conf), .full(full), .ready(ready), .dbg(dbg),
      .reg_addr(reg_addr), .reg_we(reg_we), .reg_re(reg_re), .reg_wdata(reg_wdata), .reg_rdata(reg_rdata));

  assign uo_out = {miso, dbg[1], dbg[0], ready, full, conf, pred, ack};

  wire _unused = &{1'b0, ena};
endmodule
