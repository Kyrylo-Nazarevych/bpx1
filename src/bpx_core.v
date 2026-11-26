// bpx_core - BPX-1 operation sequencer (docs/SPEC.md sections 3 to 6; golden model: model/bpx1.py).
//
// Owns the handshake, the in-flight queue, the speculative and committed histories, the staging register, the
// statistics and the control-port registers; drives the predictor datapath (bpx_pred) and the write sequencer.
//
// Read port schedule, one user per cycle, in priority order:
//   predict   a pending request with a predict part, in the cycle it executes (no table write in progress)
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
    parameter FEAT = 3'b111,       // features of the default predictor enabled after reset (CTRL[14:12]; CTRL[15] reads 0)
    parameter STATW = 16           // statistics counter width (16..32)
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
    output wire [3:0]   lastp,     // predictions of the last predict: {tournament, gshare, bimodal, default}
    // control-port registers (bpx_spi)
    input  wire [5:0]   reg_addr,
    input  wire         reg_we,
    input  wire         reg_re,
    input  wire [15:0]  reg_wdata,
    output reg  [15:0]  reg_rdata
);
  localparam SW = CB * NT + 14;     // staging register width (the top 8 bits: the twin word, docs/mosaic.md 7.3)
  localparam QW = W + 5;            // queue entry: {pdrv, trn, gsh, bim, def, id}
  localparam [STATW-1:0] SMAX = {STATW{1'b1}};

  // ---------------------------------------------------------------- control registers
  reg  [2:0] drive;                 // 0 default, 1 bimodal, 2 gshare, 3 tournament, 4..7 static not-taken
  reg        freeze;
  reg  [3:0] dsel0, dsel1;
  reg  [2:0] feat;                  // default predictor: {adaptive threshold, negation by history, reference inputs}
  reg        srst;                  // soft reset, one cycle
  wire       irst = rst | srst;
  // mosaic (docs/mosaic.md 7.2): FCTL switches the experiments (0 = the plain chip); FSTAT holds sticky fault flags
  reg  [15:0] fctl;                 // [0] TWIN_EN [1] TWIN_USE [3:2] CODE [4] RESTORE [14] DBGF; the rest storage only
  reg  [3:0]  fstat;                // [0] TW_ERR [1] TW_COR [2] TW_DET [3] TW_SDC
  reg         lpw;                  // fault event 10: the default prediction of the last predict computed with W3
  reg        init_pend, clrp_pend, snap_pend, snap_q, sclr_pend, sclr_q;

  // ---------------------------------------------------------------- state
  reg  [QW-1:0] qmem[0:3];
  reg  [1:0]    qhead;
  reg  [2:0]    qcnt;
  reg  [HL-1:0] ch, sh;             // committed and speculative history, bit 0 = most recent
  reg  [PL-1:0] pch, psh;           // committed and speculative path history: one identifier bit per branch
  wire          pb_p = id_d[0] ^ id_d[3];
  reg  [SW-1:0] stg;
  reg           stg_v;
  reg           rpend, rtk, rf_done, seen;
  reg  [3:0]    lp;                 // predictions of the last predict (debug)
  assign lastp = lp;
  reg  [STATW-1:0] n_res, mis0, mis1, mis2, mis3;
  reg  [7:0]    err_cnt;
  reg  [3:0]    eflags;             // sticky: {statistics overflow, withdrawn, predict on full, resolve on empty}
  // raw access from the control port
  reg  [6:0]    da;
  reg  [3:0]    dspace;             // 0 BC, 1 G, 7 small registers, 8.. weight tables
  reg  [1:0]    dwm;                // field write enables {field 1, field 0}
  reg           dinc;               // address + 1 after each data read
  reg  [11:0]   dq, dwd;
  reg           drd_pend, dwr_pend;

  wire          q_empty = qcnt == 3'd0;
  wire          q_full  = qcnt == 3'd4;
  wire [QW-1:0] head    = qmem[qhead];
  wire [W-1:0]  h_id    = head[W-1:0];
  wire          pb_c    = h_id[0] ^ h_id[3];
  wire [3:0]    h_preds = head[W+3:W];
  wire          h_pdrv  = head[W+4];

  // ---------------------------------------------------------------- write sequencer and datapath
  wire wcap, wact, wpulse, wbusy, wreq;
  bpx_wseq u_wseq (.clk(clk), .rst(rst), .req(wreq), .wcap(wcap), .wact(wact), .wpulse(wpulse), .busy(wbusy));

  wire [1:0]    rsel;
  wire          p_def, p_bim, p_gsh, p_trn;
  wire [3:0]    p_conf;             // per predictor, as preds
  wire [SW-1:0] look;
  wire [11:0]   t_rdata;
  wire [3:0]    tc_q;
  wire [7:0]    theta_q;
  wire          train, tk, clr_exec, dwr_tab, misc_we;
  wire [3:0]    tw_flags;
  wire          p_def3;

  bpx_pred #(.W(W), .LGB(LGB), .LGG(LGG), .HG(HG), .NT(NT), .LGT(LGT), .CB(CB), .LV(LV), .HL(HL), .PL(PL), .UBW(UBW), .UGW(UGW), .UTW(UTW), .SK(SK), .THETA(THETA), .SB(SB)) u_pred (
      .clk(clk), .rst(irst), .feat(feat), .tctl(fctl[4:0]), .tw_flags(tw_flags), .p_def3(p_def3),
      .wcap(wcap), .wact(wact), .wpulse(wpulse),
      .rsel(rsel), .id_p(id_d), .hist_p(sh), .path_p(psh), .id_c(h_id), .hist_c(ch), .path_c(pch),
      .dbg_tab(dspace), .dbg_addr(da), .dbg_rdata(t_rdata),
      .p_def(p_def), .p_bim(p_bim), .p_gsh(p_gsh), .p_trn(p_trn), .conf(p_conf), .look(look),
      .train(train), .taken(tk), .stg(stg),
      .clr(clr_exec), .dbg_we0(dwr_tab & dwm[0]), .dbg_we1(dwr_tab & dwm[1]), .dbg_wdata(dwd),
      .tc_q(tc_q), .theta_q(theta_q), .misc_we(misc_we), .misc_wdata(dwd));

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
  // The port stays on the predict lookup only in the cycle the predict executes.  (Holding it for every pending
  // predict deadlocks: a predict that waits for the deferred resolve of the previous request, or for the
  // resolve-first of its own, needs the staging lookup to happen first.  Found by the formal liveness proof.)
  wire hold_p   = p_exec;
  wire do_stage = ready & ~q_empty & ~stg_v & ~wbusy & ~hold_p & ~clr_exec;
  wire d_tab    = dspace != 4'd7;
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

  // small registers visible through the raw access port (space 7): histories, threshold and its adaptation counter
  reg [11:0] misc;
  wire [23:0] ch24 = ch;            // zero-extended
  wire [23:0] sh24 = sh;
  wire [23:0] pch24 = pch;
  wire [23:0] psh24 = psh;
  always @* begin
    case (da[3:0])
      4'd0:    misc = ch24[11:0];
      4'd1:    misc = ch24[23:12];
      4'd2:    misc = sh24[11:0];
      4'd3:    misc = sh24[23:12];
      4'd4:    misc = {theta_q, tc_q};
      4'd5:    misc = pch24[11:0];
      4'd6:    misc = pch24[23:12];
      4'd7:    misc = psh24[11:0];
      4'd8:    misc = psh24[23:12];
      default: misc = {5'd0, rpend, stg_v, qhead, qcnt};
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
      5'h01:   reg_rdata = {1'b0, feat, dsel1, dsel0, freeze, drive};
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
      5'h0f:   reg_rdata = {dinc, 1'b0, dwm, dspace, 1'b0, da};
      5'h10:   reg_rdata = {4'd0, dq};
      5'h11:   reg_rdata = fctl;
      5'h12:   reg_rdata = {12'd0, fstat};
      default: reg_rdata = 16'd0;
    endcase
  end

  wire w_ctrl = reg_we & (reg_addr == 6'h01);
  wire w_cmd  = reg_we & (reg_addr == 6'h02);
  wire w_da   = reg_we & (reg_addr == 6'h0f);
  wire w_dd   = reg_we & (reg_addr == 6'h10);
  wire r_dd   = reg_re & (reg_addr == 6'h10);
  wire w_fctl = reg_we & (reg_addr == 6'h11);

  // debug events
  wire [3:0]  lp_n = p_exec ? preds : lp;        // so that the DBG pins change together with ACK, like PRED
  wire [15:0] evt = {do_stage, reg_we | reg_re, lp_n[3], lp_n[2], lp_n[1], lp_n[0], q_empty, stg_v, wact,
                     p_refuse | r_empty | (seen & ~pending & ready), fl_exec, r_misp, r_exec, p_exec, pending & ~seen, 1'b0};
  // fault events (FCTL[14]): 0-3 the twin flags of the predict being acknowledged, 10 its default prediction with W3
  wire        lpw_n = p_exec ? p_def3 : lpw;
  wire [3:0]  tw_ev = tw_flags & {4{p_exec}};
  wire [15:0] fevt  = {5'd0, lpw_n, 6'd0, tw_ev};

  wire [2:0] qcnt_n = (fl_exec | r_misp) ? 3'd0 : qcnt + {2'b00, p_exec} - {2'b00, r_exec};
  wire       err_ev = p_refuse | r_empty | (seen & ~pending & ready);

  always @(posedge clk) begin
    srst <= 1'b0;
    if (irst) begin
      drive     <= 3'd0;
      freeze    <= 1'b0;
      dsel0     <= 4'd10;                           // DBG0: last prediction of the default predictor
      dsel1     <= 4'd13;                           // DBG1: last prediction of the tournament
      feat      <= FEAT;
      fctl      <= 16'd0;
      fstat     <= 4'd0;
      lpw       <= 1'b0;
      init_pend <= 1'b1;
      clrp_pend <= 1'b0;
      snap_pend <= 1'b0;
      snap_q    <= 1'b0;
      sclr_pend <= 1'b0;
      sclr_q    <= 1'b0;
      qhead     <= 2'd0;
      qcnt      <= 3'd0;
      ch        <= {HL{1'b0}};
      sh        <= {HL{1'b0}};
      pch       <= {PL{1'b0}};
      psh       <= {PL{1'b0}};
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
      dspace    <= 4'd0;
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
      if (wcap) begin
        snap_q <= snap_exec;
        sclr_q <= snap_exec & sclr_pend;
      end
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
        psh  <= {psh[PL-2:0], pb_p};
        pred <= pdrv;
        conf <= ~drive[2] & p_conf[drive[1:0]];     // CONF qualifies PRED: the confidence of the driving predictor
        lp   <= preds;
        lpw  <= p_def3;
        fstat <= fstat | tw_flags;                  // the twin's flags of this lookup (0 unless TWIN_EN)
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
        pch   <= {pch[PL-2:0], pb_c};
        qhead <= qhead + 2'd1;
        stg_v <= 1'b0;
        rpend <= 1'b0;
        if (n_res == SMAX) eflags[3] <= 1'b1;
        else n_res <= n_res + 1'b1;
        if ((h_preds[0] != tk) && (mis0 != SMAX)) mis0 <= mis0 + 1'b1;
        if ((h_preds[1] != tk) && (mis1 != SMAX)) mis1 <= mis1 + 1'b1;
        if ((h_preds[2] != tk) && (mis2 != SMAX)) mis2 <= mis2 + 1'b1;
        if ((h_preds[3] != tk) && (mis3 != SMAX)) mis3 <= mis3 + 1'b1;
        if (r_misp) begin                           // repair: discard everything younger
          sh  <= {ch[HL-2:0], tk};
          psh <= {pch[PL-2:0], pb_c};
        end
      end
      if (fl_exec) begin
        sh    <= ch;
        psh   <= pch;
        stg_v <= 1'b0;
      end
      qcnt <= qcnt_n;
      full <= qcnt_n == 3'd4;

      // -------- handshake and misuse
      if (ack_t) begin
        ack     <= ~ack;
        rf_done <= 1'b0;                            // also if the host changed OP in mid-request (misuse)
      end
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
      end
      if (snap_exec) begin
        snap_pend <= 1'b0;
        sclr_pend <= 1'b0;
      end
      // snapshot-and-clear: the counters restart in the cycle the snapshot latches close; no resolve can run in
      // a write-pulse cycle, so no event is lost or counted twice
      if (wact & snap_q & sclr_q) begin
        n_res <= {STATW{1'b0}};
        mis0  <= {STATW{1'b0}};
        mis1  <= {STATW{1'b0}};
        mis2  <= {STATW{1'b0}};
        mis3  <= {STATW{1'b0}};
      end

      // -------- clear predictors (also the tail of reset): in-flight work and histories go
      if (clr_exec) begin
        qcnt    <= 3'd0;
        full    <= 1'b0;
        ch      <= {HL{1'b0}};
        sh      <= {HL{1'b0}};
        pch     <= {PL{1'b0}};
        psh     <= {PL{1'b0}};
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
        feat   <= reg_wdata[14:12];         // CTRL[15] (binarised inference, removed) is ignored
      end
      if (w_fctl) begin
        fctl <= reg_wdata;
        // the staged twin word depends on TWIN_EN (masking) and CODE (correction): stage again if they change,
        // so that a restore-on-use writes what a lookup with the new setting reads (golden model: transaction-exact)
        if ((reg_wdata[0] != fctl[0]) || (reg_wdata[3:2] != fctl[3:2])) stg_v <= 1'b0;
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
          fstat   <= 4'd0;
        end
        if (reg_wdata[3] | reg_wdata[4]) snap_pend <= 1'b1;
        if (reg_wdata[4]) sclr_pend <= 1'b1;
      end
      if (w_da) begin
        da       <= reg_wdata[6:0];
        dspace   <= reg_wdata[11:8];
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

      dbg <= fctl[14] ? {fevt[dsel1], fevt[dsel0]} : {evt[dsel1], evt[dsel0]};
    end
  end

`ifdef FORMAL
  // ================================================================ formal properties (formal/bpx.sby, VR-03)
  // No assumption is made about the host or the control port: every input is free in every cycle, so each
  // assertion holds under any protocol misuse and any timing (IR-01, IR-09).  Only reset is assumed at the start.
  localparam F_N = `FV_QUIET;       // RR-03: cycles of quiet inputs after which the chip must be idle and ready
  reg f_past = 1'b0;
  reg f_rs   = 1'b0;                // a reset has been applied: the registers are defined
  always @(posedge clk) begin
    f_past <= 1'b1;
    if (irst) f_rs <= 1'b1;
  end
  always @(posedge clk) if (!f_past) assume (rst);

  wire f_idle = ready & ~pending & ~rpend & ~wbusy & ~drd_pend & ~dwr_pend & ~snap_pend & ~clrp_pend & ~init_pend &
                (q_empty | stg_v);
  reg          f_req_q;
  reg  [W-1:0] f_id_q;
  reg  [1:0]   f_op_q;
  reg  [5:0]   f_quiet = 6'd0;
  wire f_activity = irst | reg_we | reg_re | (req_s != f_req_q) | (id_d != f_id_q) | (op_d != f_op_q);
  always @(posedge clk) begin
    f_req_q <= req_s;
    f_id_q  <= id_d;
    f_op_q  <= op_d;
    if (!f_past || f_activity) f_quiet <= 6'd0;
    else if (f_quiet != 6'd63) f_quiet <= f_quiet + 6'd1;
  end

  // the driving predictions of the in-flight entries, oldest first
  wire [QW-1:0] f_e0 = qmem[qhead];
  wire [QW-1:0] f_e1 = qmem[qhead+2'd1];
  wire [QW-1:0] f_e2 = qmem[qhead+2'd2];
  wire [QW-1:0] f_e3 = qmem[qhead+2'd3];
  wire f_d0 = f_e0[W+4], f_d1 = f_e1[W+4], f_d2 = f_e2[W+4], f_d3 = f_e3[W+4];

  always @(posedge clk) if (f_past && f_rs) begin
    // ---- structural invariants
    assert (qcnt <= 3'd4);                                           // FR-03: capacity
    assert (full == (qcnt == 3'd4));
    assert (!rpend || qcnt != 3'd0);
    assert (!stg_v || qcnt != 3'd0);
    assert (!rf_done || seen);                                       // resolve-first done: the request is still waiting
    assert (!(r_exec && q_empty));                                   // a resolve always has a branch to resolve
    assert (!(fl_exec && r_exec));
    assert (!(nop_exec && (p_exec || r_exec)));
    assert (!(p_refuse && (p_exec || r_exec)));
    assert (!(r_empty && (p_exec || r_exec)));
    // ---- FR-04: the speculative history is the committed history followed by the directions the host follows
    //      for the unresolved branches; with nothing in flight the two are equal
    case (qcnt)
      3'd0:    assert (sh == ch);
      3'd1:    assert (sh == {ch[HL-2:0], f_d0});
      3'd2:    assert (sh == {ch[HL-3:0], f_d0, f_d1});
      3'd3:    assert (sh == {ch[HL-4:0], f_d0, f_d1, f_d2});
      default: assert (sh == {ch[HL-5:0], f_d0, f_d1, f_d2, f_d3});
    endcase
    // ---- RR-03: bounded return to idle-and-ready once the inputs are quiet
    if (f_quiet >= F_N && !f_activity) assert (f_idle);

    if (!$past(irst)) begin
      // ---- RR-04: the reply changes only together with ACK
      if (ack == $past(ack)) assert (pred == $past(pred) && conf == $past(conf));
      // ---- IR-01: exactly one acknowledge per request edge, whatever the timing
      if (ack != $past(ack)) assert ($past(pending) || !$past(ready));
      if ($past(ack_t)) assert (ack != $past(ack) && ack == $past(req_s));
      // ---- FR-03: a request beyond capacity is refused whole; the queue is a FIFO counter
      if ($past(p_refuse)) assert (qcnt == $past(qcnt) && qhead == $past(qhead) && sh == $past(sh) && ch == $past(ch) && !pred);
      if ($past(p_exec && !r_exec)) assert (qcnt == $past(qcnt) + 3'd1 && qhead == $past(qhead));
      if ($past(r_exec && !r_misp && !p_exec)) assert (qcnt == $past(qcnt) - 3'd1 && qhead == $past(qhead) + 2'd1);
      // ---- FR-04: repair after a mispredicted resolve
      if ($past(r_misp)) assert (qcnt == 3'd0 && sh == ch && !stg_v && !rpend);
      // ---- FR-05: flush discards everything in flight, trains nothing, returns to the committed state
      if ($past(fl_exec)) begin
        assert (qcnt == 3'd0);
        assert (sh == ch);
        assert (ch == $past(ch));
        assert (!stg_v);
        assert (!$past(train));
      end
      if ($past(fl_exec && !misc_we)) assert (tc_q == $past(tc_q) && theta_q == $past(theta_q));
      // ---- FR-01: a predict changes no trained state and starts no table write
      if ($past(p_exec && !r_exec)) assert (ch == $past(ch) && !$past(train));
      if ($past(p_exec && !r_exec && !dwr_exec && !snap_exec)) assert (!$past(wcap));
      if ($past(p_exec && !r_exec && !misc_we)) assert (tc_q == $past(tc_q) && theta_q == $past(theta_q));
      if ($past(p_exec && !r_exec && !w_cmd && !(wact && snap_q && sclr_q)))
        assert (n_res == $past(n_res) && mis0 == $past(mis0) && mis1 == $past(mis1) && mis2 == $past(mis2) && mis3 == $past(mis3));
      //      trained registers change only by a resolve, a clear or a raw write
      if (ch != $past(ch)) assert ($past(r_exec) || $past(clr_exec));
      if (tc_q != $past(tc_q) || theta_q != $past(theta_q)) assert ($past(train) || $past(misc_we) || $past(clr_exec));
      if ($past(wcap)) assert ($past(train) || $past(clr_exec) || $past(dwr_tab) || $past(snap_exec));
      if ($past(train)) assert ($past(r_exec) && !$past(freeze));
      // ---- IR-09: a resolve with nothing in flight is refused, counted, acknowledged, and changes nothing else
      if ($past(r_empty)) assert (qcnt == 3'd0 && ch == $past(ch) && sh == $past(sh) && ack != $past(ack) && !$past(train) &&
                                  ($past(w_cmd) || eflags[0]) &&
                                  ($past(w_cmd) || $past(err_cnt) == 8'hff || err_cnt == $past(err_cnt) + 8'd1));
    end

    // ---- cover: each operation completes; a full queue; a misprediction with a full queue and the next predict
    cover (p_exec && qcnt == 3'd3);
    cover (p_refuse);
    cover (r_first);
    cover (r_same);
    cover (r_late);
    cover (r_only && !r_misp);
    cover (r_only && r_misp && q_full);
    cover (fl_exec && q_full);
    cover (nop_exec);
    cover (r_empty);
    cover (f_rep && p_exec);
    cover (f_rep && f_idle && n_res == 3);
  end
`ifdef FV_NOSPI
  // Task "nowait": with a silent control port, which requests can be made to wait?  These assertions justify the
  // coverage bins marked "cannot occur" in docs/coverage.md: FLUSH, NOP and refused requests are acknowledged in
  // the first clock they are seen, and so is a RESOLVE that finds the queue full.
  // A legal host: REQ toggles only after the host has seen ACK (two synchroniser flops later here), and ID/OP
  // hold while a request is pending.
  always @(posedge clk) assume (!reg_we && !reg_re);
  reg f_past2 = 1'b0;
  always @(posedge clk) f_past2 <= f_past;
  always @(posedge clk) if (f_past2) begin
    if (req_s != $past(req_s)) assume ($past(ack, 2) == $past(req_s) && $past(ack) == $past(req_s));
    if (req_s != ack) assume (id_d == $past(id_d) && op_d == $past(op_d));
  end
  always @(posedge clk) if (f_past && f_rs) begin
    assert (!clrp_pend && !dwr_pend && !snap_pend && !drd_pend);
    assert (!(nop_exec && seen));
    assert (!(fl_exec && seen));
    assert (!(p_refuse && seen));
    assert (!(r_empty && seen));
    assert (!(r_only && q_full && seen));
    assert (!(p_exec && rf_done && !seen));          // queue full: the resolve goes first, the predict a clock later
  end
`endif
  reg f_rep = 1'b0;                 // a misprediction was repaired with the queue full
  always @(posedge clk) if (f_past && f_rs && r_misp && q_full) f_rep <= 1'b1;
`endif
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
