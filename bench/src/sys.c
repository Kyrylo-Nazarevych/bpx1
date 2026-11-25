/* BPX-1 bench: system-level code. Select one with -DSEL=<n>:
     1 sys.malloc   first-fit allocator on an address-ordered free list with splitting and
                    coalescing, under a random allocate / check / free load
     2 sys.printf   printf-style formatter (%d %u %x %X %o %c %s %%, flags - 0 + #, width, precision,
                    *) called with many formats and values
     3 sys.cpu16    emulator of a toy 16-bit RISC CPU (decode by if/else chain: RT_IF_CHAIN, no jump
                    table) running a machine-code program: shift-and-add multiply, insertion sort,
                    binary search, bit counting
     4 sys.json     JSON parser written as a character-driven state machine (a switch: GCC makes it
                    a jump table, so this program also has many indirect jumps) over generated documents

   Self-checks (exit codes 1..29): block contents survive and the heap ends as one free block;
   numbers parse back from their text; the emulated program's results equal the same work done
   directly in C; the parser counts exactly what the generator wrote. */
#include "rt.h"

#if SEL == 1
#define HEAP_WORDS 8192                 /* 32 KiB */
#define NSLOT 160
#define OPS 9000
/* Block = [size in words, including this header][payload ...]. A free block keeps the index of the
   next free block (ascending addresses, 0 = end) in its second word. Word 0..1 is the list head. */
static u32 heap[HEAP_WORDS];
static u32 slot_at[NSLOT], slot_size[NSLOT];
static u8 slot_tag[NSLOT];

static void heap_init(void) {
    heap[0] = 0;
    heap[1] = 2;
    heap[2] = HEAP_WORDS - 2;
    heap[3] = 0;
}
static u32 heap_alloc(u32 nbytes) {     /* index of the payload, 0 if no block is large enough */
    u32 units = (nbytes + 3) / 4 + 1;
    if (units < 2) units = 2;
    for (u32 prev = 0, p = heap[1]; p != 0; prev = p, p = heap[p + 1]) {
        u32 size = heap[p];
        if (size < units) continue;
        if (size <= units + 1) heap[prev + 1] = heap[p + 1];    /* (nearly) exact fit: take all of it */
        else {                                                  /* split: allocate the upper part */
            heap[p] = size - units;
            p += size - units;
            heap[p] = units;
        }
        return p + 1;
    }
    return 0;
}
static void heap_free(u32 payload) {
    u32 bp = payload - 1, p = 0, next;
    while ((next = heap[p + 1]) != 0 && next < bp) p = next;    /* p = last free block below bp */
    if (next != 0 && bp + heap[bp] == next) {                   /* join with the block above */
        heap[bp] += heap[next];
        heap[bp + 1] = heap[next + 1];
    } else heap[bp + 1] = next;
    if (p != 0 && p + heap[p] == bp) {                          /* join with the block below */
        heap[p] += heap[bp];
        heap[p + 1] = heap[bp + 1];
    } else heap[p + 1] = bp;
}
/* Free list in order, never two free blocks side by side; returns the free words, or -1. */
static int heap_check(void) {
    u32 total = 0;
    for (u32 p = heap[1]; p != 0; p = heap[p + 1]) {
        u32 next = heap[p + 1];
        if (heap[p] < 2 || p + heap[p] > HEAP_WORDS) return -1;
        if (next != 0 && p + heap[p] >= next) return -1;
        total += heap[p];
    }
    return (int)total;
}

