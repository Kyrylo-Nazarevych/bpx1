/* BPX-1 bench: searching. Select one with -DSEL=<n>:
     1 search.binary  binary search (three-way with early exit, and lower-bound) in sorted arrays of
                      several lengths, random keys: about half hits, half misses
     2 search.bst     unbalanced binary search tree: random insert / lookup / delete, with bursts of
                      ascending keys that grow long spines
     3 search.hash    open-addressing hash table (linear probing, tombstones, rebuild when clogged):
                      insert / lookup / remove on a pool of keys, high load so probes collide

   Every answer is compared with a simple reference (exit codes 1..9), then a checksum is checked. */
#include "rt.h"

#if SEL == 1
#define N 3000
#define QUERIES 1700
static int a[N];

static int bsearch3(const int *x, int n, int key) {     /* index of key or -1 */
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (x[mid] == key) return mid;
        if (x[mid] < key) lo = mid + 1;
        else              hi = mid - 1;
    }
    return -1;
}
static int lower_bound(const int *x, int n, int key) {  /* first index with x[i] >= key */
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (x[mid] < key) lo = mid + 1;
        else              hi = mid;
    }
    return lo;
}

int main(void) {
    static const int lens[8] = {3000, 1500, 100, 2047, 17, 700, 2999, 256};
    u32 chk = 0;
    int hits = 0;
    rt_srand(11);
    a[0] = (int)rt_rand_n(5);
    for (int i = 1; i < N; i++) a[i] = a[i - 1] + 1 + (int)rt_rand_n(6);     /* strictly increasing */
    for (int round = 0; round < 8; round++) {
        int n = lens[round];
        for (int q = 0; q < QUERIES; q++) {
            int key;
            if (rt_rand() & 1) key = a[rt_rand_n((u32)n)];                    /* certain hit */
            else               key = (int)rt_rand_n((u32)a[n - 1] + 10) - 4;  /* mostly a miss */
            int r = bsearch3(a, n, key);
            int lb = lower_bound(a, n, key);
            int found = lb < n && a[lb] == key;
            if ((r >= 0) != found) return 1;
            if (found && r != lb) return 2;
            hits += found;
            chk = chk * 31 + (u32)lb;
        }
    }
    return rt_check(chk + (u32)hits, 0x20a2ed);
}

#elif SEL == 2
#define KEYS 2048                   /* keys are 0 .. KEYS-1 */
#define OPS 15000
#define NIL (-1)
static int key_[KEYS], lf[KEYS], rg[KEYS];
static int root = NIL, free_head, count;
static u8 present[KEYS];            /* reference: is the key in the tree? */

static int bst_lookup(int k) {
    int n = root;
    while (n != NIL) {
        if (k == key_[n]) return 1;
        n = k < key_[n] ? lf[n] : rg[n];
    }
    return 0;
}
static int bst_insert(int k) {
    int *link = &root;
    while (*link != NIL) {
        int n = *link;
        if (k == key_[n]) return 0;
        link = k < key_[n] ? &lf[n] : &rg[n];
    }
    int n = free_head;
    free_head = lf[n];
    key_[n] = k;
    lf[n] = rg[n] = NIL;
    *link = n;
    count++;
    return 1;
}
static int bst_delete(int k) {
    int *link = &root;
    while (*link != NIL && key_[*link] != k) link = k < key_[*link] ? &lf[*link] : &rg[*link];
    int n = *link;
    if (n == NIL) return 0;
    if (lf[n] == NIL) *link = rg[n];
    else if (rg[n] == NIL) *link = lf[n];
    else {                          /* two children: the in-order successor takes the place of n */
        int *sl = &rg[n];
        while (lf[*sl] != NIL) sl = &lf[*sl];
        int s = *sl;
        *sl = rg[s];
        lf[s] = lf[n];
        rg[s] = rg[n];
        *link = s;
    }
    lf[n] = free_head;
    free_head = n;
    count--;
    return 1;
}
static int walk_prev, walk_count, walk_bad;
static void walk(int n) {           /* in-order: keys must come out ascending */
    while (n != NIL) {
        walk(lf[n]);
        if (key_[n] <= walk_prev) walk_bad = 1;
        walk_prev = key_[n];
        walk_count++;
        n = rg[n];
    }
}

