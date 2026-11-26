/* BPX-1 bench/ext: sqrt() for a CPU without FPU (IEEE-754 binary64, integer arithmetic only).
   Written for BPX-1; not derived from another C library. The result is exactly rounded (checked
   against a host libm by bench/ext/test/check_xmath.sh). In a file of its own so that a program
   that only needs sqrt does not link the rest of xmath.c. */
#include <math.h>
#include <stdint.h>

typedef union { double d; uint64_t u; } f64bits;

static inline uint64_t to_bits(double d) { f64bits b; b.d = d; return b.u; }
static inline double from_bits(uint64_t u) { f64bits b; b.u = u; return b.d; }

#define QNAN from_bits(0x7ff8000000000000ull)

double sqrt(double x)
{
    uint64_t u = to_bits(x);
    int e = (int)(u >> 52) & 0x7ff;
    uint64_t m = u & 0x000fffffffffffffull;
    uint64_t root, rem;
    uint32_t hi, lo, root32 = 0, rem32 = 0;
    int i;

    if (e == 0x7ff)
        return (m != 0 || (u >> 63) == 0) ? x : QNAN;       /* NaN, +inf; sqrt(-inf) is NaN */
    if (e == 0 && m == 0)
        return x;                                           /* +0, -0 */
    if (u >> 63)
        return QNAN;
    if (e == 0) {                                           /* subnormal: normalise */
        e = 1;
        while ((m >> 52) == 0) { m <<= 1; e--; }
    } else {
        m |= 1ull << 52;
    }
    e -= 1023;                                              /* x = (m / 2^52) * 2^e */
    if (e & 1)
        m <<= 1;                                            /* make the exponent even: m < 2^54 */
    e >>= 1;                                                /* arithmetic shift: floor(e / 2) */

    /* root = floor(sqrt(m * 2^54)), 54 bits, one bit per step: bring down the next two bits of the
       radicand (27 pairs from m, then 27 pairs of zeros) and try to subtract 4 * root + 1.
       After k steps root < 2^k and the remainder is at most 2 * root, so the first 29 steps fit
       in 32-bit arithmetic; the other 25 need 64 bits. */
#define STEP32(pair) do { \
        uint32_t trial = root32 << 2 | 1u; \
        rem32 = rem32 << 2 | (pair); \
        root32 <<= 1; \
        if (rem32 >= trial) { rem32 -= trial; root32 |= 1u; } \
    } while (0)
    hi = (uint32_t)(m >> 32);                               /* bits 53..32 of m: 11 pairs */
    lo = (uint32_t)m;                                       /* bits 31..0: 16 pairs */
    for (i = 0; i < 11; i++) {
        STEP32((hi >> 20) & 3u);
        hi <<= 2;
    }
    for (i = 0; i < 18; i++) {                              /* the last two pairs are zeros */
        STEP32(lo >> 30);
        lo <<= 2;
    }
#undef STEP32
    root = root32;
    rem = rem32;
    for (i = 0; i < 25; i++) {
        uint64_t trial = root << 2 | 1;
        rem <<= 2;
        root <<= 1;
        if (rem >= trial) { rem -= trial; root |= 1; }
    }
    /* The lowest bit of root is the rounding bit; a square root is never exactly half way. */
    m = (root + 1) >> 1;
    return from_bits(((uint64_t)(e + 1023) << 52) + (m - (1ull << 52)));
}
