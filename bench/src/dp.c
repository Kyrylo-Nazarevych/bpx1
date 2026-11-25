/* BPX-1 bench: dynamic programming. Select one with -DSEL=<n>:
     1 dp.levenshtein  edit distance (two rows, minimum of three) between words and between sentences
                       and edited copies of them
     2 dp.lcs          longest common subsequence: full table, then walk back to recover it
     3 dp.knapsack     0/1 knapsack by dynamic programming with reconstruction, and by depth-first
                       branch and bound with a greedy bound on a subset
     4 dp.lis          longest increasing subsequence by binary search over the tails, with
                       reconstruction, on sequences of several shapes

   Self-checks (exit codes 1..19): distance bounds and symmetry; the recovered subsequence is a
   subsequence of both strings and has the table's length; the chosen items fit and give the table's
   value, and branch and bound finds the same optimum; the recovered sequence increases and a
   quadratic method gives the same length. */
#include "rt.h"

#if SEL == 1
#define MAXL 96
#define NPAIRS 46
static int row0[MAXL + 1], row1[MAXL + 1];
static u8 s1[MAXL + 1], s2[MAXL + 1];

static const char *const words[] = {
    "branch", "brunch", "bench", "beach", "predict", "predictor", "prediction", "predicate", "history",
    "histogram", "table", "cable", "tablet", "counter", "encounter", "country", "taken", "token", "target",
    "garget", "pipeline", "pipelined", "register", "resistor", "address", "adders", "global", "local",
    "pattern", "lantern", "perceptron", "perception", "index", "indices", "hash", "cache", "cash", "loop"};
#define NWORDS ((int)(sizeof words / sizeof words[0]))

static int lev(const u8 *a, int la, const u8 *b, int lb) {
    int *prev = row0, *cur = row1;
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        u8 ca = a[i - 1];
        cur[0] = i;
        for (int j = 1; j <= lb; j++) {
            int best = prev[j - 1];
            if (ca != b[j - 1]) best++;                         /* substitute */
            if (prev[j] + 1 < best) best = prev[j] + 1;         /* delete */
            if (cur[j - 1] + 1 < best) best = cur[j - 1] + 1;   /* insert */
            cur[j] = best;
        }
        int *t = prev; prev = cur; cur = t;
    }
    return prev[lb];
}
static int sentence(u8 *s, int want) {              /* words separated by spaces, about `want` long */
    int n = 0;
    while (n < want) {
        const char *w = words[rt_rand_n(NWORDS)];
        while (*w) s[n++] = (u8)*w++;
        s[n++] = ' ';
    }
    n--;
    s[n] = 0;
    return n;
}

int main(void) {
    u32 chk = 0;
    rt_srand(2401);
    for (int i = 0; i < 30; i++)                    /* all pairs of the first 30 words, both ways */
        for (int j = i + 1; j < 30; j++) {
            const u8 *a = (const u8 *)words[i], *b = (const u8 *)words[j];
            int la = (int)strlen(words[i]), lb = (int)strlen(words[j]);
            int d = lev(a, la, b, lb);
            if (d != lev(b, lb, a, la)) return 1;
            if (d < 1 || d > (la > lb ? la : lb)) return 2;
            chk = chk * 31 + (u32)d;
        }
    for (int p = 0; p < NPAIRS; p++) {
        int l1 = sentence(s1, 36 + (int)rt_rand_n(40)), l2, d, edits = 0;
        if (p % 5 == 4) l2 = sentence(s2, 36 + (int)rt_rand_n(40));        /* unrelated */
        else {                                      /* a copy with up to 12 edits */
            l2 = l1;
            for (int i = 0; i <= l1; i++) s2[i] = s1[i];
            edits = 1 + (int)rt_rand_n(12);
            for (int e = 0; e < edits; e++) {
                u32 op = rt_rand_n(3);
                int at = (int)rt_rand_n((u32)l2);
                if (op == 0) s2[at] = (u8)('a' + rt_rand_n(26));
                else if (op == 1 && l2 > 8) {
                    for (int i = at; i < l2; i++) s2[i] = s2[i + 1];
                    l2--;
                } else if (l2 < MAXL - 1) {
                    for (int i = l2; i >= at; i--) s2[i + 1] = s2[i];
                    s2[at] = (u8)('a' + rt_rand_n(26));
                    l2++;
                }
            }
        }
        d = lev(s1, l1, s2, l2);
        if (d < (l1 > l2 ? l1 - l2 : l2 - l1)) return 3;
        if (d > (l1 > l2 ? l1 : l2)) return 4;
        if (p % 5 != 4 && d > edits) return 5;
        if (p % 8 == 0 && lev(s1, l1, s1, l1) != 0) return 6;
        chk = chk * 31 + (u32)d;
    }
    return rt_check(chk, 0x26cd15);
}

