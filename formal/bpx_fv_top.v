// bpx_fv_top - formal top: the operation sequencer and the predictor datapath (bpx_core) with every input free.
// The table sizes are macros so that the proofs can run on small tables: the properties (end of src/bpx_core.v)
// are about the sequencer, the queue, the histories and the handshake, whose logic does not depend on how many
// entries a table has.  FV_LGB/FV_LGG/FV_LGT = 6/7/5 is the chip.
// The reset strap and the ring oscillators' outputs (bpx_chip, bpx_ro; docs/mosaic.md 7.7) are free inputs too:
// the strap may take any value in any clock (the proofs assume only a reset in the first clock), and the random
// bit and the divided ring may be anything, so the properties hold whatever the oscillators do.
`default_nettype none
module bpx_fv_top (
    input  wire        clk,
    input  wire        rst,
    input  wire        strap,
    input  wire        ro_div,
    input  wire        ro_rnd,
    input  wire        req_s,
    input  wire [7:0]  id_d,
    input  wire [1:0]  op_d,
    input  wire [5:0]  reg_addr,
    input  wire        reg_we,
    input  wire        reg_re,
    input  wire [15:0] reg_wdata,
    output wire        ack,
    output wire        pred,
    output wire        conf,
    output wire        full,
    output wire        ready,
    output wire [1:0]  dbg,
    output wire [3:0]  lastp,
    output wire [2:0]  ro_ctl,
    output wire [15:0] reg_rdata
);
  bpx_core #(.LGB(`FV_LGB), .LGG(`FV_LGG), .LGT(`FV_LGT)) u_core (
      .clk(clk), .rst(rst), .strap(strap), .ro_ctl(ro_ctl), .ro_div(ro_div), .ro_rnd(ro_rnd),
      .req_s(req_s), .id_d(id_d), .op_d(op_d),
      .ack(ack), .pred(pred), .conf(conf), .full(full), .ready(ready), .dbg(dbg), .lastp(lastp),
      .reg_addr(reg_addr), .reg_we(reg_we), .reg_re(reg_re), .reg_wdata(reg_wdata), .reg_rdata(reg_rdata));
endmodule
