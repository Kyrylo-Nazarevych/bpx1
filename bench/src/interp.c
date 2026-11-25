/* BPX-1 bench: interpreters. Select one with -DSEL=<n>:
     1 interp.vm     register bytecode VM whose dispatcher is an if/else chain (kept as conditional
                     branches: RT_IF_CHAIN, no jump table), running recursive Fibonacci, a sieve,
                     Collatz step counting and bubble sort
     2 interp.expr   generated arithmetic expressions (+ - * / % comparisons, unary minus,
                     parentheses, variables) evaluated by a recursive-descent parser and by an
                     operator-precedence stack evaluator
     3 interp.regex  backtracking regular-expression matcher (classes, \d \w, * + ?, ^ $, |) over
                     generated log-like lines

   Self-checks (exit codes 1..19): the VM results equal the same computations done directly in C;
   both evaluators return the value the generator computed while writing the expression; regex
   results agree with hand-written predicates and with what the line templates guarantee. */
#include "rt.h"

#if SEL == 1
enum { HALT, LI, MOV, ADD, SUB, MUL, AND, SHR, ADDI, LD, ST, BEQ, BNE, BLT, BGE, JMP, CALL, RET, PUSH, POP };
/* Instruction word: op | a << 8 | b << 16 | c << 24. Branches and jumps hold absolute targets. */
#define R3(op, a, b, c) ((u32)(op) | (u32)(a) << 8 | (u32)((b) & 0xff) << 16 | (u32)((c) & 0xff) << 24)
#define RI(op, a, imm)  ((u32)(op) | (u32)(a) << 8 | (u32)((imm) & 0xffff) << 16)
#define MEMSZ 1024
#define STK 256
static int vmem[MEMSZ], stk[STK];

/* Runs until HALT. Returns the number of instructions executed, or a negative error. */
RT_IF_CHAIN static int vm_run(const u32 *code, int ncode, int *r) {
    int pc = 0, sp = 0, steps = 0;
    for (;;) {
        if ((u32)pc >= (u32)ncode) return -1;
        u32 ins = code[pc++];
        u32 op = ins & 0xff, a = ins >> 8 & 15, b = ins >> 16 & 15, c = ins >> 24;
        steps++;
        if (op == LI) r[a] = (s16)(ins >> 16);
        else if (op == MOV) r[a] = r[b];
        else if (op == ADD) r[a] = (int)((u32)r[b] + (u32)r[c & 15]);
        else if (op == SUB) r[a] = (int)((u32)r[b] - (u32)r[c & 15]);
        else if (op == MUL) r[a] = (int)((u32)r[b] * (u32)r[c & 15]);
        else if (op == AND) r[a] = r[b] & r[c & 15];
        else if (op == SHR) r[a] = (int)((u32)r[b] >> (c & 31));
        else if (op == ADDI) r[a] = (int)((u32)r[b] + (u32)(s8)c);
        else if (op == LD) {
            u32 ad = (u32)r[b] + (u32)r[c & 15];
            if (ad >= MEMSZ) return -2;
            r[a] = vmem[ad];
        } else if (op == ST) {
            u32 ad = (u32)r[b] + (u32)r[c & 15];
            if (ad >= MEMSZ) return -2;
            vmem[ad] = r[a];
        } else if (op == BEQ) { if (r[a] == r[b]) pc = (int)c; }
        else if (op == BNE) { if (r[a] != r[b]) pc = (int)c; }
        else if (op == BLT) { if (r[a] < r[b]) pc = (int)c; }
        else if (op == BGE) { if (r[a] >= r[b]) pc = (int)c; }
        else if (op == JMP) pc = (int)(ins >> 16);
        else if (op == CALL) {
            if (sp >= STK) return -3;
            stk[sp++] = pc;
            pc = (int)(ins >> 16);
        } else if (op == RET) {
            if (sp == 0) return -4;
            pc = stk[--sp];
        } else if (op == PUSH) {
            if (sp >= STK) return -3;
            stk[sp++] = r[a];
        } else if (op == POP) {
            if (sp == 0) return -4;
            r[a] = stk[--sp];
        } else if (op == HALT) return steps;
        else return -5;
    }
}

