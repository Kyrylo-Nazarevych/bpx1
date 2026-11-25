/* BPX-1 bench: the team's five "hard trace" programs (Branch Predictor Hackaton 1/plan/traces_hard/src/<name>.c).
   Select one with -DSEL=<n>:  1 coremarkish  2 sorts  3 strproc  4 graph  5 vm

   The algorithms are the team's, unchanged. Each block lists what was changed, which is only:
   (a) the result: main returned a result byte; here main returns 0 when the result is right (the runtime
       turns main's value into Spike's exit status), through rt_check() on the full result;
   (b) the amount of work, so that every program executes at least about 200,000 conditional branches. */
#include "rt.h"

#if SEL == 1
/* ---- coremarkish ----------------------------------------------------------------------------------
   Changes: 54 iterations of the main loop instead of 6 (the iteration number already seeds the list
   and the matrix); main returns 0 through rt_check() on acc and crc combined (acc * 65599 + crc)
   instead of returning (acc ^ crc) & 0xff. */
/* Hard trace 4: CoreMark-like mix: linked list find/reverse, matrix with conditionals, number state machine, CRC16. */
typedef struct node { struct node *next; int key, val; } node;
static node pool[64];
static node *build(int n, unsigned s) { node *h = 0; for (int i = 0; i < n; i++) { s = s * 69069u + 1; pool[i].key = (s >> 20) & 63; pool[i].val = i; pool[i].next = h; h = &pool[i]; } return h; }
static node *find(node *h, int key) { while (h && h->key != key) h = h->next; return h; }
static node *rev(node *h) { node *p = 0; while (h) { node *n = h->next; h->next = p; p = h; h = n; } return p; }
static unsigned short crc16(unsigned short crc, unsigned char d) { for (int i = 0; i < 8; i++) { int x = (crc ^ d) & 1; d >>= 1; crc >>= 1; if (x) crc ^= 0xA001; } return crc; }
static int m[12][12], r[12][12];
static int matrix(int seed) {
  int acc = 0;
  for (int i = 0; i < 12; i++) for (int j = 0; j < 12; j++) m[i][j] = (i * 7 + j * 3 + seed) % 17 - 8;
  for (int i = 0; i < 12; i++) for (int j = 0; j < 12; j++) {
    int s = 0; for (int k = 0; k < 12; k++) s += m[i][k] * m[k][j];
    r[i][j] = s; if (s > 10) acc += s; else if (s < -10) acc -= s; else acc++;
  }
  return acc;
}
enum { START, INT, S1, FLOAT, EXP, S2, SCI, INVALID };
static int state(const char *s, int *cnt) {
  int st = START;
  for (; *s && *s != ','; s++) {
    char c = *s;
    switch (st) {
    case START: if (c >= '0' && c <= '9') st = INT; else if (c == '+' || c == '-') st = S1; else if (c == '.') st = FLOAT; else st = INVALID; break;
    case S1: if (c >= '0' && c <= '9') st = INT; else if (c == '.') st = FLOAT; else st = INVALID; break;
    case INT: if (c == '.') st = FLOAT; else if (c < '0' || c > '9') st = INVALID; break;
    case FLOAT: if (c == 'E' || c == 'e') st = S2; else if (c < '0' || c > '9') st = INVALID; break;
    case S2: if (c == '+' || c == '-') st = EXP; else st = INVALID; break;
    case EXP: if (c >= '0' && c <= '9') st = SCI; else st = INVALID; break;
    case SCI: if (c < '0' || c > '9') st = INVALID; break;
    default: break;
    }
  }
  cnt[st]++; return st;
}
static const char *inp = "5012,1234,-874,+122,35.54,.1234,-110.7,+0.64,5.500e+3,-.123e-2,-87e+832,+0.6e-12,T0.3e-1F,-T.T++Tq,1T3.4e4z,34.0e-T^";
int main(void) {
  int cnt[8] = {0}; unsigned short crc = 0; int acc = 0;
  for (int it = 0; it < 54; it++) {
    node *h = build(40, it + 1);
    for (int k = 0; k < 64; k += 3) { node *f = find(h, k); acc += f ? f->val : -1; }
    h = rev(h); acc += h->key;
    acc += matrix(it);
    const char *p = inp; while (*p) { state(p, cnt); while (*p && *p != ',') p++; if (*p) p++; }
    for (int i = 0; i < 8; i++) crc = crc16(crc, (unsigned char)(cnt[i] + acc));
  }
  return rt_check((unsigned)acc * 65599u + crc, 0x1121d4);
}

#elif SEL == 2
/* ---- sorts ----------------------------------------------------------------------------------------
   Changes: the whole job (four sorts, 200 binary and 25 linear searches) runs 6 times with new numbers
   each time (the generator keeps running); main returns 0 through rt_check(s) instead of s & 0xff. */
