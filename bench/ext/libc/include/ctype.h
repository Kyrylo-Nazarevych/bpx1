/* BPX-1 bench/ext: minimal <ctype.h> for the external benchmarks ("C" locale, ASCII).
   Real functions without a table (bench/ext/libc/xlibc.c). */
#ifndef BPX_EXT_CTYPE_H
#define BPX_EXT_CTYPE_H

int isdigit(int c);
int isxdigit(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isalpha(int c);
int isalnum(int c);
int tolower(int c);
int toupper(int c);

#endif
