/* BPX-1 bench: graph algorithms. Select one with -DSEL=<n>:
     1 graph.components  connected components three ways (BFS, recursive DFS, union-find with path
                         halving) on random sparse graphs below, at and above the density where a
                         giant component appears
     2 graph.dijkstra    single-source shortest paths with a binary heap (lazy deletion) on random,
                         grid and ring-with-shortcuts graphs
     3 graph.dense       Floyd-Warshall on small dense graphs, checked by Bellman-Ford; topological
                         sort of random DAGs (Kahn and DFS), longest path, cycle detection

   Self-checks (exit codes 1..19): the three component labelings agree; every Dijkstra result
   passes a complete optimality check over all edges; Floyd-Warshall rows equal Bellman-Ford; both
   topological orders are valid and a planted cycle is found by both methods. */
#include "rt.h"

#if SEL == 1
#define VMAX 1000
#define EMAX 2600
#define ROUNDS 14
static int eu[EMAX], ev[EMAX];
static int start[VMAX + 1], adj[2 * EMAX], next_slot[VMAX];
static int comp_bfs[VMAX], comp_dfs[VMAX], level[VMAX], queue[VMAX], parent[VMAX];
static u8 rnk[VMAX];
static int nv, ne, biggest;

static void build(int v, int e) {                   /* random edges, then adjacency lists */
    nv = v;
    ne = e;
    for (int i = 0; i < e; i++) {
        int a = (int)rt_rand_n((u32)v), b = (int)rt_rand_n((u32)v - 1);
        if (b >= a) b++;                            /* no self-loops */
        eu[i] = a;
        ev[i] = b;
    }
    for (int i = 0; i <= v; i++) start[i] = 0;
    for (int i = 0; i < e; i++) { start[eu[i] + 1]++; start[ev[i] + 1]++; }
    for (int i = 0; i < v; i++) start[i + 1] += start[i];
    for (int i = 0; i < v; i++) next_slot[i] = start[i];
    for (int i = 0; i < e; i++) {
        adj[next_slot[eu[i]]++] = ev[i];
        adj[next_slot[ev[i]]++] = eu[i];
    }
}
static int bfs_all(void) {
    int ncomp = 0;
    biggest = 0;
    for (int i = 0; i < nv; i++) comp_bfs[i] = -1;
    for (int s = 0; s < nv; s++) {
        if (comp_bfs[s] >= 0) continue;
        int head = 0, tail = 0;
        comp_bfs[s] = ncomp;
        level[s] = 0;
        queue[tail++] = s;
        while (head < tail) {
            int u = queue[head++];
            for (int k = start[u]; k < start[u + 1]; k++) {
                int w = adj[k];
                if (comp_bfs[w] < 0) {
                    comp_bfs[w] = ncomp;
                    level[w] = level[u] + 1;
                    queue[tail++] = w;
                }
            }
        }
        if (tail > biggest) biggest = tail;
        ncomp++;
    }
    return ncomp;
}
static void dfs(int u, int c) {
    comp_dfs[u] = c;
    for (int k = start[u]; k < start[u + 1]; k++) {
        int w = adj[k];
        if (comp_dfs[w] < 0) dfs(w, c);
    }
}
static int find(int x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];              /* path halving */
        x = parent[x];
    }
    return x;
}
static int unite(int a, int b) {
    a = find(a);
    b = find(b);
    if (a == b) return 0;
    if (rnk[a] < rnk[b]) { int t = a; a = b; b = t; }
    parent[b] = a;
    if (rnk[a] == rnk[b]) rnk[a]++;
    return 1;
}

