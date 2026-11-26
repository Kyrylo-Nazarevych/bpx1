// bpx_ltab - latch-based predictor table (spec AL-04).
//
// 2^AW entries of EW0+EW1 bits.  Field 0 is bits [EW0-1:0], field 1 (optional, EW1 may be 0) is the bits above it;
// each field of an entry has its own write gate, so a field can be rewritten without knowing the other.
//
// Write: three clock cycles, driven by the chip's single write sequencer (bpx_wseq), shared by all tables.
//   cycle 0 (wcap = 1) : the control logic presents we0/we1, waddr, wdata, clr; flops capture them at the edge.
//   cycle 1            : set-up cycle.  The captured address, enables and data are now stable.
//   cycle 2 (wact = 1) : wpulse is high for the first half of the cycle and opens exactly the addressed latches.
// Every signal that reaches a latch (its data, and the row and column terms of its gate) comes from flops that
// changed a full cycle before the pulse rises and cannot change again until half a cycle after it has fallen.  The
// pulse itself is the AND of a flop clocked on the rising edge and a flop clocked on the falling edge, so each of
// its edges is caused by exactly one signal.  The gates are therefore glitch-free by construction, with half a
// clock period of margin on set-up, hold and pulse width.  No gate is derived from the clock net, so clock-tree
// synthesis never sees the latches (it would otherwise build a buffered tree on every one of the gate nets).
//   gate_n[row][col][f] = NAND( AND(rowsel[row], wpulse), colsel[col] & field_enable[f] )
// The two gate levels are instantiated by name so that synthesis cannot restructure them.
//
// Read: combinational, rdata = entry[raddr].  The control logic never uses a read taken in a cycle 2 of any write,
// so a read never looks through a transparent latch.
//
// clr = 1 (captured like a write) loads INIT into every entry with one pulse.
//
// DYN = 1: dynamic (leaky) storage, an experiment.  Each bit is the output node of a tri-state inverter
// (sky130_fd_sc_hd__einvp_1) that is enabled only during the write pulse; afterwards the node floats and holds
// its charge on the gate capacitance of the read tree until leakage takes it.  No refresh here.  A third of the
// area of a latch.  The read tree isolates every unselected entry (its select input is 0), so a half-discharged
// node can draw current only in the entry being read.  Gate-level simulation shows these nodes as unknown.
//
// Define BPX_BEHAV for a flip-flop model with the same cycle behaviour (formal proofs, fast simulation).
`default_nettype none

module bpx_ltab #(
    parameter AW = 7,                          // address bits
    parameter RW = 3,                          // row bits (rows = 2^RW = 4, 8 or 16; columns = 2^(AW-RW))
    parameter EW0 = 2,                         // field 0 width (>= 1)
    parameter EW1 = 0,                         // field 1 width (0 = no second field)
    parameter [EW0+EW1-1:0] INIT = 0,          // value after clr
    parameter DYN = 0                          // 1: dynamic storage nodes instead of latches
) (
    input  wire               clk,
    input  wire               wcap,            // capture the write request at this clock edge
    input  wire               wact,            // pulse cycle (behavioural model only)
    input  wire               wpulse,          // write pulse: high in the first half of the pulse cycle
    input  wire               clr,
    input  wire               we0,
    input  wire               we1,
    input  wire [AW-1:0]      waddr,
    input  wire [EW0+EW1-1:0] wdata,
    input  wire [AW-1:0]      raddr,
    output wire [EW0+EW1-1:0] rdata
);
  localparam EW = EW0 + EW1;
  localparam N = 1 << AW;
  localparam ROWS = 1 << RW;
  localparam CW = AW - RW;
  localparam COLS = 1 << CW;

  // ---------------------------------------------------------------- captured write request
  reg           clr_q, we0_q, we1_q;
  reg  [AW-1:0] waddr_q;
  reg  [EW-1:0] wdata_q;
  always @(posedge clk) begin
    if (wcap) begin
      clr_q   <= clr;
      we0_q   <= we0;
      we1_q   <= we1;
      waddr_q <= waddr;
      wdata_q <= clr ? INIT : wdata;
    end
  end

  wire [N*EW-1:0] q;  // flat storage, entry e at q[e*EW +: EW]

`ifdef BPX_BEHAV
  // ---------------------------------------------------------------- behavioural model (same cycle behaviour)
  reg  [EW-1:0] mem[0:N-1];
  wire [EW-1:0] mask;
  integer       i;
  genvar gm, ge;
  generate
    for (gm = 0; gm < EW; gm = gm + 1) begin : g_mask
      assign mask[gm] = (gm < EW0) ? we0_q : we1_q;
    end
    for (ge = 0; ge < N; ge = ge + 1) begin : g_q
      assign q[ge*EW+:EW] = mem[ge];
    end
  endgenerate
  always @(posedge clk) begin
    if (wact) begin
      if (clr_q) begin
        for (i = 0; i < N; i = i + 1) mem[i] <= wdata_q;
      end else if (we0_q | we1_q) begin
        mem[waddr_q] <= (mem[waddr_q] & ~mask) | (wdata_q & mask);
      end
    end
  end
  wire _unused_behav = &{1'b0, wpulse};
`else
  // ---------------------------------------------------------------- sky130 latch array
  wire [ROWS-1:0] rp;                      // row select AND pulse
  wire [RW-1:0]   wrow = waddr_q[AW-1:CW];
  wire            wany = we0_q | we1_q;
  wire [EW-1:0]   wdata_n = ~wdata_q;      // the dynamic cell is an inverter
  genvar r, c, b;
  generate
    for (r = 0; r < ROWS; r = r + 1) begin : g_row
      wire sel = clr_q | (wany & (wrow == r));
      (* keep *) sky130_fd_sc_hd__and2_1 u_rp (.A(sel), .B(wpulse), .X(rp[r]));
    end
    for (c = 0; c < COLS; c = c + 1) begin : g_col
      wire csel;
      if (CW > 0) begin : g_dec
        assign csel = clr_q | (waddr_q[CW-1:0] == c);
      end else begin : g_one
        assign csel = 1'b1;
      end
      wire cf0 = csel & (clr_q | we0_q);
      for (r = 0; r < ROWS; r = r + 1) begin : g_r0
        if (DYN) begin : g_dyn
          wire g;
          (* keep *) sky130_fd_sc_hd__and2_1 u_g (.A(rp[r]), .B(cf0), .X(g));
          for (b = 0; b < EW0; b = b + 1) begin : g_b
            (* keep *) sky130_fd_sc_hd__einvp_1 u_bit (.A(wdata_n[b]), .TE(g), .Z(q[(r*COLS+c)*EW+b]));
          end
        end else begin : g_lat
          wire g_n;
          (* keep *) sky130_fd_sc_hd__nand2_1 u_g (.A(rp[r]), .B(cf0), .Y(g_n));
          for (b = 0; b < EW0; b = b + 1) begin : g_b
            (* keep *) sky130_fd_sc_hd__dlxtn_1 u_bit (.D(wdata_q[b]), .GATE_N(g_n), .Q(q[(r*COLS+c)*EW+b]));
          end
        end
      end
      if (EW1 > 0) begin : g_f1
        wire cf1 = csel & (clr_q | we1_q);
        for (r = 0; r < ROWS; r = r + 1) begin : g_r1
          wire g_n;
          (* keep *) sky130_fd_sc_hd__nand2_1 u_g (.A(rp[r]), .B(cf1), .Y(g_n));
          for (b = EW0; b < EW; b = b + 1) begin : g_b
            (* keep *) sky130_fd_sc_hd__dlxtn_1 u_bit (.D(wdata_q[b]), .GATE_N(g_n), .Q(q[(r*COLS+c)*EW+b]));
          end
        end
      end
    end
  endgenerate
  wire _unused_struct = &{1'b0, wact, wdata_n};
`endif

  // ---------------------------------------------------------------- read
  // Row select as AND-OR over the rows (one select wire per row, no per-entry decode), then a column mux.
  wire [ROWS-1:0]    rsel;
  wire [COLS*EW-1:0] rowdata;
  genvar rr, rk;
  generate
    for (rr = 0; rr < ROWS; rr = rr + 1) begin : g_rsel
      assign rsel[rr] = (raddr[AW-1:CW] == rr);
    end
    for (rk = 0; rk < COLS * EW; rk = rk + 1) begin : g_rd
      wire [ROWS-1:0] colq;
      for (rr = 0; rr < ROWS; rr = rr + 1) begin : g_t
        assign colq[rr] = q[rr*COLS*EW+rk];
      end
      bpx_aor #(.N(ROWS)) u_aor (.a(colq), .b(rsel), .y(rowdata[rk]));
    end
    if (CW > 0) begin : g_cmux
      assign rdata = rowdata[raddr[CW-1:0]*EW+:EW];
    end else begin : g_nocmux
      assign rdata = rowdata;
    end
  endgenerate

endmodule

// bpx_aor - y = OR over i of (a[i] & b[i]), N = 2, 4, 8 or 16.
// Mapped by hand onto minimum-size AND-OR-INVERT cells (the flow's synthesis may only use drive strength 2 and
// larger, which costs about 60% more area here); the place-and-route resizer is free to upsize them where needed.
module bpx_aor #(
    parameter N = 8
) (
    input  wire [N-1:0] a,
    input  wire [N-1:0] b,
    output wire         y
);
`ifdef BPX_BEHAV
  assign y = |(a & b);
`else
  generate
    if (N == 2) begin : g2
      (* keep *) sky130_fd_sc_hd__a22o_1 u0 (.A1(a[0]), .A2(b[0]), .B1(a[1]), .B2(b[1]), .X(y));
    end else if (N == 4) begin : g4
      wire [1:0] n;
      (* keep *) sky130_fd_sc_hd__a222oi_1 u0 (.A1(a[0]), .A2(b[0]), .B1(a[1]), .B2(b[1]), .C1(a[2]), .C2(b[2]), .Y(n[0]));
      (* keep *) sky130_fd_sc_hd__nand2_1 u1 (.A(a[3]), .B(b[3]), .Y(n[1]));
      (* keep *) sky130_fd_sc_hd__nand2_1 u2 (.A(n[0]), .B(n[1]), .Y(y));
    end else if (N == 8) begin : g8
      wire [2:0] n;
      (* keep *) sky130_fd_sc_hd__a222oi_1 u0 (.A1(a[0]), .A2(b[0]), .B1(a[1]), .B2(b[1]), .C1(a[2]), .C2(b[2]), .Y(n[0]));
      (* keep *) sky130_fd_sc_hd__a222oi_1 u1 (.A1(a[3]), .A2(b[3]), .B1(a[4]), .B2(b[4]), .C1(a[5]), .C2(b[5]), .Y(n[1]));
      (* keep *) sky130_fd_sc_hd__a22oi_1 u2 (.A1(a[6]), .A2(b[6]), .B1(a[7]), .B2(b[7]), .Y(n[2]));
      (* keep *) sky130_fd_sc_hd__nand3_1 u3 (.A(n[0]), .B(n[1]), .C(n[2]), .Y(y));
    end else begin : g16
      wire [5:0] n;
      wire [1:0] m;
      genvar i;
      for (i = 0; i < 5; i = i + 1) begin : g_a
        (* keep *) sky130_fd_sc_hd__a222oi_1 u (.A1(a[3*i]), .A2(b[3*i]), .B1(a[3*i+1]), .B2(b[3*i+1]), .C1(a[3*i+2]), .C2(b[3*i+2]), .Y(n[i]));
      end
      (* keep *) sky130_fd_sc_hd__nand2_1 u5 (.A(a[15]), .B(b[15]), .Y(n[5]));
      (* keep *) sky130_fd_sc_hd__nand3_1 u6 (.A(n[0]), .B(n[1]), .C(n[2]), .Y(m[0]));
      (* keep *) sky130_fd_sc_hd__nand3_1 u7 (.A(n[3]), .B(n[4]), .C(n[5]), .Y(m[1]));
      (* keep *) sky130_fd_sc_hd__or2_1 u8 (.A(m[0]), .B(m[1]), .X(y));
    end
  endgenerate
