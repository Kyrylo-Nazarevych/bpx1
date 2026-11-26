/* BPX-1 bench/ext: CoreMark port for the BPX-1 bench runtime (bench/rt: bare-metal RV32IM under
   Spike, no OS, no timer, no console). Written for BPX-1 from CoreMark's porting interface; it is
   not a copy of one of the port directories that CoreMark ships.

   The run is not a CoreMark score run: it executes a fixed, small number of iterations and the
   clock is a stub. It is used only as a workload whose result CoreMark itself checks (CRCs). */
#ifndef BPX_CORE_PORTME_H
#define BPX_CORE_PORTME_H

#include <stddef.h>
#include <stdint.h>

/* No floating point, no <time.h>, no clock(), no <stdio.h>, no printf: ee_printf is ours. */
#define HAS_FLOAT 0
#define HAS_TIME_H 0
#define USE_CLOCK 0
#define HAS_STDIO 0
#define HAS_PRINTF 0

typedef int16_t ee_s16;
typedef uint16_t ee_u16;
typedef int32_t ee_s32;
typedef uint32_t ee_u32;
typedef uint8_t ee_u8;
typedef uintptr_t ee_ptr_int;
typedef size_t ee_size_t;
typedef ee_u32 CORE_TICKS;

/* Round a pointer up to a multiple of 4. */
#define align_mem(x) ((void *)(((ee_ptr_int)(x) + 3u) & ~(ee_ptr_int)3u))

#define COMPILER_VERSION "GCC " __VERSION__
#define COMPILER_FLAGS "-march=rv32im -mabi=ilp32 -O2 (bench/ext/build_ext.sh)"
#define MEM_LOCATION "STACK"

/* Seeds are read from volatile variables (core_portme.c); the data block lives on the stack. */
#define SEED_METHOD SEED_VOLATILE
#define MEM_METHOD MEM_STACK
#define MULTITHREAD 1
#define MAIN_HAS_NOARGC 1
#define MAIN_HAS_NORETURN 0

typedef struct { ee_u8 unused; } core_portable;

extern ee_u32 default_num_contexts;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);
int ee_printf(const char *fmt, ...);

#endif
