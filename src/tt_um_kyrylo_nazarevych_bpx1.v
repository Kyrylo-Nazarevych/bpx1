// BPX-1 branch prediction coprocessor: Tiny Tapeout top level.  The chip is src/bpx_chip.v; see docs/SPEC.md.
`default_nettype none

module tt_um_kyrylo_nazarevych_bpx1 (
    input  wire [7:0] ui_in,    // ID[7:0]
    output wire [7:0] uo_out,   // {MISO, DBG1, DBG0, READY, FULL, CONF, PRED, ACK}
    input  wire [7:0] uio_in,   // {REQ, MOSI, SCK, CSn, OP[1:0], -, -}
    output wire [7:0] uio_out,  // {0, 0, 0, 0, 0, 0, PGSH, PBIM}
    output wire [7:0] uio_oe,   // always 8'h03
    input  wire       ena,
    input  wire       clk,
    input  wire       rst_n
);
  bpx_chip u_chip (
      .ui_in(ui_in), .uo_out(uo_out), .uio_in(uio_in), .uio_out(uio_out), .uio_oe(uio_oe), .ena(ena), .clk(clk), .rst_n(rst_n));
endmodule
