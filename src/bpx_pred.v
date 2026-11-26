// bpx_pred - BPX-1 predictor datapath (docs/SPEC.md sections 6 and 7; golden model: model/bpx1.py).
//
// Four predictors over shared latch tables:
//   bimodal     2^LGB x 2-bit counters                   (table BC, field 0)
//   chooser     2^LGB x 2-bit counters                   (table BC, field 1)
//   gshare      2^LGG x 2-bit counters, HG history bits  (table G)
//   tournament  the chooser picks gshare or bimodal
//   default     NT tagged tables with increasing history lengths over the tournament.  Entry = {u[2], tag[TB],
//               ctr[3]}.  The longest matching table provides the prediction, unless its entry is new and weak
//               and the use-alternate counter says the next-best prediction is better.
//
// One read port.  rsel picks the lookup: 0 = predict (id_p, speculative history), 1 = commit (id_c, committed
// history; used for staging), 2 = raw table address for the control port.  `look` is the summary of the current
// lookup that the core keeps in its staging register:
//   look = { NT x {u[1:0], ctr[2:0], hit}, gsh[1:0], cho[1:0], bim[1:0] }      (table 0 = shortest history, lowest)
//
// Training is a pure function of a staged commit lookup (`stg`), the outcome, the use-alternate counter and the
// allocation LFSR; write addresses and the tags to store come from the commit hash.  `train` must be high in the
// cycle the write sequencer captures (wcap).
`default_nettype none

module bpx_pred #(
    parameter W   = 10,            // identifier bits
    parameter LGB = 6,             // log2 bimodal / chooser entries
    parameter LGG = 7,             // log2 gshare entries
    parameter HG  = 4,             // gshare history bits (HG <= LGG)
    parameter NT  = 3,             // tagged tables (2..4)
    parameter LGT = 4,             // log2 entries of each tagged table
    parameter TB  = 7,             // tag bits
    parameter [NT*6-1:0] LV = {6'd20, 6'd9, 6'd4},   // history length of each tagged table, table 0 lowest
    parameter HL  = 20,            // history register length (>= the longest history used)
    parameter ALLOC_LFSR = 1       // 1: allocation picks among free tables with an LFSR; 0: the shortest free one
) (
    input  wire               clk,
    input  wire               rst,       // synchronous: use-alternate counter and LFSR to their initial values
    // write sequencer (shared by all tables)
    input  wire               wcap,
    input  wire               wact,
    input  wire               wpulse,
    // lookup
    input  wire [1:0]         rsel,
    input  wire [W-1:0]       id_p,
    input  wire [HL-1:0]      hist_p,
    input  wire [W-1:0]       id_c,
    input  wire [HL-1:0]      hist_c,
    input  wire [2:0]         dbg_tab,   // 0 = BC, 1 = G, 2 + i = tagged table i
    input  wire [6:0]         dbg_addr,
    output wire [11:0]        dbg_rdata, // raw entry of the table selected by dbg_tab at the current read address
    output wire               p_def,
    output wire               p_bim,
    output wire               p_gsh,
    output wire               p_trn,
    output wire               conf,
    output wire [6*NT+5:0]    look,
    // training
    input  wire               train,
    input  wire               taken,
    input  wire [6*NT+5:0]    stg,
    // clear and raw write (control port)
    input  wire               clr,
    input  wire               dbg_we0,
    input  wire               dbg_we1,
    input  wire [11:0]        dbg_wdata,
    // small registers: use-alternate counter and allocation LFSR (readable, writable)
    output wire [3:0]         ua_q,
    output wire [7:0]         lfsr_q,
    input  wire               misc_we,
    input  wire [11:0]        misc_wdata   // {lfsr[7:0], ua[3:0]}
);
  localparam CB = 3;                 // tagged counter bits
  localparam UB = 2;                 // tagged useful bits
  localparam E0 = TB + CB;           // tagged field 0: {tag, ctr}
  localparam EW = E0 + UB;           // tagged entry: {u, tag, ctr}

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
  function [2:0] sat3;
    input [2:0] v;
    input       up;
    begin
      sat3 = up ? ((v == 3'd7) ? 3'd7 : v + 3'd1) : ((v == 3'd0) ? 3'd0 : v - 3'd1);
    end
  endfunction
  function [3:0] sat4;
    input [3:0] v;
    input       up;
    begin
      sat4 = up ? ((v == 4'd15) ? 4'd15 : v + 4'd1) : ((v == 4'd0) ? 4'd0 : v - 4'd1);
    end
  endfunction

  // staged view: predictions as a strictly sequential run would make them for the branch being resolved
  wire s_pbim, s_pgsh, s_ptrn, s_pdef, s_conf, s_pprov, s_palt, s_weak;
  reg  [3:0] ua;
  reg  [7:0] lfsr;
  assign ua_q   = ua;
  assign lfsr_q = lfsr;
  bpx_combine #(.NT(NT)) u_cs (.base(stg[5:0]), .tg(stg[6*NT+5:6]), .ua_hi(ua[3]),
      .p_bim(s_pbim), .p_gsh(s_pgsh), .p_trn(s_ptrn), .p_def(s_pdef), .conf(s_conf), .p_prov(s_pprov), .p_alt(s_palt), .wk(s_weak));

  wire       cho_we  = s_pbim != s_pgsh;
  wire [1:0] cho_new = sat2(s_cho, s_pgsh == taken);
  wire [1:0] bim_new = sat2(s_bim, taken);
  wire [1:0] gsh_new = sat2(s_gsh, taken);

  wire [LGB-1:0] ra_bc = r_dbg ? dbg_addr[LGB-1:0] : r_commit ? bi_c : bi_p;
  wire [LGG-1:0] ra_g  = r_dbg ? dbg_addr[LGG-1:0] : r_commit ? gi_c : gi_p;
  wire [3:0]     r_bc;
  wire [1:0]     r_g;
  wire           dw_bc = dw & (dbg_tab == 3'd0);
  wire           dw_g  = dw & (dbg_tab == 3'd1);

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

  // ---------------------------------------------------------------- tagged tables
  // staged per-table fields
  wire [NT-1:0] s_hit;
  wire [3*NT-1:0] s_ctr;
  wire [2*NT-1:0] s_u;
  genvar i;
  generate
    for (i = 0; i < NT; i = i + 1) begin : g_s
      assign s_hit[i]       = stg[6+6*i];
      assign s_ctr[3*i+:3]  = stg[7+6*i+:3];
      assign s_u[2*i+:2]    = stg[10+6*i+:2];
    end
  endgenerate

  // provider / alternate / allocation decisions (table NT-1 has the longest history)
  wire [NT-1:0] hit_above;     // a hit in some table above i
  wire [NT-1:0] is_prov, is_alt, above, cand;
  wire [NT-1:0] hit_above2;    // at least two hits above i
  generate
    for (i = 0; i < NT; i = i + 1) begin : g_d
      if (i == NT - 1) begin : g_top
        assign hit_above[i]  = 1'b0;
        assign hit_above2[i] = 1'b0;
      end else begin : g_low
        assign hit_above[i]  = hit_above[i+1] | s_hit[i+1];
        assign hit_above2[i] = hit_above2[i+1] | (hit_above[i+1] & s_hit[i+1]);
      end
      assign is_prov[i] = s_hit[i] & ~hit_above[i];
      assign is_alt[i]  = s_hit[i] & hit_above[i] & ~hit_above2[i];
      assign above[i]   = ~s_hit[i] & ~hit_above[i];              // strictly longer than the provider
      assign cand[i]    = above[i] & (s_u[2*i+:2] == 2'd0);
    end
  endgenerate
  wire       s_prov = |s_hit;
  wire       differ = s_pprov != s_palt;
  wire       miss   = s_pdef != taken;
  wire       anyc   = |cand;
  wire [2:0] ctr_new = taken ? 3'd4 : 3'd3;

  // which candidate: the pick-th one in increasing order; pick = leading ones of the stepped LFSR, at most nc-1
  wire [7:0] lfsr_n = {1'b0, lfsr[7:1]} ^ (lfsr[0] ? 8'hB8 : 8'h00);
  reg  [2:0]      ncand;
  reg  [3*NT-1:0] cbef;        // number of candidates below table k, 3 bits each
  integer         k;
  always @* begin
    ncand = 3'd0;
    for (k = 0; k < NT; k = k + 1) begin
      cbef[3*k+:3] = ncand;
      ncand          = ncand + {2'b00, cand[k]};
    end
  end
  wire [2:0] ones = !lfsr_n[0] ? 3'd0 : !lfsr_n[1] ? 3'd1 : !lfsr_n[2] ? 3'd2 : 3'd3;
  wire [2:0] nc1  = ncand - 3'd1;
  wire [2:0] pick = !ALLOC_LFSR ? 3'd0 : (ones < nc1) ? ones : nc1;

  always @(posedge clk) begin
    if (rst | (wcap & clr)) begin
      ua   <= 4'd8;
      lfsr <= 8'hE1;
    end else if (misc_we) begin
      ua   <= misc_wdata[3:0];
      lfsr <= misc_wdata[11:4];
    end else if (wcap & train) begin
      if (s_prov & differ & s_weak) ua <= sat4(ua, s_palt == taken);
      if (ALLOC_LFSR && miss && anyc) lfsr <= lfsr_n;
    end
  end

  wire [NT-1:0]    l_hit;
  wire [12*NT-1:0] r_tx;          // raw entries, zero-extended to 12 bits each
  generate
    for (i = 0; i < NT; i = i + 1) begin : g_t
      localparam L = LV[6*i+:6];
      // hashes, predict side and commit side
      wire [LGT-1:0] idf_p, idf_c, hf_p, hf_c;
      wire [TB-1:0]  idh_p, idh_c, ha_p, ha_c;
      wire [TB-2:0]  hb_p, hb_c;
      bpx_fold #(.IW(W), .OW(LGT))       f_ip (.v(id_p), .y(idf_p));
      bpx_fold #(.IW(W), .OW(LGT))       f_ic (.v(id_c), .y(idf_c));
      bpx_fold #(.IW(W - LGT), .OW(TB))  f_hp (.v(id_p[W-1:LGT]), .y(idh_p));
      bpx_fold #(.IW(W - LGT), .OW(TB))  f_hc (.v(id_c[W-1:LGT]), .y(idh_c));
      bpx_fold #(.IW(L), .OW(LGT))       f_xp (.v(hist_p[L-1:0]), .y(hf_p));
      bpx_fold #(.IW(L), .OW(LGT))       f_xc (.v(hist_c[L-1:0]), .y(hf_c));
      bpx_fold #(.IW(L), .OW(TB))        f_ap (.v(hist_p[L-1:0]), .y(ha_p));
      bpx_fold #(.IW(L), .OW(TB))        f_ac (.v(hist_c[L-1:0]), .y(ha_c));
      bpx_fold #(.IW(L), .OW(TB - 1))    f_bp (.v(hist_p[L-1:0]), .y(hb_p));
      bpx_fold #(.IW(L), .OW(TB - 1))    f_bc (.v(hist_c[L-1:0]), .y(hb_c));
      wire [LGT-1:0] ix_p = idf_p ^ hf_p;
      wire [LGT-1:0] ix_c = idf_c ^ hf_c;
      wire [TB-1:0]  tg_p = idh_p ^ ha_p ^ {hb_p, 1'b0};
      wire [TB-1:0]  tg_c = idh_c ^ ha_c ^ {hb_c, 1'b0};

      // training of this table
      wire [2:0] c_i    = s_ctr[3*i+:3];
      wire [1:0] u_i    = s_u[2*i+:2];
      wire       alloc  = miss & cand[i] & (cbef[3*i+:3] == pick);
      wire       decay  = miss & ~anyc & above[i] & (u_i != 2'd0);
      wire       upd    = is_prov[i] | (is_alt[i] & s_weak);
      wire       uupd   = is_prov[i] & differ;
      wire       we0    = upd | alloc;
      wire       we1    = uupd | alloc | decay;
      wire [2:0] ctr_w  = alloc ? ctr_new : sat3(c_i, taken);
      wire [1:0] u_w    = alloc ? 2'd0 : uupd ? sat2(u_i, s_pprov == taken) : u_i - 2'd1;

      wire          dw_t = dw & (dbg_tab == 3'd2 + i);
      wire [EW-1:0] r_t;
      bpx_ltab #(.AW(LGT), .RW(LGT > 4 ? 3 : 2), .EW0(E0), .EW1(UB), .INIT({2'd0, {TB{1'b0}}, 3'd3})) u_t (
          .clk(clk), .wcap(wcap), .wact(wact), .wpulse(wpulse), .clr(clr),
          .we0((train & we0) | (dw_t & dbg_we0)), .we1((train & we1) | (dw_t & dbg_we1)),
          .waddr(dw ? dbg_addr[LGT-1:0] : ix_c), .wdata(dw ? dbg_wdata[EW-1:0] : {u_w, tg_c, ctr_w}),
          .raddr(r_dbg ? dbg_addr[LGT-1:0] : r_commit ? ix_c : ix_p), .rdata(r_t));

      assign l_hit[i] = r_t[E0-1:CB] == (r_commit ? tg_c : tg_p);
      assign look[6+6*i+:6] = {r_t[EW-1:E0], r_t[CB-1:0], l_hit[i]};
      assign r_tx[12*i+:12] = r_t;        // zero-extended when the entry is narrower than 12 bits
    end
  endgenerate

  // ---------------------------------------------------------------- lookup result
  assign look[5:0] = {r_g, r_bc[3:2], r_bc[1:0]};
  wire l_pprov, l_palt, l_weak;
  bpx_combine #(.NT(NT)) u_cl (.base(look[5:0]), .tg(look[6*NT+5:6]), .ua_hi(ua[3]),
      .p_bim(p_bim), .p_gsh(p_gsh), .p_trn(p_trn), .p_def(p_def), .conf(conf), .p_prov(l_pprov), .p_alt(l_palt), .wk(l_weak));

  wire [11:0] dbg_t = r_tx[12*((dbg_tab >= 3'd2 && dbg_tab < 3'd2 + NT) ? dbg_tab - 3'd2 : 3'd0)+:12];
  assign dbg_rdata = (dbg_tab == 3'd0) ? {8'd0, r_bc} : (dbg_tab == 3'd1) ? {10'd0, r_g} : dbg_t;

  wire _unused = &{1'b0, l_pprov, l_palt, l_weak, s_conf, s_ptrn, dbg_addr, dbg_wdata, l_hit};
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

// bpx_combine - the four predictions and the confidence from one set of table entries.
//   base = {gsh[1:0], cho[1:0], bim[1:0]},  tg = NT x {u[1:0], ctr[2:0], hit}, table 0 lowest.
module bpx_combine #(
    parameter NT = 3
) (
    input  wire [5:0]      base,
    input  wire [6*NT-1:0] tg,
    input  wire            ua_hi,    // use-alternate counter >= its midpoint
    output wire            p_bim,
    output wire            p_gsh,
    output wire            p_trn,
    output wire            p_def,
    output wire            conf,
    output wire            p_prov,
    output wire            p_alt,
    output wire            wk
);
  wire [1:0] bim = base[1:0];
  wire [1:0] cho = base[3:2];
  wire [1:0] gsh = base[5:4];
  assign p_bim = bim[1];
  assign p_gsh = gsh[1];
  assign p_trn = cho[1] ? p_gsh : p_bim;
  wire [1:0] basectr = cho[1] ? gsh : bim;

  // scan from the shortest history up: the last hit seen is the provider, the one below it the alternate
  reg       prov, altp, pp;
  reg [2:0] pc;
  reg [1:0] pu;
  integer   i;
  always @* begin
    prov = 1'b0;
    pp   = p_trn;
    altp = p_trn;
    pc   = 3'd0;
    pu   = 2'd0;
    for (i = 0; i < NT; i = i + 1) begin
      if (tg[6*i]) begin
        altp = pp;
        prov = 1'b1;
        pc   = tg[6*i+1+:3];
        pu   = tg[6*i+4+:2];
        pp   = tg[6*i+3];
      end
    end
  end
  assign p_prov = pp;
  assign p_alt  = altp;
  assign wk     = prov & ((pc == 3'd3) | (pc == 3'd4)) & (pu == 2'd0);
  wire use_alt  = wk & ua_hi;
  assign p_def  = use_alt ? p_alt : p_prov;
  assign conf   = prov ? (((pc == 3'd0) | (pc == 3'd7)) & ~use_alt) : ((basectr == 2'd0) | (basectr == 2'd3));
endmodule