int main(void) {
    u32 chk = 0;
    rt_srand(22);
    for (int i = 0; i < KEYS; i++) lf[i] = i + 1;       /* free list through lf[] */
    free_head = 0;
    for (int op = 0; op < OPS; op++) {
        u32 r = rt_rand_n(100);
        int k = (int)rt_rand_n(KEYS);
        if ((op & 127) == 127) {                        /* burst: 24 ascending keys */
            for (int i = 0; i < 24 && k + i < KEYS; i++) {
                if (bst_insert(k + i) == present[k + i]) return 1;
                present[k + i] = 1;
            }
        } else if (r < 40) {
            if (bst_insert(k) == present[k]) return 2;
            present[k] = 1;
        } else if (r < 75) {
            int f = bst_lookup(k);
            if (f != present[k]) return 3;
            chk = chk * 31 + (u32)f;
        } else {
            if (bst_delete(k) != present[k]) return 4;
            present[k] = 0;
        }
    }
    int expect = 0;
    for (int i = 0; i < KEYS; i++) expect += present[i];
    walk_prev = -1;
    walk(root);
    if (walk_bad) return 5;
    if (walk_count != expect || count != expect) return 6;
    return rt_check(chk * 31 + (u32)expect, 0x07b937);
}

#elif SEL == 3
#define TBITS 10
#define TSIZE (1 << TBITS)
#define TMASK (TSIZE - 1)
#define POOL 1150                   /* distinct keys in play */
#define OPS 38000
#define EMPTY 0u
#define TOMB 0xffffffffu
static u32 slot_key[TSIZE], slot_val[TSIZE], old_key[TSIZE], old_val[TSIZE];
static int live, filled;            /* filled = live + tombstones */
static u32 pool_key[POOL], ref_val[POOL];
static u8 ref_in[POOL];

static u32 hash(u32 k) { return (k * 2654435761u) >> (32 - TBITS); }

static int ht_find(u32 k) {         /* slot index or -1 */
    u32 i = hash(k);
    while (slot_key[i] != EMPTY) {
        if (slot_key[i] == k) return (int)i;
        i = (i + 1) & TMASK;
    }
    return -1;
}
static int ht_insert(u32 k, u32 v) {    /* 1 = new key, 0 = value replaced */
    u32 i = hash(k);
    int tomb = -1;
    while (slot_key[i] != EMPTY) {
        if (slot_key[i] == k) { slot_val[i] = v; return 0; }
        if (slot_key[i] == TOMB && tomb < 0) tomb = (int)i;
        i = (i + 1) & TMASK;
    }
    if (tomb >= 0) i = (u32)tomb;
    else filled++;
    slot_key[i] = k;
    slot_val[i] = v;
    live++;
    return 1;
}
static int ht_remove(u32 k) {
    int i = ht_find(k);
    if (i < 0) return 0;
    slot_key[i] = TOMB;
    live--;
    return 1;
}
static void ht_rebuild(void) {      /* drop the tombstones */
    for (int i = 0; i < TSIZE; i++) {
        old_key[i] = slot_key[i];
        old_val[i] = slot_val[i];
        slot_key[i] = EMPTY;
    }
    live = filled = 0;
    for (int i = 0; i < TSIZE; i++)
        if (old_key[i] != EMPTY && old_key[i] != TOMB) ht_insert(old_key[i], old_val[i]);
}

int main(void) {
    u32 chk = 0;
    int rebuilds = 0;
    rt_srand(33);
    for (u32 i = 0; i < POOL; i++) {                    /* distinct: odd multiplier is a bijection */
        u32 k = (i + 1) * 0x85EBCA6Bu ^ 0x01234567u;
        if (k == EMPTY || k == TOMB) k = 0x13579BDFu + i;
        pool_key[i] = k;
    }
    for (int op = 0; op < OPS; op++) {
        u32 r = rt_rand_n(100);
        u32 p = (rt_rand() & 3) ? rt_rand_n(POOL) : rt_rand_n(96);      /* a quarter goes to 96 hot keys */
        u32 k = pool_key[p];
        if (r < 40) {
            int i = ht_find(k);
            if ((i >= 0) != ref_in[p]) return 1;
            if (i >= 0) {
                if (slot_val[i] != ref_val[p]) return 2;
                chk = chk * 31 + slot_val[i];
            }
        } else if (r < 75) {
            u32 v = rt_rand();
            if (ht_insert(k, v) == ref_in[p]) return 3;
            ref_in[p] = 1;
            ref_val[p] = v;
            if (filled > TSIZE - TSIZE / 8) { ht_rebuild(); rebuilds++; }
        } else {
            if (ht_remove(k) != ref_in[p]) return 4;
            ref_in[p] = 0;
        }
    }
    int expect = 0;
    for (int i = 0; i < POOL; i++) expect += ref_in[i];
    if (expect != live) return 5;
    return rt_check(chk * 31 + (u32)(live + 1000 * rebuilds), 0x0f1e34);
}

#else
#error "search.c: build with -DSEL=1..3"
#endif
