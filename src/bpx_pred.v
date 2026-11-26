// bpx_pred - BPX-1 predictor datapath (docs/SPEC.md sections 6 and 7; golden model: model/bpx1.py).
//
// Four predictors over shared latch tables:
//   bimodal     2^LGB x 2-bit counters                   (table BC, field 0)
//   chooser     2^LGB x 2-bit counters                   (table BC, field 1)
//   gshare      2^LGG x 2-bit counters, HG history bits  (table G)
//   tournament  the chooser picks gshare or bimodal
//   default     a hashed perceptron: NT tables of CB-bit signed weights, table i indexed by a hash of the
//               identifier and of its LV[i] most recent outcomes.  The prediction is the sign of
//                   sum = SUM_i +-(2 w_i + 1)  [ + UBW (2 bim - 3) + UGW (2 gsh - 3) +- UTW ]
//               The weights learn (+-1, saturating) when the prediction was wrong or |sum| <= theta.
//               Switchable (feat): [0] the bracket: the reference predictors' counters as further inputs;
//               [1] the outcome just before a table's history segment negates its weight (stored as the bitwise
//               complement, so that one weight serves two history patterns); [2] theta adapts.
//
// One read port.  rsel picks the lookup: 0 = predict (id_p, speculative history), 1 = commit (id_c, committed
// history; used for staging), 2 = raw table address for the control port.  `look` is the summary of the current
// lookup that the core keeps in its staging register:
//   look = { tw[7:0], NT x w[CB-1:0], gsh[1:0], cho[1:0], bim[1:0] }   (table 0 = shortest history, lowest;
//                                                                       tw: the twin word, see below)
//
// Training is a pure function of a staged commit lookup (`stg`), the outcome, the committed history, theta and
// feat; write addresses come from the commit hash.  `train` must be high in the cycle the write sequencer
// captures (wcap).
//
// Leaky twin W3L (docs/mosaic.md 7.3): a 32 x 8 table of dynamic cells (bpx_ltab DYN = 1) beside W3, holding the
// code word {check[3:0], data[3:0]} of W3's weight; same index as W3 in every lookup.  tctl = FCTL[4:0] =
// {RESTORE, CODE[1:0], TWIN_USE, TWIN_EN}.  With TWIN_EN = 0 its read data is forced to 0 by named AND cells
// before anything else sees it (the floating nodes are unknown in gate-level simulation).  The decoded twin weight
// replaces W3's in the lookup sum only with TWIN_USE (or abstains: the term is left out); training always uses the
// staged static weights.  look/stg carry 8 more bits: the twin word of the lookup as restore-on-use would write it
// back (corrected when SEC-DED corrected it).  The twin is written with the standard pulse whenever W3 is written
// by training (the code word of the new weight), by clear (the code word of W3's initial value), by a raw write
// to space 12, and by restore-on-use.
`default_nettype none

module bpx_pred #(
    parameter W   = 8,             // identifier bits
    parameter LGB = 6,             // log2 bimodal / chooser entries
    parameter LGG = 7,             // log2 gshare entries
    parameter HG  = 4,             // gshare history bits (HG <= LGG)
    parameter NT  = 4,
    parameter LGT = 5,
    parameter CB  = 4,
    parameter [NT*6-1:0] LV = {6'd20, 6'd9, 6'd8, 6'd0},
    parameter HL  = 21,
    parameter PL  = 8,             // path history bits (one identifier bit per branch)
    parameter UBW = 2,
    parameter UGW = 4,
    parameter UTW = 4,
    parameter SK  = 0,
    parameter THETA = 10,
    parameter SB  = 8              // sum bits: |sum| <= NT (2^CB - 1) + 3 UBW + 3 UGW + UTW = 82 < 128
) (
    input  wire               clk,
    input  wire               rst,       // synchronous: threshold and its adaptation counter to their initial values
    input  wire [2:0]         feat,
    input  wire [4:0]         tctl,      // FCTL[4:0]: {RESTORE, CODE[1:0], TWIN_USE, TWIN_EN}
    output wire [3:0]         tw_flags,  // of the current lookup: {TW_SDC, TW_DET, TW_COR, TW_ERR}, 0 unless TWIN_EN
    output wire               p_def3,    // default prediction of the current lookup computed with W3 (not the twin)
    // write sequencer (shared by all tables)
    input  wire               wcap,
    input  wire               wact,
    input  wire               wpulse,
    // lookup
    input  wire [1:0]         rsel,
    input  wire [W-1:0]       id_p,
    input  wire [HL-1:0]      hist_p,
    input  wire [PL-1:0]      path_p,
    input  wire [W-1:0]       id_c,
    input  wire [HL-1:0]      hist_c,
    input  wire [PL-1:0]      path_c,
    input  wire [3:0]         dbg_tab,   // 0 = BC, 1 = G, 8 + i = weight table i, 12 = twin W3L
    input  wire [6:0]         dbg_addr,
    output wire [11:0]        dbg_rdata, // raw entry of the table selected by dbg_tab at the current read address
    output wire               p_def,
    output wire               p_bim,
    output wire               p_gsh,
    output wire               p_trn,
    output wire [3:0]         conf,      // confidence of {tournament, gshare, bimodal, default}
    output wire [CB*NT+13:0]  look,
    // training
    input  wire               train,
    input  wire               taken,
    input  wire [CB*NT+13:0]  stg,
    // clear and raw write (control port)
    input  wire               clr,
    input  wire               dbg_we0,
    input  wire               dbg_we1,
    input  wire [11:0]        dbg_wdata,
    // small registers: adaptation counter and threshold (readable, writable)
    output wire [3:0]         tc_q,
    output wire [7:0]         theta_q,
    input  wire               misc_we,
    input  wire [11:0]        misc_wdata   // {theta[7:0], tc[3:0]}
);
  localparam THB = 6;                // threshold bits
  wire r_commit = rsel[0];
  wire r_dbg    = rsel[1];
  wire dw       = dbg_we0 | dbg_we1;        // raw write from the control port (never together with train)

  // ---------------------------------------------------------------- base tables: bimodal + chooser, gshare
  wire [LGB-1:0] bi_p, bi_c;
  wire [LGG-1:0] gf_p, gf_c;
  bpx_fold #(.IW(W), .OW(LGB)) f_bp (.v(id_p), .y(bi_p));
  bpx_fold #(.IW(W), .OW(LGB)) f_bc (.v(id_c), .y(bi_c));
  bpx_fold #(.IW(W), .OW(LGG)) f_gp (.v(id_p), .y(gf_p));
  bpx_fold #(.IW(W), .OW(LGG)) f_gc (.v(id_c), .y(gf_c));
  wire [LGG-1:0] gi_p = gf_p ^ {hist_p[HG-1:0], {(LGG - HG) {1'b0}}};
  wire [LGG-1:0] gi_c = gf_c ^ {hist_c[HG-1:0], {(LGG - HG) {1'b0}}};

  wire [1:0] s_bim = stg[1:0];
  wire [1:0] s_cho = stg[3:2];
  wire [1:0] s_gsh = stg[5:4];

  function [1:0] sat2;
    input [1:0] v;
    input       up;
    begin
      sat2 = up ? ((v == 2'd3) ? 2'd3 : v + 2'd1) : ((v == 2'd0) ? 2'd0 : v - 2'd1);
    end
  endfunction

  reg  [THB-1:0] theta;
  reg  [3:0]     tc;               // two's complement
  assign theta_q = {{(8 - THB) {1'b0}}, theta};
  assign tc_q    = tc;

  // staged view: predictions as a strictly sequential run would make them for the branch being resolved
  wire [NT-1:0] neg_p, neg_c;
  wire s_pbim, s_pgsh, s_ptrn, s_pdef, s_palt, s_low;
  wire [3:0] s_conf;
  bpx_psum #(.NT(NT), .CB(CB), .SB(SB), .THB(THB), .UBW(UBW), .UGW(UGW), .UTW(UTW)) u_cs (
      .base(stg[5:0]), .wts(stg[CB*NT+5:6]), .neg(neg_c), .skip({NT{1'b0}}), .alt_w({CB{1'b0}}), .refs(feat[0]), .theta(theta),
      .p_bim(s_pbim), .p_gsh(s_pgsh), .p_trn(s_ptrn), .p_def(s_pdef), .p_alt(s_palt), .low(s_low), .conf(s_conf));

  wire       cho_we  = s_pbim != s_pgsh;
  wire [1:0] cho_new = sat2(s_cho, s_pgsh == taken);
  wire [1:0] bim_new = sat2(s_bim, taken);
  wire [1:0] gsh_new = sat2(s_gsh, taken);
  wire       miss    = s_pdef != taken;
  wire       learn   = miss | s_low;

  wire [LGB-1:0] ra_bc = r_dbg ? dbg_addr[LGB-1:0] : r_commit ? bi_c : bi_p;
  wire [LGG-1:0] ra_g  = r_dbg ? dbg_addr[LGG-1:0] : r_commit ? gi_c : gi_p;
  wire [3:0]     r_bc;
  wire [1:0]     r_g;
  wire           dw_bc = dw & (dbg_tab == 4'd0);
  wire           dw_g  = dw & (dbg_tab == 4'd1);

  bpx_ltab #(.AW(LGB), .RW(3), .EW0(2), .EW1(2), .INIT(4'b0101)) u_bc (
      .clk(clk), .wcap(wcap), .wact(wact), .wpulse(wpulse), .clr(clr),
      .we0(train | (dw_bc & dbg_we0)), .we1((train & cho_we) | (dw_bc & dbg_we1)),
      .waddr(dw ? dbg_addr[LGB-1:0] : bi_c), .wdata(dw ? dbg_wdata[3:0] : {cho_new, bim_new}),
      .raddr(ra_bc), .rdata(r_bc));
  bpx_ltab #(.AW(LGG), .RW(3), .EW0(2), .EW1(0), .INIT(2'b01)) u_g (
      .clk(clk), .wcap(wcap), .wact(wact), .wpulse(wpulse), .clr(clr),
      .we0(train | (dw_g & dbg_we0)), .we1(1'b0),
      .waddr(dw ? dbg_addr[LGG-1:0] : gi_c), .wdata(dw ? dbg_wdata[1:0] : gsh_new),
      .raddr(ra_g), .rdata(r_g));

  // ---------------------------------------------------------------- threshold adaptation
  wire tc_max = tc == 4'b0111;
  wire tc_min = tc == 4'b1000;
  always @(posedge clk) begin
    if (rst | (wcap & clr)) begin
      theta <= THETA;
      tc    <= 4'd0;
    end else if (misc_we) begin
      tc    <= misc_wdata[3:0];
      theta <= misc_wdata[4+:THB];
    end else if (wcap & train & feat[2]) begin
      if (miss) begin
        tc <= tc_max ? 4'd0 : tc + 4'd1;
        if (tc_max && theta != {THB{1'b1}}) theta <= theta + 1'b1;
      end else if (s_low) begin
        tc <= tc_min ? 4'd0 : tc - 4'd1;
        if (tc_min && theta != {{(THB - 1) {1'b0}}, 1'b1} && theta != {THB{1'b0}}) theta <= theta - 1'b1;
      end
    end
  end

  // ---------------------------------------------------------------- weight tables
  wire [12*NT-1:0] r_tx;          // raw entries, zero-extended to 12 bits each
  wire [LGT*NT-1:0] ix_pa, ix_ca; // predict and commit index of each table
  wire [CB*NT-1:0]  w_na;         // trained weight of each table
  genvar i;
  generate
    for (i = 0; i < NT; i = i + 1) begin : g_t
      localparam L  = LV[6*i+:6];
      localparam R  = (i * SK) % LGT;
      localparam L1 = (L == 0) ? 1 : L;
      localparam LP = (L1 < PL) ? L1 : PL;      // path bits hashed into this table's index
      // identifier hash: low bits xor the upper bits rotated by R
      wire [LGT-1:0] hi_p, hi_c;
      bpx_fold #(.IW(W - LGT), .OW(LGT)) f_ip (.v(id_p[W-1:LGT]), .y(hi_p));
      bpx_fold #(.IW(W - LGT), .OW(LGT)) f_ic (.v(id_c[W-1:LGT]), .y(hi_c));
      wire [2*LGT-1:0] rr_p = {hi_p, hi_p} << R;
      wire [2*LGT-1:0] rr_c = {hi_c, hi_c} << R;
      wire [LGT-1:0] ih_p = id_p[LGT-1:0] ^ rr_p[2*LGT-1:LGT];
      wire [LGT-1:0] ih_c = id_c[LGT-1:0] ^ rr_c[2*LGT-1:LGT];
      // history hash
      wire [LGT-1:0] hh_p, hh_c;
      if (L == 0) begin : g_h0
        assign hh_p = {LGT{1'b0}};
        assign hh_c = {LGT{1'b0}};
        assign neg_p[i] = 1'b0;
        assign neg_c[i] = 1'b0;
      end else begin : g_h
        if (L <= LGT) begin : g_short
          assign hh_p = {hist_p[L1-1:0], {(LGT - L1) {1'b0}}};
          assign hh_c = {hist_c[L1-1:0], {(LGT - L1) {1'b0}}};
        end else begin : g_long
          wire [LGT-1:0] fa_p, fa_c;
          wire [LGT-2:0] fb_p, fb_c;
          bpx_fold #(.IW(L1), .OW(LGT))     f_ap (.v(hist_p[L1-1:0]), .y(fa_p));
          bpx_fold #(.IW(L1), .OW(LGT))     f_ac (.v(hist_c[L1-1:0]), .y(fa_c));
          bpx_fold #(.IW(L1), .OW(LGT - 1)) f_bp (.v(hist_p[L1-1:0]), .y(fb_p));
          bpx_fold #(.IW(L1), .OW(LGT - 1)) f_bc (.v(hist_c[L1-1:0]), .y(fb_c));
          assign hh_p = fa_p ^ {fb_p, 1'b0};
          assign hh_c = fa_c ^ {fb_c, 1'b0};
        end
        assign neg_p[i] = feat[1] & hist_p[L1];
        assign neg_c[i] = feat[1] & hist_c[L1];
      end
      // path hash, rotated by two against the outcome hash
      wire [LGT-1:0] pf_p, pf_c;
      if (L == 0) begin : g_p0
        assign pf_p = {LGT{1'b0}};
        assign pf_c = {LGT{1'b0}};
      end else begin : g_p
        wire [LGT-1:0] px_p, px_c;
        bpx_fold #(.IW(LP), .OW(LGT)) f_pp (.v(path_p[LP-1:0]), .y(px_p));
        bpx_fold #(.IW(LP), .OW(LGT)) f_pc (.v(path_c[LP-1:0]), .y(px_c));
        assign pf_p = {px_p[LGT-3:0], px_p[LGT-1:LGT-2]};
        assign pf_c = {px_c[LGT-3:0], px_c[LGT-1:LGT-2]};
      end
      wire [LGT-1:0] ix_p = ih_p ^ hh_p ^ pf_p;
      wire [LGT-1:0] ix_c = ih_c ^ hh_c ^ pf_c;

      // training of this table: one step towards the outcome (away from it when the weight counts negated)
      wire [CB-1:0] w_i  = stg[6+CB*i+:CB];
      wire          up   = taken ^ neg_c[i];
      wire          atmx = w_i == {1'b0, {(CB - 1) {1'b1}}};
      wire          atmn = w_i == {1'b1, {(CB - 1) {1'b0}}};
      wire [CB-1:0] w_n  = up ? (atmx ? w_i : w_i + 1'b1) : (atmn ? w_i : w_i - 1'b1);

      wire          dw_t = dw & (dbg_tab == 4'd8 + i);
      wire [CB-1:0] r_t;
      bpx_ltab #(.AW(LGT), .RW(LGT > 4 ? 3 : 2), .EW0(CB), .EW1(0), .INIT(((i & 1) == (NT & 1)) ? {CB{1'b0}} : {CB{1'b1}})) u_t (
          .clk(clk), .wcap(wcap), .wact(wact), .wpulse(wpulse), .clr(clr),
          .we0((train & learn) | (dw_t & dbg_we0)), .we1(1'b0),
          .waddr(dw ? dbg_addr[LGT-1:0] : ix_c), .wdata(dw ? dbg_wdata[CB-1:0] : w_n),
          .raddr(r_dbg ? dbg_addr[LGT-1:0] : r_commit ? ix_c : ix_p), .rdata(r_t));

      assign look[6+CB*i+:CB] = r_t;
      assign r_tx[12*i+:12]   = {{(12 - CB) {1'b0}}, r_t};
      assign ix_pa[LGT*i+:LGT] = ix_p;
      assign ix_ca[LGT*i+:LGT] = ix_c;
      assign w_na[CB*i+:CB]    = w_n;
    end
  endgenerate

  // ---------------------------------------------------------------- leaky twin W3L (needs CB = 4)
  // Code word {c3, c2, c1, c0, d3, d2, d1, d0}, c_k = XOR of the three data bits other than d_k: extended Hamming
  // (8,4), distance 4, every code word of even parity.  A single data-bit error d_k gives the syndrome ~(1 << k),
  // a single check-bit error c_k the syndrome 1 << k; the parity of the word equals the parity of the syndrome.
  function [3:0] hchk;
    input [3:0] d;
    begin
      hchk = {d[0] ^ d[1] ^ d[2], d[0] ^ d[1] ^ d[3], d[0] ^ d[2] ^ d[3], d[1] ^ d[2] ^ d[3]};
    end
  endfunction

  wire       tw_en   = tctl[0];
  wire       tw_use  = tctl[1] & tctl[0];
  wire [1:0] tw_code = tctl[3:2];
  wire       tw_rst  = tctl[4] & tctl[0];          // restore-on-use
  wire [LGT-1:0] ix3_p = ix_pa[LGT*(NT-1)+:LGT];
  wire [LGT-1:0] ix3_c = ix_ca[LGT*(NT-1)+:LGT];
  wire [CB-1:0]  wn3   = w_na[CB*(NT-1)+:CB];
  wire [CB-1:0]  w3    = look[6+CB*(NT-1)+:CB];   // W3's weight in the current lookup
  wire [7:0]     s_tw  = stg[CB*NT+6+:8];         // staged twin word (restore value)
  wire           dw_tw = dw & (dbg_tab == 4'd12);
  wire           tw_rw = ~learn & tw_rst;          // a resolve that trains without writing W3 restores the twin entry
  wire [7:0]     tw_r;                             // raw read: unknown in gate-level simulation
  // the initial value is the code word of W3's initial value, -1 (table NT-1 always starts at all ones)
  bpx_ltab #(.AW(LGT), .RW(LGT > 4 ? 3 : 2), .EW0(8), .EW1(0), .INIT(8'hFF), .DYN(1)) u_tw (
      .clk(clk), .wcap(wcap), .wact(wact), .wpulse(wpulse), .clr(clr),
      .we0((train & (learn | tw_rw)) | (dw_tw & dbg_we0)), .we1(1'b0),
      .waddr(dw ? dbg_addr[LGT-1:0] : ix3_c), .wdata(dw ? dbg_wdata[7:0] : learn ? {hchk(wn3), wn3} : s_tw),
      .raddr(r_dbg ? dbg_addr[LGT-1:0] : r_commit ? ix3_c : ix3_p), .rdata(tw_r));

  // TWIN_EN = 0: the read data is forced to 0 here, before anything else sees it
  wire [7:0] tw_m;
`ifdef BPX_BEHAV
  assign tw_m = tw_r & {8{tw_en}};