/* Hard trace 2: five sorting and searching kernels on pseudo-random data. */
#define N 160
static int a[N], b[N], tmp[N];
static unsigned seed = 7;
static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7fff; }
static void fill(void) { for (int i = 0; i < N; i++) a[i] = rnd() % 500; }
static void insertion(int *x, int n) { for (int i = 1; i < n; i++) { int v = x[i], j = i - 1; while (j >= 0 && x[j] > v) { x[j+1] = x[j]; j--; } x[j+1] = v; } }
static void shell(int *x, int n) { for (int g = n/2; g > 0; g /= 2) for (int i = g; i < n; i++) { int v = x[i], j = i; while (j >= g && x[j-g] > v) { x[j] = x[j-g]; j -= g; } x[j] = v; } }
static void sift(int *x, int s, int n) { for (;;) { int c = 2*s+1; if (c >= n) return; if (c+1 < n && x[c+1] > x[c]) c++; if (x[s] >= x[c]) return; int t = x[s]; x[s] = x[c]; x[c] = t; s = c; } }
static void heap(int *x, int n) { for (int i = n/2-1; i >= 0; i--) sift(x, i, n); for (int e = n-1; e > 0; e--) { int t = x[0]; x[0] = x[e]; x[e] = t; sift(x, 0, e); } }
static void merge(int *x, int lo, int hi) { if (hi - lo < 2) return; int m = (lo+hi)/2; merge(x, lo, m); merge(x, m, hi); int i = lo, j = m, k = lo; while (i < m && j < hi) tmp[k++] = (x[i] <= x[j]) ? x[i++] : x[j++]; while (i < m) tmp[k++] = x[i++]; while (j < hi) tmp[k++] = x[j++]; for (k = lo; k < hi; k++) x[k] = tmp[k]; }
static int bsearch_(const int *x, int n, int key) { int lo = 0, hi = n-1; while (lo <= hi) { int m = (lo+hi) >> 1; if (x[m] < key) lo = m+1; else if (x[m] > key) hi = m-1; else return m; } return -1; }
static int lsearch(const int *x, int n, int key) { for (int i = 0; i < n; i++) if (x[i] == key) return i; return -1; }
static int check(const int *x, int n) { for (int i = 1; i < n; i++) if (x[i-1] > x[i]) return 0; return 1; }
int main(void) {
    int s = 0;
    for (int round = 0; round < 6; round++) {
    fill(); for (int i = 0; i < N; i++) b[i] = a[i]; insertion(b, N); s += check(b, N);
    fill(); for (int i = 0; i < N; i++) b[i] = a[i]; shell(b, N); s += check(b, N);
    fill(); for (int i = 0; i < N; i++) b[i] = a[i]; heap(b, N); s += check(b, N);
    fill(); for (int i = 0; i < N; i++) b[i] = a[i]; merge(b, 0, N); s += check(b, N);
    for (int q = 0; q < 200; q++) { int k = rnd() % 500; s += bsearch_(b, N, k) >= 0; if ((q & 7) == 0) s += lsearch(a, N, k) >= 0; }
    }
    return rt_check((unsigned)s, 0x0001a2);
}

#elif SEL == 3
/* ---- strproc --------------------------------------------------------------------------------------
   Changes: 18 passes over the text instead of 3; main returns 0 through rt_check of the same sum of
   counters instead of the sum & 0xff. */
/* Hard trace 3: string processing: tokenizer, keyword lookup, number parsing, wildcard match. */
static const char text[] =
  "int main ( void ) { if ( x > 10 && y < 20 ) return 42 ; while ( n -- ) sum += n * 3 ; "
  "for ( i = 0 ; i < 100 ; i ++ ) { if ( a [ i ] == key ) break ; else continue ; } "
  "switch ( c ) { case 1 : x = 0x1F ; break ; default : y = 077 ; } return sum ; } "
  "The quick brown fox jumps over the lazy dog 12345 times, and 678 more!";
