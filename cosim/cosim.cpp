// cosim.cpp - BPX-1 system co-simulation (spec VR-06): the U1 CPU contract (docs/SPEC.md section 9) replays one
// benchmark program against the RTL (Verilated bpx_chip) through the real pins.
//
//   cosim <prog.evt> <prog.text> <drive 0..4> [K=8] [oplog.bin]
//
// The CPU model is cycle-accurate for control flow: BTB at fetch, predict in IF (BTB hit) or ID (miss), resolve in
// EX, outcome reported the cycle after, wrong-path predicts until the misprediction resolves, FLUSH after JALR.
// One request per CPU cycle; a CPU cycle is K chip clocks.  Output: one line of key=value results; optionally a
// binary log of every request and reply (4 bytes each: op, id low, id high, pred | conf<<1) for model/replay.py.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include "Vbpx_chip.h"
#include "verilated.h"

typedef uint32_t u32;
typedef uint64_t u64;
struct Rec { u32 pc, target, info, extra; };

static Vbpx_chip* top;
static u64 chip_cycles = 0;
static int req = 0, uio = 0x10;

// ------------------------------------------------------------------ background control port (concurrent with requests)
// A queue of pin steps for CSn (0x10), SCK (0x20) and MOSI (0x40), advanced by every clock, so that control-port
// transactions overlap requests.  Each SCK phase lasts PH clocks (the minimum of the specification).  Every word
// leaves two marks with the request counters: for a read word at the SCK fall that takes the word and at its first
// rise; for a written word at its last rise and one phase later.
struct BgStep { unsigned char pins, clocks, flags; };          // flags: 1 sample MISO, 2 end of word, 4 mark
struct BgMark { u64 acked, started; };
static const int PH = 4;
static std::vector<BgStep> bgq; static size_t bgp = 0; static int bgl = 0;
static std::vector<BgMark> bg_marks; static std::vector<u32> bg_words; static u32 bg_w = 0;
static u64 n_started = 0, n_acked = 0;
static bool bg_busy() { return bgp < bgq.size() || bgl > 0; }
static void bg_tick() {
    while (bgl == 0) {
        if (bgp >= bgq.size()) return;
        BgStep& s = bgq[bgp++];
        if (s.flags & 1) bg_w = (bg_w << 1) | ((top->uo_out >> 7) & 1);
        if (s.flags & 2) { bg_words.push_back(bg_w & 0xffff); bg_w = 0; }
        if (s.flags & 4) bg_marks.push_back({n_acked, n_started});
        uio = (uio & ~0x70) | s.pins; top->uio_in = uio | (req << 7);
        bgl = s.clocks;
    }
    bgl--;
}
static void bg_add(int pins, int clocks, int flags = 0) { bgq.push_back({(unsigned char)pins, (unsigned char)clocks, (unsigned char)flags}); }
static void bg_xfer(int addr, bool wr, const u32* v, int n) {
    bg_add(0, 4);
    u32 cmd = (wr ? 0x80 : 0) | addr;
    for (int i = 7; i >= 0; i--) { int m = ((cmd >> i) & 1) << 6; bg_add(m, PH); bg_add(m | 0x20, PH); }
    for (int k = 0; k < n; k++) {
        for (int i = 15; i >= 0; i--) {
            int m = wr ? ((v[k] >> i) & 1) << 6 : 0;
            bg_add(m, PH, (!wr && i == 15) ? 4 : 0);
            bg_add(m | 0x20, PH, (wr ? 0 : 1) | ((wr ? i == 0 : i == 15) ? 4 : 0));
        }
        bg_add(0, 0, 2 | (wr ? 4 : 0));
    }
    bg_add(0, 4); bg_add(0x10, 6);
}
// DBG one clock after the ACK of an accepted predict (fault events that happen in the clock after it: the canary;
// docs/mosaic.md 7.5): checked after the first clock that follows the acknowledge, whatever step comes next
static int post_mask = 0, post_val = 0; static unsigned long long post_line = 0, post_bad = 0;
static void tick() {
    bg_tick(); top->clk = 1; top->eval(); top->clk = 0; top->eval(); chip_cycles++;
    if (post_mask) {
        int g = ((top->uo_out >> 5) & 1) | (((top->uo_out >> 6) & 1) << 1);
        if ((g & post_mask) != (post_val & post_mask)) {
            if (!post_bad) fprintf(stderr, "line %llu: DBG one clock after ACK %d, model %d (mask %d)\n", post_line, g, post_val, post_mask);
            post_bad++;
        }
        post_mask = 0;
    }
}

