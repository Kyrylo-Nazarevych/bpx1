// rvtrace: Spike instruction log -> BPX-1 event trace. The file formats are specified in bench/README.md.
//
//   spike --isa=rv32im -l --log=/dev/stdout prog.elf | rvtrace <name> <prog.elf> <outdir>
//
// stdin    Spike "-l" log, one line per executed instruction:
//              core   0: 0x80000000 (0x00000297) auipc   t0, 0x0
//          Every other line is ignored, except an optional trailer "spike_status <n>" (Spike's exit
//          status, appended by build.sh), which is copied into summary.json (-1 when it is missing).
// output   <outdir>/prog.evt      event trace
//          <outdir>/prog.text     instruction words of the executable ELF sections
//          <outdir>/summary.json  totals
//
// What is traced: every instruction whose pc lies in the executable sections, from the first one
// executed up to and including the store to HTIF tohost. Spike's boot ROM (pc 0x1000) lies outside
// and is skipped. The store is found by the ELF symbol __tohost_store (bench/rt/crt0.S); without
// that symbol the trace ends just before the first "jal x0, 0" (jump to itself).
//
// Exit status: 0 = complete trace; 2 = the log ended before the exit, or the control flow in the
// log contradicts the decoded instructions (outputs are still written); 1 = usage or file error.
//
// C++17, no dependencies:  g++ -O2 -std=c++17 -o rvtrace rvtrace.cc

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

[[noreturn]] static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "rvtrace: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

// ---------------------------------------------------------------------------------------- ELF32

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

struct Image {
    uint32_t base = 0;                  // address of the first instruction word
    std::vector<uint32_t> words;        // executable sections, gaps filled with 0
    bool have_end = false;
    uint32_t end_pc = 0;                // address of the store to tohost
};

static Image load_elf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s: %s", path, strerror(errno));
    std::vector<uint8_t> d;
    uint8_t tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) d.insert(d.end(), tmp, tmp + n);
    fclose(f);
    if (d.size() < 52 || memcmp(d.data(), "\177ELF", 4) != 0) die("%s is not an ELF file", path);
    if (d[4] != 1 || d[5] != 1) die("%s is not a little-endian ELF32 file", path);
    if (rd16(&d[18]) != 243) die("%s is not a RISC-V ELF file", path);
    const uint32_t shoff = rd32(&d[32]), shentsize = rd16(&d[46]), shnum = rd16(&d[48]);
    if (shentsize < 40 || (uint64_t)shoff + (uint64_t)shnum * shentsize > d.size())
        die("%s: bad section header table", path);
    auto sh = [&](uint32_t i) { return &d[shoff + i * shentsize]; };

    Image im;
    uint32_t lo = 0xffffffffu, hi = 0;
    for (uint32_t i = 0; i < shnum; i++) {
        const uint8_t *s = sh(i);
        const uint32_t type = rd32(s + 4), flags = rd32(s + 8), addr = rd32(s + 12), size = rd32(s + 20);
        if (type == 1 && (flags & 2) && (flags & 4) && size) {      // PROGBITS, ALLOC, EXECINSTR
            if (addr < lo) lo = addr;
            if (addr + size > hi) hi = addr + size;
        }
    }
    if (hi <= lo) die("%s: no executable section", path);
    if (lo & 3) die("%s: executable sections start at an unaligned address", path);
    im.base = lo;
    std::vector<uint8_t> bytes(((hi - lo) + 3) & ~3u, 0);
    for (uint32_t i = 0; i < shnum; i++) {
        const uint8_t *s = sh(i);
        const uint32_t type = rd32(s + 4), flags = rd32(s + 8), addr = rd32(s + 12);
        const uint32_t off = rd32(s + 16), size = rd32(s + 20);
        if (type == 1 && (flags & 2) && (flags & 4) && size) {
            if ((uint64_t)off + size > d.size()) die("%s: section data outside the file", path);
            memcpy(&bytes[addr - lo], &d[off], size);
        }
    }
    im.words.resize(bytes.size() / 4);
    for (size_t i = 0; i < im.words.size(); i++) im.words[i] = rd32(&bytes[4 * i]);

    for (uint32_t i = 0; i < shnum; i++) {                          // symbol __tohost_store
        const uint8_t *s = sh(i);
        if (rd32(s + 4) != 2) continue;                             // SHT_SYMTAB
        const uint32_t off = rd32(s + 16), size = rd32(s + 20), link = rd32(s + 24);
        if (link >= shnum || (uint64_t)off + size > d.size()) continue;
        const uint32_t stroff = rd32(sh(link) + 16), strsize = rd32(sh(link) + 20);
        if ((uint64_t)stroff + strsize > d.size()) continue;
        static const char want[] = "__tohost_store";
        for (uint32_t k = 0; k + 16 <= size; k += 16) {
            const uint8_t *sym = &d[off + k];
            const uint32_t name = rd32(sym);
            if (name < strsize && strsize - name >= sizeof want &&
                memcmp(&d[stroff + name], want, sizeof want) == 0) {
                im.have_end = true;
                im.end_pc = rd32(sym + 4);
            }
        }
    }
    return im;
}