static const char *kw[] = { "int", "void", "if", "return", "while", "for", "break", "else", "continue", "switch", "case", "default" };
static int isalpha_(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int isdigit_(int c) { return c >= '0' && c <= '9'; }
static int isspace_(int c) { return c == ' ' || c == '\n' || c == '\t'; }
static int streq(const char *a, const char *b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return b[n] == 0; }
static int parse_num(const char *p, int *len) {
  int v = 0, i = 0, base = 10;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; i = 2; } else if (p[0] == '0' && isdigit_(p[1])) { base = 8; i = 1; }
  for (;; i++) { int c = p[i], d; if (isdigit_(c)) d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else break; if (d >= base) break; v = v * base + d; }
  *len = i; return v;
}
static int match(const char *pat, const char *s) {
  if (*pat == 0) return *s == 0;
  if (*pat == '*') return match(pat + 1, s) || (*s && match(pat, s + 1));
  if (*s && (*pat == '?' || *pat == *s)) return match(pat + 1, s + 1);
  return 0;
}
static const char *pats[] = { "*fox*", "int*", "*dog*times*", "?nt main*", "*zzz*" };
int main(void) {
  int kwc = 0, ids = 0, nums = 0, ops = 0, numsum = 0, words = 0, upper = 0;
  for (int rep = 0; rep < 18; rep++) {
    const char *p = text;
    while (*p) {
      if (isspace_(*p)) { p++; continue; }
      if (isalpha_(*p)) {
        int n = 0; while (isalpha_(p[n]) || isdigit_(p[n])) n++;
        int k; for (k = 0; k < 12; k++) if (streq(p, kw[k], n)) break;
        if (k < 12) kwc++; else ids++;
        if (*p >= 'A' && *p <= 'Z') upper++;
        p += n; continue;
      }
      if (isdigit_(*p)) { int n; numsum += parse_num(p, &n); nums++; p += n; continue; }
      ops++; p++;
    }
    for (int i = 0; i < 5; i++) words += match(pats[i], text);
  }
  return rt_check((unsigned)(kwc + ids + nums + ops + numsum + words + upper), 0x03ae66);
}

#elif SEL == 4
/* ---- graph ----------------------------------------------------------------------------------------
   Changes: the whole job (new random graph, 4 BFS, 4 Dijkstra, union-find, compress 300 bytes) runs
   4 times (the generator keeps running, so graph and buffer differ each time); main returns 0 through
   rt_check(s) instead of s & 0xff. */
/* Hard trace 5: graph kernels (BFS, array Dijkstra, union-find) and a small LZ+RLE compressor. */
#define V 40
static unsigned char adj[V][V];
static int dist[V], q[V], par[V], done_[V];
static unsigned sd = 99;
static unsigned rnd(void) { sd ^= sd << 13; sd ^= sd >> 17; sd ^= sd << 5; return sd; }
static int find(int x) { while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; } return x; }
static int bfs(int s) {
  for (int i = 0; i < V; i++) dist[i] = -1;
  int h = 0, t = 0; q[t++] = s; dist[s] = 0;
  while (h < t) { int u = q[h++]; for (int v = 0; v < V; v++) if (adj[u][v] && dist[v] < 0) { dist[v] = dist[u] + 1; q[t++] = v; } }
  return t;
}
static int dijkstra(int s) {
  for (int i = 0; i < V; i++) { dist[i] = 1 << 20; done_[i] = 0; }
  dist[s] = 0; int tot = 0;
  for (int it = 0; it < V; it++) {
    int u = -1;
    for (int i = 0; i < V; i++) if (!done_[i] && (u < 0 || dist[i] < dist[u])) u = i;
    if (dist[u] >= 1 << 20) break;
    done_[u] = 1; tot += dist[u];
    for (int v = 0; v < V; v++) if (adj[u][v] && dist[u] + adj[u][v] < dist[v]) dist[v] = dist[u] + adj[u][v];
  }
  return tot;
}
static unsigned char buf[300], out[600];
static const char motif[] = "abcabcabdxyzzzzq";
static int compress(int n) {
  int o = 0;
  for (int i = 0; i < n; ) {
    int best = 0, off = 0;
    for (int j = (i > 32 ? i - 32 : 0); j < i; j++) { int l = 0; while (i + l < n && l < 15 && buf[j + l] == buf[i + l]) l++; if (l > best) { best = l; off = i - j; } }
    if (best >= 3) { out[o++] = 0x80 | best; out[o++] = off; i += best; }
    else { int r = 1; while (i + r < n && r < 8 && buf[i + r] == buf[i]) r++; if (r >= 2) { out[o++] = 0x40 | r; out[o++] = buf[i]; i += r; } else out[o++] = buf[i++]; }
  }
  return o;
}
int main(void) {
  int s = 0;
  for (int round = 0; round < 4; round++) {
  for (int i = 0; i < V; i++) for (int j = 0; j < V; j++) adj[i][j] = (i != j && (rnd() % 100) < 8) ? 1 + rnd() % 9 : 0;
  for (int k = 0; k < 4; k++) s += bfs(k * 7) + dijkstra(k * 5);
  for (int i = 0; i < V; i++) par[i] = i;
  for (int i = 0; i < V; i++) for (int j = i + 1; j < V; j++) if (adj[i][j]) { int a = find(i), b = find(j); if (a != b) par[a] = b; }
  for (int i = 0; i < V; i++) s += find(i) == i;
  for (int i = 0; i < 300; i++) buf[i] = (i % 37 < 20) ? motif[i % 16] : (rnd() & 7) + 'a';
  s += compress(300);
  }
  return rt_check((unsigned)s, 0x0023b1);
}

