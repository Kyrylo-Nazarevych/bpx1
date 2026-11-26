// bpsim.cc - BPX-1 predictor design-space exploration engine (spec section 6: EX-02, EX-03).
//
// Replays benchmark event traces (bench/README.md) through candidate direction predictors and reports, per program:
//   mis_seq  mispredictions with one branch in flight (predict, then train, strictly sequential)
//   mis_cpu  mispredictions inside the U1 CPU timing model (training delayed until the branch resolves)
//   cycles   CPU cycles with this predictor driving;  snt  CPU cycles with static not-taken (no predictor)
// plus a storage and area estimate for each candidate.
//
// Chip semantics modelled (docs/SPEC.md): a prediction reads the trained tables as they are, with the history of all
// older branches; training happens at resolve, in program order, and recomputes its lookup from the trained state and
// the committed history, so the trained state always equals that of a strictly sequential run.  On the correct path
// the speculative history equals the true history, and wrong-path predictions are discarded without trace, so the
// replay needs only a delayed-training queue.
//
// Build: g++ -O2 -std=c++17 -pthread -o bpsim bpsim.cc
// Use:   bpsim <configs.txt> <out.tsv> <trace.evt>...   (options as leading key=value: W=10 R=1 rf=0 threads=8)
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
using namespace std;
typedef uint32_t u32;
typedef uint64_t u64;

// ---------------------------------------------------------------------------------------------- traces
struct Rec { u32 pc, target, info, extra; };
struct Trace { string name; vector<Rec> r; };

static bool load_evt(const string& path, Trace& t) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    u32 hdr[4];
    if (fread(hdr, 4, 4, f) != 4 || memcmp(hdr, "BPXE", 4) != 0) { fclose(f); return false; }
    t.r.resize(hdr[2]);
    size_t got = fread(t.r.data(), sizeof(Rec), hdr[2], f);
    fclose(f);
    return got == hdr[2];
}
// Plain "pc taken" text traces (screening only: no program, so no timing): 5 instructions assumed per branch.
static bool load_txt(const string& path, Trace& t) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    unsigned pc; int tk;
    while (fscanf(f, "%x %d", &pc, &tk) == 2) t.r.push_back({pc, pc - 4, (u32)((tk ? 4 : 0) | (5u << 8)), 0});
    fclose(f);
    return !t.r.empty();
}

// ---------------------------------------------------------------------------------------------- helpers
static inline u32 mask32(int b) { return b >= 32 ? 0xffffffffu : ((1u << b) - 1); }
static inline u64 mask64(int b) { return b >= 64 ? ~0ull : ((1ull << b) - 1); }
static inline u32 fold(u64 v, int inbits, int outbits) {  // XOR-fold the low inbits of v into outbits
    if (outbits <= 0) return 0;
    v &= mask64(inbits);
    u32 r = 0;
    while (v) { r ^= (u32)(v & mask32(outbits)); v >>= outbits; }
    return r;
}
static inline int ilog2(int n) { int l = 0; while ((1 << l) < n) l++; return l; }
static inline void sat(uint8_t& c, bool up, int mx) { if (up) { if (c < mx) c++; } else { if (c > 0) c--; } }

struct Params {
    map<string, string> kv;
    int geti(const string& k, int d) const { auto it = kv.find(k); return it == kv.end() ? d : atoi(it->second.c_str()); }
    double getd(const string& k, double d) const { auto it = kv.find(k); return it == kv.end() ? d : atof(it->second.c_str()); }
    string gets(const string& k, const string& d = "") const { auto it = kv.find(k); return it == kv.end() ? d : it->second; }
    vector<int> getl(const string& k) const {
        vector<int> v; auto it = kv.find(k); if (it == kv.end()) return v;
        stringstream ss(it->second); string s;
        while (getline(ss, s, ',')) if (!s.empty()) v.push_back(atoi(s.c_str()));
        return v;
    }
};

// Area model, um^2, sky130_fd_sc_hd.  Table constants are a fit to Yosys syntheses of src/bpx_ltab.v against the PDK
// liberty file with LibreLane's synthesis cell exclusions (64..256 x 2 bits, 16..64 x 9..11 bits; fit error < 3%):
//   table = 550 + entries * (20.5 * entry_bits + 6.5)
// (latch 15.0 + hand-mapped AND-OR read tree per bit; write gate and column-mux share per entry; column clock gates,
// row-select and write-data latches and decoders per table).  Logic uses the drive-strength-2 cells the flow's
// synthesis is restricted to: XOR 16.3, flop 21.3 (22 with a share of its enable logic), mux2 11.3, full adder 20.
static const double A_BIT = 20.5, A_ENT = 6.5, A_ENT2 = 6.5, A_TAB = 550, A_FF = 22.0, A_XOR = 16.3, A_MUX = 11.3, A_FA = 20.0;
static double table_area(double entries, double ebits, bool two_fields = false) { return entries * ebits * A_BIT + entries * (two_fields ? A_ENT2 : A_ENT) + A_TAB; }
struct Cost { double bits = 0, sbits = 0, area = 0, sarea = 0; };  // s* = part shared with the reference predictors

struct Ctx { u32 id; bool bias; u64 gh; int same; u64 ph = 0; };   // ph: path history, one identifier bit per branch

struct Pred {
    virtual ~Pred() {}
    virtual bool predict(const Ctx&) = 0;
    virtual void train(const Ctx&, bool taken) = 0;
    virtual Cost cost() = 0;
    // predict and let the predictor count its own observations against the outcome (fault campaign); returns the
    // driving prediction.  report(): those counts as text for the TSV.
    virtual bool predict_count(const Ctx& c, bool) { return predict(c); }
    virtual string report() { return ""; }
};