int main(void) {
    static const int ratio[5] = {4, 7, 10, 15, 26};     /* edges per 10 vertices */
    u32 chk = 0;
    for (int round = 0; round < ROUNDS; round++) {
        int v = 600 + 25 * round, e = v * ratio[round % 5] / 10;
        rt_srand(1601 + (u32)round);
        build(v, e);
        int c1 = bfs_all(), c2 = 0, c3 = v, depth = 0;
        for (int i = 0; i < v; i++) comp_dfs[i] = -1;
        for (int s = 0; s < v; s++)
            if (comp_dfs[s] < 0) dfs(s, c2++);
        for (int i = 0; i < v; i++) { parent[i] = i; rnk[i] = 0; }
        for (int i = 0; i < e; i++) c3 -= unite(eu[i], ev[i]);
        if (c1 != c2) return 1;
        if (c1 != c3) return 2;
        for (int i = 0; i < v; i++) {
            if (comp_bfs[i] != comp_dfs[i]) return 3;
            if (comp_bfs[find(i)] != comp_bfs[i]) return 4;
            depth += level[i];
        }
        for (int i = 0; i < e; i++)
            if (find(eu[i]) != find(ev[i])) return 5;
        chk = chk * 31 + (u32)c1 * 7 + (u32)biggest * 3 + (u32)depth;
    }
    return rt_check(chk, 0x0052ea);
}

#elif SEL == 2
#define VMAX 600
#define EMAX 4200
#define INF 0x3fffffff
#define ROUNDS 7
static int ea[EMAX], eb[EMAX], ew[EMAX];            /* edges as generated: a -> b, weight */
static int start[VMAX + 1], next_slot[VMAX], to[EMAX], wt[EMAX], from[EMAX];
static int dist[VMAX], pred_edge[VMAX];
static int heap_d[EMAX + 2], heap_v[EMAX + 2], heap_n;
static int nv, ne;

static void add_edge(int a, int b, int w) {
    ea[ne] = a;
    eb[ne] = b;
    ew[ne] = w;
    ne++;
}
static void build(int kind) {
    ne = 0;
    if (kind == 0) {                                /* random, about 5 edges per vertex */
        nv = 520;
        for (int i = 0; i < 2600; i++) {
            int a = (int)rt_rand_n((u32)nv), b = (int)rt_rand_n((u32)nv);
            add_edge(a, b, 1 + (int)rt_rand_n(100));
        }
    } else if (kind == 1) {                         /* 24 x 24 grid, both directions, own weights */
        nv = 576;
        for (int y = 0; y < 24; y++)
            for (int x = 0; x < 24; x++) {
                int u = y * 24 + x;
                if (x + 1 < 24) { add_edge(u, u + 1, 1 + (int)rt_rand_n(20)); add_edge(u + 1, u, 1 + (int)rt_rand_n(20)); }
                if (y + 1 < 24) { add_edge(u, u + 24, 1 + (int)rt_rand_n(20)); add_edge(u + 24, u, 1 + (int)rt_rand_n(20)); }
            }
    } else {                                        /* ring with random shortcuts */
        nv = 600;
        for (int i = 0; i < nv; i++) add_edge(i, i + 1 < nv ? i + 1 : 0, 1 + (int)rt_rand_n(10));
        for (int i = 0; i < 900; i++) {
            int a = (int)rt_rand_n((u32)nv), b = (int)rt_rand_n((u32)nv);
            add_edge(a, b, 5 + (int)rt_rand_n(60));
        }
    }
    for (int i = 0; i <= nv; i++) start[i] = 0;
    for (int i = 0; i < ne; i++) start[ea[i] + 1]++;
    for (int i = 0; i < nv; i++) start[i + 1] += start[i];
    for (int i = 0; i < nv; i++) next_slot[i] = start[i];
    for (int i = 0; i < ne; i++) {
        int k = next_slot[ea[i]]++;
        to[k] = eb[i];
        wt[k] = ew[i];
        from[k] = ea[i];
    }
}
static void heap_push(int d, int v) {
    int i = heap_n++;
    while (i > 0) {
        int p = (i - 1) >> 1;
        if (heap_d[p] <= d) break;
        heap_d[i] = heap_d[p];
        heap_v[i] = heap_v[p];
        i = p;
    }
    heap_d[i] = d;
    heap_v[i] = v;
}
static int heap_pop(int *d) {                       /* returns the vertex, *d its key */
    int v = heap_v[0], ld, lv, i = 0;
    *d = heap_d[0];
    heap_n--;
    ld = heap_d[heap_n];
    lv = heap_v[heap_n];
    for (;;) {
        int c = 2 * i + 1;
        if (c >= heap_n) break;
        if (c + 1 < heap_n && heap_d[c + 1] < heap_d[c]) c++;
        if (heap_d[c] >= ld) break;
        heap_d[i] = heap_d[c];
        heap_v[i] = heap_v[c];
        i = c;
    }
    heap_d[i] = ld;
    heap_v[i] = lv;
    return v;
}
static int dijkstra(int src) {                      /* returns the number of vertices reached */
    int reached = 0;
    for (int i = 0; i < nv; i++) { dist[i] = INF; pred_edge[i] = -1; }
    dist[src] = 0;
    heap_n = 0;
    heap_push(0, src);
    while (heap_n > 0) {
        int d, u = heap_pop(&d);
        if (d > dist[u]) continue;                  /* stale entry */
        reached++;
        for (int k = start[u]; k < start[u + 1]; k++) {
            int v = to[k], nd = d + wt[k];
            if (nd < dist[v]) {
                dist[v] = nd;
                pred_edge[v] = k;
                heap_push(nd, v);
            }
        }
    }
    return reached;
}
/* dist[] is right if and only if: dist[src] = 0, no edge can shorten anything, and every other
   reached vertex has an edge that gives exactly its distance. */
