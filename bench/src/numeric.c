/* BPX-1 bench: integer-only numeric code. Select one with -DSEL=<n>:
     1 numeric.softfloat  IEEE-754 single precision in software: add, multiply, compare, int -> float
                          (unpack, align, normalise, round to nearest even, special values), on random
                          operands and on a small filter / polynomial / sort job
     2 numeric.bigint     multi-word integers (16-bit limbs): schoolbook multiply, Knuth long
                          division, decimal conversion both ways
     3 numeric.primes     binary GCD against Euclid, modular exponentiation, Miller-Rabin over a
                          range against a sieve, shift-and-add modular multiply for 31-bit numbers
     4 numeric.mandelbrot fixed-point Mandelbrot set, two views: escape loops of data-dependent length
     5 numeric.dsp        fixed-point FIR filters and radix-2 FFT: regular loops (the easy program)

   Self-checks (exit codes 1..19): arithmetic identities that must hold exactly (commutativity,
   int -> float sums and products, (a * b + c) / b == a rem c, GCD agreement, prime counts, FIR
   impulse response, FFT peak position and inverse), then a checksum. */
#include "rt.h"

#if SEL == 1
typedef u32 f32;                        /* the bits of a float */
#define F_INF 0x7f800000u
#define F_QNAN 0x7fc00000u
#define NPAIRS 9000

/* sign, biased exponent e >= 1, significand m = 24 bits << 3 with guard, round and sticky bits
   below (bit 26 = leading one; if it is clear, e must be 1: subnormal). */
static f32 sf_pack(u32 sign, int e, u32 m) {
    u32 grs = m & 7;
    m >>= 3;
    if (grs > 4 || (grs == 4 && (m & 1))) {         /* round to nearest, ties to even */
        m++;
        if (m == 0x1000000u) { m >>= 1; e++; }
    }
    if (e >= 255) return sign << 31 | F_INF;
    if (!(m & 0x800000u)) e = 0;                    /* subnormal or zero */
    return sign << 31 | (u32)e << 23 | (m & 0x7fffffu);
}

static f32 sf_add(f32 a, f32 b) {
    u32 sa = a >> 31, sb = b >> 31, ma = a & 0x7fffffu, mb = b & 0x7fffffu, m;
    int ea = (int)(a >> 23 & 0xff), eb = (int)(b >> 23 & 0xff);
    if (ea == 255) {
        if (ma) return F_QNAN;
        if (eb == 255) return (mb || sa != sb) ? F_QNAN : a;        /* inf - inf is not a number */
        return a;
    }
    if (eb == 255) return mb ? F_QNAN : b;
    if (ea) ma |= 0x800000u; else ea = 1;
    if (eb) mb |= 0x800000u; else eb = 1;
    ma <<= 3;
    mb <<= 3;
    if (ea < eb || (ea == eb && ma < mb)) {         /* make a the operand of larger magnitude */
        u32 t = ma; ma = mb; mb = t;
        t = sa; sa = sb; sb = t;
        int te = ea; ea = eb; eb = te;
    }
    int d = ea - eb;
    if (d > 0) {                                    /* align b; lost bits collect in the sticky bit */
        if (d >= 27) mb = mb != 0;
        else mb = mb >> d | ((mb & ((1u << d) - 1)) != 0);
    }
    if (sa == sb) {
        m = ma + mb;
        if (m & 0x8000000u) { m = m >> 1 | (m & 1); ea++; }
    } else {
        m = ma - mb;
        if (m == 0) return 0;
        while (!(m & 0x4000000u) && ea > 1) { m <<= 1; ea--; }     /* normalise after cancellation */
    }
    return sf_pack(sa, ea, m);
}