// ---------------------------------------------------------------------------------------------- fault campaign helpers
// docs/fault_campaign.md.  Random numbers: splitmix64 (seeded per run from the seed, the program and the scenario).
struct Rng {
    u64 s;
    explicit Rng(u64 seed = 1) : s(seed) {}
    u64 next() { u64 z = (s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
    double uni() { return ((double)(next() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }   // in (0, 1)
    double expo(double mean) { return -mean * log(uni()); }
    double norm() { double a = uni(), b = uni(); return sqrt(-2.0 * log(a)) * cos(6.283185307179586 * b); }
    u32 below(u32 n) { return (u32)(next() % n); }
};
static u64 hash_str(const string& s) { u64 h = 1469598103934665603ull; for (char ch : s) { h ^= (uint8_t)ch; h *= 1099511628211ull; } return h; }
static u64 mix(u64 a, u64 b) { Rng r(a ^ (b * 0x9E3779B97F4A7C15ull)); return r.next(); }

// The twin's code (docs/mosaic.md 7.3, model/bpx1.py tw_*): word = {check[3:0], data[3:0]}, c_k = XOR of the data
// bits other than d_k (extended Hamming (8,4)).  Decode by CODE: 0 none, 1 parity -> abstain, 2 SEC-DED, 3 detect all.
static inline int tw_check(int d) { int p = (d ^ (d >> 1) ^ (d >> 2) ^ (d >> 3)) & 1, r = 0; for (int k = 0; k < 4; k++) r |= (p ^ ((d >> k) & 1)) << k; return r; }
static inline int tw_encode(int d) { d &= 15; return (tw_check(d) << 4) | d; }
static inline void tw_decode(int word, int code, int& d, bool& ab, bool& cor) {
    d = word & 15; ab = false; cor = false;
    int syn = tw_check(d) ^ (word >> 4), par = __builtin_popcount((unsigned)word & 0xFF) & 1;
    if (code == 1) ab = par;
    else if (code == 2) {
        if (par) { for (int k = 0; k < 4; k++) if (syn == (15 ^ (1 << k))) d ^= 1 << k; cor = true; }
        else ab = syn != 0;
    } else if (code == 3) ab = syn != 0;
}

// Fault targets: the chip's tables as the model's raw spaces see them (bimodal and chooser are one raw word).
enum { TG_BIM, TG_CHO, TG_GSH, TG_W0, TG_W1, TG_W2, TG_W3, TG_TW, TG_N };
static const char* TG_NAME[TG_N] = {"bim", "cho", "gsh", "w0", "w1", "w2", "w3", "tw"};
struct Flip { u64 k; int tab, e, b; };    // applied just before resolve number k (1-based), in every instance

// One decay (or flip) scenario of the leaky twin, with its three physical copies: 0 RESTORE off (every CODE reads
// the same words), 1 RESTORE on with CODE 0, 1, 3 (restore writes the word as read), 2 RESTORE on with CODE 2
// (restore writes the corrected word).  Every copy is written with W3's code word whenever W3 trains.
// Decay: from its last write each bit keeps its value for a lifetime L (in resolved branches) and then reads as the
// cell's preferred level; a read at time t (resolves started) sees the decayed level if t >= t_write + ceil(L).
static const u32 NEVER = 0xFFFFFFFFu;
struct TwinScen {
    int kind = 0;                 // 0 no decay (flips only), 1 exponential lifetime drawn at every write, 2 per-cell fixed log-normal lifetime
    double tau = 0; int prefmode = 0;
    string spec;
    uint8_t pref[32] = {};        // preferred level, bit b of entry e
    u32 life[32][8] = {};
    struct Store { uint8_t w[32]; u32 D[32][8]; u32 dmin[32]; Rng rng; };
    Store st[3];
    u64 mis[4][2] = {}, ev[4][2][4] = {};   // per CODE x RESTORE: mispredictions of the twin-using default; TW_ERR, TW_COR, TW_DET, TW_SDC
    vector<array<u64, 4>> log;    // effective decay events of one copy (validation): resolve index, entry, bit, level
    int logst = -1;
    bool parse(const string& s, double sig, u64 seed) {
        spec = s;
        if (s == "x") kind = 0;
        else {
            if (s[0] == 'e') kind = 1; else if (s[0] == 'l') kind = 2; else return false;
            size_t p = s.find('p'); if (p == string::npos) return false;
            tau = atof(s.substr(1, p - 1).c_str()); char m = s[p + 1];
            prefmode = m == '0' ? 0 : m == '1' ? 1 : m == 'm' ? 2 : -1;
            if (tau <= 0 || prefmode < 0) return false;
        }
        Rng r(mix(seed, hash_str(s)));      // per-cell properties: the same "chip" for every program
        for (int e = 0; e < 32; e++) {
            pref[e] = prefmode == 0 ? 0 : prefmode == 1 ? 0xFF : (uint8_t)(r.next() & 0xFF);
            for (int b = 0; b < 8; b++) { double L = tau * exp(sig * r.norm()); life[e][b] = L >= 4e9 ? NEVER - 1 : (u32)max(1.0, ceil(L)); }
        }
        return true;
    }
    inline int read(int si, int e, u64 now) const {
        const Store& s = st[si]; int w = s.w[e];
        if (kind && now >= s.dmin[e])
            for (int b = 0; b < 8; b++) if (now >= s.D[e][b]) w = (w & ~(1 << b)) | (((pref[e] >> b) & 1) << b);
        return w;
    }
    void logdecays(int si, int e, u64 now) {
        Store& s = st[si];
        for (int b = 0; b < 8; b++) if (s.D[e][b] <= now) log.push_back({(u64)s.D[e][b], (u64)e, (u64)b, (u64)((pref[e] >> b) & 1)});
    }
    void write(int si, int e, int val, u64 now) {
        Store& s = st[si];
        if (kind && si == logst) logdecays(si, e, now);
        s.w[e] = (uint8_t)val; s.dmin[e] = NEVER;
        if (!kind) return;
        for (int b = 0; b < 8; b++) {
            u32 D = NEVER;
            if (((val >> b) & 1) != ((pref[e] >> b) & 1)) {
                double L = kind == 1 ? max(1.0, ceil(s.rng.expo(tau))) : (double)life[e][b];
                double t = (double)now + L; D = t >= 4e9 ? NEVER - 1 : (u32)t;
            }
            s.D[e][b] = D; s.dmin[e] = min(s.dmin[e], D);
        }
    }
};

// ---------------------------------------------------------------------------------------------- simple predictors
struct StaticPred : Pred {
    int mode;  // 0 not-taken, 1 taken, 2 hint (backward taken)
    StaticPred(int m) : mode(m) {}
    bool predict(const Ctx& c) override { return mode == 1 || (mode == 2 && c.bias); }
    void train(const Ctx&, bool) override {}
    Cost cost() override { return {}; }
};

struct Bimodal {
    int n = 0, cb = 2, mx = 3; bool agree = false; vector<uint8_t> t;
    void init(int n_, int cb_, bool agree_, int ini) {
        n = n_; cb = cb_; mx = (1 << cb) - 1; agree = agree_;
        if (ini < 0) ini = agree ? (1 << (cb - 1)) : (1 << (cb - 1)) - 1;  // weakly agree / weakly not-taken
        t.assign(max(n, 1), (uint8_t)ini);
    }
    inline int idx(const Ctx& c) const { return fold(c.id, 32, ilog2(n)) & (n - 1); }
    inline bool pred(const Ctx& c) const { bool b = t[idx(c)] >> (cb - 1); return agree ? (b == c.bias) : b; }
    inline bool strong(const Ctx& c) const { uint8_t v = t[idx(c)]; return v == 0 || v == mx; }
    inline void upd(const Ctx& c, bool taken) { sat(t[idx(c)], agree ? (taken == c.bias) : taken, mx); }
    double bits() const { return (double)n * cb; }
    double area() const { return n ? table_area(n, cb) : 0; }
};

struct GShare {
    int n = 0, h = 0, cb = 2, mx = 3, lg = 0; bool agree = false; vector<uint8_t> t;
    void init(int n_, int h_, int cb_, bool agree_, int ini) {
        n = n_; h = h_; cb = cb_; mx = (1 << cb) - 1; lg = ilog2(n); agree = agree_;
        if (ini < 0) ini = agree ? (1 << (cb - 1)) : (1 << (cb - 1)) - 1;
        t.assign(max(n, 1), (uint8_t)ini);
    }
    inline int idx(const Ctx& c) const {
        u32 hist = (h <= lg) ? ((u32)(c.gh & mask64(h)) << (lg - h)) : fold(c.gh, h, lg);
        return (fold(c.id, 32, lg) ^ hist) & (n - 1);
    }
    inline bool pred(const Ctx& c) const { bool b = t[idx(c)] >> (cb - 1); return agree ? (b == c.bias) : b; }
    inline void upd(const Ctx& c, bool taken) { sat(t[idx(c)], agree ? (taken == c.bias) : taken, mx); }
    double bits() const { return (double)n * cb; }
    double area() const { return n ? table_area(n, cb) + A_XOR * max(h, 1) : 0; }
};

struct BimodalP : Pred {
    Bimodal b;
    BimodalP(const Params& p) { b.init(p.geti("n", 128), p.geti("cb", 2), p.geti("agree", 0), p.geti("init", -1)); }
    bool predict(const Ctx& c) override { return b.pred(c); }
    void train(const Ctx& c, bool t) override { b.upd(c, t); }
    Cost cost() override { Cost k; k.bits = b.bits(); k.area = b.area(); return k; }
};
struct GShareP : Pred {
    GShare g;
    GShareP(const Params& p) { g.init(p.geti("n", 256), p.geti("h", 8), p.geti("cb", 2), p.geti("agree", 0), p.geti("init", -1)); }
    bool predict(const Ctx& c) override { return g.pred(c); }
    void train(const Ctx& c, bool t) override { g.upd(c, t); }
    Cost cost() override { Cost k; k.bits = g.bits(); k.area = g.area() + 2 * A_FF * g.h; return k; }
};

// gselect (McFarling): index = {address bits, history bits} concatenated.
struct GSelectP : Pred {
    int a, h; vector<uint8_t> t;
    GSelectP(const Params& p) { a = p.geti("a", 5); h = p.geti("h", 2); t.assign((size_t)1 << (a + h), 1); }
    inline int idx(const Ctx& c) const { return (int)(((fold(c.id, 32, a) & mask32(a)) << h) | (u32)(c.gh & mask64(h))); }
    bool predict(const Ctx& c) override { return t[idx(c)] >= 2; }
    void train(const Ctx& c, bool tk) override { sat(t[idx(c)], tk, 3); }
    Cost cost() override { Cost k; k.bits = 2.0 * t.size(); k.area = table_area((double)t.size(), 2) + 2 * A_FF * h; return k; }
};

// Alloyed MAs (Skadron, Martonosi, Clark 2000): index = {global history, local history, address}.
struct AlloyP : Pred {
    int g, pl, a, nl; vector<uint16_t> bht; vector<uint8_t> t;
    AlloyP(const Params& p) { g = p.geti("g", 3); pl = p.geti("p", 2); a = p.geti("a", 4); nl = p.geti("nl", 64); bht.assign(nl, 0); t.assign((size_t)1 << (g + pl + a), 1); }
    inline int li(const Ctx& c) const { return fold(c.id, 32, ilog2(nl)) & (nl - 1); }
    inline int idx(const Ctx& c) const { return (int)((((u32)(c.gh & mask64(g))) << (pl + a)) | ((u32)(bht[li(c)] & mask32(pl)) << a) | (fold(c.id, 32, a) & mask32(a))); }
    bool predict(const Ctx& c) override { return t[idx(c)] >= 2; }
    void train(const Ctx& c, bool tk) override { sat(t[idx(c)], tk, 3); uint16_t& l = bht[li(c)]; l = (uint16_t)(((l << 1) | (tk ? 1 : 0)) & mask32(pl)); }
    Cost cost() override { Cost k; k.bits = 2.0 * t.size() + (double)nl * pl; k.area = table_area((double)t.size(), 2) + table_area(nl, pl) + 2 * A_FF * g; return k; }
};

// Bimodal with shared hysteresis (Seznec): one direction bit per entry, one hysteresis bit per `sh` entries.
struct BimHystP : Pred {
    int n, sh; vector<uint8_t> d, hy;
    BimHystP(const Params& p) { n = p.geti("n", 256); sh = p.geti("sh", 4); d.assign(n, 0); hy.assign(n / sh, 0); }
    inline int idx(const Ctx& c) const { return fold(c.id, 32, ilog2(n)) & (n - 1); }
    bool predict(const Ctx& c) override { return d[idx(c)]; }
    void train(const Ctx& c, bool tk) override {
        int i = idx(c); uint8_t& h = hy[i / sh];
        if ((bool)d[i] == tk) h = 1; else if (h) h = 0; else d[i] = tk;
    }
    Cost cost() override { Cost k; k.bits = n + (double)n / sh; k.area = table_area(n, 1) + table_area((double)n / sh, 1); return k; }
};

// McFarling combining predictor: bimodal + gshare + chooser (2-bit, indexed by branch id, or by history if chist=1).
struct Tourn {
    Bimodal b; GShare g; vector<uint8_t> ch; int cn = 0; bool chist = false;
    void init(int nb, int ng, int h, int nc, bool agree, int ini, bool chist_) {
        b.init(nb, 2, agree, ini); g.init(ng, h, 2, agree, ini); cn = nc; chist = chist_; ch.assign(max(cn, 1), 1);
    }
    inline int cidx(const Ctx& c) const { return (chist ? fold(c.gh, ilog2(cn), ilog2(cn)) : fold(c.id, 32, ilog2(cn))) & (cn - 1); }
    inline bool pred(const Ctx& c) const { return (ch[cidx(c)] >= 2) ? g.pred(c) : b.pred(c); }
    inline void upd(const Ctx& c, bool taken) {
        bool pb = b.pred(c), pg = g.pred(c);
        if (pb != pg) sat(ch[cidx(c)], pg == taken, 3);
        b.upd(c, taken); g.upd(c, taken);
    }
    double bits() const { return b.bits() + g.bits() + 2.0 * cn; }
    double area() const { return b.area() + g.area() + table_area(cn, 2) + A_MUX; }
};
struct TournP : Pred {
    Tourn t;
    TournP(const Params& p) { t.init(p.geti("nb", 128), p.geti("ng", 256), p.geti("h", 8), p.geti("nc", 128), p.geti("agree", 0), p.geti("init", -1), p.geti("chist", 0)); }
    bool predict(const Ctx& c) override { return t.pred(c); }
    void train(const Ctx& c, bool tk) override { t.upd(c, tk); }
    Cost cost() override { Cost k; k.bits = t.bits(); k.area = t.area() + 2 * A_FF * t.g.h; return k; }
};

// Bi-mode (Lee, Chen, Mudge 1997): choice table by id; two direction tables by gshare hash.
struct BimodeP : Pred {
    Bimodal choice; GShare dt, dn;
    BimodeP(const Params& p) {
        choice.init(p.geti("nc", 64), 2, false, -1);
        dt.init(p.geti("nd", 128), p.geti("h", 7), 2, false, 2); dn.init(p.geti("nd", 128), p.geti("h", 7), 2, false, 1);
    }
    bool predict(const Ctx& c) override { return choice.pred(c) ? dt.pred(c) : dn.pred(c); }
    void train(const Ctx& c, bool t) override {
        bool ch = choice.pred(c); bool p = ch ? dt.pred(c) : dn.pred(c);
        if (ch) dt.upd(c, t); else dn.upd(c, t);
        if (!(ch != t && p == t)) choice.upd(c, t);
    }
    Cost cost() override { Cost k; k.bits = choice.bits() + dt.bits() + dn.bits(); k.area = choice.area() + dt.area() + dn.area() + 2 * A_FF * dt.h; return k; }
};

// YAGS (Eden, Mudge 1998): choice table; two tagged exception caches.
struct YagsP : Pred {
    Bimodal choice; int n, lg, h, tb; struct E { uint16_t tag; uint8_t c; }; vector<E> ct, cn_;
    YagsP(const Params& p) {
        choice.init(p.geti("nc", 128), 2, false, -1);
        n = p.geti("n", 32); lg = ilog2(n); h = p.geti("h", 6); tb = p.geti("tb", 6);
        ct.assign(n, {0xffff, 2}); cn_.assign(n, {0xffff, 1});
    }
    inline int idx(const Ctx& c) const { return (fold(c.id, 32, lg) ^ fold(c.gh, h, lg)) & (n - 1); }
    inline u32 tag(const Ctx& c) const { return (c.id ^ (fold(c.gh, h, tb) << 1)) & mask32(tb); }
    bool predict(const Ctx& c) override {
        bool ch = choice.pred(c); const E& e = ch ? cn_[idx(c)] : ct[idx(c)];
        return (e.tag == tag(c)) ? (e.c >= 2) : ch;
    }
    void train(const Ctx& c, bool t) override {
        bool ch = choice.pred(c); E& e = ch ? cn_[idx(c)] : ct[idx(c)];
        bool hit = e.tag == tag(c); bool p = hit ? (e.c >= 2) : ch;
        if (hit) sat(e.c, t, 3);
        else if (ch != t) { e.tag = (uint16_t)tag(c); e.c = t ? 2 : 1; }
        if (!(ch != t && p == t)) choice.upd(c, t);
    }
    Cost cost() override { Cost k; k.bits = choice.bits() + 2.0 * n * (tb + 2); k.area = choice.area() + 2 * table_area(n, tb + 2) + 2 * tb * 10.3 + 2 * A_FF * h + A_XOR * 2 * h; return k; }
};

// Enhanced gskew (Michaud, Seznec, Uhlig 1997): bimodal + two history tables with different hashes, majority vote.
struct GskewP : Pred {
    Bimodal b; GShare g0, g1; int h1;
    GskewP(const Params& p) {
        int n = p.geti("n", 128);
        b.init(n, 2, false, -1); g0.init(n, p.geti("h0", 5), 2, false, -1); g1.init(n, p.geti("h1", 10), 2, false, -1); h1 = p.geti("h1", 10);
    }
    bool predict(const Ctx& c) override { return (int)b.pred(c) + g0.pred(c) + g1.pred(c) >= 2; }
    void train(const Ctx& c, bool t) override {
        bool pb = b.pred(c), p0 = g0.pred(c), p1 = g1.pred(c); bool p = (int)pb + p0 + p1 >= 2;
        if (p != t) { b.upd(c, t); g0.upd(c, t); g1.upd(c, t); }
        else { if (pb == t) b.upd(c, t); if (p0 == t) g0.upd(c, t); if (p1 == t) g1.upd(c, t); }
    }
    Cost cost() override { Cost k; k.bits = b.bits() + g0.bits() + g1.bits(); k.area = b.area() + g0.area() + g1.area() + 2 * A_FF * h1; return k; }
};

// Two-level local predictor PAg (Yeh, Patt): per-branch history table -> shared pattern table.
struct Local {
    int nl = 0, hl = 0, cb = 2, mx = 3; vector<uint16_t> bht; vector<uint8_t> pht;
    void init(int nl_, int hl_, int cb_) { nl = nl_; hl = hl_; cb = cb_; mx = (1 << cb) - 1; bht.assign(max(nl, 1), 0); pht.assign(1 << hl, (uint8_t)((1 << (cb - 1)) - 1)); }
    inline int li(const Ctx& c) const { return fold(c.id, 32, ilog2(nl)) & (nl - 1); }
    inline bool pred(const Ctx& c) const { return pht[bht[li(c)]] >> (cb - 1); }
    inline void upd(const Ctx& c, bool t) { uint16_t& hsr = bht[li(c)]; sat(pht[hsr], t, mx); hsr = (uint16_t)(((hsr << 1) | (t ? 1 : 0)) & mask32(hl)); }
    double bits() const { return (double)nl * hl + (double)(1 << hl) * cb; }
    double area() const { return table_area(nl, hl) + table_area(1 << hl, cb); }
};
struct LocalP : Pred {
    Local l;
    LocalP(const Params& p) { l.init(p.geti("nl", 16), p.geti("hl", 6), p.geti("cb", 2)); }
    bool predict(const Ctx& c) override { return l.pred(c); }
    void train(const Ctx& c, bool t) override { l.upd(c, t); }
    Cost cost() override { Cost k; k.bits = l.bits(); k.area = l.area(); return k; }
};
// Alpha 21264 style: local two-level + global (history-indexed) + chooser indexed by history.
struct AlphaP : Pred {
    Local l; int ng, lg; vector<uint8_t> g, ch;
    AlphaP(const Params& p) { l.init(p.geti("nl", 16), p.geti("hl", 6), p.geti("cb", 3)); ng = p.geti("ng", 128); lg = ilog2(ng); g.assign(ng, 1); ch.assign(ng, 1); }
    inline int gi(const Ctx& c) const { return (int)(c.gh & (u64)(ng - 1)); }
    bool predict(const Ctx& c) override { return ch[gi(c)] >= 2 ? (g[gi(c)] >= 2) : l.pred(c); }
    void train(const Ctx& c, bool t) override {
        bool pl = l.pred(c), pg = g[gi(c)] >= 2;
        if (pl != pg) sat(ch[gi(c)], pg == t, 3);
        sat(g[gi(c)], t, 3); l.upd(c, t);
    }
    Cost cost() override { Cost k; k.bits = l.bits() + 4.0 * ng; k.area = l.area() + 2 * table_area(ng, 2) + 2 * A_FF * lg; return k; }
};

// Perceptron (Jimenez, Lin 2001).
struct PerceptronP : Pred {
    int np, h, wb, wmax, theta; vector<int8_t> w;
    PerceptronP(const Params& p) { np = p.geti("np", 8); h = p.geti("h", 12); wb = p.geti("wb", 6); wmax = (1 << (wb - 1)) - 1; theta = p.geti("theta", (int)(1.93 * h + 14)); w.assign((size_t)np * (h + 1), 0); }
    inline int row(const Ctx& c) const { return fold(c.id, 32, ilog2(np)) & (np - 1); }
    inline int out(const Ctx& c) const { const int8_t* r = &w[(size_t)row(c) * (h + 1)]; int y = r[0]; for (int i = 0; i < h; i++) y += ((c.gh >> i) & 1) ? r[i + 1] : -r[i + 1]; return y; }
    bool predict(const Ctx& c) override { return out(c) >= 0; }
    void train(const Ctx& c, bool t) override {
        int y = out(c);
        if ((y >= 0) != t || abs(y) <= theta) {
            int8_t* r = &w[(size_t)row(c) * (h + 1)];
            auto adj = [&](int8_t& v, bool up) { if (up) { if (v < wmax) v++; } else { if (v > -wmax - 1) v--; } };
            adj(r[0], t);
            for (int i = 0; i < h; i++) adj(r[i + 1], (((c.gh >> i) & 1) != 0) == t);
        }
    }
    Cost cost() override { Cost k; k.bits = (double)np * (h + 1) * wb; k.area = table_area(np, (h + 1) * wb) + (h + 1) * (wb + 3) * A_FA * 1.2 + (h + 1) * wb * A_XOR + 2 * A_FF * h; return k; }
};

// GEHL / O-GEHL (Seznec 2005): tables of signed counters indexed with geometric history lengths, summed.
struct OgehlP : Pred {
    int nt, n, lg, cb, cmax, theta; vector<int> hl; vector<vector<int8_t>> t;
    OgehlP(const Params& p) {
        n = p.geti("n", 64); lg = ilog2(n); cb = p.geti("cb", 4); cmax = (1 << (cb - 1)) - 1; hl = p.getl("hl");
        if (hl.empty()) hl = {0, 3, 7, 14};
        nt = (int)hl.size(); theta = p.geti("theta", nt); t.assign(nt, vector<int8_t>(n, 0));
    }
    inline int idx(int i, const Ctx& c) const { return (fold(c.id, 32, lg) ^ (hl[i] ? fold(c.gh, hl[i], lg) ^ (fold(c.gh, hl[i], lg - 1) << 1) : 0) ^ (i * 5)) & (n - 1); }
    inline int sum(const Ctx& c) const { int s = 0; for (int i = 0; i < nt; i++) s += 2 * t[i][idx(i, c)] + 1; return s; }
    bool predict(const Ctx& c) override { return sum(c) >= 0; }
    void train(const Ctx& c, bool tk) override {
        int s = sum(c);
        if ((s >= 0) != tk || abs(s) <= 2 * theta)
            for (int i = 0; i < nt; i++) { int8_t& v = t[i][idx(i, c)]; if (tk) { if (v < cmax) v++; } else { if (v > -cmax - 1) v--; } }
    }
    Cost cost() override {
        Cost k; int hm = *max_element(hl.begin(), hl.end());
        k.bits = (double)nt * n * cb; k.area = nt * table_area(n, cb) + nt * (cb + 2) * A_FA + 2 * A_FF * hm; for (int h : hl) k.area += A_XOR * h; return k;
    }
};

// Hashed perceptron (GEHL) beside the chip's reference predictors (owner requirement, 26 Nov 2026).  The prediction
// is the sign of a sum of signed weights (2w+1, never zero) read from nt tables, table i indexed by a hash of the id
// and of the hl[i] most recent outcomes.  Options:
//   ub, ug   the bimodal and the gshare counter enter the sum as (2c-3) times ub, ug;  ut: the tournament prediction as +-ut
//   sx       the outcome just before a table's history segment negates its weight (one stored weight serves two patterns)
//   sk       table-specific id hash (two branches that collide in one table do not collide in the others)
//   dyn      adaptive threshold (O-GEHL): a counter of mispredictions against low-margin correct predictions moves theta
//   cb=1     binarised weights: every table votes +-1 and the prediction is the majority (XNOR and popcount with sx)
//   pr       probabilistic update: a weight in the outer half of its range moves further out only on a pseudo-random bit
//            (a narrow weight then behaves like a wider one)
//   pp       path history: one identifier bit of each of the last branches is hashed into the index as well
//   bin      binarised inference: the prediction is the majority of the weights' signs; training is unchanged
//   un       area only: weights stored in unary (thermometer) code, summed by a population count
// The weights train on a misprediction or when |sum| <= theta; the references train by their own rules.
//
// Fault campaign (docs/fault_campaign.md); everything below is off unless named on the configuration line:
//   fi_tgt, fi_rate, fi_seed   Poisson bit flips (rate per resolved branch) into one table: bim cho gsh w0..w3 tw
//   flist=<file>               an explicit flip list, lines "k table entry bit" (table 0..7 as above), applied just
//                              before resolve k (1-based) in every instance
//   tws=<s>,<s>...             leaky-twin scenarios, each with its three copies and all CODE x RESTORE modes counted
//                              in the shadow of the static predictor (the twin never changes training): x = no decay
//                              (flips with table tw go to these), e<tau>p<0|1|m> = exponential lifetime of mean tau
//                              drawn at every write, l<tau>p<0|1|m> = per-cell fixed log-normal lifetime (median tau,
//                              sigma dsig, default 0.5); preferred level 0, 1 or mixed (per cell, drawn once); dseed
//   twuse=1 code= rest=        the driving default prediction takes the twin of scenario 0 in that mode (TWIN_USE)
//   dlog=<file>                the effective decay events of that copy, sequential instance (validation)
//   lt=1                       fault lifetime: the fault-free predictor runs in lockstep; per flip, resolves until the
//                              flipped entry, and until the whole state, equal the fault-free ones again
struct GehlRP : Pred {
    Tourn base; int nt, ent[8], lg[8], hl[8], cb, cmax, W, ub, ug, ut, theta, dyn, tc = 0, tcm, sx, sk, thmin, thmax, ini, pr, pp, un, bin; u32 lfsr = 0xACE1u;
    vector<int8_t> T[8];
    u64 now = 0;                                    // resolves started (the time unit of the campaign)
    vector<Flip> flips; size_t fcur = 0; u64 nflip[TG_N] = {};
    vector<TwinScen> tws; int twuse = 0, tcode = 0, trest = 0; string dlog;
    u64 m4[4] = {}, mab = 0;                        // mispredictions: default, bimodal, gshare, tournament; default with W3 left out
    unique_ptr<GehlRP> gold;
    struct Pend { u64 k; int tab, e; bool erec; };
    vector<Pend> pend; u64 lt_n = 0, lt_erec = 0, lt_esum = 0, lt_grec = 0, lt_gsum = 0;
    static int store(int code, int rest) { return rest == 0 ? 0 : code == 2 ? 2 : 1; }
    int tsize(int t) const { return t == TG_BIM ? base.b.n : t == TG_CHO ? base.cn : t == TG_GSH ? base.g.n : t == TG_TW ? ent[nt - 1] : ent[t - TG_W0]; }
    int tbits(int t) const { return t <= TG_GSH ? 2 : t == TG_TW ? 8 : cb; }
    GehlRP(const Params& p) {
        base.init(p.geti("bn", 64), p.geti("gn", 128), p.geti("gh", 4), p.geti("cn", 64), 0, -1, 0);
        vector<int> te = p.getl("te"), th = p.getl("th");
        nt = (int)th.size();
        cb = p.geti("cb", 4); cmax = (1 << (cb - 1)) - 1; W = p.geti("W", 10);
        ub = p.geti("ub", 0); ug = p.geti("ug", 0); ut = p.geti("ut", 0);
        theta = p.geti("theta", 2 * nt); dyn = p.geti("dyn", 0); tcm = (1 << (p.geti("tcb", 4) - 1)) - 1;
        thmin = p.geti("thmin", 1); thmax = p.geti("thmax", 63); sx = p.geti("sx", 0); sk = p.geti("sk", 0); ini = p.geti("ini", 1); pr = p.geti("pr", 0); pp = p.geti("pp", 0); un = p.geti("un", 0); bin = p.geti("bin", 0);
        for (int i = 0; i < nt; i++) {
            ent[i] = te[i < (int)te.size() ? i : te.size() - 1]; lg[i] = ilog2(ent[i]); hl[i] = th[i];
            T[i].assign(ent[i], (int8_t)((ini && (i & 1) == (nt & 1)) ? 0 : (ini ? -1 : 0)));   // alternate +1 / -1: the sum starts near zero, leaning not-taken
        }
        setup_faults(p);
    }
    void setup_faults(const Params& p) {
        u64 N = (u64)p.geti("_n", 0), ph = hash_str(p.gets("_prog"));
        double rate = p.getd("fi_rate", 0);
        if (rate > 0) {
            string tg = p.gets("fi_tgt"); int t = -1;
            for (int i = 0; i < TG_N; i++) if (tg == TG_NAME[i]) t = i;
            if (t < 0 || (t >= TG_W0 && t < TG_TW && t - TG_W0 >= nt)) { fprintf(stderr, "unknown fi_tgt %s\n", tg.c_str()); exit(2); }
            Rng r(mix(mix((u64)p.geti("fi_seed", 1), ph), hash_str(tg) ^ (u64)llround(rate * 1e12)));
            for (double x = r.expo(1.0 / rate); x < (double)N; x += r.expo(1.0 / rate))
                flips.push_back({(u64)x + 1, t, (int)r.below(tsize(t)), (int)r.below(tbits(t))});
        }
        string fl = p.gets("flist");
        if (!fl.empty()) {
            FILE* f = fopen(fl.c_str(), "r"); if (!f) { fprintf(stderr, "cannot open %s\n", fl.c_str()); exit(1); }
            unsigned long long k; int t, e, b;
            while (fscanf(f, "%llu %d %d %d", &k, &t, &e, &b) == 4) {
                if (t < 0 || t >= TG_N || e < 0 || e >= tsize(t) || b < 0 || b >= tbits(t)) { fprintf(stderr, "bad flip %llu %d %d %d\n", k, t, e, b); exit(2); }
                flips.push_back({(u64)k, t, e, b});
            }
            fclose(f);
            stable_sort(flips.begin(), flips.end(), [](const Flip& x, const Flip& y) { return x.k < y.k; });
        }
        string ts = p.gets("tws"); u64 dseed = (u64)p.geti("dseed", 1);
        for (size_t a = 0; a < ts.size();) {
            size_t z = ts.find(',', a); if (z == string::npos) z = ts.size();
            TwinScen sc;
            if (!sc.parse(ts.substr(a, z - a), p.getd("dsig", 0.5), dseed)) { fprintf(stderr, "bad twin scenario %s\n", ts.substr(a, z - a).c_str()); exit(2); }
            tws.push_back(sc); a = z + 1;
        }
        for (TwinScen& sc : tws)
            for (int si = 0; si < 3; si++) {
                sc.st[si].rng = Rng(mix(mix(dseed, ph), hash_str(sc.spec)));    // the same draws for the three copies while they agree
                for (int e = 0; e < ent[nt - 1]; e++) sc.write(si, e, tw_encode(T[nt - 1][e]), 0);
            }
        for (const Flip& f : flips) if (f.tab == TG_TW && none_of(tws.begin(), tws.end(), [](const TwinScen& s) { return s.kind == 0; })) { fprintf(stderr, "twin flips need a twin scenario x\n"); exit(2); }
        twuse = p.geti("twuse", 0); tcode = p.geti("code", 0); trest = p.geti("rest", 0);
        if (twuse && tws.empty()) { fprintf(stderr, "twuse needs tws\n"); exit(2); }
        dlog = p.gets("dlog");
        if (!dlog.empty() && p.gets("_inst") == "s" && !tws.empty()) tws[0].logst = store(tcode, trest);
        if (p.geti("lt", 0)) {
            Params q = p;
            for (const char* k : {"fi_rate", "flist", "tws", "lt", "twuse", "dlog"}) q.kv.erase(k);
            gold.reset(new GehlRP(q));
        }
    }
    int val(int t, int e) const { return t == TG_BIM ? base.b.t[e] : t == TG_CHO ? base.ch[e] : t == TG_GSH ? base.g.t[e] : T[t - TG_W0][e]; }
    bool same_state(const GehlRP& o) const {
        for (int i = 0; i < nt; i++) if (T[i] != o.T[i]) return false;
        return base.b.t == o.base.b.t && base.g.t == o.base.g.t && base.ch == o.base.ch && theta == o.theta && tc == o.tc && lfsr == o.lfsr;
    }
    void apply(const Flip& f) {
        nflip[f.tab]++;
        if (f.tab == TG_BIM) base.b.t[f.e] ^= (uint8_t)(1 << f.b);
        else if (f.tab == TG_CHO) base.ch[f.e] ^= (uint8_t)(1 << f.b);
        else if (f.tab == TG_GSH) base.g.t[f.e] ^= (uint8_t)(1 << f.b);
        else if (f.tab == TG_TW) { for (TwinScen& sc : tws) if (!sc.kind) for (int si = 0; si < 3; si++) sc.st[si].w[f.e] ^= (uint8_t)(1 << f.b); }
        else { int8_t& v = T[f.tab - TG_W0][f.e]; int u = (v & (int)mask32(cb)) ^ (1 << f.b); v = (int8_t)((u >> (cb - 1)) ? u - (1 << cb) : u); }
        if (gold && f.tab != TG_TW) { lt_n++; pend.push_back({now, f.tab, f.e, false}); }
    }
    void track() {
        if (pend.empty()) return;
        bool all = same_state(*gold);
        for (size_t i = 0; i < pend.size();) {
            Pend& q = pend[i];
            if (!q.erec && val(q.tab, q.e) == gold->val(q.tab, q.e)) { q.erec = true; lt_erec++; lt_esum += now - q.k + 1; }
            if (all) { lt_grec++; lt_gsum += now - q.k + 1; pend.erase(pend.begin() + i); } else i++;
        }
    }
    inline int idx(int i, const Ctx& c) const {
        int l = lg[i]; u32 ih;
        if (sk) { u32 lo = c.id & mask32(l), hi = fold(c.id >> l, 32, l); int r = (i * sk) % l; ih = lo ^ (((hi << r) | (hi >> (l - r))) & mask32(l)); }
        else ih = fold(c.id, 32, l);
        u32 hh = hl[i] == 0 ? 0 : hl[i] <= l ? (u32)(c.gh & mask64(hl[i])) << (l - hl[i]) : fold(c.gh, hl[i], l) ^ (fold(c.gh, hl[i], l - 1) << 1);
        if (pp && hl[i] > 0) { u32 x = fold(c.ph & mask64(min(hl[i], pp)), min(hl[i], pp), l); hh ^= ((x << 2) | (x >> (l - 2))) & mask32(l); }
        return (ih ^ hh) & (ent[i] - 1);
    }
    inline bool neg(int i, const Ctx& c) const { return sx && hl[i] > 0 && ((c.gh >> hl[i]) & 1); }
    // b: binarised inference (bin): only the signs of the weights vote; the weights still train in full
    inline int sum(const Ctx& c, bool b = false) const {
        int s = 0;
        for (int i = 0; i < nt; i++) { int w = T[i][idx(i, c)]; if (b) w = w < 0 ? -1 : 0; int v = 2 * w + 1; s += neg(i, c) ? -v : v; }
        if (ub) s += ub * (2 * (int)base.b.t[base.b.idx(c)] - 3);
        if (ug) s += ug * (2 * (int)base.g.t[base.g.idx(c)] - 3);
        if (ut) s += base.pred(c) ? ut : -ut;
        return s;
    }
    bool predict(const Ctx& c) override { return sum(c, bin) >= 0; }
    // The chip's four predictions from one state (the reference tables are shared, docs/SPEC.md), the default with W3
    // left out, and every twin scenario x CODE x RESTORE as the chip would predict with TWIN_EN = TWIN_USE = 1 (the
    // twin is read at the same index as W3; an abstaining word leaves the term out).  Flags as the chip's FSTAT.
    bool predict_count(const Ctx& c, bool tk) override {
        int s = sum(c, bin); bool pd = s >= 0, drv = pd;
        m4[0] += pd != tk; m4[1] += base.b.pred(c) != tk; m4[2] += base.g.pred(c) != tk; m4[3] += base.pred(c) != tk;
        int i3 = nt - 1, e3 = idx(i3, c), sg = neg(i3, c) ? -1 : 1, w3 = T[i3][e3], t3 = sg * (2 * w3 + 1), cw = tw_encode(w3), p3 = w3 & 15;
        mab += (s - t3 >= 0) != tk;
        for (size_t j = 0; j < tws.size(); j++) {
            TwinScen& sc = tws[j];
            int raw[3] = {sc.read(0, e3, now), sc.read(1, e3, now), sc.read(2, e3, now)};
            for (int code = 0; code < 4; code++)
                for (int rest = 0; rest < 2; rest++) {
                    int rw = raw[store(code, rest)], d; bool ab, cor;
                    tw_decode(rw, code, d, ab, cor);
                    int wd = d >= 8 ? d - 16 : d;
                    bool p = s - t3 + (ab ? 0 : sg * (2 * wd + 1)) >= 0;
                    sc.mis[code][rest] += p != tk;
                    u64* ev = sc.ev[code][rest];
                    ev[0] += rw != cw; ev[1] += cor; ev[2] += ab; ev[3] += !ab && d != p3;
                    if (j == 0 && twuse && code == tcode && rest == trest) drv = p;
                }
        }
        return drv;
    }
    void train(const Ctx& c, bool tk) override {
        now++;
        while (fcur < flips.size() && flips[fcur].k <= now) apply(flips[fcur++]);
        int s = sum(c); bool pd = (bin ? sum(c, true) : s) >= 0, low = abs(s) <= theta, w3w = false;
        int e3 = idx(nt - 1, c);
        if (pd != tk || low)
        {
            if (this->pr) lfsr = (lfsr >> 1) ^ ((lfsr & 1) ? 0xB400u : 0);
            int half = (cmax + 1) / 2;
            for (int i = 0; i < nt; i++) {
                int8_t& v = T[i][idx(i, c)]; bool up = tk != neg(i, c);
                if (this->pr && ((up && v >= half) || (!up && v < -half)) && !((lfsr >> i) & 1)) continue;
                if (up) { if (v < cmax) v++; } else { if (v > -cmax - 1) v--; }
            }
            w3w = true;
        }
        if (dyn) {
            if (pd != tk) { if (++tc > tcm) { tc = 0; if (theta < thmax) theta++; } }
            else if (low) { if (--tc < -tcm - 1) { tc = 0; if (theta > thmin) theta--; } }
        }
        base.upd(c, tk);
        // the twin: written with W3 whenever W3 trains; otherwise restore-on-use rewrites the word read now (copy 1
        // as read, copy 2 corrected if SEC-DED corrected it)
        for (TwinScen& sc : tws) {
            if (w3w) { int w = tw_encode(T[nt - 1][e3]); for (int si = 0; si < 3; si++) sc.write(si, e3, w, now); }
            else {
                sc.write(1, e3, sc.read(1, e3, now), now);
                int r2 = sc.read(2, e3, now), d; bool ab, cor;
                tw_decode(r2, 2, d, ab, cor);
                sc.write(2, e3, cor ? tw_encode(d) : r2, now);
            }
        }
        if (gold) { gold->train(c, tk); track(); }
    }
    string report() override {
        char b[256]; string o;
        u64 nf = 0; for (int t = 0; t < TG_TW; t++) nf += nflip[t];
        snprintf(b, sizeof b, "m4=%llu,%llu,%llu,%llu;ab=%llu;nf=%llu;nftw=%llu;now=%llu", (unsigned long long)m4[0], (unsigned long long)m4[1],
                 (unsigned long long)m4[2], (unsigned long long)m4[3], (unsigned long long)mab, (unsigned long long)nf, (unsigned long long)nflip[TG_TW], (unsigned long long)now);
        o += b;
        for (size_t j = 0; j < tws.size(); j++)
            for (int code = 0; code < 4; code++)
                for (int rest = 0; rest < 2; rest++) {
                    const u64* ev = tws[j].ev[code][rest];
                    snprintf(b, sizeof b, ";s%zuc%dr%d=%llu,%llu,%llu,%llu,%llu", j, code, rest, (unsigned long long)tws[j].mis[code][rest],
                             (unsigned long long)ev[0], (unsigned long long)ev[1], (unsigned long long)ev[2], (unsigned long long)ev[3]);
                    o += b;
                }
        if (gold) {
            u64 cs = 0; for (const Pend& q : pend) cs += now - q.k + 1;
            snprintf(b, sizeof b, ";lt=%llu,%llu,%llu,%llu,%llu,%zu,%llu", (unsigned long long)lt_n, (unsigned long long)lt_erec, (unsigned long long)lt_esum,
                     (unsigned long long)lt_grec, (unsigned long long)lt_gsum, pend.size(), (unsigned long long)cs);
            o += b;
        }
        if (!tws.empty() && tws[0].logst >= 0) {
            TwinScen& sc = tws[0];
            for (int e = 0; e < ent[nt - 1]; e++) sc.logdecays(sc.logst, e, now);
            sort(sc.log.begin(), sc.log.end());
            FILE* f = fopen(dlog.c_str(), "w");
            if (f) { for (const auto& x : sc.log) fprintf(f, "%llu %llu %llu %llu\n", (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[3]); fclose(f); }
        }
        return o;
    }
    Cost cost() override {
        Cost k; int hm = 0;
        for (int i = 0; i < nt; i++) { int sb = un ? (1 << cb) - 1 : cb; k.bits += (double)ent[i] * sb; k.area += table_area(ent[i], sb) + A_XOR * hl[i] + (cb + 3) * A_FA + cb * A_FA; hm = max(hm, hl[i] + sx); }
        k.area += 2 * A_FF * max(0, hm - base.g.h) + (dyn ? 12 * A_FF : 0) + (pp ? 2 * A_FF * pp : 0) + (pr ? 16 * A_FF : 0);
        k.sbits = base.bits(); k.sarea = base.area() + 2 * A_FF * base.g.h;
        k.bits += k.sbits; k.area += k.sarea;
        return k;
    }
};

// ---------------------------------------------------------------------------------------------- TAGE family
// Tagged geometric-history tables over a base predictor (Seznec, Michaud 2006), reduced to what a tiny budget
// affords.  The base is a bimodal table, or a bimodal + gshare + chooser tournament.  With basemode=1 the base
// tables train exactly as stand-alone classic predictors (every branch, classic rules), which is what lets the chip
// share them with its reference predictors (share=1 marks their cost as shared).
struct TageP : Pred {
    Tourn base; bool hasG; int basemode, share;
    int nt; int ent[8], lg[8], tb[8], hl[8];
    int cb, ub, cmax, umax, half, ua_bits, ua, ua_mid, ua_max, alloc, W, altw, lw; u32 lfsr = 0xACE1u;
    struct E { uint16_t tag; uint8_t ctr, u; };
    vector<E> T[8];
    struct L { int idx[8]; u32 tag[8]; bool hit[8]; int prov, alt; bool bpred, ppred, apred, pred, weak; };

    TageP(const Params& p) {
        int bn = p.geti("bn", 128), gn = p.geti("gn", 0);
        hasG = gn > 0; basemode = p.geti("basemode", 1); share = p.geti("share", 1);
        base.init(bn, hasG ? gn : 2, p.geti("gh", 8), hasG ? p.geti("cn", 64) : 2, p.geti("bagree", 0), p.geti("init", -1), p.geti("chist", 0));
        vector<int> te = p.getl("te"), tt = p.getl("tt"), th = p.getl("th");
        nt = (int)te.size();
        for (int i = 0; i < nt; i++) { ent[i] = te[i]; lg[i] = ilog2(te[i]); tb[i] = tt[i < (int)tt.size() ? i : tt.size() - 1]; hl[i] = th[i]; }
        cb = p.geti("cb", 3); ub = p.geti("ub", 1); cmax = (1 << cb) - 1; umax = (1 << ub) - 1; half = 1 << (cb - 1);
        ua_bits = p.geti("ua", 4); ua_max = (1 << max(ua_bits, 1)) - 1; ua_mid = (ua_max + 1) / 2; ua = ua_mid;
        alloc = p.geti("alloc", 0); W = p.geti("W", 10); altw = p.geti("altw", 1);
        lw = p.geti("lw", 16); if (lw == 8) lfsr = 0xE1u;   // allocation LFSR: 16 bits (taps 0xB400) or 8 bits (taps 0xB8)
        for (int i = 0; i < nt; i++) T[i].assign(ent[i], {0, (uint8_t)(half - 1), 0});
    }
    inline bool basepred(const Ctx& c) const { return hasG ? base.pred(c) : base.b.pred(c); }
    void lookup(const Ctx& c, L& l) const {
        l.prov = l.alt = -1;
        for (int i = 0; i < nt; i++) {
            l.idx[i] = (fold(c.id, W, lg[i]) ^ fold(c.gh, hl[i], lg[i])) & (ent[i] - 1);
            u32 tagpc = fold(c.id >> lg[i], max(W - lg[i], 1), tb[i]);
            l.tag[i] = (tagpc ^ fold(c.gh, hl[i], tb[i]) ^ (fold(c.gh, hl[i], tb[i] - 1) << 1)) & mask32(tb[i]);
            l.hit[i] = T[i][l.idx[i]].tag == l.tag[i];
        }
        for (int i = nt - 1; i >= 0; i--) if (l.hit[i]) { if (l.prov < 0) l.prov = i; else { l.alt = i; break; } }
        l.bpred = basepred(c);
        if (l.prov < 0) { l.pred = l.ppred = l.apred = l.bpred; l.weak = false; return; }
        const E& e = T[l.prov][l.idx[l.prov]];
        l.ppred = e.ctr >= half;
        l.apred = l.alt >= 0 ? (T[l.alt][l.idx[l.alt]].ctr >= half) : l.bpred;
        l.weak = (e.ctr == half || e.ctr == half - 1) && e.u == 0;
        l.pred = (l.weak && ua_bits && ua >= ua_mid) ? l.apred : l.ppred;
    }
    bool predict(const Ctx& c) override { L l; lookup(c, l); return l.pred; }
    void train(const Ctx& c, bool t) override {
        L l; lookup(c, l);
        if (l.prov >= 0) {
            E& e = T[l.prov][l.idx[l.prov]];
            if (l.ppred != l.apred) {
                if (l.weak && ua_bits) { if (l.apred == t) { if (ua < ua_max) ua++; } else { if (ua > 0) ua--; } }
                sat(e.u, l.ppred == t, umax);
            }
            sat(e.ctr, t, cmax);
            if (altw && l.weak && l.alt >= 0) sat(T[l.alt][l.idx[l.alt]].ctr, t, cmax);
        }
        if (l.pred != t && l.prov < nt - 1) {
            int cand[8], nc = 0;
            for (int j = l.prov + 1; j < nt; j++) if (T[j][l.idx[j]].u == 0) cand[nc++] = j;
            if (nc) {
                int pick = 0;
                if (alloc == 1) {  // geometric preference for the shortest history, from an LFSR stepped at resolve
                    lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & (lw == 8 ? 0xB8u : 0xB400u));
                    while (pick < nc - 1 && ((lfsr >> pick) & 1)) pick++;
                }
                E& n = T[cand[pick]][l.idx[cand[pick]]];
                n.tag = (uint16_t)l.tag[cand[pick]]; n.ctr = (uint8_t)(t ? half : half - 1); n.u = 0;
            } else {
                for (int j = l.prov + 1; j < nt; j++) { E& n = T[j][l.idx[j]]; if (n.u > 0) n.u--; }
            }
        }
        if (basemode == 1) { if (hasG) base.upd(c, t); else base.b.upd(c, t); }
        else if (l.prov < 0 || (l.weak && l.alt < 0)) { if (hasG) base.upd(c, t); else base.b.upd(c, t); }
    }
    Cost cost() override {
        Cost k; double bb = hasG ? base.bits() : base.b.bits(), ba = hasG ? base.area() : base.b.area();
        int hm = hasG ? base.g.h : 0;
        k.bits = bb; k.area = ba;
        if (share) { k.sbits = bb; k.sarea = ba; }
        for (int i = 0; i < nt; i++) {
            int eb = tb[i] + cb + ub; k.bits += (double)ent[i] * eb;
            k.area += table_area(ent[i], eb, true) + tb[i] * 10.3 + 150 + (cb + ub + 1) * A_FF + A_XOR * (hl[i] + hl[i] / 2 + lg[i] + tb[i]);
            hm = max(hm, hl[i]);
        }
        k.area += 2 * A_FF * hm + A_MUX * hm + (ua_bits ? ua_bits * 40 : 0) + nt * 3 * A_MUX;
        return k;
    }
};

// ---------------------------------------------------------------------------------------------- loop predictor
// Counts iterations of regular loops and predicts the exit (after Sherwood, Calder 2000; the L of L-TAGE).  State is
// trained at resolve only; a prediction adds the unresolved instances of the same branch (c.same) to the count.
struct LoopWrap : Pred {
    unique_ptr<Pred> m; int n, tagb, cntb, cmax, confmax;
    struct E { uint16_t tag; uint16_t trip, iter; uint8_t conf, age; bool dir, valid; };
    vector<E> e;
    LoopWrap(Pred* main, const Params& p) : m(main) {
        n = p.geti("ln", 4); tagb = p.geti("ltag", 8); cntb = p.geti("lcnt", 7); cmax = (1 << cntb) - 1; confmax = p.geti("lconf", 2);
        e.assign(n, {0, 0, 0, 0, 0, false, false});
    }
    inline int find(const Ctx& c) const { u32 tg = fold(c.id, 32, tagb); for (int i = 0; i < n; i++) if (e[i].valid && e[i].tag == tg) return i; return -1; }
    bool predict(const Ctx& c) override {
        int i = find(c);
        if (i >= 0 && e[i].conf >= confmax) return ((int)e[i].iter + c.same == (int)e[i].trip) ? !e[i].dir : e[i].dir;
        return m->predict(c);
    }
    void train(const Ctx& c, bool t) override {
        Ctx c0 = c; c0.same = 0;
        bool mp = m->predict(c0);
        int i = find(c);
        if (i >= 0) {
            E& x = e[i];
            if (t == x.dir) {
                if (x.iter < cmax) x.iter++; else { x.valid = false; }                 // too long to count
                if (x.valid && x.conf >= confmax && x.iter > x.trip) { x.conf = 0; }  // ran past the learned trip
            } else {
                if (x.iter == x.trip) { if (x.conf < 3) x.conf++; if (x.age < 3) x.age++; }
                else { x.trip = x.iter; x.conf = 0; }
                x.iter = 0;
            }
        } else if (mp != t) {  // allocate on a misprediction of the main predictor: assume this was a loop exit
            int v = -1;
            for (int j = 0; j < n; j++) if (!e[j].valid) { v = j; break; }
            if (v < 0) { for (int j = 0; j < n; j++) if (e[j].age == 0) { v = j; break; } }
            if (v < 0) { for (int j = 0; j < n; j++) e[j].age--; }
            else e[v] = {(uint16_t)fold(c.id, 32, tagb), (uint16_t)cmax, 0, 0, 1, !t, true};
        }
        m->train(c0, t);
    }
    Cost cost() override {
        Cost k = m->cost(); int eb = tagb + 2 * cntb + 2 + 2 + 2;
        k.bits += (double)n * eb; k.area += n * eb * (A_BIT + 2) + n * tagb * 10.3 + cntb * 3 * A_FA + 400;
        return k;
    }
};

// ---------------------------------------------------------------------------------------------- factory
static Pred* make(const string& kind, const Params& p) {
    Pred* m = nullptr;
    if (kind == "snt") m = new StaticPred(0);
    else if (kind == "st") m = new StaticPred(1);
    else if (kind == "btfn") m = new StaticPred(2);
    else if (kind == "bimodal") m = new BimodalP(p);
    else if (kind == "gshare") m = new GShareP(p);
    else if (kind == "gselect") m = new GSelectP(p);
    else if (kind == "alloy") m = new AlloyP(p);
    else if (kind == "bimhyst") m = new BimHystP(p);
    else if (kind == "tourn") m = new TournP(p);
    else if (kind == "bimode") m = new BimodeP(p);
    else if (kind == "yags") m = new YagsP(p);
    else if (kind == "gskew") m = new GskewP(p);
    else if (kind == "local") m = new LocalP(p);
    else if (kind == "alpha") m = new AlphaP(p);
    else if (kind == "perceptron") m = new PerceptronP(p);
    else if (kind == "ogehl") m = new OgehlP(p);
    else if (kind == "tage") m = new TageP(p);
    else if (kind == "gehlr") m = new GehlRP(p);
    else { fprintf(stderr, "unknown kind %s\n", kind.c_str()); exit(2); }
    if (p.geti("ln", 0) > 0) m = new LoopWrap(m, p);
    return m;
}

// ---------------------------------------------------------------------------------------------- CPU timing model
// U1 CPU contract (docs/SPEC.md section 9): in-order RV32IM, stages IF ID EX MEM WB, with a direct-mapped
// branch-target buffer of `btb` entries read in IF.  A conditional branch that hits the BTB asks BPX-1 in IF and
// follows the answer with no bubble; one that misses is found in ID, asks there, and a taken prediction costs one
// bubble.  Branches and JALR resolve R stages after ID (R=1: in EX); a misprediction costs R+1 cycles.  JAL costs
// nothing on a BTB hit and one bubble on a miss.  btb=0 removes the BTB (every branch asks in ID).
// Static not-taken (the same CPU without BPX-1): a taken branch costs R+1 cycles.
// The outcome is reported to the chip in the cycle after it resolves; a predict of that same cycle is served before
// the resolve trains (rf=0) or after it (rf=1).  `cyc` is the cycle in which the current instruction is in ID.
struct CpuCfg { int R = 1, lu = 1, mul = 0, div = 32, rf = 0, W = 10, hint = 1, btb = 256, seq = 1; };
struct Res { u64 br = 0, mis_seq = 0, mis_cpu = 0, cyc = 0, snt = 0, ins = 0, btbmiss = 0; string xs, xc; };

static inline u32 fold_id(u32 pc, int W) { return fold((u64)(pc >> 2), 30, W); }

static Res run_trace(const Trace& tr, const string& kind, const Params& p, const CpuCfg& cc) {
    Res res;
    u64 nbr = 0;                         // conditional branches: the length of the fault process
    for (const Rec& r : tr.r) { u32 k = r.info & 3; if (k == 3) break; if (k == 0) nbr++; }
    Params pps = p, ppc = p;
    pps.kv["_n"] = ppc.kv["_n"] = to_string(nbr); pps.kv["_prog"] = ppc.kv["_prog"] = tr.name; pps.kv["_inst"] = "s"; ppc.kv["_inst"] = "c";
    unique_ptr<Pred> ps(make(kind, pps)), pc(make(kind, ppc));
    struct Pend { u64 when; Ctx c; bool taken; };
    deque<Pend> q;
    vector<u32> btb(cc.btb ? cc.btb : 1, 0xffffffffu);
    auto btb_hit = [&](u32 a) -> bool {
        if (!cc.btb) return false;
        u32 i = (a >> 2) & (u32)(cc.btb - 1);
        bool h = btb[i] == a;
        btb[i] = a;                      // written when the instruction is decoded
        return h;
    };
    u64 gh = 0, ph = 0, cyc = 1, snt = 1, last_id_req = ~0ull;
    const int pm = cc.R + 1;
    for (const Rec& r : tr.r) {
        u32 kindr = r.info & 3, nb = r.info >> 8;
        u64 base = (u64)nb + (u64)(r.extra & 0xffff) * cc.lu + (u64)((r.extra >> 16) & 0xff) * cc.div + (u64)(r.extra >> 24) * cc.mul;
        cyc += base; snt += base; res.ins += nb;
        if (kindr == 3) break;
        res.ins++;
        if (kindr == 0) {
            bool taken = (r.info >> 2) & 1;
            Ctx c; c.id = fold_id(r.pc, cc.W); c.bias = cc.hint ? (r.target < r.pc) : false; c.gh = gh; c.same = 0; c.ph = ph;
            if (cc.seq) {                // strictly sequential reference: one branch in flight
                if (ps->predict_count(c, taken) != taken) res.mis_seq++;
                ps->train(c, taken);
            }
            bool hit = btb_hit(r.pc);
            if (!hit) res.btbmiss++;
            // one request per cycle: if the previous instruction asked from ID in the cycle this one is in IF,
            // this one waits a cycle (one bubble)
            if (hit && cyc - 1 == last_id_req) cyc += 1;
            if (!hit) last_id_req = cyc;
            u64 tp = hit ? cyc - 1 : cyc;                       // cycle of the predict request
            while (!q.empty() && (cc.rf ? q.front().when <= tp : q.front().when < tp)) { pc->train(q.front().c, q.front().taken); q.pop_front(); }
            for (const Pend& x : q) if (x.c.id == c.id) c.same++;
            bool pr = pc->predict_count(c, taken);
            q.push_back({cyc + cc.R + 1, c, taken});            // cycle of the resolve request
            if (pr != taken) { res.mis_cpu++; cyc += 1 + pm; }
            else cyc += 1 + ((taken && !hit) ? 1 : 0);
            snt += 1 + (taken ? pm : 0);
            gh = (gh << 1) | (taken ? 1 : 0);
            ph = (ph << 1) | ((c.id ^ (c.id >> 3)) & 1);
            res.br++;
        } else if (kindr == 1) { int b = btb_hit(r.pc) ? 0 : 1; cyc += 1 + b; snt += 1 + b; }
        else { cyc += 1 + pm; snt += 1 + pm; }
    }
    res.cyc = cyc; res.snt = snt;
    if (cc.seq) res.xs = ps->report();
    res.xc = pc->report();
    return res;
}

int main(int argc, char** argv) {
    CpuCfg cc; int threads = 8; int ai = 1;
    for (; ai < argc && strchr(argv[ai], '=') && !strchr(argv[ai], '/'); ai++) {
        string a = argv[ai]; string k = a.substr(0, a.find('=')); int v = atoi(a.substr(a.find('=') + 1).c_str());
        if (k == "W") cc.W = v; else if (k == "R") cc.R = v; else if (k == "rf") cc.rf = v; else if (k == "lu") cc.lu = v;
        else if (k == "mul") cc.mul = v; else if (k == "div") cc.div = v; else if (k == "hint") cc.hint = v; else if (k == "threads") threads = v;
        else if (k == "btb") cc.btb = v; else if (k == "seq") cc.seq = v;
        else { fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
    }
    if (argc - ai < 3) { fprintf(stderr, "usage: bpsim [W= R= btb= rf= hint= seq= threads=] configs.txt out.tsv traces...\n"); return 2; }
    string cfgpath = argv[ai++], outpath = argv[ai++];
    vector<Trace> traces;
    for (; ai < argc; ai++) {
        Trace t; string path = argv[ai];
        bool ok = path.size() > 4 && path.substr(path.size() - 4) == ".evt" ? load_evt(path, t) : load_txt(path, t);
        if (!ok) { fprintf(stderr, "cannot load %s\n", path.c_str()); return 1; }
        // name: parent directory for .../<name>/prog.evt, else file stem
        size_t s = path.find_last_of('/'); string fn = s == string::npos ? path : path.substr(s + 1);
        if (fn == "prog.evt" || fn == "trace_full.txt" || fn == "trace.txt") { string d = path.substr(0, s); size_t s2 = d.find_last_of('/'); t.name = s2 == string::npos ? d : d.substr(s2 + 1); }
        else t.name = fn.substr(0, fn.find_last_of('.'));
        traces.push_back(move(t));
    }
    struct Cfg { string name, kind; Params p; };
    vector<Cfg> cfgs;
    {
        FILE* f = fopen(cfgpath.c_str(), "r"); if (!f) { fprintf(stderr, "cannot open %s\n", cfgpath.c_str()); return 1; }
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#' || line[0] == '\n') continue;
            stringstream ss(line); Cfg c; ss >> c.name >> c.kind; string tok;
            while (ss >> tok) { size_t e = tok.find('='); if (e != string::npos) c.p.kv[tok.substr(0, e)] = tok.substr(e + 1); }
            if (!c.p.kv.count("W")) c.p.kv["W"] = to_string(cc.W);
            if (!c.kind.empty()) cfgs.push_back(c);
        }
        fclose(f);
    }
    vector<vector<Res>> out(cfgs.size(), vector<Res>(traces.size()));
    vector<Cost> costs(cfgs.size());
    atomic<size_t> next(0);
    auto work = [&]() {
        for (;;) {
            size_t j = next++; if (j >= cfgs.size() * traces.size()) break;
            size_t ci = j / traces.size(), ti = j % traces.size();
            out[ci][ti] = run_trace(traces[ti], cfgs[ci].kind, cfgs[ci].p, cc);
        }
    };
    vector<thread> th; for (int i = 0; i < threads; i++) th.emplace_back(work);
    for (auto& t : th) t.join();
    for (size_t i = 0; i < cfgs.size(); i++) { unique_ptr<Pred> p(make(cfgs[i].kind, cfgs[i].p)); costs[i] = p->cost(); }
    FILE* fo = fopen(outpath.c_str(), "w");
    fprintf(fo, "config\tkind\tbits\tsbits\tarea\tsarea\tprogram\tbranches\tmis_seq\tmis_cpu\tcycles\tsnt_cycles\tinstr\tx_seq\tx_cpu\n");
    for (size_t i = 0; i < cfgs.size(); i++)
        for (size_t t = 0; t < traces.size(); t++) {
            const Res& r = out[i][t];
            fprintf(fo, "%s\t%s\t%.0f\t%.0f\t%.0f\t%.0f\t%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%s\t%s\n", cfgs[i].name.c_str(), cfgs[i].kind.c_str(),
                    costs[i].bits, costs[i].sbits, costs[i].area, costs[i].sarea, traces[t].name.c_str(), (unsigned long long)r.br,
                    (unsigned long long)r.mis_seq, (unsigned long long)r.mis_cpu, (unsigned long long)r.cyc, (unsigned long long)r.snt, (unsigned long long)r.ins,
                    r.xs.empty() ? "-" : r.xs.c_str(), r.xc.empty() ? "-" : r.xc.c_str());
        }
    fclose(fo);
    fprintf(stderr, "%zu configs x %zu traces done\n", cfgs.size(), traces.size());
    return 0;
}
