/* BPX-1 bench/ext: minimal <stdlib.h> for the external benchmarks (bare metal, no C library installed).
   Implemented in bench/ext/libc/xlibc.c and xmalloc.c; exit() ends the program through
   bench/rt/crt0.S (_exit). */
#ifndef BPX_EXT_STDLIB_H
#define BPX_EXT_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void exit(int code) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int abs(int x);
void *malloc(size_t n);
void free(void *p);

#endif
