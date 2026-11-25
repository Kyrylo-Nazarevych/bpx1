/* BPX-1 bench: the team's six small programs (quad-branch-predictor/tools/precomputed/<name>_O2/<name>.c).
   Select one with -DSEL=<n>:  1 interpreter  2 matmul  3 nqueens  4 quicksort  5 sort_and_search  6 strsearch

   The algorithms are the team's, unchanged. Each block lists what was changed, which is only:
   (a) the result: main returned a result byte; here main returns 0 when the result is right (the runtime
       turns main's value into Spike's exit status), through a cheap check and rt_check() on a checksum;
   (b) the amount of work, so that every program executes at least about 200,000 conditional branches. */
#include "rt.h"

#if SEL == 1
/* ---- interpreter ----------------------------------------------------------------------------------
   Changes: the bytecode loop count (code[1], was the constant 400) is patched before each of 8 runs
   (400, 500, ... 1100 iterations; the sum of squares still fits in an int), so `code` is no longer const;
   each result is checked against n(n+1)(2n+1)/6; main returns 0 instead of run() & 0x7f.
   One more change: run() is marked RT_IF_CHAIN. The team's own -O2 trace has only 2 static branches,
   because GCC -O2 rewrote this if-chain as a jump table; the mark keeps the chain a chain, which is
   what the comment below says it is meant to be. */
/* Workload: a tiny bytecode interpreter running a loop of 400 iterations (sum of squares).
   The decoder is an if-chain on purpose: a switch would become a jump table, an indirect jump the chip does not
   predict. Every decoded instruction walks the chain, so many static branches run with patterns that follow the
   bytecode, and they compete for the chip's 64 table entries and 8 perceptrons. */
enum { PUSH, LOAD, STORE, ADD, SUB, MUL, JNZ, HALT };

static int code[] = {
    PUSH, 400, STORE, 0,                   /* r0 = 400   loop counter */
    PUSH, 0, STORE, 1,                     /* r1 = 0     sum */
    LOAD, 0, LOAD, 0, MUL,                 /* loop: push r0 * r0 */
    LOAD, 1, ADD, STORE, 1,                /* r1 += it */
    LOAD, 0, PUSH, 1, SUB, STORE, 0,       /* r0 -= 1 */
    LOAD, 0, JNZ, 8,                       /* while r0 != 0 */
    HALT
};

RT_IF_CHAIN static int run(void) {
    int stack[16], sp = 0, reg[4] = {0, 0, 0, 0}, pc = 0;
    for (;;) {
        int op = code[pc++];
        if (op == PUSH)       stack[sp++] = code[pc++];
        else if (op == LOAD)  stack[sp++] = reg[code[pc++]];
        else if (op == STORE) reg[code[pc++]] = stack[--sp];
        else if (op == ADD)   { sp--; stack[sp - 1] += stack[sp]; }
        else if (op == SUB)   { sp--; stack[sp - 1] -= stack[sp]; }
        else if (op == MUL)   { sp--; stack[sp - 1] *= stack[sp]; }
        else if (op == JNZ)   { int t = code[pc++]; if (stack[--sp] != 0) pc = t; }
        else                  return reg[1];
    }
}

int main(void) {
    unsigned sum = 0;
    for (int rep = 0; rep < 8; rep++) {
        unsigned n = 400 + 100 * (unsigned)rep;
        code[1] = (int)n;
        unsigned r = (unsigned)run();
        if (r != n * (n + 1) * (2 * n + 1) / 6) return 1;
        sum = sum * 31 + r;
    }
    return rt_check(sum, 0x2f997f);
}

#elif SEL == 2
/* ---- matmul ---------------------------------------------------------------------------------------
   Changes: 120 multiplications instead of 3; the inputs are refilled before each one with the repeat
   number as seed (was: filled once with i + j and i - j); the checksum picks c[rep % N][N - 1 - rep % N]
   (was c[rep][N - 1 - rep]); main returns 0 instead of check & 0x7f. */
/* Workload: 12 x 12 integer matrix multiply, three times.
   Only counted loops with fixed trip counts: the most regular code there is. Every predictor should be near 100%,
   missing little more than the loop exits (inner loop of 12, like pattern_loop12). Compare with sort_and_search,
   whose data-dependent compare is what makes it hard. */
#define N 12
static int a[N][N], b[N][N], c[N][N];

static void multiply(void) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            int s = 0;
            for (int k = 0; k < N; k++) s += a[i][k] * b[k][j];
            c[i][j] = s;
        }
}

int main(void) {
    unsigned check = 0;
    for (int rep = 0; rep < 120; rep++) {
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) { a[i][j] = i + j + rep; b[i][j] = i - j - rep; }
        multiply();
        check = check * 31 + (unsigned)c[rep % N][N - 1 - rep % N];
    }
    return rt_check(check, 0x130f74);
}

#elif SEL == 3
/* ---- nqueens --------------------------------------------------------------------------------------
   Changes: N = 9 (352 solutions) instead of N = 7 (40 solutions); main returns 0 when the count is 352
   instead of returning the count. */
/* Workload: N-queens by backtracking (N = 7, 40 solutions).
   Recursion and early exits: the "is this square attacked?" loop stops at the first conflict, so its exit depends
   on the board, and the recursion depth changes the history the predictors see. A realistic mix between the
   pattern programs and sorting. */