#elif SEL == 2
#define LMAX 170
#define NPAIRS 10
static u8 sa[LMAX + 2], sb[LMAX + 2], common[LMAX + 2];
static u16 tab[LMAX + 1][LMAX + 1];

static int lcs_table(int la, int lb) {
    for (int j = 0; j <= lb; j++) tab[0][j] = 0;
    for (int i = 1; i <= la; i++) {
        tab[i][0] = 0;
        for (int j = 1; j <= lb; j++) {
            if (sa[i - 1] == sb[j - 1]) tab[i][j] = (u16)(tab[i - 1][j - 1] + 1);
            else if (tab[i - 1][j] >= tab[i][j - 1]) tab[i][j] = tab[i - 1][j];
            else tab[i][j] = tab[i][j - 1];
        }
    }
    return tab[la][lb];
}
static int walk_back(int la, int lb) {              /* fills common[]; returns its length or -1 */
    int i = la, j = lb, n = tab[la][lb], k = n;
    while (i > 0 && j > 0) {
        if (sa[i - 1] == sb[j - 1]) {
            if (k == 0) return -1;
            common[--k] = sa[i - 1];
            i--;
            j--;
        } else if (tab[i - 1][j] >= tab[i][j - 1]) i--;
        else j--;
    }
    return k == 0 ? n : -1;
}
static int is_subsequence(const u8 *s, int ns, const u8 *t, int nt) {
    int i = 0;
    for (int j = 0; j < nt && i < ns; j++)
        if (t[j] == s[i]) i++;
    return i == ns;
}

int main(void) {
    u32 chk = 0;
    rt_srand(2501);
    for (int p = 0; p < NPAIRS; p++) {
        int alphabet = p & 1 ? 20 : 4;              /* DNA-like or protein-like */
        int la = 120 + (int)rt_rand_n(LMAX - 119), lb;
        for (int i = 0; i < la; i++) sa[i] = (u8)('A' + rt_rand_n((u32)alphabet));
        if (p % 3 == 2) {                           /* unrelated */
            lb = 120 + (int)rt_rand_n(LMAX - 119);
            for (int i = 0; i < lb; i++) sb[i] = (u8)('A' + rt_rand_n((u32)alphabet));
        } else {                                    /* a copy with substitutions, deletions and insertions */
            lb = 0;
            for (int i = 0; i < la && lb < LMAX - 1; i++) {
                u32 r = rt_rand_n(12);
                if (r == 0) continue;
                if (r == 1) sb[lb++] = (u8)('A' + rt_rand_n((u32)alphabet));
                sb[lb++] = r == 2 ? (u8)('A' + rt_rand_n((u32)alphabet)) : sa[i];
            }
        }
        int n = lcs_table(la, lb);
        if (walk_back(la, lb) != n) return 1;
        if (!is_subsequence(common, n, sa, la)) return 2;
        if (!is_subsequence(common, n, sb, lb)) return 3;
        if (n > la || n > lb) return 4;
        chk = chk * 31 + (u32)n;
        for (int i = 0; i < n; i += 7) chk = chk * 31 + common[i];
    }
    return rt_check(chk, 0x055ab8);
}

#elif SEL == 3
#define NI 48
#define CAP 600
#define NB 22                           /* items given to branch and bound */
#define ROUNDS 6
static int wgt[NI], val[NI], dp[CAP + 1], ord[NB];
static u8 keep[NI][CAP + 1];
static int bb_best, bb_nodes;

static int knapsack(int n, int cap) {
    for (int w = 0; w <= cap; w++) dp[w] = 0;
    for (int i = 0; i < n; i++) {
        int wi = wgt[i], vi = val[i], w = cap;
        for (; w >= wi; w--) {
            int with = dp[w - wi] + vi;
            if (with > dp[w]) { dp[w] = with; keep[i][w] = 1; }
            else keep[i][w] = 0;
        }
        for (; w >= 0; w--) keep[i][w] = 0;
    }
    return dp[cap];
}
/* Items ord[k..NB) are still open; `room` capacity is left; `value` is what has been taken. */
static void bb(int k, int room, int value) {
    bb_nodes++;
    if (value > bb_best) bb_best = value;
    if (k == NB) return;
    int bound = value, left = room;                 /* greedy bound: whole items, then a fraction */
    for (int i = k; i < NB; i++) {
        int it = ord[i];
        if (wgt[it] <= left) { left -= wgt[it]; bound += val[it]; }
        else { bound += val[it] * left / wgt[it]; break; }
    }
    if (bound <= bb_best) return;
    int it = ord[k];
    if (wgt[it] <= room) bb(k + 1, room - wgt[it], value + val[it]);
    bb(k + 1, room, value);
}