static f32 sf_mul(f32 a, f32 b) {
    u32 sign = (a ^ b) >> 31, ma = a & 0x7fffffu, mb = b & 0x7fffffu, m;
    int ea = (int)(a >> 23 & 0xff), eb = (int)(b >> 23 & 0xff);
    if (ea == 255) {
        if (ma) return F_QNAN;
        if (eb == 255) return mb ? F_QNAN : sign << 31 | F_INF;
        if (eb == 0 && mb == 0) return F_QNAN;      /* inf * 0 */
        return sign << 31 | F_INF;
    }
    if (eb == 255) {
        if (mb) return F_QNAN;
        if (ea == 0 && ma == 0) return F_QNAN;
        return sign << 31 | F_INF;
    }
    if (ea == 0) {
        if (ma == 0) return sign << 31;
        ea = 1;
        while (!(ma & 0x800000u)) { ma <<= 1; ea--; }               /* normalise a subnormal */
    } else ma |= 0x800000u;
    if (eb == 0) {
        if (mb == 0) return sign << 31;
        eb = 1;
        while (!(mb & 0x800000u)) { mb <<= 1; eb--; }
    } else mb |= 0x800000u;
    u64 p = (u64)ma * mb;                           /* 2^46 <= p < 2^48 */
    u32 hi = (u32)(p >> 32), lo = (u32)p;
    int e = ea + eb - 127;
    if (hi & 0x8000u) {                             /* p >= 2^47 */
        m = hi << 11 | lo >> 21 | ((lo & 0x1fffffu) != 0);
        e++;
    } else m = hi << 12 | lo >> 20 | ((lo & 0xfffffu) != 0);
    if (e < 1) {                                    /* underflow: shift into the subnormal range */
        int sh = 1 - e;
        if (sh >= 27) m = 1;
        else m = m >> sh | ((m & ((1u << sh) - 1)) != 0);
        e = 1;
    }
    return sf_pack(sign, e, m);
}

static int sf_isnan(f32 a) { return (a & 0x7fffffffu) > F_INF; }
static int sf_eq(f32 a, f32 b) {
    if (sf_isnan(a) || sf_isnan(b)) return 0;
    return a == b || ((a | b) << 1) == 0;           /* +0 == -0 */
}
static int sf_lt(f32 a, f32 b) {
    if (sf_isnan(a) || sf_isnan(b)) return 0;
    u32 sa = a >> 31, sb = b >> 31;
    if (sa != sb) return sa && ((a | b) << 1) != 0;
    if (a == b) return 0;
    return sa ? a > b : a < b;
}
static f32 sf_from_int(int v) {
    if (v == 0) return 0;
    u32 sign = v < 0, m = sign ? 0u - (u32)v : (u32)v;
    int e = 127 + 26;
    while (m & 0xf8000000u) { m = m >> 1 | (m & 1); e++; }
    while (!(m & 0x4000000u)) { m <<= 1; e--; }
    return sf_pack(sign, e, m);
}

static f32 random_float(void) {
    u32 r = rt_rand_n(16), sign = rt_rand() & 0x80000000u, frac = rt_rand() & 0x7fffffu, e;
    if (r == 0) {
        u32 k = rt_rand_n(6);
        if (k == 0) return sign;                            /* zero */
        if (k == 1) return sign | F_INF;
        if (k == 2) return sign | F_INF | (frac | 1);       /* NaN */
        if (k == 3) return sign | frac;                     /* subnormal */
        if (k == 4) return sign | (250 + rt_rand_n(5)) << 23 | frac;
        return sign | (1 + rt_rand_n(4)) << 23 | frac;
    }
    if (r < 12) e = 115 + rt_rand_n(25);                    /* near 1: every alignment distance */
    else e = 1 + rt_rand_n(254);
    if (r == 1 || r == 12) frac &= 0x7fffffu << rt_rand_n(23);     /* short significands: exact ties */
    return sign | e << 23 | frac;
}

#define NV 160
static f32 v[NV];

