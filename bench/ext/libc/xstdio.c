/* BPX-1 bench/ext: <stdio.h> without a console and without a file system. Written for BPX-1.
   Output functions discard their output and report success; fopen() fails; fscanf() reads nothing.
   A benchmark that prints its results is therefore checked by its port, not by its output. */
#include <stdio.h>

struct bpx_ext_file { int unused; };
static struct bpx_ext_file out_stream, err_stream;
FILE *stdout = &out_stream;
FILE *stderr = &err_stream;

int printf(const char *fmt, ...) { (void)fmt; return 0; }
int fprintf(FILE *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }
int putchar(int c) { return (unsigned char)c; }
int puts(const char *s) { (void)s; return 0; }
int fflush(FILE *f) { (void)f; return 0; }
FILE *fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
int fclose(FILE *f) { (void)f; return 0; }
int fscanf(FILE *f, const char *fmt, ...) { (void)f; (void)fmt; return EOF; }