/* r1 = n  ->  r2 = fib(n), recursively */
static const u32 prog_fib[] = {
    /*  0 */ RI(CALL, 0, 2),
    /*  1 */ R3(HALT, 0, 0, 0),
    /*  2 */ RI(LI, 3, 2),              /* fib: */
    /*  3 */ R3(BLT, 1, 3, 14),         /*   if (n < 2) goto base */
    /*  4 */ R3(PUSH, 1, 0, 0),
    /*  5 */ R3(ADDI, 1, 1, -1),
    /*  6 */ RI(CALL, 0, 2),            /*   r2 = fib(n - 1) */
    /*  7 */ R3(POP, 1, 0, 0),
    /*  8 */ R3(PUSH, 2, 0, 0),
    /*  9 */ R3(ADDI, 1, 1, -2),
    /* 10 */ RI(CALL, 0, 2),            /*   r2 = fib(n - 2) */
    /* 11 */ R3(POP, 3, 0, 0),
    /* 12 */ R3(ADD, 2, 2, 3),
    /* 13 */ R3(RET, 0, 0, 0),
    /* 14 */ R3(MOV, 2, 1, 0),          /* base: fib(n) = n */
    /* 15 */ R3(RET, 0, 0, 0)};
/* r1 = N  ->  r4 = number of primes below N (sieve in vmem[0..N)) */
static const u32 prog_sieve[] = {
    /*  0 */ RI(LI, 0, 0),
    /*  1 */ RI(LI, 6, 1),
    /*  2 */ RI(LI, 2, 0),
    /*  3 */ R3(BGE, 2, 1, 7),          /* clear: while (i < N) */
    /*  4 */ R3(ST, 0, 2, 0),           /*   mem[i] = 0 */
    /*  5 */ R3(ADDI, 2, 2, 1),
    /*  6 */ RI(JMP, 0, 3),
    /*  7 */ RI(LI, 2, 2),              /* i = 2 */
    /*  8 */ RI(LI, 4, 0),              /* count = 0 */
    /*  9 */ R3(BGE, 2, 1, 20),         /* outer: while (i < N) */
    /* 10 */ R3(LD, 5, 2, 0),
    /* 11 */ R3(BNE, 5, 0, 18),         /*   if (mem[i]) goto next */
    /* 12 */ R3(ADDI, 4, 4, 1),         /*   count++ */
    /* 13 */ R3(MUL, 3, 2, 2),          /*   j = i * i */
    /* 14 */ R3(BGE, 3, 1, 18),         /*   inner: while (j < N) */
    /* 15 */ R3(ST, 6, 3, 0),           /*     mem[j] = 1 */
    /* 16 */ R3(ADD, 3, 3, 2),          /*     j += i */
    /* 17 */ RI(JMP, 0, 14),
    /* 18 */ R3(ADDI, 2, 2, 1),         /* next: i++ */
    /* 19 */ RI(JMP, 0, 9),
    /* 20 */ R3(HALT, 0, 0, 0)};
/* r1 = N  ->  r4 = total number of Collatz steps of 1..N */
static const u32 prog_collatz[] = {
    /*  0 */ RI(LI, 0, 0),
    /*  1 */ RI(LI, 6, 1),
    /*  2 */ RI(LI, 7, 3),
    /*  3 */ RI(LI, 4, 0),
    /*  4 */ RI(LI, 2, 1),              /* n = 1 */
    /*  5 */ R3(BLT, 1, 2, 18),         /* outer: while (n <= N) */
    /*  6 */ R3(MOV, 3, 2, 0),          /*   x = n */
    /*  7 */ R3(BEQ, 3, 6, 16),         /*   while (x != 1) */
    /*  8 */ R3(AND, 5, 3, 6),
    /*  9 */ R3(BEQ, 5, 0, 13),         /*     if (x & 1) */
    /* 10 */ R3(MUL, 3, 3, 7),          /*       x = 3 * x + 1 */
    /* 11 */ R3(ADDI, 3, 3, 1),
    /* 12 */ RI(JMP, 0, 14),
    /* 13 */ R3(SHR, 3, 3, 1),          /*     else x >>= 1 */
    /* 14 */ R3(ADDI, 4, 4, 1),         /*     steps++ */
    /* 15 */ RI(JMP, 0, 7),
    /* 16 */ R3(ADDI, 2, 2, 1),         /*   n++ */
    /* 17 */ RI(JMP, 0, 5),
    /* 18 */ R3(HALT, 0, 0, 0)};