#define N 9
static int col[N];                         /* col[r] = column of the queen in row r */

static int safe(int r, int c) {
    for (int i = 0; i < r; i++) {
        int d = col[i] - c;
        if (d == 0 || d == r - i || d == i - r) return 0;
    }
    return 1;
}

static int solve(int r) {
    if (r == N) return 1;
    int count = 0;
    for (int c = 0; c < N; c++)
        if (safe(r, c)) { col[r] = c; count += solve(r + 1); }
    return count;
}

int main(void) {
    return solve(0) == 352 ? 0 : 1;
}

#elif SEL == 4
/* ---- quicksort ------------------------------------------------------------------------------------
   Changes: N = 3000 numbers instead of 300, sorted in 3 rounds with new numbers each round (the
   generator keeps running); main returns 0 through a checksum of three elements per round instead of
   data[N / 2] & 0x7f (99 if not sorted, as before). */
/* Workload: recursive quicksort (Lomuto partition) of 300 pseudo-random numbers, then a check that they are sorted.
   Compare with bubble sort in sort_and_search: the partition compare (line 17) is just as data-dependent (about
   50/50, the hard branch), but quicksort runs it far fewer times, and the recursion adds its own branches. */
#define N 3000
static int data[N];
static unsigned int state = 521288629u;

static unsigned int rnd(void) {            /* xorshift32 pseudo-random numbers */
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}

static void quicksort(int *a, int lo, int hi) {
    while (lo < hi) {
        int pivot = a[hi], p = lo;
        for (int i = lo; i < hi; i++) {
            if (a[i] < pivot) { int t = a[i]; a[i] = a[p]; a[p] = t; p++; }
        }
        int t = a[p]; a[p] = a[hi]; a[hi] = t;
        if (p - lo < hi - p) { quicksort(a, lo, p - 1); lo = p + 1; }   /* recurse into the smaller side */
        else                 { quicksort(a, p + 1, hi); hi = p - 1; }
    }
}

int main(void) {
    unsigned sum = 0;
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < N; i++) data[i] = rnd() % 1000;
        quicksort(data, 0, N - 1);
        for (int i = 1; i < N; i++)
            if (data[i - 1] > data[i]) return 99;   /* not sorted: should never happen */
        sum = sum * 31 + (unsigned)(data[N / 2] + 3 * data[N / 7] + 5 * data[N - 1]);
    }
    return rt_check(sum, 0x19c43e);
}

#elif SEL == 5
/* ---- sort_and_search ------------------------------------------------------------------------------
   Changes: the whole job (fill, bubble sort, 300 binary searches, count evens) runs 6 times with new
   numbers each time (the generator keeps running); main returns 0 through a checksum of found + evens
   of every round instead of (found + evens) & 0x7f. */
/* Example workload: no printf, no OS. Result is returned from main (exit code). */
#define N 200
static int data[N];
static unsigned int seed = 12345;
static unsigned int rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

static void bubble_sort(int *a, int n) {
    for (int i = 0; i < n - 1; i++) {
        int swapped = 0;
        for (int j = 0; j < n - 1 - i; j++)
            if (a[j] > a[j + 1]) { int t = a[j]; a[j] = a[j + 1]; a[j + 1] = t; swapped = 1; }
        if (!swapped) break;
    }
}
static int binary_search(const int *a, int n, int key) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a[mid] == key) return mid;
        if (a[mid] < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
int main(void) {
    unsigned sum = 0;
    for (int round = 0; round < 6; round++) {
        for (int i = 0; i < N; i++) data[i] = rnd() % 1000;
        bubble_sort(data, N);
        int found = 0;
        for (int q = 0; q < 300; q++) if (binary_search(data, N, rnd() % 1000) >= 0) found++;
        int evens = 0;
        for (int i = 0; i < N; i++) if ((data[i] & 1) == 0) evens++;
        sum = sum * 31 + (unsigned)(found + evens);
    }
    return rt_check(sum, 0x378b5f);
}

#elif SEL == 6
/* ---- strsearch ------------------------------------------------------------------------------------
   Changes: LEN = 10,000 letters instead of 2,000, searched in 8 rounds with a new text each round
   (the generator keeps running); main returns 0 through a checksum of the eight match counts instead
   of count_matches() & 0x7f. */
/* Workload: naive string search for "abca" in 2,000 pseudo-random letters from {a, b, c, d}.
   With only 4 letters, partial matches are common: the compare loop (line 19) runs 1, 2, 3 or 4 steps depending on
   the text, a data-dependent early exit like the binary search in sort_and_search. The "found it" branch is rare,
   which bias alone predicts well. */
#define LEN 10000
static char text[LEN];
static const char pat[4] = {'a', 'b', 'c', 'a'};
static unsigned int state = 362436069u;

static unsigned int rnd(void) {            /* xorshift32 pseudo-random numbers */
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}

static int count_matches(void) {
    int found = 0;
    for (int i = 0; i + 4 <= LEN; i++) {
        int k = 0;
        while (k < 4 && text[i + k] == pat[k]) k++;
        if (k == 4) found++;
    }
    return found;
}

int main(void) {
    unsigned sum = 0;
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < LEN; i++) text[i] = (char)('a' + (rnd() & 3));
        sum = sum * 31 + (unsigned)count_matches();
    }
    return rt_check(sum, 0x0e9e9b);
}

#else
#error "team_small.c: build with -DSEL=1..6"
#endif
