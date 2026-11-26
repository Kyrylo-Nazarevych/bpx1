/* BPX-1 bench/ext: minimal <math.h> for the external benchmarks (bare metal, no C library installed).
   IEEE-754 binary64 on soft-float (libgcc). Implemented in bench/ext/libc/xmath.c, which states the
   accuracy and the argument range of every function. */
#ifndef BPX_EXT_MATH_H
#define BPX_EXT_MATH_H

#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923

/* The compiler expands these in line, as it does with a hosted C library. */
static inline double fabs(double x) { return __builtin_fabs(x); }
static inline float fabsf(float x) { return __builtin_fabsf(x); }

double sqrt(double x);
double sin(double x);
double cos(double x);
double atan(double x);
double acos(double x);
double exp(double x);
double log(double x);
double pow(double x, double y);

#endif