// ------------------------------------------------------------------------------------- tracing

namespace {

enum { K_BRANCH = 0, K_JAL = 1, K_JALR = 2, K_END = 3 };

struct Tracer {
    Image im;
    uint32_t text_bytes = 0;
    FILE *evt = nullptr;
    std::vector<uint32_t> out;          // records waiting to be written (4 words each)
    std::vector<uint8_t> site;          // per instruction word: executed as a conditional branch

    // totals
    uint64_t instructions = 0, cond = 0, taken = 0, sites = 0, jal = 0, jalr = 0;
    uint64_t loaduse = 0, mul = 0, div = 0, records = 0, identity = 0;
    uint64_t foreign = 0, bad_flow = 0, saturated = 0, degenerate = 0;
    long spike_status = -1;

    // current span: instructions since the last record, not counting a pending instruction
    uint32_t sp_n = 0, sp_lu = 0, sp_mul = 0, sp_div = 0;
    // control-transfer instruction waiting for the next pc (which tells taken / the JALR target)
    bool pending = false;
    uint32_t p_pc = 0, p_target = 0, p_info = 0, p_extra = 0;
    int p_kind = 0;

    uint32_t prev_load_rd = 0;          // rd of the previous instruction if it was a load, else 0
    uint32_t last_pc = 0;
    bool started = false, done = false;

    void emit(uint32_t pc, uint32_t target, uint32_t info, uint32_t extra) {
        out.push_back(pc);
        out.push_back(target);
        out.push_back(info);
        out.push_back(extra);
        records++;
        if ((info & 3) == K_END) identity += info >> 8;
        else identity += (uint64_t)(info >> 8) + 1;
        if (out.size() >= (1u << 18)) flush();
    }
    void flush() {
        std::vector<uint8_t> b(out.size() * 4);
        for (size_t i = 0; i < out.size(); i++) {
            b[4 * i] = (uint8_t)out[i];
            b[4 * i + 1] = (uint8_t)(out[i] >> 8);
            b[4 * i + 2] = (uint8_t)(out[i] >> 16);
            b[4 * i + 3] = (uint8_t)(out[i] >> 24);
        }
        if (!b.empty() && fwrite(b.data(), 1, b.size(), evt) != b.size()) die("write error on prog.evt");
        out.clear();
    }
    uint32_t pack_extra() {
        if (sp_lu > 0xffff || sp_div > 0xff || sp_mul > 0xff) saturated++;
        const uint32_t lu = sp_lu > 0xffff ? 0xffff : sp_lu;
        const uint32_t dv = sp_div > 0xff ? 0xff : sp_div;
        const uint32_t ml = sp_mul > 0xff ? 0xff : sp_mul;
        return lu | dv << 16 | ml << 24;
    }
    void resolve(uint32_t next_pc, bool known) {    // complete the pending record
        if (p_kind == K_BRANCH) {
            const bool t = known && next_pc != p_pc + 4;
            if (t && next_pc != p_target) bad_flow++;
            if (t) { p_info |= 4; taken++; }
        } else if (p_kind == K_JAL) {
            if (known && next_pc != p_target) bad_flow++;
            p_info |= 4;
        } else {
            p_target = known ? next_pc : 0;
            p_info |= 4;
        }
        emit(p_pc, p_target, p_info, p_extra);
        pending = false;
    }
    void finish() {                                 // write the END record
        if (pending) resolve(0, false);             // the last instruction was a control transfer
        if (sp_n > 0xffffff) die("more than 2^24-1 instructions without a control transfer");
        emit(last_pc, 0, K_END | sp_n << 8, pack_extra());
        done = true;
    }