int main(void) {
    u32 chk = 0;
    for (int round = 0; round < ROUNDS; round++) {
        int total_w = 0, total_v = 0, small_w = 0;
        rt_srand(2601 + (u32)round);
        for (int i = 0; i < NI; i++) {
            wgt[i] = 5 + (int)rt_rand_n(116);
            val[i] = round % 3 == 0 ? wgt[i] + (int)rt_rand_n(25) : 10 + (int)rt_rand_n(190);
        }
        int best = knapsack(NI, CAP);
        for (int i = NI - 1, w = CAP; i >= 0; i--)          /* which items were taken? */
            if (keep[i][w]) { w -= wgt[i]; total_w += wgt[i]; total_v += val[i]; }
        if (total_w > CAP) return 1;
        if (total_v != best) return 2;
        chk = chk * 31 + (u32)best;

        for (int i = 0; i < NB; i++) {                      /* order by value per weight, best first */
            int j = i;
            while (j > 0 && val[ord[j - 1]] * wgt[i] < val[i] * wgt[ord[j - 1]]) { ord[j] = ord[j - 1]; j--; }
            ord[j] = i;
            small_w += wgt[i];
        }
        int small_cap = small_w / 2 < CAP ? small_w / 2 : CAP;
        bb_best = bb_nodes = 0;
        bb(0, small_cap, 0);
        if (bb_best != knapsack(NB, small_cap)) return 3;
        chk = chk * 31 + (u32)bb_best + (u32)bb_nodes;
    }
    return rt_check(chk, 0x0b78fc);
}

#elif SEL == 4
#define NMAX 3000
#define NQUAD 300
#define ROUNDS 8
static int seq[NMAX], tails[NMAX], tail_at[NMAX], pred[NMAX], chain[NMAX], quad[NQUAD];

static int lis(int n) {                 /* length of the longest strictly increasing subsequence */
    int len = 0;
    for (int i = 0; i < n; i++) {
        int x = seq[i], lo = 0, hi = len;
        while (lo < hi) {               /* first tail >= x */
            int mid = (lo + hi) >> 1;
            if (tails[mid] < x) lo = mid + 1;
            else                hi = mid;
        }
        tails[lo] = x;
        tail_at[lo] = i;
        pred[i] = lo > 0 ? tail_at[lo - 1] : -1;
        if (lo == len) len++;
    }
    return len;
}

int main(void) {
    u32 chk = 0;
    for (int round = 0; round < ROUNDS; round++) {
        int n = NMAX - 150 * round, v = 500000;
        rt_srand(2701 + (u32)round);
        for (int i = 0; i < n; i++) {
            int shape = round % 7;
            if (shape == 0) seq[i] = (int)rt_rand_n(1000000);
            else if (shape == 1) seq[i] = i * 10 + (int)rt_rand_n(400);          /* rising, noisy */
            else if (shape == 2) seq[i] = i % 97 * 50 + (int)rt_rand_n(30);      /* sawtooth */
            else if (shape == 3) { v += (int)rt_rand_n(201) - 100; seq[i] = v; } /* random walk */
            else if (shape == 4) seq[i] = (int)rt_rand_n(40);                    /* few values */
            else if (shape == 5) seq[i] = (n - i) * 10 + (int)rt_rand_n(600);    /* falling, noisy */
            else seq[i] = i & 1 ? i * 3 : 20000 + i;                             /* two rising runs */
        }
        if (round < 2) {                /* cross-check on a prefix with the quadratic method */
            int best = 0;
            for (int i = 0; i < NQUAD; i++) {
                int b = 1;
                for (int j = 0; j < i; j++)
                    if (seq[j] < seq[i] && quad[j] + 1 > b) b = quad[j] + 1;
                quad[i] = b;
                if (b > best) best = b;
            }
            if (lis(NQUAD) != best) return 1;
        }
        int len = lis(n), k = tail_at[len - 1];
        for (int j = len - 1; j >= 0; j--) {        /* recover one longest subsequence */
            if (k < 0) return 2;
            chain[j] = seq[k];
            k = pred[k];
        }
        if (k != -1) return 3;
        for (int j = 1; j < len; j++)
            if (chain[j - 1] >= chain[j]) return 4;
        chk = chk * 31 + (u32)len + (u32)chain[len / 2];
    }
    return rt_check(chk, 0x27c90a);
}

#else
#error "dp.c: build with -DSEL=1..4"
#endif
