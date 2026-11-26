/* BPX-1 bench/ext: the <math.h> functions the external benchmarks call, for a CPU without FPU
   (IEEE-754 binary64 on libgcc soft-float). Written for BPX-1; not derived from another C library.

   sqrt   in xsqrt.c: exactly rounded (integer digit-by-digit method).
   sin, cos, atan, acos, exp, log   table/series methods, a few ulp of error (measured against a host
          libm by bench/ext/test/check_xmath.sh). sin and cos accept |x| <= 2^20 and return NaN beyond.
   pow    exp(y * log(x)): a few ulp while |y * log(x)| is small; the relative error grows with it.

   The constants were computed with exact rational arithmetic and rounded to nearest. Compile without
   floating-point contraction and with -fno-builtin (implied by -ffreestanding). */
#include <math.h>
#include <stdint.h>

typedef union { double d; uint64_t u; } f64bits;

static inline uint64_t to_bits(double d) { f64bits b; b.d = d; return b.u; }
static inline double from_bits(uint64_t u) { f64bits b; b.u = u; return b.d; }

#define QNAN from_bits(0x7ff8000000000000ull)
#define PINF from_bits(0x7ff0000000000000ull)

static const double PI_HI = 0x1.921fb54442d18p+1, PI_LO = 0x1.1a62633145c07p-53;
static const double PIO2_HI = 0x1.921fb54442d18p+0, PIO2_LO = 0x1.1a62633145c07p-54;
static const double PIO4 = 0x1.921fb54442d18p-1;
static const double TWO_OVER_PI = 0x1.45f306dc9c883p-1;
/* pi/2 = PIO2_1 + PIO2_2 + PIO2_3; the first two have 33 significant bits */
static const double PIO2_1 = 0x1.921fb54400000p+0, PIO2_2 = 0x1.0b4611a600000p-34;
static const double PIO2_3 = 0x1.3198a2e037073p-69;
/* ln 2 = LN2_HI + LN2_LO; LN2_HI has 32 significant bits */
static const double LN2_HI = 0x1.62e42fee00000p-1, LN2_LO = 0x1.a39ef35793c76p-33;
static const double INV_LN2 = 0x1.71547652b82fep+0;
static const double SQRT2 = 0x1.6a09e667f3bcdp+0;

/* -------------------------------------------------------------------------------------- sin, cos */

/* sin(r) and cos(r) for |r| <= pi/4 (Taylor series; the first omitted terms are below 2^-60) */
static double sin_kernel(double r)
{
    double z = r * r;
    double p = z * (-1.0 / 6 + z * (1.0 / 120 + z * (-1.0 / 5040 + z * (1.0 / 362880
             + z * (-1.0 / 39916800 + z * (1.0 / 6227020800.0 + z * (-1.0 / 1307674368000.0
             + z * (1.0 / 355687428096000.0))))))));
    return r + r * p;
}

static double cos_kernel(double r)
{
    double z = r * r;
    double p = z * (1.0 / 24 + z * (-1.0 / 720 + z * (1.0 / 40320 + z * (-1.0 / 3628800
             + z * (1.0 / 479001600 + z * (-1.0 / 87178291200.0 + z * (1.0 / 20922789888000.0)))))));
    return 1.0 - (0.5 * z - z * p);
}

/* ax = n * pi/2 + r with |r| <= pi/4 (nearly); returns n mod 4. For pi/4 < ax <= 2^20:
   n < 2^20, so n * PIO2_1 and n * PIO2_2 are exact. */
static int reduce_pio2(double ax, double *r)
{
    int n = (int)(ax * TWO_OVER_PI + 0.5);
    double dn = (double)n;
    *r = ((ax - dn * PIO2_1) - dn * PIO2_2) - dn * PIO2_3;
    return n & 3;
}

double cos(double x)
{
    double ax = fabs(x), r;
    if (!(ax <= 1048576.0))
        return QNAN;                                        /* NaN, infinite or out of range */
    if (ax <= PIO4)
        return cos_kernel(ax);
    switch (reduce_pio2(ax, &r)) {
    case 0:  return cos_kernel(r);
    case 1:  return -sin_kernel(r);
    case 2:  return -cos_kernel(r);
    default: return sin_kernel(r);
    }
}

double sin(double x)
{
    double ax = fabs(x), r, s;
    if (!(ax <= 1048576.0))
        return QNAN;
    if (ax <= PIO4)
        return sin_kernel(x);
    switch (reduce_pio2(ax, &r)) {
    case 0:  s = sin_kernel(r); break;
    case 1:  s = cos_kernel(r); break;
    case 2:  s = -sin_kernel(r); break;
    default: s = -cos_kernel(r); break;
    }
    return x < 0.0 ? -s : s;
}

/* ------------------------------------------------------------------------------------ atan, acos */

