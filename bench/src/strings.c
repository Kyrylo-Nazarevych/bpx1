/* BPX-1 bench: text processing. Select one with -DSEL=<n>:
     1 strings.search  substring search three ways (naive, KMP, Horspool) over pseudo-English text
     2 strings.lexer   tokenizer for generated C-like source: identifiers, keywords (binary search),
                       decimal and hex numbers, strings with escapes, comments, 1- and 2-char operators
     3 strings.glob    wildcard matching (* ? [a-z] [!x]) of generated patterns against file names,
                       iterative with backtracking and recursive
     4 strings.utf8    UTF-8 validation of generated text (some of it corrupted), then base64 encode
                       and decode of the same bytes

   Self-checks (exit codes 1..9): the three searches agree; the lexer finds exactly the tokens the
   generator wrote; both matchers agree and every pattern matches the name it was made from; the
   validator accepts clean text with the right count and rejects corrupted text; base64 round-trips. */
#include "rt.h"

#if SEL == 1 || SEL == 3
static const char *const words[] = {
    "the", "of", "and", "to", "in", "is", "that", "it", "was", "for", "on", "are", "as", "with", "his",
    "they", "at", "be", "this", "from", "have", "or", "one", "had", "by", "word", "but", "not", "what",
    "all", "were", "we", "when", "your", "can", "said", "there", "use", "an", "each", "which", "she",
    "do", "how", "their", "if", "will", "up", "other", "about", "out", "many", "then", "them", "these",
    "so", "some", "her", "would", "make", "like", "him", "into", "time", "has", "look", "two", "more",
    "write", "go", "see", "number", "no", "way", "could", "people", "than", "first", "water", "been"};
#define NWORDS ((int)(sizeof words / sizeof words[0]))

/* A word, frequent ones (the start of the list) much more often than rare ones. */
static const char *pick_word(void) { return words[rt_rand_n(rt_rand_n(NWORDS) + 1)]; }
#endif

#if SEL == 1
#define TLEN 5200
#define NPAT 16
#define MAXP 16
static u8 text[TLEN + 1];
static int fail[MAXP];
static int skip[256];

static int naive(const u8 *t, int n, const u8 *p, int m) {
    int count = 0;
    for (int i = 0; i + m <= n; i++) {
        int k = 0;
        while (k < m && t[i + k] == p[k]) k++;
        if (k == m) count++;
    }
    return count;
}
static int kmp(const u8 *t, int n, const u8 *p, int m) {
    int k = 0, count = 0;
    fail[0] = 0;
    for (int i = 1; i < m; i++) {
        while (k > 0 && p[i] != p[k]) k = fail[k - 1];
        if (p[i] == p[k]) k++;
        fail[i] = k;
    }
    k = 0;
    for (int i = 0; i < n; i++) {
        while (k > 0 && t[i] != p[k]) k = fail[k - 1];
        if (t[i] == p[k]) k++;
        if (k == m) { count++; k = fail[k - 1]; }
    }
    return count;
}
static int horspool(const u8 *t, int n, const u8 *p, int m) {
    int count = 0;
    for (int c = 0; c < 256; c++) skip[c] = m;
    for (int i = 0; i < m - 1; i++) skip[p[i]] = m - 1 - i;
    for (int i = 0; i + m <= n; i += skip[t[i + m - 1]]) {
        int k = m - 1;
        while (k >= 0 && t[i + k] == p[k]) k--;
        if (k < 0) count++;
    }
    return count;
}

int main(void) {
    u32 chk = 0;
    int n = 0;
    rt_srand(101);
    while (n < TLEN - 12) {                             /* words, with some punctuation */
        const char *w = pick_word();
        while (*w) text[n++] = (u8)*w++;
        u32 r = rt_rand_n(16);
        if (r == 0) text[n++] = ',';
        else if (r == 1) text[n++] = '.';
        text[n++] = ' ';
    }
    text[n] = 0;
    for (int q = 0; q < NPAT; q++) {
        u8 pat[MAXP];
        int m = 2 + (int)rt_rand_n(MAXP - 3);           /* 2 .. 14 characters taken from the text */
        int pos = (int)rt_rand_n((u32)(n - m));
        for (int i = 0; i < m; i++) pat[i] = text[pos + i];
        if ((q & 3) == 3) {                             /* damaged: likely absent */
            u32 at = rt_rand_n((u32)m);
            pat[at] = (u8)('a' + rt_rand_n(26));
        }
        int c1 = naive(text, n, pat, m);
        int c2 = kmp(text, n, pat, m);
        int c3 = horspool(text, n, pat, m);
        if (c1 != c2) return 1;
        if (c1 != c3) return 2;
        if ((q & 3) != 3 && c1 < 1) return 3;
        chk = chk * 31 + (u32)c1;
    }
    return rt_check(chk, 0x34ecf6);
}