`else
  genvar k;
  generate
    for (k = 0; k < 8; k = k + 1) begin : g_twm
      (* keep *) sky130_fd_sc_hd__and2_1 u_m (.A(tw_r[k]), .B(tw_en), .X(tw_m[k]));
    end
  endgenerate
`endif

  // decode by CODE: 0 data as stored; 1 odd parity -> abstain; 2 SEC-DED: odd parity -> correct the single bit,
  // even parity with a non-zero syndrome -> abstain; 3 any non-zero syndrome -> abstain
  wire [3:0] td  = tw_m[3:0];
  wire [3:0] syn = hchk(td) ^ tw_m[7:4];
  wire       par = ^tw_m;
  wire       snz = |syn;
  wire [3:0] dfl = {syn == 4'b0111, syn == 4'b1011, syn == 4'b1101, syn == 4'b1110};
  wire       sec = tw_code == 2'd2;
  wire [3:0] tdec = td ^ (dfl & {4{sec}});
  wire       tabs = ((tw_code == 2'd1) & par) | (sec & ~par & snz) | ((tw_code == 2'd3) & snz);
  wire       tcor = sec & par;
  wire       terr = tw_m != {hchk(w3), w3};
  wire       tsdc = ~tabs & (tdec != w3);
  assign tw_flags = {tsdc, tabs, tcor, terr} & {4{tw_en}};
  assign look[CB*NT+6+:8] = tcor ? {hchk(tdec), tdec} : tw_m;

  // ---------------------------------------------------------------- lookup result
  // TWIN_USE: the decoded twin weight replaces W3's (an abstaining twin leaves the term out); p_def3 is the
  // prediction with W3 (fault event 10)
  assign look[5:0] = {r_g, r_bc[3:2], r_bc[1:0]};
  wire l_low;
  wire [CB*NT-1:0] l_wts = {tw_use ? tdec : w3, look[CB*(NT-1)+5:6]};
  bpx_psum #(.NT(NT), .CB(CB), .SB(SB), .THB(THB), .UBW(UBW), .UGW(UGW), .UTW(UTW)) u_cl (
      .base(look[5:0]), .wts(l_wts), .neg(r_commit ? neg_c : neg_p), .skip({tw_use & tabs, {(NT - 1) {1'b0}}}),
      .alt_w(w3), .refs(feat[0]), .theta(theta),
      .p_bim(p_bim), .p_gsh(p_gsh), .p_trn(p_trn), .p_def(p_def), .p_alt(p_def3), .low(l_low), .conf(conf));

  wire [2:0]  dbg_ti = dbg_tab[2:0];
  wire [11:0] dbg_t  = r_tx[12*((dbg_tab[3] && dbg_ti < NT) ? dbg_ti : 3'd0)+:12];
  assign dbg_rdata = (dbg_tab == 4'd0) ? {8'd0, r_bc} : (dbg_tab == 4'd1) ? {10'd0, r_g} :
                     (dbg_tab == 4'd12) ? {4'd0, tw_m} : dbg_t;

  wire _unused = &{1'b0, l_low, s_conf, s_ptrn, s_palt, dbg_addr, dbg_wdata, misc_wdata};
endmodule

// bpx_fold - XOR of the consecutive OW-bit slices of v.
module bpx_fold #(
    parameter IW = 14,
    parameter OW = 5
) (
    input  wire [IW-1:0] v,
    output wire [OW-1:0] y
);
  reg [OW-1:0] r;
  integer i;
  always @* begin
    r = {OW{1'b0}};
    for (i = 0; i < IW; i = i + 1) r[i%OW] = r[i%OW] ^ v[i];
  end
  assign y = r;
endmodule

// bpx_psum - the four predictions and their confidences from one set of table entries.
//   base = {gsh[1:0], cho[1:0], bim[1:0]},  wts = NT x w[CB-1:0] (two's complement), table 0 lowest.
//   A weight w counts as 2w+1; negated it counts as -(2w+1) = 2(~w)+1: the complement of the stored bits.
//   skip[i] leaves table i's term out (an abstaining twin).  p_alt is the prediction with the last table's term
//   taken from alt_w instead (not skipped): the base of the sum is shared, only the last addition is doubled.
module bpx_psum #(
    parameter NT  = 5,
    parameter CB  = 4,
    parameter SB  = 9,
    parameter THB = 6,
    parameter UBW = 1,
    parameter UGW = 1,
    parameter UTW = 0
) (
    input  wire [5:0]       base,
    input  wire [CB*NT-1:0] wts,
    input  wire [NT-1:0]    neg,
    input  wire [NT-1:0]    skip,
    input  wire [CB-1:0]    alt_w,
    input  wire             refs,     // the reference predictors' counters are inputs of the sum
    input  wire [THB-1:0]   theta,
    output wire             p_bim,
    output wire             p_gsh,
    output wire             p_trn,
    output wire             p_def,
    output wire             p_alt,
    output wire             low,      // |sum| <= theta
    output wire [3:0]       conf
);
  wire [1:0] bim = base[1:0];
  wire [1:0] cho = base[3:2];
  wire [1:0] gsh = base[5:4];
  assign p_bim = bim[1];
  assign p_gsh = gsh[1];
  assign p_trn = cho[1] ? p_gsh : p_bim;
  wire [1:0] trnctr = cho[1] ? gsh : bim;

  // 2c - 3 of a 2-bit counter c: -3, -1, 1, 3 = {~c[1], c[0], 1}
  wire signed [2:0]    vb = {~bim[1], bim[0], 1'b1};
  wire signed [2:0]    vg = {~gsh[1], gsh[0], 1'b1};
  wire signed [SB-1:0] rsum = $signed(vb) * UBW + $signed(vg) * UGW + (p_trn ? UTW : -UTW);

  reg signed [SB-1:0] acc, acc0, acca;
  reg signed [CB:0]   v, va;
  integer i;
  always @* begin
    acc0 = refs ? rsum : {SB{1'b0}};
    for (i = 0; i < NT - 1; i = i + 1) begin
      v    = skip[i] ? {(CB + 1) {1'b0}} : {wts[CB*i+:CB] ^ {CB{neg[i]}}, 1'b1};
      acc0 = acc0 + v;
    end
    v    = skip[NT-1] ? {(CB + 1) {1'b0}} : {wts[CB*(NT-1)+:CB] ^ {CB{neg[NT-1]}}, 1'b1};
    va   = {alt_w ^ {CB{neg[NT-1]}}, 1'b1};
    acc  = acc0 + v;
    acca = acc0 + va;
  end
  wire [SB-1:0] mag = acc[SB-1] ? (~acc + 1'b1) : acc;
  assign p_def = ~acc[SB-1];
  assign p_alt = ~acca[SB-1];
  assign low   = mag <= {{(SB - THB) {1'b0}}, theta};
  assign conf  = {(trnctr == 2'd0) | (trnctr == 2'd3), (gsh == 2'd0) | (gsh == 2'd3), (bim == 2'd0) | (bim == 2'd3), ~low};
endmodule
