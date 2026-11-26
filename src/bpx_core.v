// bpx_core - BPX-1 operation sequencer (docs/SPEC.md sections 3 to 6; golden model: model/bpx1.py).
//
// Owns the handshake, the in-flight queue, the speculative and committed histories, the staging register, the
// statistics and the control-port registers; drives the predictor datapath (bpx_pred) and the write sequencer.
//
// Read port schedule, one user per cycle, in priority order:
//   predict   a pending request with a predict part, when no table write is in progress
//   staging   the commit-side lookup of the oldest in-flight branch, as soon as it has become the oldest
//   debug     a raw table read for the control port
// A resolve needs no read: it trains from the staging register.  A predict made with an empty queue is its own
// staging lookup (speculative history = committed history then).
//
// Order rules (identical in the golden model, independent of timing):
//   * a new request starts only when every part of the previous one has been performed;
//   * a predict reads the tables only when no write is pending or in progress, so it sees every earlier training;
//   * PREDICT+RESOLVE = predict, then resolve the oldest entry (possibly the one just appended);
//     with the queue full the resolve goes first.
`default_nettype none

module bpx_core #(
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
    parameter STATW = 20           // statistics counter width (17..32)
) (
    input  wire         clk,
    input  wire         rst,       // synchronous, already synchronised
    // predict port
    input  wire         req_s,     // REQ after two flops
    input  wire [W-1:0] id_d,      // ID after one flop (stable whenever a request is pending)
    input  wire [1:0]   op_d,      // OP after one flop
    output reg          ack,
    output reg          pred,
    output reg          conf,
    output reg          full,
    output reg          ready,
    output reg  [1:0]   dbg,
    // control-port registers (bpx_spi)
    input  wire [5:0]   reg_addr,
    input  wire         reg_we,
    input  wire         reg_re,
    input  wire [15:0]  reg_wdata,
    output reg  [15:0]  reg_rdata
);
  localparam SW = 6 * NT + 6;       // staging register width
  localparam QW = W + 5;            // queue entry: {pdrv, trn, gsh, bim, def, id}
  localparam [STATW-1:0] SMAX = {STATW{1'b1}};

  // ---------------------------------------------------------------- control registers
  reg  [2:0] drive;                 // 0 default, 1 bimodal, 2 gshare, 3 tournament, 4..7 static not-taken
  reg        freeze;
  reg  [3:0] dsel0, dsel1;
  reg        srst;                  // soft reset, one cycle
  wire       irst = rst | srst;
  reg        init_pend, clrp_pend, snap_pend, snap_q;

  // ---------------------------------------------------------------- state
  reg  [QW-1:0] qmem[0:3];
  reg  [1:0]    qhead;
  reg  [2:0]    qcnt;
  reg  [HL-1:0] ch, sh;             // committed and speculative history, bit 0 = most recent
  reg  [SW-1:0] stg;
  reg           stg_v;
  reg           rpend, rtk, rf_done, seen;
  reg  [3:0]    lp;                 // predictions of the last predict (debug)
  reg  [STATW-1:0] n_res, mis0, mis1, mis2, mis3;
  reg  [7:0]    err_cnt;
  reg  [3:0]    eflags;             // sticky: {statistics overflow, withdrawn, predict on full, resolve on empty}
  // raw access from the control port
  reg  [6:0]    da;
  reg  [2:0]    dspace;             // 0 BC, 1 G, 2.. tagged tables, 7 small registers
  reg  [1:0]    dwm;                // field write enables {field 1, field 0}
  reg           dinc;               // address + 1 after each data read
  reg  [11:0]   dq, dwd;
  reg           drd_pend, dwr_pend;

  wire          q_empty = qcnt == 3'd0;
  wire          q_full  = qcnt == 3'd4;
  wire [QW-1:0] head    = qmem[qhead];
  wire [W-1:0]  h_id    = head[W-1:0];
  wire [3:0]    h_preds = head[W+3:W];
  wire          h_pdrv  = head[W+4];

  // ---------------------------------------------------------------- write sequencer and datapath
  wire wcap, wact, wpulse, wbusy, wreq;
  bpx_wseq u_wseq (.clk(clk), .rst(rst), .req(wreq), .wcap(wcap), .wact(wact), .wpulse(wpulse), .busy(wbusy));

  wire [1:0]    rsel;
  wire          p_def, p_bim, p_gsh, p_trn, p_conf;
  wire [SW-1:0] look;
  wire [11:0]   t_rdata;
  wire [3:0]    ua_q;
  wire [7:0]    lfsr_q;
  wire          train, tk, clr_exec, dwr_tab, misc_we;

  bpx_pred #(.W(W), .LGB(LGB), .LGG(LGG), .HG(HG), .NT(NT), .LGT(LGT), .TB(TB), .LV(LV), .HL(HL), .ALLOC_LFSR(ALLOC_LFSR)) u_pred (
      .clk(clk), .rst(irst), .wcap(wcap), .wact(wact), .wpulse(wpulse),
      .rsel(rsel), .id_p(id_d), .hist_p(sh), .id_c(h_id), .hist_c(ch),
      .dbg_tab(dspace), .dbg_addr(da), .dbg_rdata(t_rdata),
      .p_def(p_def), .p_bim(p_bim), .p_gsh(p_gsh), .p_trn(p_trn), .conf(p_conf), .look(look),
      .train(train), .taken(tk), .stg(stg),
      .clr(clr_exec), .dbg_we0(dwr_tab & dwm[0]), .dbg_we1(dwr_tab & dwm[1]), .dbg_wdata(dwd),
      .ua_q(ua_q), .lfsr_q(lfsr_q), .misc_we(misc_we), .misc_wdata(dwd));

  // ---------------------------------------------------------------- request decode
  wire pending = ready & (req_s != ack);
  wire new_ok  = pending & ~rpend;                  // the previous request is complete
  wire op_p    = op_d != 2'b00;
  wire op_cr   = op_d[1] & ~rf_done;                // combined resolve still to do
  wire sp      = op_d == 2'b00;
  wire sp_nop  = sp & (id_d[1:0] == 2'b00);
  wire sp_fl   = sp & (id_d[1:0] == 2'b01);
  wire sp_r    = sp & id_d[1];
  wire tk_in   = sp ? id_d[0] : op_d[0];

  assign clr_exec = (init_pend | clrp_pend) & ~wbusy;
  wire go      = new_ok & ~clr_exec;

  wire want_p   = go & op_p;
  wire p_refuse = want_p & q_full & ~op_cr;                         // plain predict, queue full
  wire p_exec   = want_p & ~q_full & ~wbusy;
  wire r_first  = want_p & q_full & op_cr & stg_v & ~wbusy;         // queue full: resolve before the predict
  wire r_same   = p_exec & op_cr & ~q_empty & stg_v;                // predict and resolve in one cycle
  wire r_defer  = p_exec & op_cr & ~r_same;                         // resolve follows (staging not ready, or own entry)
  wire r_only   = go & sp_r & ~q_empty & stg_v & ~wbusy;
  wire r_empty  = go & sp_r & q_empty;                              // refused
  wire r_late   = rpend & stg_v & ~wbusy & ~clr_exec;
  wire r_exec   = r_first | r_same | r_only | r_late;
  assign tk     = rpend ? rtk : tk_in;
  wire fl_exec  = go & sp_fl;
  wire nop_exec = go & sp_nop;
  wire ack_t    = p_exec | p_refuse | r_only | r_empty | fl_exec | nop_exec;
  wire r_misp   = r_exec & (h_pdrv != tk);
  assign train  = r_exec & ~freeze;

  // read port
  wire hold_p   = pending & op_p;                                   // keep the port on the predict lookup
  wire do_stage = ready & ~q_empty & ~stg_v & ~wbusy & ~hold_p & ~clr_exec;
  wire d_tab    = dspace != 3'd7;
  wire drd_exec = drd_pend & (~d_tab | (~wbusy & ~hold_p & ~do_stage & ~clr_exec));
  assign rsel   = do_stage ? 2'd1 : (drd_exec & d_tab) ? 2'd2 : 2'd0;

  // write port
  wire dwr_exec = dwr_pend & ~wbusy & ~r_exec & ~clr_exec;
  assign dwr_tab = dwr_exec & d_tab;
  assign misc_we = dwr_exec & ~d_tab & (da[3:0] == 4'd4);
  wire snap_exec = snap_pend & ~wbusy & ~r_exec & ~clr_exec & ~dwr_exec;
  assign wreq    = clr_exec | train | dwr_tab | snap_exec;

  // the prediction returned to the host
  wire [3:0] preds = {p_trn, p_gsh, p_bim, p_def};
  wire       pdrv  = drive[2] ? 1'b0 : preds[drive[1:0]];

  // small registers visible through the raw access port (space 7)
  reg [11:0] misc;
  wire [23:0] ch24 = ch;            // zero-extended
  wire [23:0] sh24 = sh;
  wire [35:0] stg36 = stg;
  wire [15:0] q0 = qmem[0], q1 = qmem[1], q2 = qmem[2], q3 = qmem[3];
  always @* begin
    case (da[3:0])
      4'd0:    misc = ch24[11:0];
      4'd1:    misc = ch24[23:12];
      4'd2:    misc = sh24[11:0];
      4'd3:    misc = sh24[23:12];
      4'd4:    misc = {lfsr_q, ua_q};
      4'd5:    misc = stg36[11:0];
      4'd6:    misc = stg36[23:12];
      4'd7:    misc = stg36[35:24];
      4'd8:    misc = {5'd0, rpend, stg_v, qhead, qcnt};
      4'd12:   misc = q0[11:0];
      4'd13:   misc = q1[11:0];
      4'd14:   misc = q2[11:0];
      4'd15:   misc = q3[11:0];
      default: misc = {q3[14:12], q2[14:12], q1[14:12], q0[14:12]};
    endcase
  end

  // ---------------------------------------------------------------- statistics snapshot
  wire [5*STATW-1:0] shadow;
  bpx_shadow #(.N(5 * STATW)) u_shadow (.clk(clk), .gate(snap_q), .wact(wact), .wpulse(wpulse),
      .d({mis3, mis2, mis1, mis0, n_res}), .q(shadow));
  wire [31:0] sn0 = shadow[0*STATW+:STATW];
  wire [31:0] sn1 = shadow[1*STATW+:STATW];
  wire [31:0] sn2 = shadow[2*STATW+:STATW];
  wire [31:0] sn3 = shadow[3*STATW+:STATW];
  wire [31:0] sn4 = shadow[4*STATW+:STATW];

  // ---------------------------------------------------------------- control-port read
  always @* begin
    case (reg_addr[4:0])
      5'h00:   reg_rdata = 16'hB101;
      5'h01:   reg_rdata = {4'd0, dsel1, dsel0, freeze, drive};
      5'h03:   reg_rdata = {4'd0, eflags, drd_pend | dwr_pend, wbusy, pending, stg_v, ready, qcnt};
      5'h04:   reg_rdata = {8'd0, err_cnt};
      5'h05:   reg_rdata = sn0[15:0];
      5'h06:   reg_rdata = sn0[31:16];
      5'h07:   reg_rdata = sn1[15:0];
      5'h08:   reg_rdata = sn1[31:16];
      5'h09:   reg_rdata = sn2[15:0];
      5'h0a:   reg_rdata = sn2[31:16];
      5'h0b:   reg_rdata = sn3[15:0];
      5'h0c:   reg_rdata = sn3[31:16];
      5'h0d:   reg_rdata = sn4[15:0];
      5'h0e:   reg_rdata = sn4[31:16];
      5'h0f:   reg_rdata = {dinc, 1'b0, dwm, 1'b0, dspace, 1'b0, da};
      5'h10:   reg_rdata = {4'd0, dq};
      default: reg_rdata = 16'd0;
    endcase
  end

  wire w_ctrl = reg_we & (reg_addr == 6'h01);
  wire w_cmd  = reg_we & (reg_addr == 6'h02);
  wire w_da   = reg_we & (reg_addr == 6'h0f);
  wire w_dd   = reg_we & (reg_addr == 6'h10);
  wire r_dd   = reg_re & (reg_addr == 6'h10);

  // debug events
  wire [15:0] evt = {do_stage, reg_we | reg_re, lp[3], lp[2], lp[1], lp[0], q_empty, stg_v, wact,
                     p_refuse | r_empty | (seen & ~pending & ready), fl_exec, r_misp, r_exec, p_exec, pending & ~seen, 1'b0};

  wire [2:0] qcnt_n = (fl_exec | r_misp) ? 3'd0 : qcnt + {2'b00, p_exec} - {2'b00, r_exec};
  wire       err_ev = p_refuse | r_empty | (seen & ~pending & ready);

  always @(posedge clk) begin
    srst <= 1'b0;
    if (irst) begin
      drive     <= 3'd0;
      freeze    <= 1'b0;
      dsel0     <= 4'd0;
      dsel1     <= 4'd0;
      init_pend <= 1'b1;
      clrp_pend <= 1'b0;
      snap_pend <= 1'b0;
      snap_q    <= 1'b0;
      qhead     <= 2'd0;
      qcnt      <= 3'd0;
      ch        <= {HL{1'b0}};
      sh        <= {HL{1'b0}};
      stg       <= {SW{1'b0}};
      stg_v     <= 1'b0;
      rpend     <= 1'b0;
      rtk       <= 1'b0;
      rf_done   <= 1'b0;
      seen      <= 1'b0;
      lp        <= 4'd0;
      n_res     <= {STATW{1'b0}};
      mis0      <= {STATW{1'b0}};
      mis1      <= {STATW{1'b0}};
      mis2      <= {STATW{1'b0}};
      mis3      <= {STATW{1'b0}};
      err_cnt   <= 8'd0;
      eflags    <= 4'd0;
      da        <= 7'd0;
      dspace    <= 3'd0;
      dwm       <= 2'd0;
      dinc      <= 1'b0;
      dq        <= 12'd0;
      dwd       <= 12'd0;
      drd_pend  <= 1'b0;
      dwr_pend  <= 1'b0;
      ack       <= req_s;
      pred      <= 1'b0;
      conf      <= 1'b0;
      full      <= 1'b0;
      ready     <= 1'b0;
      dbg       <= 2'b00;
    end else begin
      // -------- initialisation and clears
      if (wcap) snap_q <= snap_exec;
      if (clr_exec) begin
        init_pend <= 1'b0;
        clrp_pend <= 1'b0;
      end
      if (!ready) begin
        ack <= req_s;                               // a request level present at reset is not a request
        if (!init_pend && !wbusy) ready <= 1'b1;
      end

      // -------- predict
      if (p_exec) begin
        qmem[qhead+qcnt[1:0]] <= {pdrv, preds, id_d};
        sh   <= {sh[HL-2:0], pdrv};
        pred <= pdrv;
        conf <= p_conf;
        lp   <= preds;
        if (q_empty) begin                          // the predict lookup is the commit lookup
          stg   <= look;
          stg_v <= 1'b1;
        end
        if (r_defer) begin
          rpend <= 1'b1;
          rtk   <= tk_in;
        end
        rf_done <= 1'b0;
      end
      if (p_refuse) begin
        pred <= 1'b0;
        conf <= 1'b0;
      end
      if (r_first) rf_done <= 1'b1;
      if (do_stage) begin
        stg   <= look;
        stg_v <= 1'b1;
      end

      // -------- resolve
      if (r_exec) begin
        ch    <= {ch[HL-2:0], tk};
        qhead <= qhead + 2'd1;
        stg_v <= 1'b0;
        rpend <= 1'b0;
        if (n_res == SMAX) eflags[3] <= 1'b1;
        else n_res <= n_res + 1'b1;
        if ((h_preds[0] != tk) && (mis0 != SMAX)) mis0 <= mis0 + 1'b1;
        if ((h_preds[1] != tk) && (mis1 != SMAX)) mis1 <= mis1 + 1'b1;
        if ((h_preds[2] != tk) && (mis2 != SMAX)) mis2 <= mis2 + 1'b1;
        if ((h_preds[3] != tk) && (mis3 != SMAX)) mis3 <= mis3 + 1'b1;
        if (r_misp) sh <= {ch[HL-2:0], tk};         // repair: discard everything younger
      end
      if (fl_exec) begin
        sh    <= ch;
        stg_v <= 1'b0;
      end
      qcnt <= qcnt_n;
      full <= qcnt_n == 3'd4;

      // -------- handshake and misuse
      if (ack_t) ack <= ~ack;
      seen <= pending & ~ack_t;
      if (seen & ~pending & ready) rf_done <= 1'b0; // request withdrawn
      if (err_ev) begin
        if (err_cnt != 8'hff) err_cnt <= err_cnt + 8'd1;
        if (r_empty) eflags[0] <= 1'b1;
        if (p_refuse) eflags[1] <= 1'b1;
        if (seen & ~pending & ready) eflags[2] <= 1'b1;
      end

      // -------- raw access from the control port
      if (drd_exec) begin
        dq       <= d_tab ? t_rdata : misc;
        drd_pend <= 1'b0;
      end
      if (dwr_exec) begin
        dwr_pend <= 1'b0;
        drd_pend <= 1'b1;                           // read back what is there afterwards
        stg_v    <= 1'b0;
        if (!d_tab) begin
          case (da[3:0])
            4'd0: ch <= (ch & ~{{(HL-12){1'b0}}, 12'hfff}) | {{(HL-12){1'b0}}, dwd};
            4'd1: ch <= (ch & {{(HL-12){1'b0}}, 12'hfff}) | ({{(HL-12){1'b0}}, dwd} << 12);
            4'd2: sh <= (sh & ~{{(HL-12){1'b0}}, 12'hfff}) | {{(HL-12){1'b0}}, dwd};
            4'd3: sh <= (sh & {{(HL-12){1'b0}}, 12'hfff}) | ({{(HL-12){1'b0}}, dwd} << 12);
            default: ;
          endcase
        end
      end
      if (snap_exec) snap_pend <= 1'b0;

      // -------- clear predictors (also the tail of reset): in-flight work and histories go
      if (clr_exec) begin
        qcnt    <= 3'd0;
        full    <= 1'b0;
        ch      <= {HL{1'b0}};
        sh      <= {HL{1'b0}};
        stg_v   <= 1'b0;
        rpend   <= 1'b0;
        rf_done <= 1'b0;
      end

      // -------- control-port writes
      if (w_ctrl) begin
        drive  <= reg_wdata[2:0];
        freeze <= reg_wdata[3];
        dsel0  <= reg_wdata[7:4];
        dsel1  <= reg_wdata[11:8];
      end
      if (w_cmd) begin
        if (reg_wdata[0]) srst <= 1'b1;
        if (reg_wdata[1]) clrp_pend <= 1'b1;
        if (reg_wdata[2]) begin
          n_res   <= {STATW{1'b0}};
          mis0    <= {STATW{1'b0}};
          mis1    <= {STATW{1'b0}};
          mis2    <= {STATW{1'b0}};
          mis3    <= {STATW{1'b0}};
          err_cnt <= 8'd0;
          eflags  <= 4'd0;
        end
        if (reg_wdata[3]) snap_pend <= 1'b1;
      end
      if (w_da) begin
        da       <= reg_wdata[6:0];
        dspace   <= reg_wdata[10:8];
        dwm      <= reg_wdata[13:12];
        dinc     <= reg_wdata[15];
        drd_pend <= 1'b1;
      end
      if (w_dd) begin
        dwd      <= reg_wdata[11:0];
        dwr_pend <= 1'b1;
      end
      if (r_dd & dinc) begin
        da       <= da + 7'd1;
        drd_pend <= 1'b1;
      end

      dbg <= {evt[dsel1], evt[dsel0]};
    end
  end
endmodule

// bpx_shadow - snapshot register for the statistics counters: N latches that copy d during the write pulse of a
// write-sequencer cycle whose request had the snapshot flag (gate).  d comes from flops, which are stable for the
// whole pulse; gate was captured a full cycle before the pulse.  Same construction as the tables (bpx_ltab).
module bpx_shadow #(
    parameter N = 100
) (
    input  wire         clk,
    input  wire         gate,
    input  wire         wact,
    input  wire         wpulse,
    input  wire [N-1:0] d,
    output wire [N-1:0] q
);
`ifdef BPX_BEHAV
  reg [N-1:0] r;
  always @(posedge clk) if (wact & gate) r <= d;
  assign q = r;
  wire _unused = &{1'b0, wpulse};
`else
  wire g_n;
  (* keep *) sky130_fd_sc_hd__nand2_1 u_g (.A(gate), .B(wpulse), .Y(g_n));
  genvar i;
  generate
    for (i = 0; i < N; i = i + 1) begin : g_b
      (* keep *) sky130_fd_sc_hd__dlxtn_1 u_bit (.D(d[i]), .GATE_N(g_n), .Q(q[i]));
    end
  endgenerate
  wire _unused = &{1'b0, clk, wact};
`endif
endmodule