static u32 fold(u64 v, int inb, int outb) { v &= (inb >= 64 ? ~0ull : ((1ull << inb) - 1)); u32 r = 0; while (v) { r ^= (u32)(v & ((1u << outb) - 1)); v >>= outb; } return r; }
static u32 fold_id(u32 pc) { return fold((u64)(pc >> 2), 30, 8); }

// ------------------------------------------------------------------ pins
static int K = 8;
static FILE* oplog = nullptr;
static u64 n_req = 0, lat_sum = 0, lat_max = 0, late = 0;
static u64 lat_hist[16];
static void drive(int ui) { top->ui_in = ui; top->uio_in = uio | (req << 7); }
// one request in CPU cycle t (a CPU cycle is K chip clocks); returns PRED.  A second request in the same CPU
// cycle (BPX_DUAL) starts one clock after the first was acknowledged.
static u64 last_t = ~0ull, pairs = 0;
static int used = 0, used_max = 0;
static int request(int op, int id, u64 t) {
    if (t != last_t) { if (last_t != ~0ull) for (; used < K; used++) tick(); used = 0; last_t = t; }
    else { tick(); used++; pairs++; }
    uio = (uio & 0x70) | ((op & 3) << 2);
    int ack0 = top->uo_out & 1;
    req ^= 1;
    drive(id & 0xff);
    int n = 0;
    while ((top->uo_out & 1) == ack0) { tick(); if (++n > 200) { fprintf(stderr, "no acknowledge\n"); exit(3); } }
    int pred = (top->uo_out >> 1) & 1, conf = (top->uo_out >> 2) & 1;
    n_req++; lat_sum += n; if ((u64)n > lat_max) lat_max = n; lat_hist[n < 15 ? n : 15]++;
    used += n; if (used > used_max) used_max = used;
    if (used > K) late++;
    if (oplog) { unsigned char b[4] = {(unsigned char)op, (unsigned char)(id & 0xff), (unsigned char)(id >> 8), (unsigned char)(pred | (conf << 1))}; fwrite(b, 1, 4, oplog); }
    return pred;
}
static u32 spi_bits(u32 v, int nb) {
    u32 got = 0;
    for (int i = nb - 1; i >= 0; i--) {
        uio = (uio & ~0x40) | (((v >> i) & 1) << 6); drive(top->ui_in); for (int k = 0; k < 5; k++) tick();
        uio |= 0x20; drive(top->ui_in); got = (got << 1) | ((top->uo_out >> 7) & 1); for (int k = 0; k < 5; k++) tick();
        uio &= ~0x20; drive(top->ui_in);
    }
    return got;
}
static void spi(int addr, bool wr, u32* words, int n) {
    uio &= ~0x10; drive(top->ui_in); for (int k = 0; k < 4; k++) tick();
    spi_bits((wr ? 0x80 : 0) | addr, 8);
    for (int i = 0; i < n; i++) { if (wr) spi_bits(words[i], 16); else words[i] = spi_bits(0, 16); }
    for (int k = 0; k < 4; k++) tick();
    uio |= 0x10; drive(top->ui_in); for (int k = 0; k < 4; k++) tick();
}
static u64 stat[5];
static void snapshot_clear() {            // exact totals with 16-bit counters: snapshot-and-clear, accumulate
    u32 c = 0x0010; spi(0x02, true, &c, 1);
    for (int k = 0; k < 8; k++) tick();
    u32 w[10]; spi(0x05, false, w, 10);
    for (int k = 0; k < 5; k++) stat[k] += w[2 * k] | ((u64)w[2 * k + 1] << 16);
}