#elif SEL == 5
/* ---- vm -------------------------------------------------------------------------------------------
   Changes: 4 rounds instead of 1, each with larger limits patched into the two bytecode programs
   (primes below 150, 300, 450, 600; gcd sums over i < 40, 48, 56, 64 and j < 12, 15, 18, 21), so the
   two arrays are no longer const; the prime counts are checked (35, 62, 87, 109); main returns 0
   through rt_check(s) instead of s & 0xff (s is now unsigned, it only collects a checksum).
   The dispatcher is a switch, which GCC compiles to a jump table (one JALR per bytecode), as it did
   for the team: this is the one team program that is mostly indirect jumps. */
/* Hard trace 1: a small stack bytecode interpreter (switch dispatch) running two programs once (primes, gcd sums). */
enum { PUSH, LOAD, STORE, ADD, SUB, MUL, MOD, LT, EQ, JZ, JMP, DUP, POP, HALT, INC, SWAP };
static int st[64], mem[16];
static int run(const int *code) {
    int sp = 0, pc = 0, steps = 0;
    for (;;) {
        int op = code[pc++]; steps++;
        switch (op) {
        case PUSH: st[sp++] = code[pc++]; break;
        case LOAD: st[sp++] = mem[code[pc++]]; break;
        case STORE: mem[code[pc++]] = st[--sp]; break;
        case ADD: sp--; st[sp-1] += st[sp]; break;
        case SUB: sp--; st[sp-1] -= st[sp]; break;
        case MUL: sp--; st[sp-1] *= st[sp]; break;
        case MOD: sp--; st[sp-1] = st[sp] ? st[sp-1] % st[sp] : 0; break;
        case LT: sp--; st[sp-1] = st[sp-1] < st[sp]; break;
        case EQ: sp--; st[sp-1] = st[sp-1] == st[sp]; break;
        case JZ: if (st[--sp] == 0) pc = code[pc]; else pc++; break;
        case JMP: pc = code[pc]; break;
        case DUP: st[sp] = st[sp-1]; sp++; break;
        case POP: sp--; break;
        case INC: mem[code[pc++]]++; break;
        case SWAP: { int t = st[sp-1]; st[sp-1] = st[sp-2]; st[sp-2] = t; } break;
        case HALT: return steps;
        default: return -1;
        }
    }
}
/* count primes below 150 by trial division: mem0=n, mem1=d, mem2=count, mem3=isprime */
static int primes[] = {
    PUSH,2, STORE,0, PUSH,0, STORE,2,
    /*8*/  LOAD,0, PUSH,150, LT, JZ,67,
    /*15*/ PUSH,1, STORE,3, PUSH,2, STORE,1,
    /*23*/ LOAD,1, LOAD,1, MUL, LOAD,0, SWAP, LT, PUSH,0, EQ, JZ,57,
    /*37*/ LOAD,0, LOAD,1, MOD, PUSH,0, EQ, JZ,53, PUSH,0, STORE,3, JMP,57,
    /*53*/ INC,1, JMP,23,
    /*57*/ LOAD,3, JZ,63, INC,2,
    /*63*/ INC,0, JMP,8,
    /*67*/ HALT };
/* sum of gcd(i, j) for i < 40, j < 12 by Euclid: mem0=i, mem1=j, mem2=a, mem3=b, mem4=total */
static int gcds[] = {
    PUSH,1, STORE,0, PUSH,0, STORE,4,
    /*8*/  LOAD,0, PUSH,40, LT, JZ,66,
    /*15*/ PUSH,1, STORE,1,
    /*19*/ LOAD,1, PUSH,12, LT, JZ,62,
    /*26*/ LOAD,0, STORE,2, LOAD,1, STORE,3,
    /*34*/ LOAD,3, JZ,51,
    /*38*/ LOAD,2, LOAD,3, MOD, LOAD,3, STORE,2, STORE,3, JMP,34,
    /*51*/ LOAD,2, LOAD,4, ADD, STORE,4, INC,1, JMP,19,
    /*62*/ INC,0, JMP,8,
    /*66*/ HALT };
int main(void) {
    static const int n_primes[4] = {35, 62, 87, 109};
    unsigned s = 0;
    for (int r = 0; r < 4; r++) {
        primes[11] = 150 + 150 * r; gcds[11] = 40 + 8 * r; gcds[22] = 12 + 3 * r;
        s += run(primes); if (mem[2] != n_primes[r]) return 1;
        s += mem[2]; s += run(gcds); s += mem[4];
    }
    return rt_check(s, 0x04071f);
}

#else
#error "team_hard.c: build with -DSEL=1..5"
#endif