#elif SEL == 2
#define SRC_MAX 12000
#define ROUNDS 6
enum { T_IDENT, T_KEYWORD, T_DEC, T_HEX, T_STRING, T_OP1, T_OP2, T_COMMENT, T_KINDS };
static u8 src[SRC_MAX + 64];
static int src_len;
static int gen_count[T_KINDS], lex_count[T_KINDS];
static u32 gen_numsum, lex_numsum, gen_kwsum, lex_kwsum, gen_idlen, lex_idlen;
static int gen_lines, lex_lines;

static const char *const keywords[] = {                 /* sorted, for the binary search */
    "break", "case", "char", "const", "continue", "default", "do", "else", "enum", "for", "goto", "if",
    "int", "long", "return", "short", "signed", "sizeof", "static", "struct", "switch", "unsigned",
    "void", "while"};
#define NKW ((int)(sizeof keywords / sizeof keywords[0]))
static const char *const idents[] = {
    "i", "j", "n", "x", "count", "len", "buf", "ptr", "tmp", "node", "next", "value", "index", "total",
    "flags", "result", "ch", "p", "size_max", "do_it", "iff", "integer", "forx", "whiles", "_t", "a1"};
#define NID ((int)(sizeof idents / sizeof idents[0]))
static const char ops1[] = "+-*/%=<>!&|^~?:;,.()[]{}";
static const char *const ops2[] = {"==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "++", "--", "+=", "-=", "->", "*="};
#define NOPS2 ((int)(sizeof ops2 / sizeof ops2[0]))

static void put(u8 c) { src[src_len++] = c; }
static void puts_(const char *s) { while (*s) put((u8)*s++); }

static void generate(u32 seed) {
    rt_srand(seed);
    src_len = 0;
    for (int k = 0; k < T_KINDS; k++) gen_count[k] = 0;
    gen_numsum = gen_kwsum = gen_idlen = 0;
    gen_lines = 0;
    int indent = 0;
    while (src_len < SRC_MAX) {
        u32 r = rt_rand_n(100);
        if (r < 28) {
            const char *id = idents[rt_rand_n(NID)];
            int start = src_len;
            puts_(id);
            if (rt_rand_n(4) == 0) put((u8)('0' + rt_rand_n(10)));
            gen_idlen += (u32)(src_len - start);
            gen_count[T_IDENT]++;
        } else if (r < 44) {
            u32 k = rt_rand_n(NKW);
            puts_(keywords[k]);
            gen_kwsum += k + 1;
            gen_count[T_KEYWORD]++;
        } else if (r < 56) {
            u32 v = rt_rand_n(4) ? rt_rand_n(1000) : rt_rand() >> 4;
            char d[12];
            int nd = 0;
            gen_numsum += v;
            do { d[nd++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (nd) put((u8)d[--nd]);
            gen_count[T_DEC]++;
        } else if (r < 62) {
            u32 v = rt_rand();
            v >>= rt_rand_n(28);
            int nd = 0, upper = (int)(rt_rand() & 1);
            char d[8];
            gen_numsum += v;
            do { u32 h = v & 15; d[nd++] = (char)(h < 10 ? '0' + h : (upper ? 'A' : 'a') + h - 10); v >>= 4; } while (v);
            put('0');
            put(upper ? 'X' : 'x');
            while (nd) put((u8)d[--nd]);
            gen_count[T_HEX]++;
        } else if (r < 68) {
            int len = (int)rt_rand_n(14);
            put('"');
            for (int i = 0; i < len; i++) {
                u32 c = rt_rand_n(32);
                if (c == 0) { put('\\'); put('"'); }
                else if (c == 1) { put('\\'); put('\\'); }
                else if (c == 2) { put('\\'); put('n'); }
                else if (c < 6) put(' ');
                else put((u8)('a' + c - 6));
            }
            put('"');
            gen_count[T_STRING]++;
        } else if (r < 86) {
            u8 c = (u8)ops1[rt_rand_n(sizeof ops1 - 1)];
            put(c);
            if (c == '{') indent++;
            if (c == '}' && indent > 0) indent--;
            gen_count[T_OP1]++;
        } else if (r < 96) {
            puts_(ops2[rt_rand_n(NOPS2)]);
            gen_count[T_OP2]++;
        } else {
            int len = (int)rt_rand_n(30);
            int block = (int)(rt_rand() & 1);
            puts_(block ? "/*" : "//");
            for (int i = 0; i < len; i++) put(rt_rand_n(6) ? (u8)('a' + rt_rand_n(26)) : (u8)' ');
            if (block) puts_("*/");
            else { put('\n'); gen_lines++; }
            gen_count[T_COMMENT]++;
        }
        /* at least one white-space character between tokens */
        u32 w = rt_rand_n(10);
        if (w < 7) put(' ');
        else if (w == 7) { put(' '); put(' '); }
        else if (w == 8) put('\t');
        else { put('\n'); gen_lines++; for (int i = 0; i < indent && i < 6; i++) { put(' '); put(' '); } }
    }
    src[src_len] = 0;
}

static int is_alpha(u8 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(u8 c) { return c >= '0' && c <= '9'; }

/* Index of the keyword equal to s[0..n), or -1. */
static int keyword_index(const u8 *s, int n) {
    int lo = 0, hi = NKW - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1, i = 0, d = 0;
        const char *k = keywords[mid];
        while (i < n && k[i] && (d = (int)s[i] - (int)(u8)k[i]) == 0) i++;
        if (i == n && k[i] == 0) return mid;
        if (i == n) d = -1;                 /* s is a proper prefix of the keyword */
        else if (k[i] == 0) d = 1;          /* the keyword is a proper prefix of s */
        if (d < 0) hi = mid - 1;
        else       lo = mid + 1;
    }
    return -1;
}

static int lex(void) {                      /* 0 = ok */
    const u8 *s = src;
    int i = 0, lines = 0;
    for (int k = 0; k < T_KINDS; k++) lex_count[k] = 0;
    lex_numsum = lex_kwsum = lex_idlen = 0;
    for (;;) {
        u8 c = s[i];
        if (c == 0) break;
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '\n') { lines++; i++; continue; }
        if (is_alpha(c)) {
            int j = i + 1;
            while (is_alpha(s[j]) || is_digit(s[j])) j++;
            int kw = keyword_index(s + i, j - i);
            if (kw >= 0) { lex_count[T_KEYWORD]++; lex_kwsum += (u32)kw + 1; }
            else { lex_count[T_IDENT]++; lex_idlen += (u32)(j - i); }
            i = j;
            continue;
        }
        if (is_digit(c)) {
            u32 v = 0;
            if (c == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                i += 2;
                for (;;) {
                    u8 h = s[i];
                    u32 d;
                    if (is_digit(h)) d = h - '0';
                    else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                    else break;
                    v = v << 4 | d;
                    i++;
                }
                lex_count[T_HEX]++;
            } else {
                while (is_digit(s[i])) v = v * 10 + (s[i++] - '0');
                lex_count[T_DEC]++;
            }
            lex_numsum += v;
            continue;
        }
        if (c == '"') {
            i++;
            while (s[i] != '"') {
                if (s[i] == 0) return 1;
                if (s[i] == '\\') i++;
                i++;
            }
            i++;
            lex_count[T_STRING]++;
            continue;
        }
        if (c == '/' && s[i + 1] == '/') {
            i += 2;
            while (s[i] != '\n') {
                if (s[i] == 0) return 2;
                i++;
            }
            lex_count[T_COMMENT]++;
            continue;
        }
        if (c == '/' && s[i + 1] == '*') {
            i += 2;
            while (!(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == 0) return 3;
                i++;
            }
            i += 2;
            lex_count[T_COMMENT]++;
            continue;
        }
        {   /* operator: the longest match wins */
            int k, two = 0;
            u8 d = s[i + 1];
            for (k = 0; k < NOPS2; k++)
                if ((u8)ops2[k][0] == c && (u8)ops2[k][1] == d) { two = 1; break; }
            if (two) { lex_count[T_OP2]++; i += 2; continue; }
            for (k = 0; ops1[k]; k++)
                if ((u8)ops1[k] == c) break;
            if (!ops1[k]) return 4;
            lex_count[T_OP1]++;
            i++;
        }
    }
    lex_lines = lines;
    return 0;
}

int main(void) {
    u32 chk = 0;
    for (int round = 0; round < ROUNDS; round++) {
        generate(202 + (u32)round);
        int e = lex();
        if (e) return e;
        for (int k = 0; k < T_KINDS; k++) {
            if (lex_count[k] != gen_count[k]) return 5;
            chk = chk * 31 + (u32)lex_count[k];
        }
        if (lex_numsum != gen_numsum) return 6;
        if (lex_kwsum != gen_kwsum) return 7;
        if (lex_idlen != gen_idlen) return 8;
        if (lex_lines != gen_lines) return 9;
        chk = chk * 31 + lex_numsum + lex_kwsum + (u32)lex_lines;
    }
    return rt_check(chk, 0x126941);
}

#elif SEL == 3
#define NNAMES 160
#define NPATS 44
#define MAXN 40
static u8 names[NNAMES][MAXN];
static u8 pats[NPATS][5 * MAXN];    /* a name character becomes at most 5 pattern characters */
static int pat_src[NPATS];          /* the name a generated pattern was made from, or -1 */

/* p points just after '['. Does the class match c? *end is set just after the closing ']'. */
static int class_match(const u8 *p, u8 c, const u8 **end) {
    int neg = 0, ok = 0;
    if (*p == '!') { neg = 1; p++; }
    while (*p && *p != ']') {
        if (p[1] == '-' && p[2] && p[2] != ']') {
            if (c >= p[0] && c <= p[2]) ok = 1;
            p += 3;
        } else {
            if (c == *p) ok = 1;
            p++;
        }
    }
    if (*p == ']') p++;
    *end = p;
    return ok != neg;
}

/* Iterative matcher: remembers the last '*' and retries from there. */
static int glob_iter(const u8 *pat, const u8 *str) {
    const u8 *star = NULL, *mark = NULL, *e;
    while (*str) {
        u8 pc = *pat;
        if (pc == '*') { star = pat++; mark = str; continue; }
        if (pc == '?') { pat++; str++; continue; }
        if (pc == '[') {
            if (class_match(pat + 1, *str, &e)) { pat = e; str++; continue; }
        } else if (pc == *str) { pat++; str++; continue; }
        if (!star) return 0;
        pat = star + 1;
        str = ++mark;
    }
    while (*pat == '*') pat++;
    return *pat == 0;
}

/* Recursive matcher: a '*' tries every possible length. */
static int glob_rec(const u8 *pat, const u8 *str) {
    const u8 *e;
    for (;;) {
        u8 pc = *pat;
        if (pc == 0) return *str == 0;
        if (pc == '*') {
            while (pat[1] == '*') pat++;
            if (pat[1] == 0) return 1;
            for (;; str++) {
                if (glob_rec(pat + 1, str)) return 1;
                if (*str == 0) return 0;
            }
        }
        if (*str == 0) return 0;
        if (pc == '[') {
            if (!class_match(pat + 1, *str, &e)) return 0;
            pat = e;
            str++;
            continue;
        }
        if (pc != '?' && pc != *str) return 0;
        pat++;
        str++;
    }
}

static const char *const exts[] = {".c", ".h", ".txt", ".md", ".py", ".json"};
static const char *const fixed_pats[] = {"*.c", "*/*", "*_*.h", "?*[0-9].*", "*the*", "[a-m]*/[!a-m]*", "*.??", "*"};
#define NFIXED ((int)(sizeof fixed_pats / sizeof fixed_pats[0]))

int main(void) {
    u32 chk = 0;
    int total = 0;
    rt_srand(303);
    for (int i = 0; i < NNAMES; i++) {                  /* names like "time/number_of7.txt" */
        int n = 0, parts = 1 + (int)rt_rand_n(4);
        for (int k = 0; k < parts; k++) {
            const char *w = pick_word();
            while (*w && n < MAXN - 12) names[i][n++] = (u8)*w++;
            if (k + 1 < parts) names[i][n++] = rt_rand_n(3) ? '_' : '/';
        }
        if (rt_rand_n(3) == 0) names[i][n++] = (u8)('0' + rt_rand_n(10));
        const char *x = exts[rt_rand_n(6)];
        while (*x) names[i][n++] = (u8)*x++;
        names[i][n] = 0;
    }
    for (int q = 0; q < NPATS; q++) {
        if (q < NFIXED) {
            const char *f = fixed_pats[q];
            int n = 0;
            while (*f) pats[q][n++] = (u8)*f++;
            pats[q][n] = 0;
            pat_src[q] = -1;
            continue;
        }
        /* rewrite a name into a pattern that still matches it */
        int from = (int)rt_rand_n(NNAMES), n = 0, stars = 0;
        const u8 *s = names[from];
        pat_src[q] = from;
        while (*s) {
            u32 r = rt_rand_n(20);
            u8 c = *s;
            if (r < 3 && stars < 3) {                   /* a star swallows 0..5 characters */
                int eat = (int)rt_rand_n(6);
                pats[q][n++] = '*';
                stars++;
                while (eat-- && *s) s++;
                continue;
            }
            if (r < 5) pats[q][n++] = '?';
            else if (r < 7 && c >= 'a' && c <= 'z') {   /* a range around the letter */
                pats[q][n++] = '[';
                pats[q][n++] = (u8)(c > 'c' ? c - 3 : 'a');
                pats[q][n++] = '-';
                pats[q][n++] = (u8)(c < 'x' ? c + 2 : 'z');
                pats[q][n++] = ']';
            } else if (r < 8 && c != ']' && c != '!') { /* anything but some other character */
                pats[q][n++] = '[';
                pats[q][n++] = '!';
                pats[q][n++] = (u8)(c == 'q' ? 'z' : 'q');
                pats[q][n++] = ']';
            } else pats[q][n++] = c;
            s++;
        }
        pats[q][n] = 0;
    }
    for (int q = 0; q < NPATS; q++) {
        int hits = 0;
        for (int i = 0; i < NNAMES; i++) {
            int m1 = glob_iter(pats[q], names[i]);
            int m2 = glob_rec(pats[q], names[i]);
            if (m1 != m2) return 1;
            if (i == pat_src[q] && !m1) return 2;
            hits += m1;
        }
        total += hits;
        chk = chk * 31 + (u32)hits;
    }
    if (total < NPATS - NFIXED) return 3;
    return rt_check(chk, 0x161786);
}

#elif SEL == 4
#define BLEN 2400
#define ROUNDS 18
static u8 raw[BLEN + 8], b64[BLEN * 2], back[BLEN + 8];

static int put_utf8(u8 *d, u32 cp) {
    if (cp < 0x80) { d[0] = (u8)cp; return 1; }
    if (cp < 0x800) { d[0] = (u8)(0xC0 | cp >> 6); d[1] = (u8)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        d[0] = (u8)(0xE0 | cp >> 12);
        d[1] = (u8)(0x80 | (cp >> 6 & 0x3F));
        d[2] = (u8)(0x80 | (cp & 0x3F));
        return 3;
    }
    d[0] = (u8)(0xF0 | cp >> 18);
    d[1] = (u8)(0x80 | (cp >> 12 & 0x3F));
    d[2] = (u8)(0x80 | (cp >> 6 & 0x3F));
    d[3] = (u8)(0x80 | (cp & 0x3F));
    return 4;
}

/* Number of code points, or -1 - (offset of the first bad sequence). */
static int utf8_validate(const u8 *s, int n) {
    int i = 0, count = 0;
    while (i < n) {
        u8 c = s[i];
        if (c < 0x80) i++;
        else if (c < 0xC2) return -1 - i;               /* stray continuation byte, or overlong 2-byte */
        else if (c < 0xE0) {
            if (i + 1 >= n || (s[i + 1] & 0xC0) != 0x80) return -1 - i;
            i += 2;
        } else if (c < 0xF0) {
            if (i + 2 >= n) return -1 - i;
            u8 c1 = s[i + 1];
            if ((c1 & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80) return -1 - i;
            if (c == 0xE0 && c1 < 0xA0) return -1 - i;  /* overlong */
            if (c == 0xED && c1 >= 0xA0) return -1 - i; /* surrogate */
            i += 3;
        } else if (c < 0xF5) {
            if (i + 3 >= n) return -1 - i;
            u8 c1 = s[i + 1];
            if ((c1 & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80 || (s[i + 3] & 0xC0) != 0x80) return -1 - i;
            if (c == 0xF0 && c1 < 0x90) return -1 - i;  /* overlong */
            if (c == 0xF4 && c1 >= 0x90) return -1 - i; /* above U+10FFFF */
            i += 4;
        } else return -1 - i;
        count++;
    }
    return count;
}

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const u8 *s, int n, u8 *d) {      /* '=' padding, a newline every 76 characters */
    int o = 0, col = 0, i = 0;
    while (i + 2 < n) {
        u32 v = (u32)s[i] << 16 | (u32)s[i + 1] << 8 | s[i + 2];
        d[o++] = (u8)b64chars[v >> 18];
        d[o++] = (u8)b64chars[v >> 12 & 63];
        d[o++] = (u8)b64chars[v >> 6 & 63];
        d[o++] = (u8)b64chars[v & 63];
        i += 3;
        col += 4;
        if (col >= 76) { d[o++] = '\n'; col = 0; }
    }
    if (i < n) {
        u32 v = (u32)s[i] << 16;
        if (i + 1 < n) v |= (u32)s[i + 1] << 8;
        d[o++] = (u8)b64chars[v >> 18];
        d[o++] = (u8)b64chars[v >> 12 & 63];
        d[o++] = (u8)(i + 1 < n ? b64chars[v >> 6 & 63] : '=');
        d[o++] = '=';
    }
    return o;
}

static int b64_decode(const u8 *s, int n, u8 *d) {      /* decoded length, or -1 */
    u32 acc = 0;
    int bits = 0, o = 0, pad = 0;
    for (int i = 0; i < n; i++) {
        u8 c = s[i];
        u32 v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '\n' || c == ' ') continue;
        else if (c == '=') { pad++; continue; }
        else return -1;
        if (pad) return -1;                             /* data after padding */
        acc = acc << 6 | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            d[o++] = (u8)(acc >> bits);
        }
    }
    return o;
}

int main(void) {
    u32 chk = 0;
    for (int round = 0; round < ROUNDS; round++) {
        int n = 0, cps = 0, style = round % 6;
        rt_srand(404 + (u32)round);
        while (n < BLEN - 4) {
            u32 r = rt_rand_n(100), cp;
            /* styles: 0 ASCII, 1 Latin, 2 Cyrillic-like, 3 CJK-like, 4 with emoji, 5 everything */
            if (style == 0 || r < 55) cp = r < 12 ? ' ' : 'a' + rt_rand_n(26);
            else if (style == 1) cp = 0xC0 + rt_rand_n(0x40);
            else if (style == 2) cp = 0x400 + rt_rand_n(0x100);
            else if (style == 3) cp = 0x4E00 + rt_rand_n(0x5000);
            else if (style == 4) cp = r < 80 ? 0x1F300 + rt_rand_n(0x300) : 0x2000 + rt_rand_n(0x70);
            else {
                u32 k = rt_rand_n(4);
                cp = k == 0 ? 0x80 + rt_rand_n(0x780) : k == 1 ? 0x800 + rt_rand_n(0xD000 - 0x800)
                   : k == 2 ? 0xE000 + rt_rand_n(0x2000) : 0x10000 + rt_rand_n(0x100000);
            }
            n += put_utf8(raw + n, cp);
            cps++;
        }
        int bad_at = -1;
        if (round % 3 == 2) {                           /* corrupt one byte so the text is invalid */
            u32 kind = rt_rand_n(4);
            int pos = (int)rt_rand_n((u32)n - 8);
            if (kind == 2) {                            /* a continuation byte becomes a letter */
                while (pos < n && (raw[pos] & 0xC0) != 0x80) pos++;
                if (pos < n) { raw[pos] = 'A'; bad_at = pos; }
            } else if (kind == 3) {                     /* a lead byte becomes a continuation byte */
                while (pos < n && raw[pos] < 0xC2) pos++;
                if (pos < n) { raw[pos] = 0x80; bad_at = pos; }
            }
            if (bad_at < 0) {                           /* 0xFF and 0xC0 are never valid */
                pos = (int)rt_rand_n((u32)n - 8);
                while ((raw[pos] & 0xC0) == 0x80) pos++;    /* start of a sequence */
                raw[pos] = kind & 1 ? 0xFF : 0xC0;
                bad_at = pos;
            }
        }
        int v = utf8_validate(raw, n);
        if (bad_at < 0) {
            if (v != cps) return 1;
        } else {
            if (v >= 0) return 2;
            int at = -1 - v;                            /* the bad sequence starts at or just before bad_at */
            if (at > bad_at || at < bad_at - 3) return 3;
        }
        int e = b64_encode(raw, n, b64);
        int m = b64_decode(b64, e, back);
        if (m != n) return 4;
        if (memcmp(raw, back, (size_t)n) != 0) return 5;
        chk = chk * 31 + (u32)v + (u32)e;
        for (int i = 0; i < e; i += 61) chk = chk * 31 + b64[i];
    }
    return rt_check(chk, 0x2cf21b);
}

#else
#error "strings.c: build with -DSEL=1..4"
#endif
