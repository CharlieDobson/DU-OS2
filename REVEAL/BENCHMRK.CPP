/*===========================================================================
 * BENCHMRK.CPP  -  CPU, FPU and memory benchmark
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * The OS/2 copy of DUSOURCE\REVEAL\BENCHMRK.CPP, by way of the Win32s one.
 * Same three tests, same two seconds each, same scaling, compiled as flat
 * 32-bit code - so, as with Reveal32, the scores are NOT comparable with the
 * 16-bit builds' on the same machine.  Compare 32-bit with 32-bit.
 *
 * This file runs on the benchmark worker thread (PMDATA.CPP): it makes no PM
 * calls, and polls 'cancel' once per batch so that closing Reveal part way
 * through does not have to wait out the full six seconds.
 *===========================================================================*/

#include "benchmrk.hpp"
#include <stdlib.h>             // malloc, free
#include <time.h>               // clock()

// forward declarations
static unsigned long CpuKiloInstructionsPerSecond( const volatile unsigned long* cancel );
static unsigned long FpuKiloOperationsPerSecond( const volatile unsigned long* cancel );
static unsigned long MemoryKiloOperationsPerSecond( const volatile unsigned long* cancel );

static int Stopped( const volatile unsigned long* cancel )
{
    return cancel != NULL && *cancel != 0;
}

void RunBenchmark( benchmark_results_t* results, int has_fpu,
                   const volatile unsigned long* cancel )
{
    if ( !results ) return;

    results->cpu_kips   = CpuKiloInstructionsPerSecond( cancel );
    results->fpu_kflops = ( has_fpu && !Stopped( cancel ) )
                          ? FpuKiloOperationsPerSecond( cancel ) : 0;
    results->mem_kops   = Stopped( cancel ) ? 0 : MemoryKiloOperationsPerSecond( cancel );
}

static unsigned long CpuKiloInstructionsPerSecond( const volatile unsigned long* cancel )
{
    unsigned long batches = 0;
    clock_t       start_time = clock();

    if ( start_time == (clock_t)-1 ) return 0;

    do {
        volatile unsigned long result;
        unsigned int           i;

        for ( i = 0; i < 10000; ++i ) {
            // simulate tight loop of CPU instructions.  EBX is the one
            // register the compiler wants kept, and it is saved AFTER the
            // last variable is named and restored before the store.
            __asm {
                push ebx
                add eax, ecx
                sub eax, edx
                xor edx, eax
                and ebx, eax
                shl edx, 1
                or eax, ebx
                dec edx
                pop ebx
                mov result, eax
            }
        }
        ++batches;
    } while( ( clock() - start_time ) < ( 2 * CLOCKS_PER_SEC ) && !Stopped( cancel ) );

    return (unsigned long)( (double)batches * 80.0 / 2.0 );    /* see the memory test */
}

static unsigned long FpuKiloOperationsPerSecond( const volatile unsigned long* cancel )
{
    unsigned long batches = 0;
    clock_t       start_time;

    __asm { fninit }   // initialize FPU

    start_time = clock();
    if ( start_time == (clock_t)-1 ) return 0;

    do {
        volatile double result;
        unsigned int    i;

        for ( i = 0; i < 10000; ++i ) {
            // simulate tight loop of FPU instructions
            __asm {
                fldpi                   // ST(0)=pi
                fld1                    // ST(0)=1.0, ST(1)=pi
                fadd st(0), st(1)       // ST(0)=1+pi
                fmul st(0), st(1)       // ST(0)=(1+pi)*pi
                fdiv st(0), st(1)       // ST(0)=(1+pi)
                fabs
                fchs                    // ST(0)=-(1+pi)
                fstp result             // store, pop; ST(0)=pi
                fstp st(0)              // pop remaining; stack empty
            }
        }
        ++batches;
    } while( ( clock() - start_time ) < ( 2 * CLOCKS_PER_SEC ) && !Stopped( cancel ) );

    return (unsigned long)( (double)batches * 90.0 / 2.0 );    /* see the memory test */
}

static unsigned long MemoryKiloOperationsPerSecond( const volatile unsigned long* cancel )
{
    unsigned long   batches = 0;
    const size_t    buffer_size = 32767;
    char*           buffer1 = (char*)malloc( buffer_size );
    char*           buffer2 = (char*)malloc( buffer_size );
    clock_t         start_time;
    size_t          i;

    if ( !buffer1 || !buffer2 ) {
        free( buffer1 );
        free( buffer2 );
        return 0;
    }

    start_time = clock();
    if ( start_time == (clock_t)-1 ) { free( buffer1 ); free( buffer2 ); return 0; }

    do {
        // simulate memory operations
        for ( i = 0; i < buffer_size; ++i ) {
            buffer1[i] = (char)( i & 0xFF );
        }

        for ( i = 0; i < buffer_size; ++i ) {
            buffer2[i] = buffer1[i];
        }

        for ( i = 0; i < buffer_size; ++i ) {
            volatile char temp = buffer2[i];
            (void)temp;
        }
        ++batches;
    } while( ( clock() - start_time ) < ( 2 * CLOCKS_PER_SEC ) && !Stopped( cancel ) );

    free( buffer1 );
    free( buffer2 );

    /* In double, not unsigned long: the DOS original capped 'batches' at
       43,690 so that batches * 98,304 could not overflow, and a current
       processor reaches that cap well inside the two seconds.  See the
       Win32s copy. */
    return (unsigned long)( (double)batches * 98304.0 / 2000.0 );
}