/* r1 = n: bubble sort of vmem[0..n), stops when a pass swaps nothing */
static const u32 prog_sort[] = {
    /*  0 */ RI(LI, 0, 0),
    /*  1 */ RI(LI, 6, 1),
    /*  2 */ R3(ADDI, 2, 1, -1),        /* end = n - 1 */
    /*  3 */ R3(BGE, 0, 2, 18),         /* outer: while (end > 0) */
    /*  4 */ RI(LI, 7, 0),              /*   swapped = 0 */
    /*  5 */ RI(LI, 3, 0),              /*   j = 0 */
    /*  6 */ R3(BGE, 3, 2, 15),         /*   inner: while (j < end) */
    /*  7 */ R3(LD, 4, 3, 0),           /*     a = mem[j] */
    /*  8 */ R3(LD, 5, 3, 6),           /*     b = mem[j + 1] */
    /*  9 */ R3(BGE, 5, 4, 13),         /*     if (b < a) swap */
    /* 10 */ R3(ST, 5, 3, 0),
    /* 11 */ R3(ST, 4, 3, 6),
    /* 12 */ RI(LI, 7, 1),
    /* 13 */ R3(ADDI, 3, 3, 1),         /*     j++ */
    /* 14 */ RI(JMP, 0, 6),
    /* 15 */ R3(BEQ, 7, 0, 18),         /*   if (!swapped) break */
    /* 16 */ R3(ADDI, 2, 2, -1),        /*   end-- */
    /* 17 */ RI(JMP, 0, 3),
    /* 18 */ R3(HALT, 0, 0, 0)};
#define COUNT(p) ((int)(sizeof(p) / sizeof(p)[0]))

int main(void) {
    static const int fib_n[2] = {15, 12};
    static const int sieve_n[2] = {700, 400}, sieve_primes[2] = {125, 78};
    static const int sort_n[2] = {40, 24};
    u32 chk = 0;
    int r[16], steps;
    rt_srand(801);
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 16; i++) r[i] = 0;
        r[1] = fib_n[k];
        steps = vm_run(prog_fib, COUNT(prog_fib), r);
        if (steps < 0) return 1;
        int f0 = 0, f1 = 1;
        for (int i = 0; i < fib_n[k]; i++) { int t = f0 + f1; f0 = f1; f1 = t; }
        if (r[2] != f0) return 2;
        chk = chk * 31 + (u32)steps;

        for (int i = 0; i < 16; i++) r[i] = 0;
        r[1] = sieve_n[k];
        steps = vm_run(prog_sieve, COUNT(prog_sieve), r);
        if (steps < 0) return 3;
        if (r[4] != sieve_primes[k]) return 4;
        chk = chk * 31 + (u32)steps;

        for (int i = 0; i < 16; i++) r[i] = 0;
        r[1] = 50 + 30 * k;
        steps = vm_run(prog_collatz, COUNT(prog_collatz), r);
        if (steps < 0) return 5;
        int total = 0;
        for (u32 n = 1; n <= (u32)(50 + 30 * k); n++)
            for (u32 x = n; x != 1; total++) x = x & 1 ? 3 * x + 1 : x >> 1;
        if (r[4] != total) return 6;
        chk = chk * 31 + (u32)steps;

        int n = sort_n[k], sum = 0;
        for (int i = 0; i < n; i++) { vmem[i] = (int)rt_rand_n(1000) - 500; sum += vmem[i]; }
        for (int i = 0; i < 16; i++) r[i] = 0;
        r[1] = n;
        steps = vm_run(prog_sort, COUNT(prog_sort), r);
        if (steps < 0) return 7;
        for (int i = 0; i < n; i++) sum -= vmem[i];
        if (sum != 0) return 8;
        for (int i = 1; i < n; i++)
            if (vmem[i - 1] > vmem[i]) return 9;
        chk = chk * 31 + (u32)steps + (u32)vmem[n / 2];
    }
    return rt_check(chk, 0x0f7a01);
}

