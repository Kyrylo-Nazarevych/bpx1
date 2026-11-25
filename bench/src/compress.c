/* BPX-1 bench: data compression. Select one with -DSEL=<n>:
     1 compress.lzss     LZSS with hash chains (4 KiB window, matches of 3..18 bytes), then expansion
     2 compress.huffman  Huffman coding: count, build the tree with a binary heap, assign codes,
                         encode, decode bit by bit through the tree
     3 compress.rle      delta coding + run-length coding (literal blocks and runs) and back, with a
                         bit-by-bit CRC-32 of the plain and of the packed data

   The inputs are semi-repetitive blocks of several kinds (word text, binary records, runs, a mix that
   repeats earlier parts, smooth samples). Self-checks (exit codes 1..9): every block must come back
   identical after decompression; the CRC routine must give the standard check value. */
#include "rt.h"

static const char *const words[] = {
    "the", "of", "and", "to", "in", "is", "that", "it", "was", "for", "on", "are", "as", "with", "his",
    "they", "at", "be", "this", "from", "have", "or", "one", "had", "by", "word", "but", "not", "what",
    "all", "were", "we", "when", "your", "can", "said", "there", "use", "an", "each", "which", "she",
    "branch", "predictor", "history", "counter", "table", "taken", "pipeline", "register"};
#define NWORDS ((int)(sizeof words / sizeof words[0]))

/* Fill buf[0..n). kind: 0 text, 1 records of 16 bytes, 2 runs, 3 mix with repeats, 4 smooth samples,
   5 skewed byte values. */
static void gen_data(u8 *buf, int n, int kind) {
    int i = 0;
    if (kind == 0) {
        while (i < n) {
            const char *w = words[rt_rand_n(rt_rand_n(NWORDS) + 1)];
            while (*w && i < n) buf[i++] = (u8)*w++;
            if (i < n) buf[i++] = rt_rand_n(12) ? ' ' : '\n';
        }
    } else if (kind == 1) {
        u32 id = rt_rand();
        while (i < n) {
            u8 rec[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xDE, 0xAD, 0, 0, 0, 0, 0xFF, 0xFF};
            rec[0] = (u8)id;
            rec[1] = (u8)(id >> 8);
            rec[2] = (u8)(id >> 16);
            rec[3] = (u8)(id >> 24);
            rec[4] = (u8)("ABCD"[rt_rand_n(4)]);
            rec[5] = rt_rand_n(8) ? 0 : 0x80;
            rec[6] = (u8)rt_rand();
            rec[7] = (u8)rt_rand_n(4);
            id += 1 + rt_rand_n(3);
            for (int k = 0; k < 16 && i < n; k++) buf[i++] = rec[k];
        }
    } else if (kind == 2) {
        while (i < n) {
            u8 v = (u8)(rt_rand_n(6) * 40);
            int len = 1 + (int)rt_rand_n(rt_rand_n(40) + 1);
            while (len-- > 0 && i < n) buf[i++] = v;
        }
    } else if (kind == 3) {
        while (i < n) {
            u32 r = rt_rand_n(4);
            int len = 8 + (int)rt_rand_n(120);
            if (r == 0) {
                while (len-- > 0 && i < n) buf[i++] = (u8)rt_rand();
            } else if (r == 1 || i < 64) {
                while (len > 0 && i < n) {
                    const char *w = words[rt_rand_n(NWORDS)];
                    while (*w && i < n) { buf[i++] = (u8)*w++; len--; }
                    if (i < n) buf[i++] = ' ';
                    len--;
                }
            } else {
                int from = (int)rt_rand_n((u32)i - 8);
                while (len-- > 0 && i < n) buf[i++] = buf[from++];
            }
        }
    } else if (kind == 4) {
        u32 v = 128;
        while (i < n) {
            u32 r = rt_rand_n(16);
            if (r == 0) v += 1 + rt_rand_n(3);
            else if (r == 1) v -= 1 + rt_rand_n(3);
            else if (r == 2) v = rt_rand_n(256);
            buf[i++] = (u8)v;
        }
    } else {
        while (i < n) buf[i++] = (u8)rt_rand_n(rt_rand_n(rt_rand_n(256) + 1) + 1);
    }
}

