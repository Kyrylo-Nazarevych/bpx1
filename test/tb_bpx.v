`default_nettype none
`timescale 1ns / 1ps

// Testbench shell for the BPX-1 chip level (bpx_chip) while the repository top is still the area probe.
module tb_bpx ();
  initial begin
    $dumpfile("tb_bpx.fst");
    $dumpvars(0, tb_bpx);
    #1;
  end

  reg clk;
  reg rst_n;
  reg ena;
  reg [7:0] ui_in;
  reg [7:0] uio_in;
  wire [7:0] uo_out;
  wire [7:0] uio_out;
  wire [7:0] uio_oe;

  bpx_chip user_project (
      .ui_in(ui_in), .uo_out(uo_out), .uio_in(uio_in), .uio_out(uio_out), .uio_oe(uio_oe), .ena(ena), .clk(clk), .rst_n(rst_n));
endmodule