#elif SEL == 2
#define EMAX 3000
#define NEXPR 80
#define DEPTH 4
static u8 ebuf[EMAX + 8];
static int elen, eover;
static int vars[8];

static int apply(u8 op, int a, int b) {     /* 32-bit wrap-around; x / 0 and x % 0 are 0 */
    if (op == '+') return (int)((u32)a + (u32)b);
    if (op == '-') return (int)((u32)a - (u32)b);
    if (op == '*') return (int)((u32)a * (u32)b);
    if (op == '/') {
        if (b == 0) return 0;
        if (b == -1) return (int)(0u - (u32)a);
        return a / b;
    }
    if (op == '%') {
        if (b == 0 || b == -1) return 0;
        return a % b;
    }
    if (op == '<') return a < b;
    if (op == '>') return a > b;
    if (op == '=') return a == b;
    return a != b;                          /* '!' */
}

/* ---- generator: writes the text and computes the value on the way ---- */
static void emit(u8 c) {
    if (elen < EMAX) ebuf[elen++] = c;
    else eover = 1;
}
static void maybe_space(void) {
    if (rt_rand_n(5) == 0) emit(' ');
}
static int gen_rel(int depth);
static int gen_factor(int depth) {
    u32 r = rt_rand_n(10);
    maybe_space();
    if (depth <= 0 || r < 4) {
        u32 v = rt_rand_n(8) ? rt_rand_n(100) : rt_rand_n(100000), t = v;
        char d[8];
        int nd = 0;
        do { d[nd++] = (char)('0' + t % 10); t /= 10; } while (t);
        while (nd) emit((u8)d[--nd]);
        return (int)v;
    }
    if (r < 6) {
        u32 i = rt_rand_n(8);
        emit((u8)('a' + i));
        return vars[i];
    }
    if (r < 9) {
        emit('(');
        int v = gen_rel(depth - 1);
        maybe_space();
        emit(')');
        return v;
    }
    emit('-');
    return (int)(0u - (u32)gen_factor(depth - 1));
}
static int gen_term(int depth) {
    int v = gen_factor(depth);
    for (u32 n = rt_rand_n(3); n > 0; n--) {
        u8 op = (u8)"**/%"[rt_rand_n(4)];
        maybe_space();
        emit(op);
        v = apply(op, v, gen_factor(depth));
    }
    return v;
}
static int gen_sum(int depth) {
    int v = gen_term(depth);
    for (u32 n = rt_rand_n(4); n > 0; n--) {
        u8 op = rt_rand() & 1 ? '+' : '-';
        maybe_space();
        emit(op);
        v = apply(op, v, gen_term(depth));
    }
    return v;
}
static int gen_rel(int depth) {
    int v = gen_sum(depth);
    if (rt_rand_n(4) == 0) {
        u8 op = (u8)"<>=!"[rt_rand_n(4)];
        maybe_space();
        emit(op);
        v = apply(op, v, gen_sum(depth));
    }
    return v;
}

/* ---- evaluator 1: recursive descent ---- */
static const u8 *pp;
static int perr;
static void skip_ws(void) {
    while (*pp == ' ') pp++;
}
static int parse_rel(void);
static int parse_factor(void) {
    skip_ws();
    u8 c = *pp;
    if (c >= '0' && c <= '9') {
        u32 v = 0;
        do { v = v * 10 + (u32)(*pp++ - '0'); } while (*pp >= '0' && *pp <= '9');
        return (int)v;
    }
    if (c >= 'a' && c <= 'h') { pp++; return vars[c - 'a']; }
    if (c == '(') {
        pp++;
        int v = parse_rel();
        skip_ws();
        if (*pp != ')') { perr = 1; return 0; }
        pp++;
        return v;
    }
    if (c == '-') { pp++; return (int)(0u - (u32)parse_factor()); }
    perr = 1;
    return 0;
}
static int parse_term(void) {
    int v = parse_factor();
    for (;;) {
        skip_ws();
        u8 c = *pp;
        if (c != '*' && c != '/' && c != '%') return v;
        pp++;
        int w = parse_factor();
        v = apply(c, v, w);
    }
}
static int parse_sum(void) {
    int v = parse_term();
    for (;;) {
        skip_ws();
        u8 c = *pp;
        if (c != '+' && c != '-') return v;
        pp++;
        int w = parse_term();
        v = apply(c, v, w);
    }
}
static int parse_rel(void) {
    int v = parse_sum();
    skip_ws();
    u8 c = *pp;
    if (c == '<' || c == '>' || c == '=' || c == '!') {
        pp++;
        int w = parse_sum();
        v = apply(c, v, w);
    }
    return v;
}