#if SEL == 1
#define NMAX 9000
#define WSIZE 4096
#define MINM 3
#define MAXM 18
#define HBITS 11
#define CHAIN 8
static u8 in[NMAX], comp[NMAX + NMAX / 8 + 16], out[NMAX];
static int head[1 << HBITS], prev[NMAX];

static u32 hash3(const u8 *p) { return ((u32)p[0] << 6 ^ (u32)p[1] << 3 ^ p[2]) & ((1u << HBITS) - 1); }

/* Stream: groups of up to 8 items after a flag byte; flag bit set = match (12-bit offset - 1,
   4-bit length - 3, little-endian), clear = literal byte. */
static int lz_compress(const u8 *s, int n, u8 *d) {
    int i = 0, o = 0, flag_at = 0, nflag = 8;
    for (int h = 0; h < (1 << HBITS); h++) head[h] = -1;
    while (i < n) {
        int best = 0, best_off = 0;
        if (nflag == 8) { flag_at = o; d[o++] = 0; nflag = 0; }
        if (i + MINM <= n) {
            int max = n - i < MAXM ? n - i : MAXM;
            int cand = head[hash3(s + i)], chain = CHAIN;
            while (cand >= 0 && i - cand <= WSIZE && chain-- > 0) {
                if (s[cand + best] == s[i + best]) {    /* else it cannot beat the best so far */
                    int len = 0;
                    while (len < max && s[cand + len] == s[i + len]) len++;
                    if (len > best) {
                        best = len;
                        best_off = i - cand;
                        if (len == max) break;
                    }
                }
                cand = prev[cand];
            }
        }
        if (best >= MINM) {
            u32 v = (u32)(best_off - 1) | (u32)(best - MINM) << 12;
            d[flag_at] |= (u8)(1u << nflag);
            d[o++] = (u8)v;
            d[o++] = (u8)(v >> 8);
        } else {
            best = 1;
            d[o++] = s[i];
        }
        nflag++;
        for (int k = 0; k < best; k++, i++)             /* enter the consumed positions into the chains */
            if (i + MINM <= n) {
                u32 h = hash3(s + i);
                prev[i] = head[h];
                head[h] = i;
            }
    }
    return o;
}

static int lz_expand(const u8 *c, int clen, u8 *d, int n) {     /* bytes produced, or -1 */
    int ip = 0, op = 0;
    while (op < n) {
        if (ip >= clen) return -1;
        u32 flags = c[ip++];
        for (int bit = 0; bit < 8 && op < n; bit++) {
            if (flags & (1u << bit)) {
                if (ip + 2 > clen) return -1;
                u32 v = c[ip] | (u32)c[ip + 1] << 8;
                int off = (int)(v & 0xfff) + 1, len = (int)(v >> 12) + MINM;
                ip += 2;
                if (off > op || op + len > n) return -1;
                for (int k = 0; k < len; k++, op++) d[op] = d[op - off];
            } else {
                if (ip >= clen) return -1;
                d[op++] = c[ip++];
            }
        }
    }
    return ip == clen ? op : -1;
}

int main(void) {
    static const int kinds[4] = {0, 3, 1, 2};
    static const int sizes[4] = {7000, 9000, 6000, 8000};
    u32 chk = 0;
    for (int b = 0; b < 4; b++) {
        int n = sizes[b];
        rt_srand(501 + (u32)b);
        gen_data(in, n, kinds[b]);
        int clen = lz_compress(in, n, comp);
        if (clen >= n) return 1;                        /* all four kinds must shrink */
        if (lz_expand(comp, clen, out, n) != n) return 2;
        if (memcmp(in, out, (size_t)n) != 0) return 3;
        chk = chk * 31 + (u32)clen;
        for (int i = 0; i < clen; i += 97) chk = chk * 31 + comp[i];
    }
    return rt_check(chk, 0x1ad6fe);
}