static int verify(int src) {
    if (dist[src] != 0) return 1;
    for (int k = 0; k < ne; k++)
        if (dist[from[k]] != INF && dist[from[k]] + wt[k] < dist[to[k]]) return 2;
    for (int v = 0; v < nv; v++) {
        if (v == src || dist[v] == INF) continue;
        int k = pred_edge[v];
        if (k < 0 || to[k] != v || dist[from[k]] + wt[k] != dist[v]) return 3;
    }
    return 0;
}

int main(void) {
    u32 chk = 0;
    for (int round = 0; round < ROUNDS; round++) {
        rt_srand(1701 + (u32)round);
        build(round % 3);
        for (int s = 0; s < 2; s++) {
            int src = (int)rt_rand_n((u32)nv);
            int reached = dijkstra(src);
            int e = verify(src);
            if (e) return e;
            u32 sum = 0;
            for (int v = 0; v < nv; v++)
                if (dist[v] != INF) sum += (u32)dist[v];
            chk = chk * 31 + sum + (u32)reached;
        }
    }
    return rt_check(chk, 0x118ed9);
}

#elif SEL == 3
#define FV 34
#define FE (FV * FV)
#define INF 0x3fffffff
#define TV 64
#define TE 400
static int d[FV][FV], bf[FV];
static int fu[FE], fv[FE], fw[FE];
static int tu[TE + 1], tv[TE + 1], tstart[TV + 1], tslot[TV], tadj[TE + 1];
static int indeg[TV], order1[TV], order2[TV], posn[TV], perm[TV], longest[TV];
static u8 state[TV];
static int tn, te, cycle, fill_at;

static void floyd(int n) {
    for (int k = 0; k < n; k++)
        for (int i = 0; i < n; i++) {
            int dik = d[i][k];
            if (dik == INF) continue;
            for (int j = 0; j < n; j++) {
                int t = dik + d[k][j];
                if (t < d[i][j]) d[i][j] = t;
            }
        }
}
static void bellman(int src, int n, int ne) {
    for (int i = 0; i < n; i++) bf[i] = INF;
    bf[src] = 0;
    for (int pass = 0; pass < n; pass++) {
        int changed = 0;
        for (int e = 0; e < ne; e++)
            if (bf[fu[e]] != INF && bf[fu[e]] + fw[e] < bf[fv[e]]) {
                bf[fv[e]] = bf[fu[e]] + fw[e];
                changed = 1;
            }
        if (!changed) break;
    }
}