int main(void) {
    u32 chk = 0;
    rt_srand(1101);
    for (int i = 0; i < NPAIRS; i++) {
        f32 a = random_float(), b = random_float();
        f32 s = sf_add(a, b), d = sf_add(a, b ^ 0x80000000u), p = sf_mul(a, b);
        int lt = sf_lt(a, b), gt = sf_lt(b, a), eq = sf_eq(a, b);
        if ((i & 7) == 0) {
            if (sf_add(b, a) != s) return 1;
            if (sf_mul(b, a) != p) return 2;
        }
        if (sf_isnan(a) || sf_isnan(b)) {
            if (lt || gt || eq) return 3;
            if (!sf_isnan(s) || !sf_isnan(p)) return 4;
        } else if (lt + gt + eq != 1) return 5;
        chk = (chk * 31 + s) ^ d ^ (p >> 1) ^ (u32)lt;
    }
    for (int i = 0; i < 400; i++) {                         /* exact cases: small integers */
        int a = (int)rt_rand_n(8000000) - 4000000, b = (int)rt_rand_n(8000000) - 4000000;
        int c = (int)rt_rand_n(8000) - 4000, e = (int)rt_rand_n(4000) - 2000;
        if (sf_add(sf_from_int(a), sf_from_int(b)) != sf_from_int(a + b)) return 6;
        if (sf_mul(sf_from_int(c), sf_from_int(e)) != sf_from_int(c * e)) return 7;
        if (sf_lt(sf_from_int(a), sf_from_int(b)) != (a < b)) return 8;
    }
    /* A small job with related values: smoothing filter, cubic polynomial, insertion sort. */
    f32 y = 0;
    int walk = 0;
    for (int i = 0; i < NV; i++) {
        walk += (int)rt_rand_n(41) - 20;
        f32 x = sf_from_int(walk);
        y = sf_add(sf_mul(y, 0x3f600000u), sf_mul(x, 0x3e000000u));        /* y = 0.875 y + 0.125 x */
        f32 t = sf_mul(sf_from_int(i - 80), 0x3d800000u);                  /* t = (i - 80) / 16 */
        f32 poly = sf_add(sf_mul(sf_add(sf_mul(sf_add(sf_mul(0x3e800000u, t), 0xbf000000u), t), 0xc0400000u), t), 0x3f800000u);
        v[i] = sf_add(y, poly);                                             /* 0.25 t^3 - 0.5 t^2 - 3 t + 1 */
    }
    for (int i = 1; i < NV; i++) {
        f32 key = v[i];
        int j = i - 1;
        while (j >= 0 && sf_lt(key, v[j])) { v[j + 1] = v[j]; j--; }
        v[j + 1] = key;
    }
    for (int i = 1; i < NV; i++) {
        if (sf_lt(v[i], v[i - 1])) return 9;
        chk = chk * 31 + v[i];
    }
    return rt_check(chk, 0x231907);
}

#elif SEL == 2
#define MAXL 36                         /* limbs of an operand (16 bits each, little-endian) */
#define TRIALS 450
static u16 A[MAXL], B[MAXL], C[MAXL], P[2 * MAXL + 2], Q[2 * MAXL + 2], R[MAXL + 1];
static u16 un[2 * MAXL + 3], vn[MAXL + 1], tmp[2 * MAXL + 2], back[MAXL + 2];
static u8 dec[MAXL * 5 + 8];