int main(void) {
    u32 chk = 0, live_words = 0;
    int failed = 0;
    rt_srand(2801);
    heap_init();
    for (int op = 0; op < OPS; op++) {
        u32 s = rt_rand_n(NSLOT);
        if (slot_at[s] == 0) {
            u32 r = rt_rand_n(20);
            u32 size = r < 14 ? 1 + rt_rand_n(48) : r < 19 ? 49 + rt_rand_n(208) : 257 + rt_rand_n(900);
            u32 at = heap_alloc(size);
            if (at == 0) { failed++; continue; }
            u8 *p = (u8 *)&heap[at], tag = (u8)(op * 7 + 1);
            u32 mark = size < 24 ? size : 24;                   /* stamp the first bytes and the last one */
            for (u32 i = 0; i < mark; i++) p[i] = (u8)(tag + i);
            p[size - 1] = (u8)(tag ^ 0x5a);
            slot_at[s] = at;
            slot_size[s] = size;
            slot_tag[s] = tag;
            live_words += heap[at - 1];
            chk = chk * 31 + at;
        } else {
            u32 at = slot_at[s], size = slot_size[s];
            u8 *p = (u8 *)&heap[at], tag = slot_tag[s];
            u32 mark = size < 24 ? size : 24;
            if (p[size - 1] != (u8)(tag ^ 0x5a)) return 1;      /* somebody wrote into this block */
            for (u32 i = 0; i + 1 < mark; i++)
                if (p[i] != (u8)(tag + i)) return 2;
            live_words -= heap[at - 1];
            heap_free(at);
            slot_at[s] = 0;
        }
        if ((op & 511) == 511) {
            int free_words = heap_check();
            if (free_words < 0) return 3;
            if ((u32)free_words + live_words != HEAP_WORDS - 2) return 4;
            chk = chk * 31 + (u32)free_words;
        }
    }
    for (int s = 0; s < NSLOT; s++)
        if (slot_at[s]) heap_free(slot_at[s]);
    if (heap[1] != 2 || heap[2] != HEAP_WORDS - 2 || heap[3] != 0) return 5;    /* one free block again */
    return rt_check(chk + (u32)failed, 0x024c26);
}

#elif SEL == 2
#include <stdarg.h>
#define CALLS 4000

static int utoa(u32 v, u32 base, int upper, char *out) {       /* returns the number of digits */
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char rev[34];
    int n = 0;
    do { rev[n++] = digits[v % base]; v /= base; } while (v);
    for (int i = 0; i < n; i++) out[i] = rev[n - 1 - i];
    return n;
}
/* Like vsnprintf: writes at most cap - 1 characters and a 0; returns the full length. */
static int format(char *out, int cap, const char *f, va_list ap) {
    int n = 0;
#define PUT(c) do { if (n < cap - 1) out[n] = (char)(c); n++; } while (0)
    while (*f) {
        if (*f != '%') { char c = *f++; PUT(c); continue; }
        f++;
        int left = 0, zero = 0, plus = 0, alt = 0, width = 0, prec = -1, numeric = 1, len = 0, zeros = 0, pad;
        char tmp[36], sign = 0, conv;
        const char *body = tmp, *prefix = "";
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '+') plus = 1;
            else if (*f == '#') alt = 1;
            else break;
        }
        if (*f == '*') { width = va_arg(ap, int); f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++;
            prec = 0;
            while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        conv = *f++;
        if (conv == 'd' || conv == 'i') {
            int v = va_arg(ap, int);
            if (v < 0) sign = '-';
            else if (plus) sign = '+';
            len = utoa(v < 0 ? 0u - (u32)v : (u32)v, 10, 0, tmp);
        } else if (conv == 'u') len = utoa(va_arg(ap, u32), 10, 0, tmp);
        else if (conv == 'x' || conv == 'X') {
            u32 v = va_arg(ap, u32);
            len = utoa(v, 16, conv == 'X', tmp);
            if (alt && v) prefix = conv == 'X' ? "0X" : "0x";
        } else if (conv == 'o') len = utoa(va_arg(ap, u32), 8, 0, tmp);
        else {
            numeric = 0;
            if (conv == 'c') { tmp[0] = (char)va_arg(ap, int); len = 1; }
            else if (conv == 's') {
                body = va_arg(ap, const char *);
                len = (int)strlen(body);
                if (prec >= 0 && len > prec) len = prec;
            } else if (conv == 0) break;                        /* '%' at the very end */
            else { tmp[0] = conv; len = 1; }                    /* "%%" and unknown letters */
        }
        if (numeric && prec > len) zeros = prec - len;
        pad = width - (sign != 0) - (int)strlen(prefix) - zeros - len;
        if (pad < 0) pad = 0;
        if (zero && !left && numeric && prec < 0) { zeros += pad; pad = 0; }
        if (!left)
            for (; pad > 0; pad--) PUT(' ');
        if (sign) PUT(sign);
        for (; *prefix; prefix++) PUT(*prefix);
        for (; zeros > 0; zeros--) PUT('0');
        for (int i = 0; i < len; i++) PUT(body[i]);
        for (; pad > 0; pad--) PUT(' ');
    }
