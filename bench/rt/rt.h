/* BPX-1 bench runtime: the small C library the benchmarks may use (bare-metal RV32IM, no OS, no I/O).
   Implemented in rt.c; start-up and exit are in crt0.S. */
#ifndef BPX_RT_H
#define BPX_RT_H

typedef __SIZE_TYPE__ size_t;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef short s16;
typedef int s32;
typedef long long s64;
#ifndef NULL
#define NULL ((void *)0)
#endif

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);

/* Deterministic pseudo-random numbers (xorshift32). Every program seeds it itself. */
void rt_srand(u32 seed);
u32 rt_rand(void);          /* 32 random bits */
u32 rt_rand_n(u32 n);       /* 0 .. n-1, n >= 1 (multiply-high reduction: no division) */

/* End-of-program check: "return rt_check(sum, want);" from main.
   The 32-bit sum is folded to 22 bits: f = (sum ^ (sum >> 22)) & 0x3fffff. Returns 0 when f == want.
   Otherwise returns (f << 8) | 0xff: Spike then exits with status 255 and prints
   "*** FAILED *** (tohost = <n>)", where n >> 8 is the value to put in the source as `want`
   (22 bits because Spike keeps 31 bits of the exit code). */
int rt_check(u32 sum, u32 want);

/* GCC -O2 turns a chain of if/else tests of one variable into a switch and then into a jump table
   (an indirect jump). A function marked RT_IF_CHAIN keeps such chains as conditional branches. */
#define RT_IF_CHAIN __attribute__((optimize("no-jump-tables")))

/* Leave the program at once with this exit code (0 = pass); main returning does the same. */
void _exit(int code) __attribute__((noreturn));

#endif
