/*===========================================================================
 * BENCHMRK.HPP  -  CPU, FPU and memory benchmark
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * The OS/2 copy of DUSOURCE\REVEAL\BENCHMRK.HPP, by way of the Win32s one.
 * RunBenchmark takes the coprocessor answer from the caller instead of
 * probing for one itself - OS/2 emulates a missing FPU, so a probe would
 * always say yes and the "KFLOPS" would be the emulator's.
 *
 * It runs on a worker thread here, so it also takes a cancel flag: every
 * timing loop checks it, and a set flag ends the run within one batch.
 * Scores from a cancelled run are meaningless and the caller discards them.
 *===========================================================================*/

#ifndef BENCHMRK_HPP
#define BENCHMRK_HPP

// struct to hold benchmark results
typedef struct benchmark_results_t
{
    unsigned long cpu_kips;
    unsigned long fpu_kflops;
    unsigned long mem_kops;
} benchmark_results_t;

void RunBenchmark( benchmark_results_t* results, int has_fpu,
                   const volatile unsigned long* cancel );

#endif // BENCHMRK_HPP