`endif
endmodule

// bpx_wseq - the chip's single write sequencer.
// A write request accepted in cycle 0 (req = 1 while idle) is captured by the tables at the next edge (wcap),
// set up during cycle 1, and pulsed in the first half of cycle 2 (wact, wpulse).  A new request can be accepted
// again in cycle 2.  busy = 1 in cycles 1 and 2.
//   wpulse = a & b:  a is set at the rising edge that starts cycle 2 and cleared at the next one;
//                    b is clocked on the falling edge and is the complement of a, half a cycle late.
// So wpulse rises when a rises (b already 1) and falls when b falls (a still 1): one cause per edge, no glitch.
module bpx_wseq (
    input  wire clk,
    input  wire rst,
    input  wire req,
    output wire wcap,
    output wire wact,
    output wire wpulse,
    output wire busy
);
  reg s1, a, b;
  assign wcap = req & ~s1;
  always @(posedge clk) begin
    if (rst) begin
      s1 <= 1'b0;
      a  <= 1'b0;
    end else begin
      s1 <= wcap;
      a  <= s1;
    end
  end
  assign wact = a;
  assign busy = s1 | a;
`ifdef FORMAL
  // single-edge model for the proofs: the behavioural tables write on wact and do not use the pulse
  assign wpulse = 1'b0;
  always @(posedge clk) b <= 1'b0;
`elsif BPX_BEHAV
  always @(negedge clk) b <= ~a;
  assign wpulse = a & b;
`else
  always @(negedge clk) b <= ~a;
  (* keep *) sky130_fd_sc_hd__and2_1 u_wp (.A(a), .B(b), .X(wpulse));
`endif
endmodule