/* ---- evaluator 2: value stack and operator stack ('u' = unary minus) ---- */
#define SMAX 128
static int vals[SMAX];
static u8 ops[SMAX];
static int nv, no;
static int prec(u8 op) {
    if (op == 'u') return 4;
    if (op == '*' || op == '/' || op == '%') return 3;
    if (op == '+' || op == '-') return 2;
    if (op == '(') return 0;
    return 1;
}
static void reduce(void) {
    u8 op = ops[--no];
    if (op == 'u') vals[nv - 1] = (int)(0u - (u32)vals[nv - 1]);
    else {
        int b = vals[--nv];
        vals[nv - 1] = apply(op, vals[nv - 1], b);
    }
}
static int eval_stack(const u8 *s, int *err) {
    int want_operand = 1;
    nv = no = 0;
    for (;;) {
        u8 c = *s;
        if (c == ' ') { s++; continue; }
        if (nv >= SMAX - 1 || no >= SMAX - 1) { *err = 1; return 0; }
        if (want_operand) {
            if (c >= '0' && c <= '9') {
                u32 v = 0;
                do { v = v * 10 + (u32)(*s++ - '0'); } while (*s >= '0' && *s <= '9');
                vals[nv++] = (int)v;
                want_operand = 0;
            } else if (c >= 'a' && c <= 'h') {
                vals[nv++] = vars[c - 'a'];
                s++;
                want_operand = 0;
            } else if (c == '(') { ops[no++] = '('; s++; }
            else if (c == '-') { ops[no++] = 'u'; s++; }
            else { *err = 1; return 0; }
        } else if (c == 0 || c == ')') {
            while (no > 0 && ops[no - 1] != '(') reduce();
            if (c == 0) {
                if (no != 0 || nv != 1) *err = 1;
                return vals[0];
            }
            if (no == 0) { *err = 1; return 0; }
            no--;                               /* the matching '(' */
            s++;
        } else {
            int p = prec(c);
            while (no > 0 && prec(ops[no - 1]) >= p) reduce();
            ops[no++] = c;
            s++;
            want_operand = 1;
        }
    }
}

int main(void) {
    u32 chk = 0;
    int done = 0, chars = 0;
    rt_srand(901);
    while (done < NEXPR) {
        for (int i = 0; i < 8; i++) vars[i] = (int)rt_rand_n(200) - 100;
        elen = eover = 0;
        int expect = gen_rel(DEPTH);
        if (eover || elen < 8) continue;            /* too long or trivial: make another one */
        ebuf[elen] = 0;
        pp = ebuf;
        perr = 0;
        int v1 = parse_rel();
        skip_ws();
        if (perr || *pp != 0) return 1;
        if (v1 != expect) return 2;
        int err = 0;
        int v2 = eval_stack(ebuf, &err);
        if (err) return 3;
        if (v2 != expect) return 4;
        chk = chk * 31 + (u32)v1 + (u32)elen;
        chars += elen;
        done++;
    }
    return rt_check(chk + (u32)chars, 0x173978);
}

#elif SEL == 3
#define NLINES 60
#define LMAX 80
#define NPAT 16
static u8 lines[NLINES][LMAX];
static u8 kind[NLINES], is_error[NLINES];

static const char *const words[] = {
    "time", "people", "water", "number", "sound", "place", "little", "world", "house", "picture", "animal",
    "mother", "answer", "earth", "school", "idea", "queue", "audio", "serious", "beautiful", "area", "gray",
    "grey", "color", "colour", "index", "about", "login", "cache", "user"};
