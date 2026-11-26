/* BPX-1 bench/ext: the C library functions the external benchmarks call beyond bench/rt/rt.c
   (which has memcpy, memmove, memset, memcmp, strlen, strcmp, strncmp, strcpy). Written for BPX-1.
   Build with -fno-tree-loop-distribute-patterns, like rt.c. */
#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void _exit(int code) __attribute__((noreturn));     /* bench/rt/crt0.S */

/* Exit codes of the runtime itself; the programs use small codes. */
#define EXIT_CODE_ASSERT 97
#define EXIT_CODE_ABORT 98

void exit(int code) { _exit(code); }
void abort(void) { _exit(EXIT_CODE_ABORT); }
void bpx_ext_assert_fail(void) { _exit(EXIT_CODE_ASSERT); }

int abs(int x) { return x < 0 ? -x : x; }

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (*s == 0) return NULL;
    }
}

int isdigit(int c) { return (unsigned)c - '0' < 10u; }
int isupper(int c) { return (unsigned)c - 'A' < 26u; }
int islower(int c) { return (unsigned)c - 'a' < 26u; }
int isalpha(int c) { return ((unsigned)c | 32u) - 'a' < 26u; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || ((unsigned)c | 32u) - 'a' < 6u; }
int isspace(int c) { return c == ' ' || (unsigned)c - '\t' < 5u; }      /* \t \n \v \f \r */
int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }
int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }
