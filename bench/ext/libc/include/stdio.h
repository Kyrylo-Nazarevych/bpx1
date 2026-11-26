/* BPX-1 bench/ext: minimal <stdio.h> for the external benchmarks. The runtime has no console and
   no file system (bench/ext/libc/xstdio.c): output is discarded, fopen() always fails. */
#ifndef BPX_EXT_STDIO_H
#define BPX_EXT_STDIO_H

#include <stddef.h>

typedef struct bpx_ext_file FILE;
extern FILE *stdout;
extern FILE *stderr;
#define EOF (-1)

int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int putchar(int c);
int puts(const char *s);
int fflush(FILE *f);
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
int fscanf(FILE *f, const char *fmt, ...);

#endif