#define NWORDS ((int)(sizeof words / sizeof words[0]))
static const char *const patterns[NPAT] = {
    "^[0-9]+$",                     /*  0: checked against all_digits() */
    "[a-z]+[0-9]?@[a-z]+\\.com",    /*  1: every mail line */
    "GET|POST",                     /*  2: checked against contains() */
    "^\\d+\\.\\d+\\.\\d+\\.\\d+ ",  /*  3: every access line */
    "ERROR.*code=\\d+$",            /*  4: every ERROR line */
    " [45]\\d\\d ",
    "\\.html? ",
    "0x[0-9a-fA-F]+",               /*  7: every hex line */
    "^\\w+=\\w+;",                  /*  8: every key=value line */
    "a.*e.*i",
    "[^ ]+\\.[a-z]+ \\d+ \\d+$",    /* 10: every access line */
    "colou?r|gr[ae]y",
    "\\d\\d:\\d\\d:\\d\\d",         /* 12: every log line */
    "^[A-Z][a-z]+ [A-Z]",           /* 13: every sentence line */
    "[aeiou][aeiou][aeiou]",        /* 14: checked against three_vowels() */
    "e.*e.*s$"};

static int elem_len(const u8 *p) {          /* length of one pattern element */
    if (*p == '\\') return 2;
    if (*p == '[') {
        int n = 1;
        while (p[n] != ']') n++;
        return n + 1;
    }
    return 1;
}
static int elem_match(const u8 *p, u8 c) {  /* does the element at p match the character c? */
    u8 k = p[0];
    if (c == 0) return 0;
    if (k == '.') return 1;
    if (k == '\\') {
        k = p[1];
        if (k == 'd') return c >= '0' && c <= '9';
        if (k == 'w') return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        return c == k;
    }
    if (k == '[') {
        int neg = 0, ok = 0;
        p++;
        if (*p == '^') { neg = 1; p++; }
        while (*p != ']') {
            if (p[1] == '-' && p[2] != ']') {
                if (c >= p[0] && c <= p[2]) ok = 1;
                p += 3;
            } else {
                if (c == *p) ok = 1;
                p++;
            }
        }
        return ok != neg;
    }
    return c == k;
}
/* Does re[0..end) match at the start of s? Greedy * and +, with backtracking. */
static int match_here(const u8 *re, const u8 *end, const u8 *s) {
    for (;;) {
        if (re == end) return 1;
        if (*re == '$' && re + 1 == end) return *s == 0;
        int n = elem_len(re);
        u8 q = re + n < end ? re[n] : 0;
        if (q == '*' || q == '+') {
            int max = 0;
            while (elem_match(re, s[max])) max++;
            for (int k = max; k >= (q == '+'); k--)
                if (match_here(re + n + 1, end, s + k)) return 1;
            return 0;
        }
        if (q == '?') {
            if (elem_match(re, *s) && match_here(re + n + 1, end, s + 1)) return 1;
            re += n + 1;
            continue;
        }
        if (!elem_match(re, *s)) return 0;
        re += n;
        s++;
    }
}
/* Does the pattern (alternatives separated by '|') match anywhere in s? */
static int re_search(const u8 *re, const u8 *s) {
    const u8 *a = re;
    for (;;) {
        const u8 *b = a;
        while (*b && *b != '|') b += elem_len(b);
        if (*a == '^') {
            if (match_here(a + 1, b, s)) return 1;
        } else {
            for (const u8 *t = s;; t++) {
                if (match_here(a, b, t)) return 1;
                if (*t == 0) break;
            }
        }
        if (*b == 0) return 0;
        a = b + 1;
    }
}

/* ---- reference predicates ---- */
static int all_digits(const u8 *s) {
    if (*s == 0) return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return 0;
    return 1;
}
static int contains(const u8 *s, const char *w) {
    for (; *s; s++) {
        int k = 0;
        while (w[k] && s[k] == (u8)w[k]) k++;
        if (!w[k]) return 1;
    }
    return 0;
}
static int is_vowel(u8 c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }
static int three_vowels(const u8 *s) {
    for (; s[0] && s[1] && s[2]; s++)
        if (is_vowel(s[0]) && is_vowel(s[1]) && is_vowel(s[2])) return 1;
    return 0;
}

