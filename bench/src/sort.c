/* BPX-1 bench: sorting. Select one with -DSEL=<n>:
     1 sort.quick   quicksort: median-of-3 pivot, Hoare partition, insertion sort below 12 elements
     2 sort.heap    heapsort (sift-down)
     3 sort.merge   top-down merge sort, skips the merge of runs that are already in order
     4 sort.shell   shell sort, gap sequence 701 301 132 57 23 10 4 1

   Each program sorts ROUNDS arrays of about N ints. The inputs cycle through five shapes: random,
   nearly sorted, few distinct values, descending with noise, concatenated sorted runs. After every
   sort the result is checked: non-decreasing, same sum and xor as the input (exit code 1 or 2). */
#include "rt.h"

#define N 2000
static int a[N];

/* Fill a[0..n) for this round. Returns sum and xor of the elements through the pointers. */
static void fill(int round, int n, u32 *sum, u32 *x) {
    int shape = round % 5;
    rt_srand(7001 + (u32)round);
    if (shape == 0) {
        for (int i = 0; i < n; i++) a[i] = (int)(rt_rand() >> 4) - (1 << 27);
    } else if (shape == 1) {
        for (int i = 0; i < n; i++) a[i] = i * 16 + (int)rt_rand_n(64);
        for (int k = 0; k < n / 20; k++) {
            int i = (int)rt_rand_n((u32)n), j = (int)rt_rand_n((u32)n), t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
    } else if (shape == 2) {
        for (int i = 0; i < n; i++) a[i] = (int)rt_rand_n(12) * 1000 - 5000;
    } else if (shape == 3) {
        for (int i = 0; i < n; i++) a[i] = (n - i) * 8 + (int)rt_rand_n(40);
    } else {
        int i = 0;
        while (i < n) {
            int len = 16 + (int)rt_rand_n(180), v = (int)rt_rand_n(100000);
            for (int k = 0; k < len && i < n; k++) {
                a[i++] = v;
                v += (int)rt_rand_n(50);
            }
        }
    }
    u32 s = 0, e = 0;
    for (int i = 0; i < n; i++) { s += (u32)a[i]; e ^= (u32)a[i]; }
    *sum = s;
    *x = e;
}

#if SEL == 1
#define ROUNDS 15
#define WANT 0x176a30
#define CUTOFF 12
static void insertion(int *x, int lo, int hi) {         /* sorts x[lo..hi] */
    for (int i = lo + 1; i <= hi; i++) {
        int v = x[i], j = i - 1;
        while (j >= lo && x[j] > v) { x[j + 1] = x[j]; j--; }
        x[j + 1] = v;
    }
}
static void quick(int *x, int lo, int hi) {             /* sorts x[lo..hi] */
    while (hi - lo >= CUTOFF) {
        int mid = lo + (hi - lo) / 2, t;
        if (x[mid] < x[lo]) { t = x[mid]; x[mid] = x[lo]; x[lo] = t; }
        if (x[hi] < x[lo]) { t = x[hi]; x[hi] = x[lo]; x[lo] = t; }
        if (x[hi] < x[mid]) { t = x[hi]; x[hi] = x[mid]; x[mid] = t; }
        int pivot = x[mid], i = lo, j = hi;
        while (i <= j) {
            while (x[i] < pivot) i++;
            while (x[j] > pivot) j--;
            if (i <= j) { t = x[i]; x[i] = x[j]; x[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { quick(x, lo, j); lo = i; }       /* recurse into the smaller part */
        else                 { quick(x, i, hi); hi = j; }
    }
    insertion(x, lo, hi);
}
static void sort(int *x, int n) { quick(x, 0, n - 1); }

#elif SEL == 2
#define ROUNDS 8
#define WANT 0x1c5276
static void sift_down(int *x, int root, int n) {
    int v = x[root];
    for (;;) {
        int child = 2 * root + 1;
        if (child >= n) break;
        if (child + 1 < n && x[child + 1] > x[child]) child++;
        if (x[child] <= v) break;
        x[root] = x[child];
        root = child;
    }
    x[root] = v;
}
static void sort(int *x, int n) {
    for (int i = n / 2 - 1; i >= 0; i--) sift_down(x, i, n);
    for (int end = n - 1; end > 0; end--) {
        int t = x[0];
        x[0] = x[end];
        x[end] = t;
        sift_down(x, 0, end);
    }
}

#elif SEL == 3
#define ROUNDS 9
#define WANT 0x2f8339
static int tmp[N / 2 + 1];
static void msort(int *x, int lo, int hi) {             /* sorts x[lo..hi) */
    if (hi - lo < 2) return;
    int mid = lo + (hi - lo) / 2;
    msort(x, lo, mid);
    msort(x, mid, hi);
    if (x[mid - 1] <= x[mid]) return;                   /* the two halves are already in order */
    int n1 = mid - lo;
    for (int i = 0; i < n1; i++) tmp[i] = x[lo + i];
    int i = 0, j = mid, k = lo;
    while (i < n1 && j < hi) {
        if (x[j] < tmp[i]) x[k++] = x[j++];
        else               x[k++] = tmp[i++];
    }
    while (i < n1) x[k++] = tmp[i++];
}
static void sort(int *x, int n) { msort(x, 0, n); }

#elif SEL == 4
#define ROUNDS 9
#define WANT 0x2f8339
static const int gaps[8] = {701, 301, 132, 57, 23, 10, 4, 1};
static void sort(int *x, int n) {
    for (int g = 0; g < 8; g++) {
        int gap = gaps[g];
        for (int i = gap; i < n; i++) {
            int v = x[i], j = i;
            while (j >= gap && x[j - gap] > v) { x[j] = x[j - gap]; j -= gap; }
            x[j] = v;
        }
    }
}

#else
#error "sort.c: build with -DSEL=1..4"
#endif

int main(void) {
    u32 chk = 0;
    for (int round = 0; round < ROUNDS; round++) {
        int n = N - (round * 137) % 400;                /* 1600 .. 2000 elements */
        u32 sum0, xor0, sum1 = 0, xor1 = 0;
        fill(round, n, &sum0, &xor0);
        sort(a, n);
        for (int i = 1; i < n; i++)
            if (a[i - 1] > a[i]) return 1;
        for (int i = 0; i < n; i++) { sum1 += (u32)a[i]; xor1 ^= (u32)a[i]; }
        if (sum1 != sum0 || xor1 != xor0) return 2;
        chk = chk * 31 + (u32)a[n / 3] + 7 * (u32)a[n - 1 - round];
    }
    return rt_check(chk, WANT);
}
