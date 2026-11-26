/* BPX-1 bench/ext: driver for MiBench network/dijkstra (dijkstra_small.c). Written for BPX-1.

   MiBench's own main reads a 100 x 100 cost matrix from a file and calls dijkstra(i, j) for 20
   pairs (i, 50 + i); the results are only printed. This driver replaces that main:
     - it fills AdjMatrix in the program: about one edge in four is absent (NONE), the others cost
       1..100;
     - it calls the unchanged dijkstra() for PATHS pairs (i, 50 + i), as MiBench does;
     - after every call it follows rgnNodes[].iPrev back from the end node: the path must reach the
       start node within NUM_NODES steps over existing edges, and its cost must be the distance
       that dijkstra() left in rgnNodes[end].iDist;
     - distance and path of every pair go into a checksum that must equal GOLDEN.
   The native build (bench/ext/test/mibench_host.sh) also proves every distance to be the shortest
   one with an independent Bellman-Ford pass; that is too slow to be part of the benchmark.

   Exit code: 0 = pass; 2 = end node not reached; 3 = broken path; 4 = path cost differs from the
   distance; 5 (native build only) = a distance is not the shortest; 255 = checksum differs.
   Scale: PATHS (programs_ext.txt), with the GOLDEN that belongs to it. */
#include "drv.h"

#if !defined(PATHS) || !defined(GOLDEN)
#error "PATHS and GOLDEN must be defined on the command line (see programs_ext.txt)"
#endif

/* Interface of dijkstra_small.c (it has no header). */
#define NUM_NODES 100
#define NONE 9999
struct _NODE { int iDist; int iPrev; };
extern int AdjMatrix[NUM_NODES][NUM_NODES];
extern struct _NODE rgnNodes[NUM_NODES];
int dijkstra(int chStart, int chEnd);

#ifdef BPX_HOST
/* Shortest distance from one node to another by Bellman-Ford, for the native cross-check. */
static int reference_distance(int from, int to)
{
    static int dist[NUM_NODES];
    int round, u, v, changed = 1;

    for (v = 0; v < NUM_NODES; v++) dist[v] = NONE * NUM_NODES;
    dist[from] = 0;
    for (round = 0; round < NUM_NODES && changed; round++) {
        changed = 0;
        for (u = 0; u < NUM_NODES; u++)
            for (v = 0; v < NUM_NODES; v++)
                if (AdjMatrix[u][v] != NONE && dist[u] + AdjMatrix[u][v] < dist[v]) {
                    dist[v] = dist[u] + AdjMatrix[u][v];
                    changed = 1;
                }
    }
    return dist[to];
}
#endif

int main(void)
{
    u32 state = 12345, sum = DRV_SUM_INIT;
    int row, col, path, from, to;

    for (row = 0; row < NUM_NODES; row++)
        for (col = 0; col < NUM_NODES; col++) {
            u32 r = drv_rand(&state);
            AdjMatrix[row][col] = (r & 3u) == 0 ? NONE : (int)((r >> 2) % 100u) + 1;
        }

    for (path = 0, to = NUM_NODES / 2; path < PATHS; path++, to++) {
        int node, steps = 0, cost = 0;

        from = path % NUM_NODES;
        to = to % NUM_NODES;
        dijkstra(from, to);

        if (rgnNodes[to].iDist == NONE)
            return 2;
        sum = drv_sum(sum, (u32)rgnNodes[to].iDist);
        for (node = to; node != from; node = rgnNodes[node].iPrev) {
            int prev = rgnNodes[node].iPrev;
            if (prev < 0 || prev >= NUM_NODES || AdjMatrix[prev][node] == NONE || ++steps >= NUM_NODES)
                return 3;
            cost += AdjMatrix[prev][node];
            sum = drv_sum(sum, (u32)node);
        }
        if (cost != rgnNodes[to].iDist)
            return 4;
#ifdef BPX_HOST
        if (reference_distance(from, to) != rgnNodes[to].iDist) {
            fprintf(stderr, "dijkstra: %d -> %d: distance %d, shortest %d\n", from, to, rgnNodes[to].iDist,
                    reference_distance(from, to));
            return 5;
        }
#endif
    }
#ifdef BPX_HOST
    fprintf(stderr, "dijkstra: %d paths, every distance is the shortest (Bellman-Ford)\n", (int)PATHS);
#endif
    return drv_finish(sum, GOLDEN);
}