#elif SEL == 2
#define NMAX 6000
#define NSYM 256
#define MAXLEN 24
static u8 in[NMAX], enc[2 * NMAX], out[NMAX];
static u32 freq[NSYM], weight[2 * NSYM], code[NSYM];
static int left[2 * NSYM], right[2 * NSYM];
static int heap[NSYM + 1], heap_n;
static u8 clen[NSYM];
static int too_long;

static int lighter(int a, int b) {      /* order by weight, then by node number: fully determined */
    return weight[a] < weight[b] || (weight[a] == weight[b] && a < b);
}
static void heap_push(int v) {
    int i = heap_n++;
    while (i > 0) {
        int p = (i - 1) >> 1;
        if (!lighter(v, heap[p])) break;
        heap[i] = heap[p];
        i = p;
    }
    heap[i] = v;
}
static int heap_pop(void) {
    int top = heap[0], v = heap[--heap_n], i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= heap_n) break;
        if (c + 1 < heap_n && lighter(heap[c + 1], heap[c])) c++;
        if (!lighter(heap[c], v)) break;
        heap[i] = heap[c];
        i = c;
    }
    heap[i] = v;
    return top;
}
/* Leaves are nodes 0..255 (the byte values), inner nodes follow. Returns the root. */
static int build_tree(void) {
    int next = NSYM;
    heap_n = 0;
    for (int s = 0; s < NSYM; s++)
        if (freq[s]) { weight[s] = freq[s]; heap_push(s); }
    if (heap_n == 1) {                  /* one symbol only: still give it a 1-bit code */
        int a = heap_pop();
        left[next] = right[next] = a;
        weight[next] = weight[a];
        return next;
    }
    while (heap_n > 1) {
        int a = heap_pop(), b = heap_pop();
        left[next] = a;
        right[next] = b;
        weight[next] = weight[a] + weight[b];
        heap_push(next);
        next++;
    }
    return heap_pop();
}
static void assign(int node, u32 c, int len) {
    if (node < NSYM) {
        if (len > MAXLEN) too_long = 1;
        code[node] = c;
        clen[node] = (u8)len;
        return;
    }
    assign(left[node], c << 1, len + 1);
    assign(right[node], c << 1 | 1, len + 1);
}
static int encode(const u8 *s, int n, u8 *d) {
    u32 acc = 0;
    int nbits = 0, o = 0;
    for (int i = 0; i < n; i++) {
        int l = clen[s[i]];
        acc = acc << l | code[s[i]];
        nbits += l;
        while (nbits >= 8) {
            nbits -= 8;
            d[o++] = (u8)(acc >> nbits);
        }
    }
    if (nbits) d[o++] = (u8)(acc << (8 - nbits));
    return o;
}
static int decode(const u8 *s, int nbytes, u8 *d, int n, int root) {
    int ip = 0, o = 0, node = root, bits = 0;
    u32 cur = 0;
    while (o < n) {
        if (bits == 0) {
            if (ip >= nbytes) return -1;
            cur = s[ip++];
            bits = 8;
        }
        bits--;
        if (cur >> bits & 1) node = right[node];
        else                 node = left[node];
        if (node < NSYM) {
            d[o++] = (u8)node;
            node = root;
        }
    }
    return ip == nbytes ? o : -1;
}

int main(void) {
    static const int kinds[5] = {0, 5, 1, 2, 3};
    static const int sizes[5] = {6000, 5000, 4000, 6000, 5500};
    u32 chk = 0;
    for (int b = 0; b < 5; b++) {
        int n = sizes[b];
        rt_srand(601 + (u32)b);
        gen_data(in, n, kinds[b]);
        for (int s = 0; s < NSYM; s++) freq[s] = 0;
        for (int i = 0; i < n; i++) freq[in[i]]++;
        int root = build_tree();
        too_long = 0;
        assign(root, 0, 0);
        if (too_long) return 1;
        int m = encode(in, n, enc);
        if (decode(enc, m, out, n, root) != n) return 2;
        if (memcmp(in, out, (size_t)n) != 0) return 3;
        chk = chk * 31 + (u32)m;
        for (int i = 0; i < m; i += 89) chk = chk * 31 + enc[i];
    }
    return rt_check(chk, 0x002de5);
}