static const double ATAN_K8[9] = {      /* atan(k / 8) */
    0.0,
    0x1.fd5ba9aac2f6ep-4, 0x1.f5b75f92c80ddp-3, 0x1.6f61941e4def1p-2, 0x1.dac670561bb4fp-2,
    0x1.1e00babdefeb4p-1, 0x1.4978fa3269ee1p-1, 0x1.700a7c5784634p-1, 0x1.921fb54442d18p-1,
};

double atan(double x)
{
    double ax = fabs(x), c, t, z, p, r;
    int invert = 0, k;

    if (ax != ax)
        return x;                                           /* NaN */
    if (ax > 1.0) {                                         /* atan(a) = pi/2 - atan(1/a) */
        ax = 1.0 / ax;
        invert = 1;
    }
    /* atan(a) = atan(c) + atan(t), c = k/8 nearest to a, t = (a - c) / (1 + a*c), |t| <= 1/16 */
    k = (int)(ax * 8.0 + 0.5);
    c = (double)k * 0.125;
    t = (ax - c) / (1.0 + ax * c);
    z = t * t;
    p = z * (-1.0 / 3 + z * (1.0 / 5 + z * (-1.0 / 7 + z * (1.0 / 9 + z * (-1.0 / 11
      + z * (1.0 / 13))))));
    r = ATAN_K8[k] + (t + t * p);
    if (invert)
        r = (PIO2_HI - r) + PIO2_LO;
    return x < 0.0 ? -r : r;
}

double acos(double x)
{
    if (!(fabs(x) <= 1.0))
        return QNAN;
    if (x == -1.0)
        return PI_HI + PI_LO;
    return 2.0 * atan(sqrt((1.0 - x) / (1.0 + x)));         /* half-angle formula */
}

/* -------------------------------------------------------------------------------- exp, log, pow */

/* y * 2^k, also when the result or k leaves the normal range */
static double scale_pow2(double y, int k)
{
    while (k > 1023) { y *= 0x1p1023; k -= 1023; }
    while (k < -1022) { y *= 0x1p-1022; k += 1022; }
    return y * from_bits((uint64_t)(k + 1023) << 52);
}

double exp(double x)
{
    double r, p;
    int k;

    if (x != x)
        return x;
    if (x > 709.79)
        return PINF;
    if (x < -745.14)
        return 0.0;
    /* x = k * ln 2 + r, |r| <= ln(2)/2; then the Taylor series of e^r up to r^13 / 13! */
    k = (int)(x * INV_LN2 + (x < 0.0 ? -0.5 : 0.5));
    r = (x - (double)k * LN2_HI) - (double)k * LN2_LO;
    p = r * (1.0 / 6 + r * (1.0 / 24 + r * (1.0 / 120 + r * (1.0 / 720 + r * (1.0 / 5040
      + r * (1.0 / 40320 + r * (1.0 / 362880 + r * (1.0 / 3628800 + r * (1.0 / 39916800
      + r * (1.0 / 479001600 + r * (1.0 / 6227020800.0)))))))))));
    p = r + r * r * (0.5 + p);
    return scale_pow2(1.0 + p, k);
}

double log(double x)
{
    uint64_t u = to_bits(x);
    int e = 0;
    double m, s, z, p;

    if ((u << 1) == 0)
        return -PINF;                                       /* log(+-0) */
    if (u >> 63)
        return QNAN;
    if ((u >> 52) == 0x7ff)
        return x;                                           /* +inf, NaN */
    if ((u >> 52) == 0) {                                   /* subnormal */
        u = to_bits(x * 0x1p54);
        e = -54;
    }
    e += (int)(u >> 52) - 1023;
    m = from_bits((u & 0x000fffffffffffffull) | 0x3ff0000000000000ull);     /* 1 <= m < 2 */
    if (m > SQRT2) {
        m *= 0.5;
        e++;
    }
    /* log(m) = 2 * atanh(s), s = (m - 1) / (m + 1), |s| < 0.1716 */
    s = (m - 1.0) / (m + 1.0);
    z = s * s;
    p = z * (1.0 / 3 + z * (1.0 / 5 + z * (1.0 / 7 + z * (1.0 / 9 + z * (1.0 / 11 + z * (1.0 / 13
      + z * (1.0 / 15 + z * (1.0 / 17 + z * (1.0 / 19 + z * (1.0 / 21))))))))));
    return (double)e * LN2_HI + ((double)e * LN2_LO + (2.0 * s + 2.0 * s * p));
}

double pow(double x, double y)
{
    double r;

    if (y == 0.0)
        return 1.0;
    if (x != x || y != y)
        return QNAN;
    if (x == 0.0)
        return y > 0.0 ? 0.0 : PINF;
    if (x < 0.0) {                                          /* only an integer power is defined */
        r = exp(y * log(-x));
        if (fabs(y) < 0x1p62) {
            long long n = (long long)y;
            if ((double)n != y)
                return QNAN;
            return (n & 1) ? -r : r;
        }
        return r;                                           /* such a large y is an even integer */
    }
    return exp(y * log(x));
}