/* ---- line generator ---- */
static u8 *out;
static void put(u8 c) { *out++ = c; }
static void put_str(const char *s) { while (*s) put((u8)*s++); }
static void put_word(void) { put_str(words[rt_rand_n(NWORDS)]); }
static void put_num(u32 v, int min_digits) {
    char d[12];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < min_digits) d[n++] = '0';
    while (n) put((u8)d[--n]);
}
static void make_line(int i) {
    static const char *const methods[] = {"GET", "POST", "PUT", "HEAD"};
    static const char *const exts[] = {"html", "htm", "png", "css", "js"};
    static const u16 status[] = {200, 200, 200, 301, 404, 500, 503, 204};
    static const char *const levels[] = {"ERROR", "WARN", "INFO"};
    u32 k = rt_rand_n(7);
    out = lines[i];
    kind[i] = (u8)k;
    is_error[i] = 0;
    if (k == 0) {                               /* access log */
        for (int j = 0; j < 4; j++) { put_num(1 + rt_rand_n(255), 1); put(j < 3 ? '.' : ' '); }
        put_str(methods[rt_rand_n(4)]);
        put_str(" /");
        put_word();
        put('/');
        put_word();
        put('.');
        put_str(exts[rt_rand_n(5)]);
        put(' ');
        put_num(status[rt_rand_n(8)], 1);
        put(' ');
        put_num(rt_rand_n(90000), 1);
    } else if (k == 1) {                        /* mail */
        put_word();
        if (rt_rand() & 1) put((u8)('0' + rt_rand_n(10)));
        put('@');
        put_word();
        put_str(".com ");
        put_word();
        put(' ');
        put_word();
    } else if (k == 2) {                        /* key=value; */
        put_word();
        put('=');
        put_num(rt_rand_n(5000), 1);
        put(';');
        put_word();
        put('=');
        put_word();
        put(';');
    } else if (k == 3) {                        /* log */
        u32 lv = rt_rand_n(3);
        put_str("2026-11-");
        put_num(1 + rt_rand_n(28), 2);
        put(' ');
        put_num(rt_rand_n(24), 2);
        put(':');
        put_num(rt_rand_n(60), 2);
        put(':');
        put_num(rt_rand_n(60), 2);
        put(' ');
        put_str(levels[lv]);
        put(' ');
        put_word();
        put(' ');
        put_word();
        put_str(" code=");
        put_num(rt_rand_n(300), 1);
        is_error[i] = lv == 0;
    } else if (k == 4) {                        /* sentence: two capitalised words, then more */
        for (int j = 0; j < 5; j++) {
            u8 *w = out;
            put_word();
            if (j < 2) *w = (u8)(*w - 'a' + 'A');
            if (j < 4) put(' ');
        }
    } else if (k == 5) {                        /* digits only */
        put_num(rt_rand(), 1);
    } else {                                    /* hex words */
        for (int j = 0; j < 2; j++) {
            u32 v = rt_rand();
            v >>= rt_rand_n(20);
            int upper = (int)(rt_rand() & 1), started = 0;
            put_str("0x");
            for (int sh = 28; sh >= 0; sh -= 4) {
                u32 h = v >> sh & 15;
                if (h || started || sh == 0) {
                    put((u8)(h < 10 ? '0' + h : (upper ? 'A' : 'a') + h - 10));
                    started = 1;
                }
            }
            put(' ');
        }
        put_word();
    }
    put(0);
}

int main(void) {
    static const u8 must[7][2] = {{3, 10}, {1, 1}, {8, 8}, {12, 12}, {13, 13}, {0, 0}, {7, 7}};
    u32 chk = 0;
    rt_srand(1001);
    for (int i = 0; i < NLINES; i++) make_line(i);
    for (int p = 0; p < NPAT; p++) {
        const u8 *re = (const u8 *)patterns[p];
        int hits = 0;
        for (int i = 0; i < NLINES; i++) {
            const u8 *s = lines[i];
            int m = re_search(re, s);
            if (p == 0 && m != all_digits(s)) return 1;
            if (p == 2 && m != (contains(s, "GET") || contains(s, "POST"))) return 2;
            if (p == 14 && m != three_vowels(s)) return 3;
            if (p == 4 && is_error[i] && !m) return 4;
            if ((p == must[kind[i]][0] || p == must[kind[i]][1]) && !m) return 5;
            hits += m;
        }
        chk = chk * 31 + (u32)hits;
    }
    return rt_check(chk, 0x2dc261);
}

#else
#error "interp.c: build with -DSEL=1..3"
#endif