#undef PUT
    if (cap > 0) out[n < cap ? n : cap - 1] = 0;
    return n;
}
static int fmt(char *out, int cap, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int n = format(out, cap, f, ap);
    va_end(ap);
    return n;
}
static int parse_int(const char *s) {
    int neg = *s == '-';
    u32 v = 0;
    if (neg) s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (u32)(*s++ - '0');
    return (int)(neg ? 0u - v : v);
}
static u32 parse_hex(const char *s) {
    u32 v = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') v = v << 4 | (u32)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') v = v << 4 | (u32)(*s - 'a' + 10);
        else return v;
    }
}

int main(void) {
    static const char *const names[8] = {"x", "count", "total_length", "buf", "", "branch_predictor", "pc", "taken"};
    static char buf[128];
    u32 chk = 0;
    rt_srand(2901);
    for (int call = 0; call < CALLS; call++) {
        u32 k = rt_rand_n(12);
        u32 ua = rt_rand(), ub;
        ua >>= 1 + rt_rand_n(30);
        ub = rt_rand();
        ub >>= 1 + rt_rand_n(30);
        int a = (int)ua, b = (int)ub, n, cap = 96;
        const char *w = names[rt_rand_n(8)];
        if (rt_rand() & 1) a = -a;
        if (k == 0) {
            n = fmt(buf, cap, "%d", a);
            if (parse_int(buf) != a) return 1;
        } else if (k == 1) {
            n = fmt(buf, cap, "[%8d|%-8d|%08d]", a, b, a);
            if (n < 28) return 2;
        } else if (k == 2) n = fmt(buf, cap, "%+d %u", a, (u32)b);
        else if (k == 3) n = fmt(buf, cap, "%x %X %#x", (u32)a, (u32)b, (u32)a);
        else if (k == 4) {
            n = fmt(buf, cap, "%08x", (u32)a);
            if (n != 8 || parse_hex(buf) != (u32)a) return 3;
        } else if (k == 5) n = fmt(buf, cap, "%s=%d;", w, a);
        else if (k == 6) {
            n = fmt(buf, cap, "[%10s][%-10s]", w, w);
            if (n != (strlen(w) > 10 ? 4 + 2 * (int)strlen(w) : 24)) return 4;
        } else if (k == 7) n = fmt(buf, cap, "%.3s:%c%c", w, 'a' + (a & 15), '0' + (b & 7));
        else if (k == 8) n = fmt(buf, cap, "%o %5.3d", (u32)a, b % 1000);
        else if (k == 9) n = fmt(buf, cap, "%3d%% of %*d", a % 101, (b & 7) + 1, b);
        else if (k == 10) n = fmt(buf, cap, "id=%u, hex=0x%04X, name=%s;", (u32)a, (u32)b & 0xffff, w);
        else {
            cap = 24;                                           /* too small for most: output is cut */
            n = fmt(buf, cap, "%s %s %s %d", w, w, w, a);
        }
        int len = (int)strlen(buf);
        if (len != (n < cap ? n : cap - 1)) return 5;
        chk = chk * 31 + (u32)n;
        for (int i = 0; i < len; i++) chk = chk * 31 + (u8)buf[i];
    }
    return rt_check(chk, 0x2231b3);
}

#elif SEL == 3
/* Toy CPU "T16": 8 registers of 16 bits (r0 reads 0), 4096 words of memory, word-addressed pc.
   Instruction: op[15:12] rd[11:9] rs[8:6] then rt[5:3], or imm6[5:0] (signed), or imm9[8:0] (signed). */
enum { ADD, SUB, AND, OR, XOR, SHL, SHR, ADDI, LI, LD, ST, BEQ, BNE, BLT, JAL, SYS };
#define RRR(op, rd, rs, rt) ((u16)((op) << 12 | (rd) << 9 | (rs) << 6 | (rt) << 3))
#define RRI(op, rd, rs, imm) ((u16)((op) << 12 | (rd) << 9 | (rs) << 6 | ((imm) & 0x3f)))
#define RI9(op, rd, imm) ((u16)((op) << 12 | (rd) << 9 | ((imm) & 0x1ff)))
#define BR(op, a, b, target, here) RRI(op, a, b, (target) - ((here) + 1))   /* pc-relative branch */
#define JMP(rd, target, here) RI9(JAL, rd, (target) - ((here) + 1))          /* jump and link */
#define JR(rs) RRI(SYS, 0, rs, 0)
#define HALT RRI(SYS, 0, 0, 1)
#define MEM 4096
#define RUNS 5
enum { XS = 0x200, YS = 0x240, SS = 0x280, KS = 0x2C0, RES = 0x100, NX = 40, NSORT = 56, NK = 24, MUL = 79 };
static u16 mem[MEM];

