/* BPX-1 bench/ext: helpers shared by the MiBench drivers (bitcount_main.c, dijkstra_main.c,
   adpcm_main.c). Written for BPX-1.

   MiBench programs read input files and print their results. Here each driver generates the input
   in the program, calls the unchanged MiBench functions, checks what they returned, and (dijkstra,
   adpcm) folds the results into a checksum that must equal GOLDEN from programs_ext.txt.

   The same driver also builds natively (bench/ext/test/mibench_host.sh, -DBPX_HOST): there it
   prints the checksum, which is how GOLDEN is obtained and cross-checked on a second machine. */
#ifndef BPX_EXT_MIBENCH_DRV_H
#define BPX_EXT_MIBENCH_DRV_H

/* build_ext.sh renames the main() of the MiBench sources (-Dmain=...); the driver's main stays. */
#undef main

#ifdef BPX_HOST
#include <stdio.h>
typedef unsigned int u32;
#else
#include "rt.h"                                 /* bench/rt: u32, rt_check() */
#endif

/* Pseudo-random numbers: 24 bits per call (linear congruential, modulus 2^32). */
static inline u32 drv_rand(u32 *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

/* Checksum over 32-bit values (multiply-xor, FNV style). Start with DRV_SUM_INIT. */
#define DRV_SUM_INIT 2166136261u
static inline u32 drv_sum(u32 sum, u32 value) { return (sum ^ value) * 16777619u; }

/* End of a driver: compare the checksum with the wanted 22-bit fold (see rt_check in bench/rt/rt.h:
   on a mismatch Spike prints the fold that the program computed). */
static inline int drv_finish(u32 sum, u32 want)
{
#ifdef BPX_HOST
    u32 fold = (sum ^ (sum >> 22)) & 0x3fffff;
    fprintf(stderr, "checksum fold 0x%06x, GOLDEN 0x%06x: %s\n", fold, want, fold == want ? "equal" : "DIFFERENT");
    return fold == want ? 0 : 1;
#else
    return rt_check(sum, want);
#endif
}

#endif
