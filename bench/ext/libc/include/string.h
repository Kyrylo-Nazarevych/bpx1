/* BPX-1 bench/ext: minimal <string.h> for the external benchmarks (bare metal, no C library installed).
   memcpy, memmove, memset, memcmp, strlen, strcmp, strncmp, strcpy come from bench/rt/rt.c,
   strchr from bench/ext/libc/xlibc.c. */
#ifndef BPX_EXT_STRING_H
#define BPX_EXT_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strchr(const char *s, int c);

#endif