/* Runs from pc 0 until HALT. Returns the number of instructions executed, or -1 after max_steps. */
RT_IF_CHAIN static int cpu_run(u16 *m, int max_steps) {
    u16 r[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    u32 pc = 0;
    int steps = 0;
    for (;;) {
        if (steps >= max_steps) return -1;
        steps++;
        u32 ins = m[pc & (MEM - 1)];
        pc++;
        u32 op = ins >> 12, rd = ins >> 9 & 7, rs = ins >> 6 & 7, rt = ins >> 3 & 7;
        u32 imm6 = ((ins & 0x3f) ^ 0x20) - 0x20;
        if (op == ADD) r[rd] = (u16)(r[rs] + r[rt]);
        else if (op == SUB) r[rd] = (u16)(r[rs] - r[rt]);
        else if (op == AND) r[rd] = r[rs] & r[rt];
        else if (op == OR) r[rd] = r[rs] | r[rt];
        else if (op == XOR) r[rd] = r[rs] ^ r[rt];
        else if (op == SHL) r[rd] = (u16)(r[rs] << (ins & 15));
        else if (op == SHR) r[rd] = (u16)(r[rs] >> (ins & 15));
        else if (op == ADDI) r[rd] = (u16)(r[rs] + imm6);
        else if (op == LI) r[rd] = (u16)(((ins & 0x1ff) ^ 0x100) - 0x100);
        else if (op == LD) r[rd] = m[(r[rs] + imm6) & (MEM - 1)];
        else if (op == ST) m[(r[rs] + imm6) & (MEM - 1)] = r[rd];
        else if (op == BEQ) { if (r[rd] == r[rs]) pc += imm6; }
        else if (op == BNE) { if (r[rd] != r[rs]) pc += imm6; }
        else if (op == BLT) { if ((s16)r[rd] < (s16)r[rs]) pc += imm6; }
        else if (op == JAL) { r[rd] = (u16)pc; pc += ((ins & 0x1ff) ^ 0x100) - 0x100; }
        else if ((ins & 0x3f) == 1) return steps;               /* SYS 1: halt */
        else pc = r[rs];                                        /* SYS 0: jump to register */
        r[0] = 0;
    }
}

static const u16 rom[] = {
    /* ---- 1: RES[0] = sum of X[i] * Y[i], i < 40 (16-bit wrap), multiply by subroutine ---- */
    /*  0 */ RI9(LI, 1, 64),
    /*  1 */ RRI(SHL, 1, 1, 3),             /* r1 = &X[0] = 0x200 */
    /*  2 */ RI9(LI, 4, 0),                 /* r4 = sum */
    /*  3 */ RRI(LD, 5, 1, 0),              /* loop: r5 = X[i] */
    /*  4 */ RI9(LI, 6, 64),
    /*  5 */ RRR(ADD, 6, 6, 1),
    /*  6 */ RRI(LD, 6, 6, 0),              /* r6 = Y[i] */
    /*  7 */ JMP(7, MUL, 7),                /* r5 = r5 * r6 */
    /*  8 */ RRR(ADD, 4, 4, 5),
    /*  9 */ RRI(ADDI, 1, 1, 1),
    /* 10 */ RI9(LI, 5, 69),
    /* 11 */ RRI(SHL, 5, 5, 3),             /* r5 = &X[40] */
    /* 12 */ BR(BNE, 1, 5, 3, 12),
    /* 13 */ RI9(LI, 6, 64),
    /* 14 */ RRI(SHL, 6, 6, 2),             /* r6 = RES */
    /* 15 */ RRI(ST, 4, 6, 0),
    /* ---- 2: insertion sort of S[0..56), signed ---- */
    /* 16 */ RI9(LI, 5, 80),
    /* 17 */ RRI(SHL, 5, 5, 3),             /* r5 = &S[0] = 0x280 */
    /* 18 */ RI9(LI, 4, 87),
    /* 19 */ RRI(SHL, 4, 4, 3),             /* r4 = &S[56] */
    /* 20 */ RRI(ADDI, 1, 5, 1),            /* r1 = &S[1] */
    /* 21 */ RRI(LD, 2, 1, 0),              /* outer: v = *r1 */
    /* 22 */ RRI(ADDI, 3, 1, -1),           /* p = r1 - 1 */
    /* 23 */ BR(BLT, 3, 5, 30, 23),         /* inner: if (p < &S[0]) goto place */
    /* 24 */ RRI(LD, 6, 3, 0),              /* t = *p */
    /* 25 */ BR(BLT, 2, 6, 27, 25),         /* if (v < t) goto shift */
    /* 26 */ JMP(0, 30, 26),                /* goto place */
    /* 27 */ RRI(ST, 6, 3, 1),              /* shift: p[1] = t */
    /* 28 */ RRI(ADDI, 3, 3, -1),           /* p-- */
    /* 29 */ JMP(0, 23, 29),
    /* 30 */ RRI(ST, 2, 3, 1),              /* place: p[1] = v */
    /* 31 */ RRI(ADDI, 1, 1, 1),
    /* 32 */ BR(BNE, 1, 4, 21, 32),
    /* ---- 3: RES[1] = how many of the 24 keys K[] are in the sorted S[] (binary search) ---- */
    /* 33 */ RI9(LI, 1, 88),
    /* 34 */ RRI(SHL, 1, 1, 3),             /* r1 = &K[0] = 0x2C0 */
    /* 35 */ RI9(LI, 7, 0),                 /* r7 = hits */
    /* 36 */ RRI(LD, 2, 1, 0),              /* next: key */
    /* 37 */ RI9(LI, 3, 0),                 /* lo = 0 */
    /* 38 */ RI9(LI, 4, 55),                /* hi = 55 */
    /* 39 */ BR(BLT, 4, 3, 53, 39),         /* loop: if (hi < lo) goto miss */
    /* 40 */ RRR(ADD, 5, 3, 4),
    /* 41 */ RRI(SHR, 5, 5, 1),             /* mid */
    /* 42 */ RI9(LI, 6, 80),
    /* 43 */ RRI(SHL, 6, 6, 3),
    /* 44 */ RRR(ADD, 6, 6, 5),
    /* 45 */ RRI(LD, 6, 6, 0),              /* r6 = S[mid] */
    /* 46 */ BR(BEQ, 6, 2, 52, 46),         /* found */
    /* 47 */ BR(BLT, 6, 2, 50, 47),         /* S[mid] < key: go right */
    /* 48 */ RRI(ADDI, 4, 5, -1),           /* hi = mid - 1 */
    /* 49 */ JMP(0, 39, 49),
    /* 50 */ RRI(ADDI, 3, 5, 1),            /* lo = mid + 1 */
    /* 51 */ JMP(0, 39, 51),
    /* 52 */ RRI(ADDI, 7, 7, 1),            /* hit: hits++ */
    /* 53 */ RRI(ADDI, 1, 1, 1),            /* miss: next key */
    /* 54 */ RI9(LI, 5, 91),
    /* 55 */ RRI(SHL, 5, 5, 3),             /* r5 = &K[24] */
    /* 56 */ BR(BNE, 1, 5, 36, 56),
    /* 57 */ RI9(LI, 6, 64),
    /* 58 */ RRI(SHL, 6, 6, 2),
    /* 59 */ RRI(ST, 7, 6, 1),
    /* ---- 4: RES[2] = number of 1 bits in X[0..40) ---- */
    /* 60 */ RI9(LI, 1, 64),
    /* 61 */ RRI(SHL, 1, 1, 3),
    /* 62 */ RI9(LI, 4, 0),                 /* r4 = count */
    /* 63 */ RRI(LD, 2, 1, 0),              /* word: w = X[i] */
    /* 64 */ BR(BEQ, 2, 0, 71, 64),         /* bit: if (w == 0) goto next */
    /* 65 */ RI9(LI, 3, 1),
    /* 66 */ RRR(AND, 3, 2, 3),
    /* 67 */ BR(BEQ, 3, 0, 69, 67),
    /* 68 */ RRI(ADDI, 4, 4, 1),
    /* 69 */ RRI(SHR, 2, 2, 1),
    /* 70 */ JMP(0, 64, 70),
    /* 71 */ RRI(ADDI, 1, 1, 1),            /* next */
    /* 72 */ RI9(LI, 5, 69),
    /* 73 */ RRI(SHL, 5, 5, 3),
    /* 74 */ BR(BNE, 1, 5, 63, 74),
    /* 75 */ RI9(LI, 6, 64),
    /* 76 */ RRI(SHL, 6, 6, 2),
    /* 77 */ RRI(ST, 4, 6, 2),
    /* 78 */ HALT,
    /* ---- subroutine: r5 = r5 * r6 by shift and add; uses r2, r3; returns through r7 ---- */
    /* 79 */ RI9(LI, 2, 0),
    /* 80 */ BR(BEQ, 6, 0, 88, 80),         /* while (r6 != 0) */
    /* 81 */ RI9(LI, 3, 1),
    /* 82 */ RRR(AND, 3, 6, 3),
    /* 83 */ BR(BEQ, 3, 0, 85, 83),         /*   if (r6 & 1) */
    /* 84 */ RRR(ADD, 2, 2, 5),             /*     r2 += r5 */
    /* 85 */ RRI(SHL, 5, 5, 1),
    /* 86 */ RRI(SHR, 6, 6, 1),
    /* 87 */ JMP(0, 80, 87),
    /* 88 */ RRR(ADD, 5, 2, 0),
    /* 89 */ JR(7)};
#define ROM_WORDS ((int)(sizeof rom / sizeof rom[0]))

int main(void) {
    u32 chk = 0;
    s16 sorted[NSORT];
    rt_srand(3001);
    for (int run = 0; run < RUNS; run++) {
        u32 dot = 0;
        int hits = 0, bits = 0;
        for (int i = 0; i < MEM; i++) mem[i] = 0;
        for (int i = 0; i < ROM_WORDS; i++) mem[i] = rom[i];
        for (int i = 0; i < NX; i++) {
            mem[XS + i] = (u16)rt_rand();
            u32 y = rt_rand() >> 16;
            mem[YS + i] = (u16)(y >> rt_rand_n(12));                      /* multipliers of all sizes */
            dot += (u32)mem[XS + i] * mem[YS + i];
            for (u32 w = mem[XS + i]; w; w >>= 1) bits += (int)(w & 1);
        }
        for (int i = 0; i < NSORT; i++) {                                 /* reference: sort in C */
            s16 v = (s16)rt_rand();
            int j = i - 1;
            mem[SS + i] = (u16)v;
            while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
            sorted[j + 1] = v;
        }
        for (int i = 0; i < NK; i++) {
            u16 key = rt_rand() & 1 ? mem[SS + rt_rand_n(NSORT)] : (u16)rt_rand();
            int present = 0;
            mem[KS + i] = key;
            for (int j = 0; j < NSORT; j++)
                if (mem[SS + j] == key) present = 1;
            hits += present;
        }
        int steps = cpu_run(mem, 200000);
        if (steps < 0) return 1;
        if (mem[RES] != (u16)dot) return 2;
        for (int i = 0; i < NSORT; i++)
            if (mem[SS + i] != (u16)sorted[i]) return 3;
        if (mem[RES + 1] != hits) return 4;
        if (mem[RES + 2] != bits) return 5;
        chk = chk * 31 + (u32)steps + mem[RES];
    }
    return rt_check(chk, 0x2743ea);
}

#elif SEL == 4
#define JMAX 14000
#define DOCS 6
#define STACK 32
static u8 doc[JMAX + 8192];             /* room to finish the containers that are open at JMAX */
static int dlen;
typedef struct { int objects, arrays, strings, numbers, trues, falses, nulls, depth; u32 intsum; } tally;
static tally gen, got;

/* ---- generator ---- */
static void put(u8 c) { doc[dlen++] = c; }
static void put_ws(void) {
    u32 r = rt_rand_n(9);
    if (r == 0) put(' ');
    else if (r == 1) put('\n');
    else if (r == 2) { put(' '); put(' '); }
}
static void gen_string(void) {
    static const char hex[] = "0123456789abcdefABCDEF";
    int len = (int)rt_rand_n(12);
    put('"');
    for (int i = 0; i < len; i++) {
        u32 r = rt_rand_n(40);
        if (r == 0) { put('\\'); put('"'); }
        else if (r == 1) { put('\\'); put('\\'); }
        else if (r == 2) { put('\\'); put('n'); }
        else if (r == 3) {
            put('\\');
            put('u');
            for (int k = 0; k < 4; k++) put((u8)hex[rt_rand_n(22)]);
        } else if (r < 8) put(' ');
        else put((u8)('a' + r - 8 < 'z' ? 'a' + r - 8 : 'z'));
    }
    put('"');
    gen.strings++;
}
static void gen_digits(u32 v) {
    char d[12];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) put((u8)d[--n]);
}
static void gen_number(void) {
    u32 kind = rt_rand_n(6), v = rt_rand_n(4) ? rt_rand_n(100) : rt_rand_n(1000000);
    int neg = (int)(rt_rand_n(4) == 0);
    if (neg) put('-');
    gen_digits(v);
    if (kind < 4) gen.intsum += neg ? 0u - v : v;                   /* a plain integer */
    if (kind >= 4) { put('.'); gen_digits(rt_rand_n(1000)); }
    if (kind == 5) {
        put(rt_rand() & 1 ? 'e' : 'E');
        if (rt_rand_n(3) == 0) put(rt_rand() & 1 ? '+' : '-');
        gen_digits(rt_rand_n(30));
    }
    gen.numbers++;
}
static void gen_value(int depth) {
    u32 r = rt_rand_n(100);
    if (depth >= 6 || dlen > JMAX) r = 34 + r * 66 / 100;           /* deep or long enough: no containers */
    if (r < 34) {
        int n = (int)rt_rand_n(r < 18 ? 6 : 8), object = r < 18;
        if (depth + 1 > gen.depth) gen.depth = depth + 1;
        put(object ? '{' : '[');
        for (int i = 0; i < n; i++) {
            put_ws();
            if (object) {
                gen_string();
                put_ws();
                put(':');
                put_ws();
            }
            gen_value(depth + 1);
            put_ws();
            if (i + 1 < n) put(',');
        }
        put_ws();
        put(object ? '}' : ']');
        if (object) gen.objects++;
        else gen.arrays++;
    } else if (r < 55) gen_string();
    else if (r < 85) gen_number();
    else if (r < 91) { put('t'); put('r'); put('u'); put('e'); gen.trues++; }
    else if (r < 96) { put('f'); put('a'); put('l'); put('s'); put('e'); gen.falses++; }
    else { put('n'); put('u'); put('l'); put('l'); gen.nulls++; }
}

