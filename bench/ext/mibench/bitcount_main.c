/* BPX-1 bench/ext: driver for MiBench automotive/bitcount. Written for BPX-1.

   MiBench's own main (bitcnts.c) times seven bit-counting methods over <iterations> numbers that
   start at rand() and advance by 13, and prints the totals. It needs argv, clock() and printf, and
   its seventh method is a static function of that file, so this driver replaces it:
     - the six exported methods of bitcnt_1.c .. bitcnt_4.c are called through a table of function
       pointers, as in bitcnts.c, each over the same ITERATIONS numbers (start + 13 * j);
     - a seventh, plain shift-and-count loop written here gives the reference total.
   Self-check: every method's total must equal the reference total.

   Exit code: 0 = all totals agree; 2 + i = method i disagrees with the reference.
   Scale: ITERATIONS (programs_ext.txt). The numbers stay below 2^31, so that a 32-bit and a
   64-bit long give the same counts. */
#include "bitops.h"
#include "drv.h"

#ifndef ITERATIONS
#error "ITERATIONS must be defined on the command line (see programs_ext.txt)"
#endif

#define METHODS 6

static int shift_and_count(long x)
{
    unsigned long bits = (unsigned long)x;
    int n = 0;

    while (bits != 0) {
        n += (int)(bits & 1u);
        bits >>= 1;
    }
    return n;
}

int main(void)
{
    static int (*method[METHODS])(long) = {
        bit_count, bitcount, ntbl_bitcnt, ntbl_bitcount, BW_btbl_bitcount, AR_btbl_bitcount
    };
    u32 state = 1;
    long start, value, total[METHODS], reference = 0;
    long j;
    int i;

    start = (long)(((drv_rand(&state) << 8) ^ drv_rand(&state)) & 0x3fffffffu);     /* below 2^30 */

    for (i = 0; i < METHODS; i++) {
        long n = 0;
        for (j = 0, value = start; j < ITERATIONS; j++, value += 13)
            n += method[i](value);
        total[i] = n;
    }
    for (j = 0, value = start; j < ITERATIONS; j++, value += 13)
        reference += shift_and_count(value);

    for (i = 0; i < METHODS; i++)
        if (total[i] != reference)
            return 2 + i;
#ifdef BPX_HOST
    fprintf(stderr, "bitcount: %ld numbers, %ld bits, all %d methods agree\n", (long)ITERATIONS, reference, METHODS);
#endif
    return 0;
}
