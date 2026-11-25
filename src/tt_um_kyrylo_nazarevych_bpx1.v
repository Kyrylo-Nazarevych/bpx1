// BPX-1, gate G0 AREA PROBE (spec EX-04, gate G0).  This is NOT the predictor: it is the calibration vehicle that
// puts the storage-heavy blocks through the complete Tiny Tapeout flow (GDS, precheck, gate-level test) before the
// RTL is written.  It holds the candidate set of latch tables (1,344 bits) around about 300 flops of placeholder
// logic standing in for the control logic, at the real pin frame.  It is replaced by the BPX-1 top level at gate G2.
//
// Stimulus: uio[7] shifts {uio[1:0], ui_in} into an identifier and uio[6] into a history register; uio[5] requests
// a write of all tables at the addresses hashed from them; the outputs are a registered mix of the table reads.
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

  // input and reset synchronisers (two flops)
  reg [7:0] ui_m, ui_q, uio_m, uio_q;
  reg       rst_m, rst_q;
  always @(posedge clk) begin
    ui_m  <= ui_in;
    ui_q  <= ui_m;
    uio_m <= uio_in;
    uio_q <= uio_m;
    rst_m <= ~rst_n;
    rst_q <= rst_m;
  end

  // write sequencer; one clear of all tables after reset, then `ready`
  wire wcap, wact, wpulse, wbusy;
  reg  clr_pend, ready, phase;
  always @(posedge clk) begin
    if (rst_q) begin
      clr_pend <= 1'b1;
      ready    <= 1'b0;
      phase    <= 1'b0;
    end else begin
      phase <= ~phase;
      if (wcap) clr_pend <= 1'b0;
      if (!clr_pend && !wbusy) ready <= 1'b1;
    end
  end
  wire lrst = ~ready;                      // local reset: nothing reads a table before it is initialised
  wire we   = uio_q[5] & phase & ready;
  bpx_wseq u_wseq (.clk(clk), .rst(rst_q), .req(we | clr_pend), .wcap(wcap), .wact(wact), .wpulse(wpulse), .busy(wbusy));

  reg [19:0] hist;
  reg [9:0]  id;
  always @(posedge clk) begin
    if (lrst) begin
      hist <= 20'h0;
      id   <= 10'h0;
    end else if (uio_q[7]) begin
      hist <= {hist[18:0], uio_q[6]};
      id   <= {uio_q[1:0], ui_q};
    end
  end

  wire [6:0]  a_b  = id[6:0] ^ {id[9:7], 4'b0};
  wire [6:0]  a_g  = id[6:0] ^ hist[6:0] ^ {4'b0, id[9:7]};
  wire [5:0]  a_c  = id[5:0] ^ id[9:4];
  wire [4:0]  a_t0 = id[4:0] ^ id[9:5] ^ hist[4:0];
  wire [4:0]  a_t1 = id[4:0] ^ id[9:5] ^ hist[4:0] ^ hist[9:5] ^ hist[14:10];
  wire [6:0]  g_t0 = id[9:3] ^ hist[6:0];
  wire [6:0]  g_t1 = id[9:3] ^ hist[6:0] ^ hist[13:7] ^ {hist[19:14], 1'b0};

  wire [1:0]  r_b, r_g, r_c;
  wire [10:0] r_t0, r_t1;
  wire [1:0]  n_b = r_b + {1'b0, uio_q[4]};
  wire [1:0]  n_g = r_g + {1'b0, uio_q[4]};
  wire [1:0]  n_c = r_c + {1'b0, uio_q[4]};
  wire        hit0 = r_t0[9:3] == g_t0;
  wire        hit1 = r_t1[9:3] == g_t1;

  bpx_ltab #(.AW(7), .RW(3), .EW0(2), .EW1(0), .INIT(2'd1)) u_b (
      .clk(clk), .wcap(wcap), .wact(wact), .wpulse(wpulse), .clr(clr_pend), .we0(we), .we1(1'b0),
      .waddr(a_b), .wdata(n_b), .raddr(a_b), .rdata(r_b));
  bpx_ltab #(.AW(7), .RW(3), .EW0(2), .EW1(0), .INIT(2'd1)) u_g (
      .clk(clk), .wcap(wcap), .wact(wact), .wpulse(wpulse), .clr(clr_pend), .we0(we), .we1(1'b0),
      .waddr(a_g), .wdata(n_g), .raddr(a_g), .rdata(r_g));
  bpx_ltab #(.AW(6), .RW(3), .EW0(2), .EW1(0), .INIT(2'd1)) u_c (
      .clk(clk), .wcap(wcap), .wact(wact), .wpulse(wpulse), .clr(clr_pend), .we0(we), .we1(1'b0),
      .waddr(a_c), .wdata(n_c), .raddr(a_c), .rdata(r_c));
  bpx_ltab #(.AW(5), .RW(2), .EW0(10), .EW1(1), .INIT(11'd3)) u_t0 (
      .clk(clk), .wcap(wcap), .wact(wact), .wpulse(wpulse), .clr(clr_pend), .we0(we & ~hit0), .we1(we & uio_q[3]),
      .waddr(a_t0), .wdata({uio_q[2], g_t0, r_t0[2:0] + 3'd1}), .raddr(a_t0), .rdata(r_t0));
  bpx_ltab #(.AW(5), .RW(2), .EW0(10), .EW1(1), .INIT(11'd3)) u_t1 (
      .clk(clk), .wcap(wcap), .wact(wact), .wpulse(wpulse), .clr(clr_pend), .we0(we & ~hit1), .we1(we & uio_q[3]),
      .waddr(a_t1), .wdata({uio_q[2], g_t1, r_t1[2:0] + 3'd1}), .raddr(a_t1), .rdata(r_t1));

  // prediction-like combine: a tag hit picks a tagged counter, else a chooser picks between two base counters
  wire base = r_c[1] ? r_g[1] : r_b[1];
  wire pred = hit1 ? r_t1[2] : hit0 ? r_t0[2] : base;

  // placeholder for the control logic: six 20-bit counters and a 128-bit mixing register.
  // Table reads are sampled only outside write-pulse cycles, as the real control logic does.
  reg [19:0] cnt[0:5];
  reg [31:0] mix[0:3];
  integer i;
  always @(posedge clk) begin
    if (lrst) begin
      for (i = 0; i < 6; i = i + 1) cnt[i] <= 20'h0;
      for (i = 0; i < 4; i = i + 1) mix[i] <= 32'h0;
    end else if (!wact) begin
      for (i = 0; i < 6; i = i + 1) if (ui_q[i] ^ pred) cnt[i] <= cnt[i] + 20'h1;
      mix[0] <= {mix[0][30:0], pred} ^ {r_t0, r_t1, r_b, r_g, r_c, 4'h0};
      for (i = 1; i < 4; i = i + 1) mix[i] <= mix[i-1] ^ {mix[i][15:0], mix[i][31:16]} ^ {12'h0, cnt[i]};
    end
  end

  reg [7:0] out_q;
  always @(posedge clk) begin
    if (lrst) out_q <= 8'h00;
    else out_q <= {ready, hit1, hit0, base, ^mix[3], ^mix[2], ^cnt[0], pred} ^ mix[3][7:0] ^ cnt[5][7:0];
  end
  assign uo_out = out_q;

  wire _unused = &{1'b0, ena};
endmodule