/* ---- parser: one character per step ---- */
enum { S_VALUE, S_FIRST, S_AFTER, S_KEY_OR_END, S_KEY, S_COLON, S_STR, S_ESC, S_HEX, S_MINUS, S_INT,
       S_FRAC0, S_FRAC, S_EXP0, S_EXP1, S_EXP, S_LIT };
static int is_ws(u8 c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }
static int is_digit(u8 c) { return c >= '0' && c <= '9'; }

static int parse(const u8 *s) {         /* 0 = well-formed; otherwise an error code */
    u8 stack[STACK];
    const char *lit = "";
    int sp = 0, state = S_VALUE, i = 0, is_key = 0, hex_left = 0, neg = 0, plain_int = 0;
    u32 acc = 0;
    for (;;) {
        u8 c = s[i];
        switch (state) {
        case S_FIRST:                   /* just after '[': a value or ']' */
            if (is_ws(c)) break;
            if (c == ']') { sp--; got.arrays++; state = S_AFTER; break; }
            state = S_VALUE;
            /* fall through */
        case S_VALUE:
            if (is_ws(c)) break;
            if (c == '{' || c == '[') {
                if (sp >= STACK) return 1;
                stack[sp++] = c;
                if (sp > got.depth) got.depth = sp;
                state = c == '{' ? S_KEY_OR_END : S_FIRST;
            } else if (c == '"') { is_key = 0; state = S_STR; }
            else if (c == '-') { neg = 1; state = S_MINUS; }
            else if (is_digit(c)) { neg = 0; acc = (u32)(c - '0'); plain_int = 1; state = S_INT; }
            else if (c == 't') { lit = "rue"; got.trues++; state = S_LIT; }
            else if (c == 'f') { lit = "alse"; got.falses++; state = S_LIT; }
            else if (c == 'n') { lit = "ull"; got.nulls++; state = S_LIT; }
            else return 2;
            break;
        case S_AFTER:                   /* after a value: ',' or the end of the container */
            if (is_ws(c)) break;
            if (sp == 0) return c == 0 ? 0 : 3;
            if (c == ',') state = stack[sp - 1] == '{' ? S_KEY : S_VALUE;
            else if (c == '}' && stack[sp - 1] == '{') { sp--; got.objects++; }
            else if (c == ']' && stack[sp - 1] == '[') { sp--; got.arrays++; }
            else return 4;
            break;
        case S_KEY_OR_END:
            if (is_ws(c)) break;
            if (c == '}') { sp--; got.objects++; state = S_AFTER; break; }
            /* fall through */
        case S_KEY:
            if (is_ws(c)) break;
            if (c != '"') return 5;
            is_key = 1;
            state = S_STR;
            break;
        case S_COLON:
            if (is_ws(c)) break;
            if (c != ':') return 6;
            state = S_VALUE;
            break;
        case S_STR:
            if (c == '"') { got.strings++; state = is_key ? S_COLON : S_AFTER; }
            else if (c == '\\') state = S_ESC;
            else if (c < 0x20) return 7;
            break;
        case S_ESC:
            if (c == 'u') { hex_left = 4; state = S_HEX; }
            else if (c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't') state = S_STR;
            else return 8;
            break;
        case S_HEX:
            if (!(is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 9;
            if (--hex_left == 0) state = S_STR;
            break;
        case S_MINUS:
            if (!is_digit(c)) return 10;
            acc = (u32)(c - '0');
            plain_int = 1;
            state = S_INT;
            break;
        case S_INT:
            if (is_digit(c)) { acc = acc * 10 + (u32)(c - '0'); break; }
            if (c == '.') { plain_int = 0; state = S_FRAC0; break; }
            if (c == 'e' || c == 'E') { plain_int = 0; state = S_EXP0; break; }
            goto number_done;
        case S_FRAC0:
            if (!is_digit(c)) return 11;
            state = S_FRAC;
            break;
        case S_FRAC:
            if (is_digit(c)) break;
            if (c == 'e' || c == 'E') { state = S_EXP0; break; }
            goto number_done;
        case S_EXP0:
            if (c == '+' || c == '-') { state = S_EXP1; break; }
            /* fall through */
        case S_EXP1:
            if (!is_digit(c)) return 12;
            state = S_EXP;
            break;
        case S_EXP:
            if (is_digit(c)) break;
        number_done:                    /* this character is not part of the number: look at it again */
            got.numbers++;
            if (plain_int) got.intsum += neg ? 0u - acc : acc;
            state = S_AFTER;
            continue;
        case S_LIT:
            if (c != (u8)*lit) return 13;
            if (*++lit == 0) state = S_AFTER;
            break;
        default:
            return 14;
        }
        if (c == 0) return 15;          /* text ended inside something */
        i++;
    }
}

int main(void) {
    static const tally zero;
    u32 chk = 0;
    for (int d = 0; d < DOCS; d++) {
        rt_srand(3101 + (u32)d);
        gen = got = zero;
        dlen = 0;
        put('[');                       /* the document: one array of values */
        gen.depth = 1;
        for (int first = 1; dlen < JMAX; first = 0) {
            if (!first) put(',');
            put_ws();
            gen_value(1);
            put_ws();
        }
        put(']');
        gen.arrays++;
        doc[dlen] = 0;
        int e = parse(doc);
        if (e) return e;
        if (got.objects != gen.objects || got.arrays != gen.arrays) return 16;
        if (got.strings != gen.strings || got.numbers != gen.numbers) return 17;
        if (got.trues != gen.trues || got.falses != gen.falses || got.nulls != gen.nulls) return 18;
        if (got.depth != gen.depth) return 19;
        if (got.intsum != gen.intsum) return 20;
        chk = chk * 31 + (u32)(got.objects + 3 * got.arrays + 5 * got.strings + 7 * got.numbers) + got.intsum + (u32)dlen;
    }
    return rt_check(chk, 0x211c48);
}

#else
#error "sys.c: build with -DSEL=1..4"
#endif