    void step(uint32_t pc, uint32_t insn) {
        if (done) return;
        if (pc - im.base >= text_bytes || (pc & 3)) {
            if (started) foreign++;                 // left the program: a trap, a wild jump
            return;
        }
        started = true;
        if (pending) resolve(pc, true);
        if (!im.have_end && insn == 0x0000006f) { finish(); return; }

        instructions++;
        last_pc = pc;
        const uint32_t opc = insn & 0x7f;
        if (prev_load_rd) {                         // load-use hazard, README.md
            const uint32_t rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31;
            bool hazard = false;
            if (opc == 0x33 || opc == 0x63 || opc == 0x23) hazard = rs1 == prev_load_rd || rs2 == prev_load_rd;
            else if (opc == 0x13 || opc == 0x03 || opc == 0x67) hazard = rs1 == prev_load_rd;
            if (hazard) { sp_lu++; loaduse++; }
        }
        prev_load_rd = opc == 0x03 ? (insn >> 7) & 31 : 0;
        if (opc == 0x33 && insn >> 25 == 1) {       // M extension
            if (insn & 0x4000) { sp_div++; div++; } else { sp_mul++; mul++; }
        }

        if (opc == 0x63 || opc == 0x6f || opc == 0x67) {
            if (opc == 0x63) {
                const int32_t imm = (int32_t)(insn & 0x80000000) >> 19 | (insn & 0x80) << 4 |
                                    (insn >> 20 & 0x7e0) | (insn >> 7 & 0x1e);
                p_kind = K_BRANCH;
                p_target = pc + (uint32_t)imm;
                cond++;
                if (imm == 4) degenerate++;         // taken and not taken go to the same place
                uint8_t &s = site[(pc - im.base) >> 2];
                if (!s) { s = 1; sites++; }
            } else if (opc == 0x6f) {
                const int32_t imm = (int32_t)(insn & 0x80000000) >> 11 | (insn & 0xff000) |
                                    (insn >> 9 & 0x800) | (insn >> 20 & 0x7fe);
                p_kind = K_JAL;
                p_target = pc + (uint32_t)imm;
                jal++;
            } else {
                p_kind = K_JALR;
                p_target = 0;
                jalr++;
            }
            if (sp_n > 0xffffff) die("more than 2^24-1 instructions without a control transfer");
            p_pc = pc;
            p_info = (uint32_t)p_kind | sp_n << 8;
            p_extra = pack_extra();
            sp_n = sp_lu = sp_mul = sp_div = 0;
            pending = true;
        } else {
            sp_n++;
        }
        if (im.have_end && pc == im.end_pc) finish();
    }
};

int8_t HEX[256];

}  // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: spike --isa=rv32im -l --log=/dev/stdout prog.elf | rvtrace <name> <prog.elf> <outdir>\n");
        return 1;
    }
    const std::string name = argv[1], dir = argv[3];
    for (char c : name)
        if (c == '"' || c == '\\' || (unsigned char)c < 0x20) die("unsuitable program name");
    memset(HEX, -1, sizeof HEX);
    for (int i = 0; i < 10; i++) HEX['0' + i] = (int8_t)i;
    for (int i = 0; i < 6; i++) HEX['a' + i] = HEX['A' + i] = (int8_t)(10 + i);

    Tracer t;
    t.im = load_elf(argv[2]);
    t.text_bytes = (uint32_t)t.im.words.size() * 4;
    t.site.assign(t.im.words.size(), 0);

    {   // prog.text
        const std::string p = dir + "/prog.text";
        FILE *f = fopen(p.c_str(), "wb");
        if (!f) die("cannot create %s: %s", p.c_str(), strerror(errno));
        std::vector<uint8_t> b(8 + (size_t)t.text_bytes);
        const uint32_t hdr[2] = {t.im.base, (uint32_t)t.im.words.size()};
        for (size_t i = 0; i < 2 + t.im.words.size(); i++) {
            const uint32_t w = i < 2 ? hdr[i] : t.im.words[i - 2];
            b[4 * i] = (uint8_t)w;
            b[4 * i + 1] = (uint8_t)(w >> 8);
            b[4 * i + 2] = (uint8_t)(w >> 16);
            b[4 * i + 3] = (uint8_t)(w >> 24);
        }
        if (fwrite(b.data(), 1, b.size(), f) != b.size() || fclose(f) != 0) die("write error on %s", p.c_str());
    }

    const std::string evt_path = dir + "/prog.evt";
    t.evt = fopen(evt_path.c_str(), "wb");
    if (!t.evt) die("cannot create %s: %s", evt_path.c_str(), strerror(errno));
    static const uint8_t zero_header[16] = {0};
    fwrite(zero_header, 1, 16, t.evt);              // rewritten at the end, when n_records is known

    // Read the log in large blocks and split it into lines by hand.
    const size_t BUFSZ = 1u << 22;
    std::vector<char> buf(BUFSZ);
    size_t have = 0;
    bool eof = false;
    while (!eof) {
        const ssize_t r = read(0, buf.data() + have, BUFSZ - have);
        if (r < 0) {
            if (errno == EINTR) continue;
            die("read error on stdin: %s", strerror(errno));
        }
        if (r == 0) {
            eof = true;
            if (have == 0) break;
            buf[have++] = '\n';                     // last line without a newline (there is room: see below)
        } else {
            have += (size_t)r;
        }
        const char *p = buf.data(), *const end = p + have;
        for (;;) {
            const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
            if (!nl) break;
            const char *q = p;
            p = nl + 1;
            if (nl - q >= 24 && q[0] == 'c' && q[1] == 'o' && q[2] == 'r' && q[3] == 'e') {
                if (t.done) continue;               // only the trailer matters after the exit
                q += 4;
                while (*q == ' ') q++;
                while ((unsigned)(*q - '0') < 10) q++;
                if (*q != ':') continue;
                q++;
                while (*q == ' ') q++;
                if (q[0] != '0' || q[1] != 'x') continue;       // e.g. "core   0: >>>>  main"
                q += 2;
                uint64_t pc = 0, insn = 0;
                int h, nd = 0;
                while ((h = HEX[(unsigned char)*q]) >= 0) { pc = pc << 4 | (unsigned)h; q++; }
                if (q[0] != ' ' || q[1] != '(' || q[2] != '0' || q[3] != 'x') continue;
                q += 4;
                while ((h = HEX[(unsigned char)*q]) >= 0) { insn = insn << 4 | (unsigned)h; q++; nd++; }
                if (*q != ')') continue;
                if (nd != 8 && (uint32_t)pc - t.im.base < t.text_bytes)
                    die("compressed (16-bit) instruction at pc 0x%08x: build with -march=rv32im", (unsigned)pc);
                t.step((uint32_t)pc, (uint32_t)insn);
            } else if (nl - q >= 14 && memcmp(q, "spike_status ", 13) == 0) {
                t.spike_status = strtol(q + 13, nullptr, 10);
            }
        }
        have = (size_t)(end - p);
        memmove(buf.data(), p, have);
        if (have >= BUFSZ - 1) die("log line longer than %zu bytes", BUFSZ - 1);
    }

    const bool complete = t.done;
    if (!t.done) {                                  // log ended early: close the trace where it stops
        if (t.started) t.finish();
        else t.emit(0, 0, K_END, 0);
    }
    t.flush();
    uint8_t header[16] = {'B', 'P', 'X', 'E', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (t.records > 0xffffffffull) die("more than 2^32-1 records");
    for (int i = 0; i < 4; i++) header[8 + i] = (uint8_t)(t.records >> (8 * i));
    if (fseek(t.evt, 0, SEEK_SET) != 0 || fwrite(header, 1, 16, t.evt) != 16 || fclose(t.evt) != 0)
        die("write error on %s", evt_path.c_str());

    {   // summary.json
        const std::string p = dir + "/summary.json";
        FILE *f = fopen(p.c_str(), "w");
        if (!f) die("cannot create %s: %s", p.c_str(), strerror(errno));
        fprintf(f,
                "{\"name\": \"%s\", \"instructions\": %llu, \"cond_branches\": %llu, \"cond_taken\": %llu, "
                "\"static_cond_sites\": %llu, \"jal\": %llu, \"jalr\": %llu, \"loaduse\": %llu, \"mul\": %llu, "
                "\"div\": %llu, \"text_bytes\": %u, \"spike_status\": %ld}\n",
                name.c_str(), (unsigned long long)t.instructions, (unsigned long long)t.cond,
                (unsigned long long)t.taken, (unsigned long long)t.sites, (unsigned long long)t.jal,
                (unsigned long long)t.jalr, (unsigned long long)t.loaduse, (unsigned long long)t.mul,
                (unsigned long long)t.div, t.text_bytes, t.spike_status);
        if (fclose(f) != 0) die("write error on %s", p.c_str());
    }

    int rc = 0;
    if (!complete) { fprintf(stderr, "rvtrace: %s: the log ends before the write to tohost\n", name.c_str()); rc = 2; }
    if (t.identity != t.instructions) {
        fprintf(stderr, "rvtrace: %s: internal error: records account for %llu instructions, counted %llu\n",
                name.c_str(), (unsigned long long)t.identity, (unsigned long long)t.instructions);
        rc = 2;
    }
    if (t.bad_flow) {
        fprintf(stderr, "rvtrace: %s: %llu control transfers went to an unexpected pc\n", name.c_str(),
                (unsigned long long)t.bad_flow);
        rc = 2;
    }
    if (t.foreign) {
        fprintf(stderr, "rvtrace: %s: %llu instructions executed outside .text (trap?)\n", name.c_str(),
                (unsigned long long)t.foreign);
        rc = 2;
    }
    if (t.saturated)
        fprintf(stderr, "rvtrace: %s: note: %llu records have a saturated load-use/mul/div count\n", name.c_str(),
                (unsigned long long)t.saturated);
    if (t.degenerate)
        fprintf(stderr, "rvtrace: %s: note: %llu executions of a branch to pc+4 (recorded as not taken)\n",
                name.c_str(), (unsigned long long)t.degenerate);
    return rc;
}
