/* BPX-1 bench/ext: board support and main() for the Embench-IoT programs on the BPX-1 bench
   runtime (bench/rt: Spike, no OS, no timer). Written for BPX-1; it only uses the interface that
   the suite's support.h declares.

   Scaling: the suite runs its kernel LOCAL_SCALE_FACTOR * CPU_MHZ times (LOCAL_SCALE_FACTOR is fixed
   in each benchmark's source). bench/ext/build_ext.sh passes -DCPU_MHZ=<n> from programs_ext.txt;
   warm_caches() adds WARMUP_HEAT more runs of the kernel before that.

   Exit code: 0 = verify_benchmark() accepted the result; 1 = it rejected the result;
              3 = the benchmark has no self-check (verify_benchmark() returned -1). */
#include "support.h"

#ifndef CPU_MHZ
#error "CPU_MHZ must be defined on the command line (see programs_ext.txt)"
#endif
#ifndef WARMUP_HEAT
#define WARMUP_HEAT 1
#endif

/* There is no timer to start and no board to set up. */
void initialise_board(void) { }
void start_trigger(void) { }
void stop_trigger(void) { }

int main(void)
{
    int result, verdict;

    initialise_board();
    initialise_benchmark();
    warm_caches(WARMUP_HEAT);
    start_trigger();
    result = benchmark();
    stop_trigger();
    verdict = verify_benchmark(result);
    if (verdict == -1)
        return 3;
    return verdict ? 0 : 1;
}