static int big_cmp(const u16 *a, int na, const u16 *b, int nb) {
    if (na != nb) return na < nb ? -1 : 1;
    for (int i = na - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static int big_mul(u16 *r, const u16 *a, int na, const u16 *b, int nb) {
    int n = na + nb;
    for (int i = 0; i < n; i++) r[i] = 0;
    for (int i = 0; i < na; i++) {
        u32 carry = 0, ai = a[i];
        if (ai == 0) continue;
        for (int j = 0; j < nb; j++) {
            u32 t = ai * b[j] + r[i + j] + carry;
            r[i + j] = (u16)t;
            carry = t >> 16;
        }
        r[i + nb] = (u16)carry;
    }
    while (n > 0 && r[n - 1] == 0) n--;
    return n;
}
static int big_add(u16 *r, int nr, const u16 *b, int nb) {     /* r += b; r has room for a carry limb */
    u32 carry = 0;
    int n = nr > nb ? nr : nb;
    for (int i = 0; i < n; i++) {
        u32 t = carry;
        if (i < nr) t += r[i];
        if (i < nb) t += b[i];
        r[i] = (u16)t;
        carry = t >> 16;
    }
    if (carry) r[n++] = (u16)carry;
    return n;
}
/* q = u / v, r = u % v (Knuth's algorithm D). v has nv >= 1 limbs, top limb not 0. Returns the
   length of q; *nr gets the length of r. */
static int big_divmod(u16 *q, u16 *r, int *nr, const u16 *u, int nu, const u16 *v, int nv) {
    int nq, n;
    if (nu < nv) {
        for (int i = 0; i < nu; i++) r[i] = u[i];
        *nr = nu;
        return 0;
    }
    if (nv == 1) {                                  /* short division */
        u32 rem = 0, d = v[0];
        for (int i = nu - 1; i >= 0; i--) {
            u32 t = rem << 16 | u[i];
            q[i] = (u16)(t / d);
            rem = t % d;
        }
        r[0] = (u16)rem;
        *nr = rem != 0;
        nq = nu;
        while (nq > 0 && q[nq - 1] == 0) nq--;
        return nq;
    }
    int s = 0;                                      /* normalise: top bit of the divisor set */
    for (u32 top = v[nv - 1]; !(top & 0x8000u); top <<= 1) s++;
    for (int i = nv - 1; i > 0; i--) vn[i] = (u16)((u32)v[i] << s | (u32)v[i - 1] >> (16 - s));
    vn[0] = (u16)((u32)v[0] << s);
    un[nu] = (u16)((u32)u[nu - 1] >> (16 - s));
    for (int i = nu - 1; i > 0; i--) un[i] = (u16)((u32)u[i] << s | (u32)u[i - 1] >> (16 - s));
    un[0] = (u16)((u32)u[0] << s);
    for (int j = nu - nv; j >= 0; j--) {
        u32 num = (u32)un[j + nv] << 16 | un[j + nv - 1];
        u32 qhat = num / vn[nv - 1], rhat = num % vn[nv - 1];
        while (qhat >= 65536 || qhat * vn[nv - 2] > (rhat << 16 | un[j + nv - 2])) {
            qhat--;                                 /* the estimate was too large */
            rhat += vn[nv - 1];
            if (rhat >= 65536) break;
        }
        u32 borrow = 0;                             /* un[j..j+nv] -= qhat * vn */
        for (int i = 0; i < nv; i++) {
            u32 p = qhat * vn[i] + borrow;
            u32 sub = p & 0xffff, cur = un[i + j];
            borrow = p >> 16;
            if (cur < sub) borrow++;
            un[i + j] = (u16)(cur - sub);
        }
        u32 top = un[j + nv];
        if (top < borrow) {                         /* still one too large (rare): add the divisor back */
            u32 carry = 0;
            qhat--;
            for (int i = 0; i < nv; i++) {
                u32 t = (u32)un[i + j] + vn[i] + carry;
                un[i + j] = (u16)t;
                carry = t >> 16;
            }
            un[j + nv] = (u16)(top - borrow + carry);
        } else un[j + nv] = (u16)(top - borrow);
        q[j] = (u16)qhat;
    }
    for (int i = 0; i < nv - 1; i++) r[i] = (u16)((u32)un[i] >> s | (u32)un[i + 1] << (16 - s));
    r[nv - 1] = (u16)((u32)un[nv - 1] >> s);
    n = nv;
    while (n > 0 && r[n - 1] == 0) n--;
    *nr = n;
    nq = nu - nv + 1;
    while (nq > 0 && q[nq - 1] == 0) nq--;
    return nq;
}
static int big_to_dec(const u16 *a, int na, u8 *out) {         /* decimal digits, most significant first */
    int n = na, nd = 0;
    for (int i = 0; i < na; i++) tmp[i] = a[i];
    if (n == 0) { out[0] = '0'; return 1; }
    while (n > 0) {
        u32 rem = 0;
        for (int i = n - 1; i >= 0; i--) {                      /* tmp /= 10000 */
            u32 x = rem << 16 | tmp[i];
            tmp[i] = (u16)(x / 10000);
            rem = x % 10000;
        }
        while (n > 0 && tmp[n - 1] == 0) n--;
        for (int k = 0; k < 4; k++) {
            out[nd++] = (u8)('0' + rem % 10);
            rem /= 10;
            if (n == 0 && rem == 0) break;                      /* no leading zeros */
        }
    }
    for (int i = 0, j = nd - 1; i < j; i++, j--) { u8 t = out[i]; out[i] = out[j]; out[j] = t; }
    return nd;
}
static int big_from_dec(const u8 *s, int nd, u16 *r) {
    int n = 0;
    for (int k = 0; k < nd; k++) {
        u32 carry = (u32)(s[k] - '0');
        for (int i = 0; i < n; i++) {
            u32 x = (u32)r[i] * 10 + carry;
            r[i] = (u16)x;
            carry = x >> 16;
        }
        if (carry) r[n++] = (u16)carry;
    }
    return n;
}
static void big_random(u16 *a, int n) {             /* top limb not 0; some limbs all zeros or all ones */
    for (int i = 0; i < n; i++) {
        u32 r = rt_rand_n(10);
        a[i] = r == 0 ? 0 : r == 1 ? 0xffff : (u16)rt_rand();
    }
    if (a[n - 1] == 0) a[n - 1] = (u16)(1 + rt_rand_n(0xffff));
}

int main(void) {
    u32 chk = 0;
    rt_srand(1201);
    for (int t = 0; t < TRIALS; t++) {
        int na = 2 + (int)rt_rand_n(MAXL - 1), nb = 1 + (int)rt_rand_n((u32)na), nc, np, nq, nr;
        if ((t & 15) == 15) nb = 1;
        big_random(A, na);
        big_random(B, nb);
        for (int i = 0; i < nb; i++) C[i] = (u16)rt_rand();     /* C < B: smaller top limb */
        C[nb - 1] = (u16)rt_rand_n(B[nb - 1]);
        nc = nb;
        while (nc > 0 && C[nc - 1] == 0) nc--;
        np = big_mul(P, A, na, B, nb);
        np = big_add(P, np, C, nc);
        nq = big_divmod(Q, R, &nr, P, np, B, nb);
        if (big_cmp(Q, nq, A, na) != 0) return 1;
        if (big_cmp(R, nr, C, nc) != 0) return 2;
        chk = chk * 31 + P[0] + ((u32)P[np - 1] << 8) + (u32)np;
        if ((t & 3) == 0) {
            int nd = big_to_dec(A, na, dec);
            int nback = big_from_dec(dec, nd, back);
            if (big_cmp(back, nback, A, na) != 0) return 3;
            chk = chk * 31 + (u32)nd + dec[0] + dec[nd - 1];
        }
    }
    return rt_check(chk, 0x00433a);
}

#elif SEL == 3
#define LIMIT 14000                     /* primality of every n below LIMIT */
#define NGCD 2200
static u8 composite[LIMIT];

static u32 gcd_binary(u32 a, u32 b) {
    int shift = 0;
    if (a == 0) return b;
    if (b == 0) return a;
    while (((a | b) & 1) == 0) { a >>= 1; b >>= 1; shift++; }
    while ((a & 1) == 0) a >>= 1;
    do {
        while ((b & 1) == 0) b >>= 1;
        if (a > b) { u32 t = a; a = b; b = t; }
        b -= a;
    } while (b != 0);
    return a << shift;
}
static u32 gcd_euclid(u32 a, u32 b) {
    while (b) { u32 t = a % b; a = b; b = t; }
    return a;
}
static u32 powmod16(u32 base, u32 e, u32 m) {       /* m < 2^16: products fit in 32 bits */
    u32 r = 1;
    base %= m;
    while (e) {
        if (e & 1) r = r * base % m;
        base = base * base % m;
        e >>= 1;
    }
    return r;
}
static int miller_rabin16(u32 n) {                  /* n odd, 3 < n < 2^16: bases 2 and 3 decide */
    u32 d = n - 1;
    int s = 0;
    while ((d & 1) == 0) { d >>= 1; s++; }
    for (u32 a = 2; a <= 3; a++) {
        u32 x = powmod16(a, d, n);
        int witness = 1;
        if (x == 1 || x == n - 1) continue;
        for (int r = 1; r < s; r++) {
            x = x * x % n;
            if (x == n - 1) { witness = 0; break; }
            if (x == 1) break;
        }
        if (witness) return 0;
    }
    return 1;
}
static int is_prime16(u32 n) {
    static const u8 small[6] = {2, 3, 5, 7, 11, 13};
    if (n < 2) return 0;
    for (int i = 0; i < 6; i++) {
        if (n == small[i]) return 1;
        if (n % small[i] == 0) return 0;
    }
    return miller_rabin16(n);
}
/* 31-bit modulus, no 64-bit arithmetic: multiply by shift and add. */
static u32 mulmod31(u32 a, u32 b, u32 m) {
    u32 r = 0;
    while (b) {
        if (b & 1) { r += a; if (r >= m) r -= m; }
        a <<= 1;
        if (a >= m) a -= m;
        b >>= 1;
    }
    return r;
}
static u32 powmod31(u32 base, u32 e, u32 m) {
    u32 r = 1;
    base %= m;
    while (e) {
        if (e & 1) r = mulmod31(r, base, m);
        base = mulmod31(base, base, m);
        e >>= 1;
    }
    return r;
}
static int miller_rabin31(u32 n) {                  /* n odd, 7 < n < 2^31: bases 2, 3, 5, 7 decide */
    static const u8 bases[4] = {2, 3, 5, 7};
    u32 d = n - 1;
    int s = 0;
    while ((d & 1) == 0) { d >>= 1; s++; }
    for (int i = 0; i < 4; i++) {
        u32 x = powmod31(bases[i], d, n);
        int witness = 1;
        if (x == 1 || x == n - 1) continue;
        for (int r = 1; r < s; r++) {
            x = mulmod31(x, x, n);
            if (x == n - 1) { witness = 0; break; }
            if (x == 1) break;
        }
        if (witness) return 0;
    }
    return 1;
}

int main(void) {
    static const u32 big_primes[3] = {2147483647u, 1000000007u, 998244353u};
    u32 chk = 0;
    int count = 0;
    rt_srand(1301);
    for (u32 i = 2; i * i < LIMIT; i++)             /* reference: sieve of Eratosthenes */
        if (!composite[i])
            for (u32 j = i * i; j < LIMIT; j += i) composite[j] = 1;
    for (u32 n = 2; n < LIMIT; n++) {
        int p = is_prime16(n);
        if (p == composite[n]) return 1;
        count += p;
    }
    chk = (u32)count;
    for (int i = 0; i < NGCD; i++) {
        u32 a = rt_rand(), b;
        a >>= rt_rand_n(20);
        b = rt_rand();
        b >>= rt_rand_n(20);
        if (rt_rand_n(4) == 0) { u32 f = 1 + rt_rand_n(1000); a = (a >> 10) * f; b = (b >> 10) * f; }
        u32 g = gcd_binary(a, b);
        if (g != gcd_euclid(a, b)) return 2;
        chk = chk * 31 + g;
    }
    for (int i = 0; i < 3; i++)
        if (!miller_rabin31(big_primes[i])) return 3;
    for (int i = 0; i < 4; i++) {                   /* products of two primes near 40000: composite */
        u32 p = 39000 + rt_rand_n(6000), q = 39000 + rt_rand_n(6000);
        while (!is_prime16(p)) p++;
        while (!is_prime16(q)) q++;
        if (miller_rabin31(p * q)) return 4;
        chk = chk * 31 + p * q;
    }
    return rt_check(chk, 0x0aef29);
}

#elif SEL == 4
#define FB 24                           /* fixed point: 24 fraction bits, range +-128 */
#define ONE (1 << FB)

static int fmul(int a, int b) { return (int)(((s64)a * b) >> FB); }

static int escape_time(int cx, int cy, int maxit) {
    int xq = cx - ONE / 4, y2 = fmul(cy, cy);
    int q = fmul(xq, xq) + y2;
    if (fmul(q, q + xq) <= y2 >> 2) return maxit;           /* inside the main cardioid */
    int xp = cx + ONE;
    if (fmul(xp, xp) + y2 <= ONE / 16) return maxit;        /* inside the period-2 disc */
    int x = 0, y = 0, it = 0;
    while (it < maxit) {
        int xx = fmul(x, x), yy = fmul(y, y);
        if (xx + yy > 4 * ONE) break;
        y = 2 * fmul(x, y) + cy;
        x = xx - yy + cx;
        it++;
    }
    return it;
}

/* Renders w x h points starting at (x0, y0) with the given step; returns a checksum of the counts. */
static u32 render(int x0, int y0, int step, int w, int h, int maxit, int *inside) {
    u32 sum = 0;
    for (int j = 0; j < h; j++) {
        int cy = y0 + j * step;
        for (int i = 0; i < w; i++) {
            int it = escape_time(x0 + i * step, cy, maxit);
            if (it == maxit) (*inside)++;
            sum = sum * 31 + (u32)it;
        }
    }
    return sum;
}

int main(void) {
    int inside = 0;
    if (escape_time(0, 0, 50) != 50) return 1;
    if (escape_time(ONE, ONE, 50) != 2) return 2;
    if (escape_time(-2 * ONE, 0, 50) != 50) return 3;
    if (escape_time(ONE / 2, 0, 50) != 5) return 4;
    /* whole set: x in [-2.2, 0.8], y in [-1.2, 1.2] */
    u32 a = render(-(22 * ONE) / 10, -(12 * ONE) / 10, ONE / 32, 96, 78, 48, &inside);
    /* a detailed region on the boundary, around -0.745 + 0.11i */
    u32 b = render(-760 * (ONE / 1000), 95 * (ONE / 1000), ONE / 3000, 90, 60, 180, &inside);
    if (inside < 500) return 5;
    return rt_check(a * 31 + b + (u32)inside, 0x090c62);
}

#elif SEL == 5
#define NS 5600                         /* samples through the filters */
#define NT 48                           /* taps of the first filter */
#define FN 512                          /* FFT points */
static s16 x[NS + NT], y[NS + 16], z[NS / 2];
static int h[NT];
static int cs[FN / 2], sn[FN / 2];      /* cos and sin of 2 pi k / FN, 15 fraction bits */
static int re[FN], im[FN], keep[FN];

static int sat16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return v;
}
/* out[n] = sum of h[k] * in[n + NT - 1 - k]: in[] holds NT - 1 samples of history before sample 0. */
static void fir(const s16 *in, s16 *out, int n) {
    for (int i = 0; i < n; i++) {
        int acc = 0;
        for (int k = 0; k < NT; k++) acc += h[k] * in[i + NT - 1 - k];
        out[i] = (s16)sat16(acc >> 15);
    }
}
/* Halve the sample rate with a 16-tap filter of alternating sign (keeps the fast part). */
static void decimate(const s16 *in, s16 *out, int n) {
    static const s16 g[16] = {-200, 500, -1100, 2100, -3600, 5600, -8000, 10400,
                              -10400, 8000, -5600, 3600, -2100, 1100, -500, 200};
    for (int i = 0; i < n; i++) {
        int acc = 0;
        for (int k = 0; k < 16; k++) acc += g[k] * in[2 * i + k];
        out[i] = (s16)sat16(acc >> 14);
    }
}
static void make_tables(void) {         /* rotate by 2 pi / 512 again and again, 30 fraction bits */
    s64 c = 1 << 30, s = 0;
    for (int k = 0; k < FN / 2; k++) {
        cs[k] = (int)(c >> 15);
        sn[k] = (int)(s >> 15);
        s64 nc = (c * 1073660973 - s * 13176464) >> 30;
        s = (c * 13176464 + s * 1073660973) >> 30;
        c = nc;
    }
}
static int cos_q15(int k) {             /* cos(2 pi k / FN), any k >= 0 */
    k &= FN - 1;
    return k < FN / 2 ? cs[k] : -cs[k - FN / 2];
}
/* In-place radix-2 FFT. Forward: halves at every stage (result = DFT / FN). Inverse: no scaling.
   The products are formed in 64 bits, so the data may use most of a 32-bit word. */
static void fft(int inverse) {
    for (int i = 1, j = 0; i < FN; i++) {                   /* bit-reversal permutation */
        int bit = FN >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            int t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= FN; len <<= 1) {
        int half = len >> 1, step = FN / len;
        for (int i = 0; i < FN; i += len) {
            for (int j = 0, k = 0; j < half; j++, k += step) {
                int wr = cs[k], wi = inverse ? sn[k] : -sn[k];
                int p = i + j, q = p + half;
                int tr = (int)(((s64)re[q] * wr - (s64)im[q] * wi) >> 15);
                int ti = (int)(((s64)re[q] * wi + (s64)im[q] * wr) >> 15);
                if (inverse) {
                    re[q] = re[p] - tr; im[q] = im[p] - ti;
                    re[p] += tr;        im[p] += ti;
                } else {
                    re[q] = (re[p] - tr) >> 1; im[q] = (im[p] - ti) >> 1;
                    re[p] = (re[p] + tr) >> 1; im[p] = (im[p] + ti) >> 1;
                }
            }
        }
    }
}

int main(void) {
    u32 chk = 0;
    int clipped = 0;
    rt_srand(1501);
    for (int k = 0; k < NT; k++) h[k] = 130 * (k < NT / 2 ? k + 1 : NT - k);   /* triangle, gain 2.38 */

    /* filter checks: impulse response and DC gain */
    for (int i = 0; i < NS + NT; i++) x[i] = 0;
    x[NT - 1 + 10] = 16384;
    fir(x, y, 100);
    for (int k = 0; k < NT; k++)
        if (y[10 + k] != h[k] / 2) return 1;
    for (int i = 0; i < 200; i++) x[i] = 1000;
    fir(x, y, 100);
    if (y[50] != (1000 * 130 * 600) >> 15) return 2;

    /* the signal: square wave + sawtooth + noise, loud enough to clip now and then */
    for (int i = 0; i < NS + NT; i++) {
        int v = ((i / 70) & 1 ? 9000 : -9000) + (i % 180) * 60 - 5400 + (int)rt_rand_n(9001) - 4500;
        x[i] = (s16)v;
    }
    fir(x, y, NS);
    for (int i = 0; i < 16; i++) y[NS + i] = 0;
    decimate(y, z, NS / 2);
    for (int i = 0; i < NS; i++) {
        if (y[i] == 32767 || y[i] == -32768) clipped++;
        chk = chk * 31 + (u32)y[i];
    }
    for (int i = 0; i < NS / 2; i++) chk = chk * 31 + (u32)z[i];

    /* FFT: a pure tone must peak in its bin; forward then inverse must give the signal back */
    make_tables();
    for (int t = 0; t < 5; t++) {
        int bin = 5 + 37 * t, worst = 0, best = 0, at = 0;
        for (int i = 0; i < FN; i++) {
            int v = (5000 * cos_q15(bin * i)) >> 15;
            if (t >= 2) v += (int)rt_rand_n(801) - 400;                 /* tone in noise */
            if (t == 4) v = (int)(z[i] >> 3);                           /* a piece of the filtered signal */
            keep[i] = v;
            re[i] = v * 1024;                                           /* 10 extra fraction bits */
            im[i] = 0;
        }
        fft(0);
        for (int i = 1; i < FN / 2; i++) {
            int a = re[i] >> 10, b = im[i] >> 10, mag = a * a + b * b;
            if (mag > best) { best = mag; at = i; }
            chk = chk * 31 + (u32)mag;
        }
        if (t < 4 && at != bin) return 3;
        fft(1);
        for (int i = 0; i < FN; i++) {
            int e = ((re[i] + 512) >> 10) - keep[i];
            if (e < 0) e = -e;
            if (e > worst) worst = e;
        }
        if (worst > 8) return 4;
        chk = chk * 31 + (u32)worst;
    }
    return rt_check(chk + (u32)clipped, 0x0f623b);
}

#else
#error "numeric.c: build with -DSEL=1..5"
#endif
