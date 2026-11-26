/* BPX-1 bench/ext: CoreMark port for the BPX-1 bench runtime (see core_portme.h). Written for BPX-1.

   Seeds: 0, 0, 0x66 with the default 2000-byte data block: CoreMark's "2K performance run", for
   which core_main.c knows the list, matrix and state CRCs. ITERATIONS comes from programs_ext.txt.

   Clock: none. get_time() claims 10 seconds, so that CoreMark's rule "must execute for at least
   10 secs" does not count as an error and its verdict depends only on the CRCs and data types.

   Output: there is no console. ee_printf() keeps what the run needs: the CRCs that core_main.c
   prints, whether it printed "Correct operation validated", and how many error messages it
   printed. portable_fini() turns that into the exit code:
       0 = CoreMark validated the run;  1 = CoreMark reported an error;  2 = no verdict printed.
   The record bpx_coremark_report stays in memory (bench/ext/test/coremark_crc.sh reads it). */
#include <stdarg.h>
#include "coremark.h"

#ifndef ITERATIONS
#error "ITERATIONS must be defined on the command line (see programs_ext.txt)"
#endif

void _exit(int code) __attribute__((noreturn));     /* bench/rt/crt0.S */

volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

ee_u32 default_num_contexts = 1;

struct bpx_coremark_report {
    ee_u32 validated;       /* 1: "Correct operation validated" was printed */
    ee_u32 errors;          /* error messages printed */
    ee_u32 seedcrc, crclist, crcmatrix, crcstate, crcfinal;
} bpx_coremark_report;

#define FAKE_TICKS_PER_SEC 1000u
#define FAKE_SECONDS 10u

void start_time(void) { }
void stop_time(void) { }
CORE_TICKS get_time(void) { return FAKE_SECONDS * FAKE_TICKS_PER_SEC; }
secs_ret time_in_secs(CORE_TICKS ticks) { return (secs_ret)(ticks / FAKE_TICKS_PER_SEC); }

void portable_init(core_portable *p, int *argc, char *argv[])
{
    (void)argc;
    (void)argv;
    p->unused = 0;
}

static int starts_with(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++) return 0;
    return 1;
}

/* core_main.c prints: "seedcrc          : 0x%04x" (crc), "[%d]crclist       : 0x%04x" (context, crc)
   and the same for crcmatrix, crcstate, crcfinal; errors start with "[%u]ERROR", "ERROR",
   "Errors detected" or "Cannot validate". */
int ee_printf(const char *fmt, ...)
{
    struct bpx_coremark_report *r = &bpx_coremark_report;
    va_list ap;

    va_start(ap, fmt);
    if (starts_with(fmt, "Correct operation validated")) {
        r->validated = 1;
    } else if (starts_with(fmt, "seedcrc")) {
        r->seedcrc = va_arg(ap, unsigned);
    } else if (starts_with(fmt, "[%d]crc")) {
        unsigned crc;
        (void)va_arg(ap, int);
        crc = va_arg(ap, unsigned);
        switch (fmt[7]) {
        case 'l': r->crclist = crc; break;
        case 'm': r->crcmatrix = crc; break;
        case 's': r->crcstate = crc; break;
        case 'f': r->crcfinal = crc; break;
        default: break;
        }
    } else if (starts_with(fmt, "[%u]ERROR") || starts_with(fmt, "ERROR") ||
               starts_with(fmt, "Errors detected") || starts_with(fmt, "Cannot validate")) {
        r->errors++;
    }
    va_end(ap);
    return 0;
}

void portable_fini(core_portable *p)
{
    p->unused = 0;
    if (bpx_coremark_report.errors != 0)
        _exit(1);
    if (bpx_coremark_report.validated != 1)
        _exit(2);
}