static void topo_build(void) {
    for (int i = 0; i <= tn; i++) tstart[i] = 0;
    for (int i = 0; i < te; i++) tstart[tu[i] + 1]++;
    for (int i = 0; i < tn; i++) tstart[i + 1] += tstart[i];
    for (int i = 0; i < tn; i++) tslot[i] = tstart[i];
    for (int i = 0; i < te; i++) tadj[tslot[tu[i]]++] = tv[i];
}
static int kahn(int *order) {                       /* returns how many vertices could be ordered */
    int head = 0, tail = 0;
    for (int i = 0; i < tn; i++) indeg[i] = 0;
    for (int i = 0; i < te; i++) indeg[tv[i]]++;
    for (int i = 0; i < tn; i++)
        if (indeg[i] == 0) order[tail++] = i;
    while (head < tail) {
        int u = order[head++];
        for (int k = tstart[u]; k < tstart[u + 1]; k++)
            if (--indeg[tadj[k]] == 0) order[tail++] = tadj[k];
    }
    return tail;
}
static void visit(int u) {
    state[u] = 1;                                   /* on the current path */
    for (int k = tstart[u]; k < tstart[u + 1]; k++) {
        int w = tadj[k];
        if (state[w] == 1) cycle = 1;
        else if (state[w] == 0) visit(w);
    }
    state[u] = 2;
    order2[--fill_at] = u;
}
static void dfs_topo(void) {
    cycle = 0;
    fill_at = tn;
    for (int i = 0; i < tn; i++) state[i] = 0;
    for (int i = 0; i < tn; i++)
        if (state[i] == 0) visit(i);
}
static int order_ok(const int *order) {
    for (int i = 0; i < tn; i++) posn[order[i]] = i;
    for (int i = 0; i < te; i++)
        if (posn[tu[i]] >= posn[tv[i]]) return 0;
    return 1;
}

int main(void) {
    static const int fsize[6] = {34, 30, 34, 26, 34, 32}, fdens[6] = {8, 15, 25, 5, 12, 40};
    u32 chk = 0;
    for (int round = 0; round < 6; round++) {
        int n = fsize[round], ne = 0, reach = 0;
        rt_srand(1801 + (u32)round);
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                d[i][j] = i == j ? 0 : INF;
                if (i != j && rt_rand_n(100) < (u32)fdens[round]) {
                    d[i][j] = 1 + (int)rt_rand_n(20);
                    fu[ne] = i;
                    fv[ne] = j;
                    fw[ne] = d[i][j];
                    ne++;
                }
            }
        floyd(n);
        for (int s = 0; s < n; s += 11) {
            bellman(s, n, ne);
            for (int j = 0; j < n; j++)
                if (bf[j] != d[s][j]) return 1;
        }
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                if (d[i][j] != INF) { reach++; chk += (u32)d[i][j]; }
        chk = chk * 31 + (u32)reach;
    }
    for (int round = 0; round < 28; round++) {
        rt_srand(1901 + (u32)round);
        tn = 40 + (int)rt_rand_n(TV - 39);              /* 40 .. 64 vertices */
        te = tn * (2 + (round % 5));                    /* 2 .. 6 edges per vertex */
        if (te > TE) te = TE;
        for (int i = 0; i < tn; i++) perm[i] = i;
        for (int i = tn - 1; i > 0; i--) {              /* hidden order that makes it acyclic */
            int j = (int)rt_rand_n((u32)i + 1), t = perm[i];
            perm[i] = perm[j];
            perm[j] = t;
        }
        for (int i = 0; i < te; i++) {
            int a = (int)rt_rand_n((u32)tn), b = (int)rt_rand_n((u32)tn - 1);
            if (b >= a) b++;
            if (perm[a] > perm[b]) { int t = a; a = b; b = t; }
            tu[i] = a;
            tv[i] = b;
        }
        topo_build();
        if (kahn(order1) != tn) return 2;
        dfs_topo();
        if (cycle) return 3;
        if (!order_ok(order1)) return 4;
        if (!order_ok(order2)) return 5;
        int best = 0;
        for (int i = 0; i < tn; i++) longest[i] = 0;
        for (int i = 0; i < tn; i++) {                  /* longest path, along the topological order */
            int u = order1[i];
            for (int k = tstart[u]; k < tstart[u + 1]; k++)
                if (longest[u] + 1 > longest[tadj[k]]) longest[tadj[k]] = longest[u] + 1;
            if (longest[u] > best) best = longest[u];
        }
        chk = chk * 31 + (u32)best + (u32)order1[tn / 2] + (u32)order2[tn / 3];
        int k = (int)rt_rand_n((u32)te);                /* plant a cycle: reverse copy of one edge */
        tu[te] = tv[k];
        tv[te] = tu[k];
        te++;
        topo_build();
        int ordered = kahn(order1);
        if (ordered >= tn) return 6;
        dfs_topo();
        if (!cycle) return 7;
        chk = chk * 31 + (u32)ordered;
        te--;
    }
    return rt_check(chk, 0x2d5025);
}

#else
#error "graph.c: build with -DSEL=1..3"
#endif
