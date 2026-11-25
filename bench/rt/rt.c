/* BPX-1 bench runtime: small libc for the benchmarks (see rt.h). Integer-only, no I/O.
   Build with -fno-tree-loop-distribute-patterns, or GCC turns these loops back into calls to themselves. */
#include "rt.h"

typedef u32 __attribute__((may_alias)) word_t;      /* word access to memory of any type */

void *memcpy(void *dst, const void *src, size_t n) {
    u8 *d = dst;
    const u8 *s = src;
    if ((((size_t)d | (size_t)s) & 3) == 0) {       /* both word-aligned: copy words first */
        word_t *dw = (word_t *)d;
        const word_t *sw = (const word_t *)s;
        while (n >= 4) { *dw++ = *sw++; n -= 4; }
        d = (u8 *)dw;
        s = (const u8 *)sw;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    u8 *d = dst;
    const u8 *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    u8 *d = dst;
    u8 b = (u8)c;
    while (n && ((size_t)d & 3)) { *d++ = b; n--; }
    if (n >= 4) {
        u32 w = b | (u32)b << 8;
        word_t *dw = (word_t *)d;
        w |= w << 16;
        while (n >= 4) { *dw++ = w; n -= 4; }
        d = (u8 *)dw;
    }
    while (n--) *d++ = b;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (u8)*a - (u8)*b;
        if (!*a) break;
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++) != 0) { }
    return dst;
}

static u32 rt_state = 0x2545F491u;

void rt_srand(u32 seed) {
    u32 s = seed * 0x9E3779B1u + 0x7F4A7C15u;       /* spread small seeds over all bits */
    rt_state = s ? s : 1;                           /* xorshift must not start at 0 */
}

u32 rt_rand(void) {
    u32 x = rt_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rt_state = x;
    return x;
}

u32 rt_rand_n(u32 n) {
    return (u32)(((u64)rt_rand() * n) >> 32);
}

int rt_check(u32 sum, u32 want) {
    u32 f = (sum ^ (sum >> 22)) & 0x3fffff;
    return f == want ? 0 : (int)((f << 8) | 0xff);
}
