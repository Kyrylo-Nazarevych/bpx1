/* BPX-1 bench/ext test: compare bench/ext/libc/xsqrt.c and xmath.c with the host C library.
   Host program (see check_xmath.sh): they are compiled natively with their functions renamed to
   x_<name>; IEEE-754 double arithmetic gives the same results on the host as libgcc soft-float
   gives on RV32IM, so the errors measured here are those of the benchmark builds.
   Prints the largest error of every function in units in the last place (ulp) of the host result,
   which itself is within 1 ulp of the true value. Exit status 1 when a limit is exceeded. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

double x_sqrt(double), x_sin(double), x_cos(double), x_atan(double), x_acos(double);
double x_exp(double), x_log(double), x_pow(double, double);

static uint64_t rng_state = 88172645463325252ull;

static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static double uniform(double lo, double hi) { return lo + (hi - lo) * ((double)(rng() >> 11) * 0x1p-53); }

/* any finite double >= 0, every exponent equally likely (subnormals included) */
static double any_positive(void)
{
    uint64_t u = rng() & 0x7fffffffffffffffull;
    double d;
    if ((u >> 52) == 0x7ff) u &= ~(1ull << 62);
    memcpy(&d, &u, 8);
    return d;
}

static double ulps(double got, double want)
{
    double u;
    if (isnan(got) && isnan(want)) return 0.0;
    if (got == want) return 0.0;
    if (isnan(got) || isnan(want) || isinf(got) || isinf(want)) return INFINITY;
    u = nextafter(fabs(want), INFINITY) - fabs(want);
    return fabs(got - want) / u;
}

static int failures;

static void report(const char *what, double worst, double worst_x, double limit)
{
    int bad = !(worst <= limit);
    printf("%-34s max error %8.3f ulp (at x = %.17g)  limit %g  %s\n", what, worst, worst_x, limit,
           bad ? "FAIL" : "ok");
    failures += bad;
}

#define SWEEP(what, n, gen, mine, host, limit) do { \
        double worst = 0.0, worst_x = 0.0; \
        for (long i = 0; i < (n); i++) { \
            double x = (gen); \
            double e = ulps(mine(x), host(x)); \
            if (e > worst) { worst = e; worst_x = x; } \
        } \
        report(what, worst, worst_x, limit); \
    } while (0)

static double cbrt_pow(double x) { return x_pow(x, 1 / 3.0); }
static double cbrt_host(double x) { return pow(x, 1 / 3.0); }

int main(void)
{
    static const double special[] = { 0.0, -0.0, 1.0, -1.0, 2.0, 4.0, 0.25, 1e-310, 4.9e-324, 1.7976931348623157e308,
                                      INFINITY, -INFINITY, NAN, 0.5, -0.5, 3.0, 1e300, 1e-300 };
    const long n = 2000000;
    double worst, worst_x;

    for (unsigned i = 0; i < sizeof special / sizeof special[0]; i++) {
        double x = special[i];
        if (ulps(x_sqrt(x), sqrt(x)) != 0.0 ||
            (!isnan(sqrt(x)) && !signbit(x_sqrt(x)) != !signbit(sqrt(x)))) {  /* the sign of a NaN is free */
            printf("sqrt(%g) = %g, host %g  FAIL\n", x, x_sqrt(x), sqrt(x));
            failures++;
        }
        if (ulps(x_atan(x), atan(x)) > 1.0 || ulps(x_acos(x), acos(x)) > 2.0 ||
            ulps(x_exp(x), exp(x)) > 2.0 || ulps(x_log(x), log(x)) > 2.0) {
            printf("special value %g: atan %g/%g acos %g/%g exp %g/%g log %g/%g  FAIL\n", x, x_atan(x), atan(x),
                   x_acos(x), acos(x), x_exp(x), exp(x), x_log(x), log(x));
            failures++;
        }
    }

    SWEEP("sqrt, all exponents", n, any_positive(), x_sqrt, sqrt, 0.0);
    SWEEP("sqrt, [0, 100]", n, uniform(0, 100), x_sqrt, sqrt, 0.0);
    SWEEP("cos, [-10, 10]", n, uniform(-10, 10), x_cos, cos, 4.0);
    SWEEP("cos, [-1e6, 1e6]", n, uniform(-1e6, 1e6), x_cos, cos, 4.0);
    SWEEP("sin, [-10, 10]", n, uniform(-10, 10), x_sin, sin, 4.0);
    SWEEP("sin, [-1e6, 1e6]", n, uniform(-1e6, 1e6), x_sin, sin, 4.0);
    SWEEP("atan, [-4, 4]", n, uniform(-4, 4), x_atan, atan, 4.0);
    SWEEP("atan, all exponents", n, any_positive(), x_atan, atan, 4.0);
    SWEEP("acos, [-1, 1]", n, uniform(-1, 1), x_acos, acos, 6.0);
    SWEEP("exp, [-1, 1]", n, uniform(-1, 1), x_exp, exp, 4.0);
    SWEEP("exp, [-700, 700]", n, uniform(-700, 700), x_exp, exp, 4.0);
    SWEEP("log, [0.5, 2]", n, uniform(0.5, 2), x_log, log, 4.0);
    SWEEP("log, all exponents", n, any_positive(), x_log, log, 4.0);
    SWEEP("pow(x, 1/3), [0.001, 1000]", n, uniform(0.001, 1000), cbrt_pow, cbrt_host, 8.0);

    /* pow in general: report only. The error grows with |y * log(x)| (xmath.c). */
    worst = 0.0, worst_x = 0.0;
    for (long i = 0; i < n; i++) {
        double x = uniform(0.01, 100), y = uniform(-20, 20);
        double e = ulps(x_pow(x, y), pow(x, y));
        if (e > worst) { worst = e; worst_x = x; }
    }
    printf("%-34s max error %8.3f ulp (at x = %.17g)  not limited\n", "pow, x [0.01, 100], y [-20, 20]", worst,
           worst_x);

    printf(failures ? "check_xmath: FAILED\n" : "check_xmath: ok\n");
    return failures != 0;
}