#elif SEL == 3
#define NMAX 3600
static u8 in[NMAX], dl[NMAX], packed[2 * NMAX + 16], un[NMAX], out[NMAX];

static u32 crc32(u32 crc, const u8 *s, int n) {         /* reflected polynomial 0xEDB88320, bit by bit */
    crc = ~crc;
    for (int i = 0; i < n; i++) {
        crc ^= s[i];
        for (int k = 0; k < 8; k++) {
            if (crc & 1) crc = crc >> 1 ^ 0xEDB88320u;
            else         crc >>= 1;
        }
    }
    return ~crc;
}
static void delta_encode(const u8 *s, int n, u8 *d) {
    u8 last = 0;
    for (int i = 0; i < n; i++) { d[i] = (u8)(s[i] - last); last = s[i]; }
}
static void delta_decode(const u8 *s, int n, u8 *d) {
    u8 last = 0;
    for (int i = 0; i < n; i++) { last = (u8)(last + s[i]); d[i] = last; }
}
/* Control byte c: c < 128: c + 1 literal bytes follow; c >= 128: the next byte repeats (c & 127) + 3 times. */
static int put_literals(const u8 *s, int from, int to, u8 *d, int o) {
    while (from < to) {
        int len = to - from;
        if (len > 128) len = 128;
        d[o++] = (u8)(len - 1);
        for (int k = 0; k < len; k++) d[o++] = s[from++];
    }
    return o;
}
static int rle_encode(const u8 *s, int n, u8 *d) {
    int i = 0, o = 0, lit = 0;                          /* s[lit..i) are literals not yet written */
    while (i < n) {
        int run = 1;
        while (i + run < n && run < 130 && s[i + run] == s[i]) run++;
        if (run >= 3) {
            o = put_literals(s, lit, i, d, o);
            d[o++] = (u8)(0x80 | (run - 3));
            d[o++] = s[i];
            i += run;
            lit = i;
        } else i += run;
    }
    return put_literals(s, lit, n, d, o);
}
static int rle_decode(const u8 *s, int n, u8 *d, int cap) {     /* bytes produced, or -1 */
    int i = 0, o = 0;
    while (i < n) {
        u8 c = s[i++];
        if (c & 0x80) {
            int run = (c & 0x7f) + 3;
            if (i >= n || o + run > cap) return -1;
            u8 v = s[i++];
            while (run-- > 0) d[o++] = v;
        } else {
            int len = c + 1;
            if (i + len > n || o + len > cap) return -1;
            while (len-- > 0) d[o++] = s[i++];
        }
    }
    return o;
}

int main(void) {
    static const int kinds[6] = {4, 2, 0, 1, 5, 4};
    u32 chk = 0;
    if (crc32(0, (const u8 *)"123456789", 9) != 0xCBF43926u) return 1;
    for (int b = 0; b < 6; b++) {
        int n = NMAX - 150 * b;
        rt_srand(701 + (u32)b);
        gen_data(in, n, kinds[b]);
        if (b == 5)                                     /* a ramp: constant delta, one long run */
            for (int i = n / 2; i < n; i++) in[i] = (u8)(i * 3);
        delta_encode(in, n, dl);
        int m = rle_encode(dl, n, packed);
        u32 c_plain = crc32(0, in, n), c_packed = crc32(0, packed, m);
        if (rle_decode(packed, m, un, NMAX) != n) return 2;
        delta_decode(un, n, out);
        if (memcmp(in, out, (size_t)n) != 0) return 3;
        if (crc32(0, out, n / 4) != crc32(0, in, n / 4)) return 4;
        chk = (chk * 31 + (u32)m) ^ c_plain ^ (c_packed >> 3);
    }
    return rt_check(chk, 0x2c4220);
}

#else
#error "compress.c: build with -DSEL=1..3"
#endif