// ------------------------------------------------------------------ program
static std::vector<Rec> recs;
static u32 text_base = 0; static std::vector<u32> text;
static int decode(u32 pc, u32& tgt) {      // 0 other, 1 conditional branch, 2 JAL, 3 JALR
    u32 i = (pc - text_base) >> 2;
    if (pc < text_base || i >= text.size()) return 0;
    u32 w = text[i], opc = w & 0x7f;
    if (opc == 0x63) { int32_t imm = ((int32_t)(w & 0x80000000) >> 19) | ((w & 0x80) << 4) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e); tgt = pc + imm; return 1; }
    if (opc == 0x6f) { int32_t imm = ((int32_t)(w & 0x80000000) >> 11) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe); tgt = pc + imm; return 2; }
    if (opc == 0x67) return 3;
    return 0;
}

// ------------------------------------------------------------------ random operations (VR-01, VR-02)
static void wait_ready() {
    for (int k = 0; k < 16; k++) tick();
    for (int k = 0; k < 400 && !((top->uo_out >> 4) & 1); k++) tick();
    for (int k = 0; k < 4; k++) tick();
}
// plays the steps written by model/randgen.py; one latency byte per request goes to latpath
// Background jobs (lines "X ..." of the steps file) run while the requests go on; what they read is written to
// the log with the window of requests it may reflect, and cosim/conc_check.py compares it with the model.
// "G addr value" reads one register (FCTL 0x11, FSTAT 0x12) in the foreground and compares it.
static char bg_kind = 0; static int bg_a = 0, bg_b = 0, bg_c = 0;
static u64 bg_jobs = 0, bg_dropped = 0;
static void bg_idle() { int n = 0; while (bg_busy()) { tick(); if (++n > 100000) { fprintf(stderr, "control port stuck\n"); exit(3); } } }
static u64 lo2(u64 a) { return a > 2 ? a - 2 : 0; }
static void bg_flush(FILE* cl) {
    if (!bg_kind) return;
    std::vector<BgMark>& m = bg_marks; std::vector<u32>& w = bg_words;
    if (cl) {
        if (bg_kind == 'S' || bg_kind == 'C') {
            fprintf(cl, "%c %llu %llu %llu %llu", bg_kind, (unsigned long long)lo2(m[0].acked), (unsigned long long)m[3].started,
                    (unsigned long long)lo2(m[2].acked), (unsigned long long)m[5].started);
            for (int k = 1; k <= 12; k++) fprintf(cl, " %u", w[k]);
            fprintf(cl, "\n");
        } else if (bg_kind == 'D') {
            fprintf(cl, "D %d %d %d", bg_a, bg_b, bg_c);
            for (int k = 1; k <= bg_c; k++) fprintf(cl, " %llu %llu %u", (unsigned long long)lo2(m[2 * (k - 1)].acked), (unsigned long long)m[2 * k + 1].started, w[k]);
            fprintf(cl, "\n");
        } else if (bg_kind == 'I') {
            fprintf(cl, "I %llu %llu %u %u\n", (unsigned long long)lo2(m[0].acked), (unsigned long long)m[3].started, w[0], w[1]);
        } else fprintf(cl, "W\n");
    }
    bg_kind = 0; bg_marks.clear(); bg_words.clear(); bgq.clear(); bgp = 0; bg_jobs++;
}
static int rand_play(const char* path, const char* latpath, const char* clpath) {
    FILE* f = fopen(path, "r"); FILE* lf = latpath ? fopen(latpath, "wb") : nullptr; FILE* cl = clpath ? fopen(clpath, "w") : nullptr;
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    char c; u64 line = 0, nr = 0, bad = 0, sbad = 0, dbad = 0, nstat = 0, ndump = 0, nw = 0;
    while (fscanf(f, " %c", &c) == 1) {
        line++;
        if (c == 'X') {
            char k; int a = 0, b = 0, n = 0;
            if (fscanf(f, " %c", &k) != 1) return 2;
            if (k == 'D' && fscanf(f, "%d %d %d", &a, &b, &n) != 3) return 2;
            if (k == 'W' && fscanf(f, "%d %d", &a, &b) != 2) return 2;
            if (k == 'W') bg_idle();                         // a raw write is part of the model's state: never dropped
            if (bg_busy()) { bg_dropped++; continue; }
            bg_flush(cl);
            bg_kind = k; bg_a = a; bg_b = b; bg_c = n;
            u32 z[12] = {0}, v;
            if (k == 'S' || k == 'C') { v = k == 'C' ? 0x0010 : 0x0008; bg_xfer(0x02, true, &v, 1); bg_xfer(0x03, false, z, 12); }
            else if (k == 'D') { v = 0x8000 | (a << 8) | b; bg_xfer(0x0f, true, &v, 1); for (int i = 0; i < n; i++) bg_xfer(0x10, false, z, 1); }
            else if (k == 'I') bg_xfer(0x00, false, z, 2);
            else if (k == 'W') { v = 0x3000 | a; bg_xfer(0x0f, true, &v, 1); v = b; bg_xfer(0x10, true, &v, 1); }
            else return 2;
            continue;
        }
        if (c == 'J') { bg_idle(); continue; }
        if (c != 'R') bg_idle();                             // the foreground control port needs the pins
        if (c == 'R') {
            int op, id, exp, gap, setup;
            if (fscanf(f, "%d %d %d %d %d", &op, &id, &exp, &gap, &setup) != 5) return 2;
            for (int k = 0; k < gap; k++) tick();
            uio = (uio & 0x70) | ((op & 3) << 2);
            drive(id & 0xff);
            for (int k = 0; k < setup; k++) tick();
            int ack0 = top->uo_out & 1;
            req ^= 1; drive(id & 0xff); n_started++;
            int n = 0;
            while ((top->uo_out & 1) == ack0) { tick(); if (++n > 200) { fprintf(stderr, "line %llu: no acknowledge\n", (unsigned long long)line); return 3; } }
            n_acked++;
            int got = ((top->uo_out >> 1) & 1) | (((top->uo_out >> 2) & 1) << 1);
            if (exp & 64)                                    // an accepted predict: the four predictions on the pins
                got |= (((top->uo_out >> 5) & 1) | ((top->uio_out & 3) << 1) | (((top->uo_out >> 6) & 1) << 3)) << 2 | 64;
            if (op != 0 && got != (exp & 127)) { if (!bad) fprintf(stderr, "line %llu: op %d id %d reply %d, model %d\n", (unsigned long long)line, op, id, got, exp & 127); bad++; }
            post_mask = (exp >> 7) & 3; post_val = (exp >> 9) & 3; post_line = line;   // bits 7-10: DBG one clock later
            if (lf) fputc(n > 255 ? 255 : n, lf);
            nr++;
        } else if (c == 'W') {
            int a; u32 v;
            if (fscanf(f, "%d %u", &a, &v) != 2) return 2;
            spi(a, true, &v, 1); nw++;
            if (a == 2) wait_ready();
        } else if (c == 'S' || c == 'C') {
            u64 e[8];
            for (int k = 0; k < 8; k++) if (fscanf(f, "%llu", (unsigned long long*)&e[k]) != 1) return 2;
            u32 cmd = c == 'C' ? 0x0010 : 0x0008; spi(0x02, true, &cmd, 1);
            for (int k = 0; k < 8; k++) tick();
            u32 w[12]; spi(0x03, false, w, 12);
            bool ok = (w[1] & 0xff) == e[5] && ((w[0] >> 8) & 0xb) == e[6] && (w[0] & 7) == e[7];
            for (int k = 0; k < 5; k++) ok = ok && (w[2 + 2 * k] | ((u64)w[3 + 2 * k] << 16)) == e[k];
            if (!ok) { if (!sbad) fprintf(stderr, "line %llu: statistics differ: status %04x errors %u resolved %u, model flags %llu errors %llu resolved %llu\n",
                                         (unsigned long long)line, w[0], w[1], w[2], (unsigned long long)e[6], (unsigned long long)e[5], (unsigned long long)e[0]); sbad++; }
            nstat++;
        } else if (c == 'G') {                               // one register (FCTL, FSTAT), counted with the statistics checks
            int a; u32 e, g;
            if (fscanf(f, "%d %u", &a, &e) != 2) return 2;
            spi(a, false, &g, 1);
            if (g != e) { if (!sbad) fprintf(stderr, "line %llu: register 0x%02x is %04x, model %04x\n", (unsigned long long)line, a, g, e); sbad++; }
            nstat++;
        } else if (c == 'D') {
            int space, cnt;
            if (fscanf(f, "%d %d", &space, &cnt) != 2) return 2;
            u32 a = 0x8000 | (space << 8); spi(0x0f, true, &a, 1);
            for (int k = 0; k < 8; k++) tick();
            for (int i = 0; i < cnt; i++) {
                u32 e, g; if (fscanf(f, "%u", &e) != 1) return 2;
                spi(0x10, false, &g, 1);
                if (g != e) { if (!dbad) fprintf(stderr, "line %llu: state space %d word %d is %x, model %x\n", (unsigned long long)line, space, i, g, e); dbad++; }
            }
            ndump++;
        } else return 2;
    }
    bg_idle(); bg_flush(cl);
    if (post_mask) tick();
    bad += post_bad;
    if (lf) fclose(lf);
    if (cl) { fclose(cl); printf("background_jobs=%llu dropped=%llu ", (unsigned long long)bg_jobs, (unsigned long long)bg_dropped); }
    printf("requests=%llu reply_mismatches=%llu stat_checks=%llu stat_mismatches=%llu state_dumps=%llu state_word_mismatches=%llu control_writes=%llu chip_clocks=%llu\n",
           (unsigned long long)nr, (unsigned long long)bad, (unsigned long long)nstat, (unsigned long long)sbad, (unsigned long long)ndump, (unsigned long long)dbad,
           (unsigned long long)nw, (unsigned long long)chip_cycles);
    return (bad || sbad || dbad) ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--rand")) {
        Verilated::commandArgs(argc, argv);
        top = new Vbpx_chip;
        top->ena = 1; top->rst_n = 0; drive(0);
        for (int i = 0; i < 8; i++) tick();
        top->rst_n = 1;
        wait_ready();
        return rand_play(argv[2], argc > 3 ? argv[3] : nullptr, argc > 4 ? argv[4] : nullptr);
    }
    if (argc < 4) { fprintf(stderr, "usage: cosim prog.evt prog.text drive [K] [oplog]  |  cosim --rand steps.ops [latency.bin] [background.log]\n"); return 2; }
    int drv = atoi(argv[3]);
    if (argc > 4) K = atoi(argv[4]);
    if (argc > 5) oplog = fopen(argv[5], "wb");
    { FILE* f = fopen(argv[1], "rb"); u32 h[4]; if (!f || fread(h, 4, 4, f) != 4) return 1; recs.resize(h[2]); if (fread(recs.data(), 16, h[2], f) != h[2]) return 1; fclose(f); }
    { FILE* f = fopen(argv[2], "rb"); u32 h[2]; if (!f || fread(h, 4, 2, f) != 2) return 1; text_base = h[0]; text.resize(h[1]); if (fread(text.data(), 4, h[1], f) != h[1]) return 1; fclose(f); }
    Verilated::commandArgs(argc, argv);
    top = new Vbpx_chip;
    top->ena = 1; top->rst_n = 0; drive(0);
    for (int i = 0; i < 8; i++) tick();
    top->rst_n = 1;
    for (int i = 0; i < 40 && !((top->uo_out >> 4) & 1); i++) tick();
    if (!((top->uo_out >> 4) & 1)) { fprintf(stderr, "not ready\n"); return 3; }
    // CTRL: the driving predictor; DBG selections as after reset; features of the default predictor in CTRL[14:12]
    // (BPX_FEAT 0..7, default 7 as after reset; CTRL[15] is unused and reads 0)
    { int feat = getenv("BPX_FEAT") ? atoi(getenv("BPX_FEAT")) : 7; u32 c = drv | 0x0DA0 | ((feat & 7) << 12); spi(0x01, true, &c, 1); }

    if (getenv("BPX_SEQ")) {            // one branch in flight: every record is one PREDICT+RESOLVE request (U2)
        u64 n = 0, wrong = 0, since = 0;
        for (const Rec& r : recs) {
            if ((r.info & 3) == 3) break;
            if ((r.info & 3) != 0) continue;
            int taken = (r.info >> 2) & 1;
            wrong += request(2 | taken, fold_id(r.pc), n) != taken; n++;
            if (++since >= 50000) { snapshot_clear(); since = 0; }
        }
        snapshot_clear();
        u32 e = 0; spi(0x04, false, &e, 1);
        if (oplog) fclose(oplog);
        printf("drive=%d seq=1 branches=%llu mis_drive=%llu errors=%u resolved=%llu mis_def=%llu mis_bim=%llu mis_gsh=%llu mis_trn=%llu lat_max=%llu\n", drv, (unsigned long long)n,
               (unsigned long long)wrong, e & 0xff, (unsigned long long)stat[0], (unsigned long long)stat[1], (unsigned long long)stat[2], (unsigned long long)stat[3], (unsigned long long)stat[4], (unsigned long long)lat_max);
        return 0;
    }
    const int R = 1, PM = R + 1, BTB = 256;
    const bool dual = getenv("BPX_DUAL") != nullptr;
    std::vector<u32> btb(BTB, 0xffffffffu);
    auto btb_peek = [&](u32 a) { return btb[(a >> 2) & (BTB - 1)] == a; };
    auto btb_hit = [&](u32 a) { u32 i = (a >> 2) & (BTB - 1); bool h = btb[i] == a; btb[i] = a; return h; };
    struct Ev { u64 when; int kind; int taken; bool misp; };   // kind 0 resolve, 1 flush
    std::deque<Ev> ev;
    u64 cyc = 1, snt = 1, ins = 0, br = 0, mis = 0, wp = 0, last_id_req = ~0ull, since = 0, inflight = 0, peak = 0;
    // issue every scheduled request strictly before cycle t
    auto drain = [&](u64 t) {
        while (!ev.empty() && ev.front().when < t) {
            Ev e = ev.front(); ev.pop_front();
            if (e.kind == 0) { request(0, 2 | e.taken, e.when); inflight = e.misp ? 0 : inflight - 1; if (++since >= 50000) { snapshot_clear(); since = 0; } }
            else { request(0, 1, e.when); inflight = 0; }
        }
    };
    // a predict at cycle t, combined with a resolve scheduled for the same cycle (never a misprediction: see caller)
    auto predict_at = [&](u64 t, u32 pc) {
        drain(t);
        int op = 1;
        if (!ev.empty() && ev.front().when == t && ev.front().kind == 0 && !ev.front().misp) {
            op = 2 | ev.front().taken; ev.pop_front();
            int p = request(op, fold_id(pc), t);            // predict, then resolve the oldest: count unchanged
            if (++since >= 50000) { snapshot_clear(); since = 0; }
            if (inflight + 1 > peak) peak = inflight + 1;
            return p;
        }
        int p = request(op, fold_id(pc), t);
        if (++inflight > peak) peak = inflight;
        return p;
    };
    // wrong-path instructions in IF during cycles x0..x1 starting at address a: BTB-hit branches ask and are followed
    auto wrong_path = [&](u64 x0, u64 x1, u32 a) {
        for (u64 x = x0; x <= x1; x++) {
            u32 tg = 0; int k = decode(a, tg);
            bool busy = !ev.empty() && ev.front().when == x && (ev.front().kind == 1 || ev.front().misp);
            if (k == 1 && btb_peek(a) && !busy) { wp++; a = predict_at(x, a) ? tg : a + 4; }
            else if (k == 2 && btb_peek(a)) a = tg;
            else a += 4;
        }
    };
    for (const Rec& r : recs) {
        u32 kind = r.info & 3, nb = r.info >> 8;
        u64 base = (u64)nb + (r.extra & 0xffff) + (u64)((r.extra >> 16) & 0xff) * 32;
        cyc += base; snt += base; ins += nb;
        if (kind == 3) break;
        ins++;
        if (kind == 0) {
            int taken = (r.info >> 2) & 1;
            bool hit = btb_hit(r.pc);
            // one request per CPU cycle: the request from ID goes first, the one from IF waits a cycle (one bubble).
            // BPX_DUAL: the adapter issues both in the same CPU cycle (needs enough chip clocks per CPU cycle).
            if (!dual && hit && cyc - 1 == last_id_req) cyc++;
            if (!hit) last_id_req = cyc;
            u64 tp = hit ? cyc - 1 : cyc;
            drain(tp);
            // a misprediction (or a flush) is reported alone: the predict of that cycle waits one cycle (one bubble);
            // BPX_DUAL: it follows in the same CPU cycle
            if (!ev.empty() && ev.front().when == tp && (ev.front().kind == 1 || ev.front().misp)) {
                if (dual) drain(tp + 1);
                else { cyc++; tp++; if (!hit) last_id_req = cyc; }
            }
            int p = predict_at(tp, r.pc);
            bool m = p != taken;
            // the outcome is reported in the cycle after EX
            ev.push_back({cyc + R + 1, 0, taken, m});
            if (m) {
                mis++;
                u32 wa = p ? r.target : r.pc + 4;
                if (hit) wrong_path(cyc, cyc + R, wa);
                else wrong_path(cyc + 1, cyc + R, wa);
                cyc += 1 + PM;
            } else cyc += 1 + ((taken && !hit) ? 1 : 0);
            snt += 1 + (taken ? PM : 0);
            br++;
        } else if (kind == 1) { int b = btb_hit(r.pc) ? 0 : 1; cyc += 1 + b; snt += 1 + b; }
        else {
            // JALR: resolved in EX; sequential instructions behind it are wrong-path until then; FLUSH the cycle after
            ev.push_back({cyc + R + 1, 1, 0, false});
            wrong_path(cyc, cyc + R, r.pc + 4);
            cyc += 1 + PM; snt += 1 + PM;
        }
    }
    drain(~0ull);
    snapshot_clear();
    u32 errs = 0; spi(0x04, false, &errs, 1);
    if (oplog) fclose(oplog);
    printf("errors=%u dual=%d pairs=%llu used_max=%d ", errs & 0xff, dual ? 1 : 0, (unsigned long long)pairs, used_max);
    printf("drive=%d K=%d instr=%llu branches=%llu cycles=%llu snt_cycles=%llu mis_drive=%llu wrongpath_predicts=%llu requests=%llu chip_cycles=%llu "
           "lat_avg=%.3f lat_max=%llu late=%llu peak_inflight=%llu resolved=%llu mis_def=%llu mis_bim=%llu mis_gsh=%llu mis_trn=%llu lat3=%llu lat4=%llu lat5=%llu lat6p=%llu\n",
           drv, K, (unsigned long long)ins, (unsigned long long)br, (unsigned long long)cyc, (unsigned long long)snt, (unsigned long long)mis, (unsigned long long)wp,
           (unsigned long long)n_req, (unsigned long long)chip_cycles, n_req ? (double)lat_sum / n_req : 0.0, (unsigned long long)lat_max, (unsigned long long)late,
           (unsigned long long)peak, (unsigned long long)stat[0], (unsigned long long)stat[1], (unsigned long long)stat[2], (unsigned long long)stat[3], (unsigned long long)stat[4],
           (unsigned long long)lat_hist[3], (unsigned long long)lat_hist[4], (unsigned long long)lat_hist[5],
           (unsigned long long)(n_req - lat_hist[3] - lat_hist[4] - lat_hist[5] - lat_hist[0] - lat_hist[1] - lat_hist[2]));
    delete top;
    return 0;
}
