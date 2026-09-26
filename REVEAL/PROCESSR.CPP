/*===========================================================================
 * PROCESSR.CPP  -  CPU identification: type, stepping, features, FPU
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * The OS/2 copy of the Win32s port of DUSOURCE\REVEAL\PROCESSR.CPP.  The
 * naming half - the vendor strings and the big vendor x family x model
 * table - is carried over verbatim.  The detecting half is the Win32s
 * build's flat 32-bit version, which runs unchanged at ring 3 under OS/2:
 * CPUID, RDTSC and the EFLAGS tests need no privilege.  The one change is
 * where a CPU without CPUID learns about its coprocessor: DosDevConfig
 * instead of the BIOS data area or the registry.
 *
 * What the Win32s build notes about the DOS original's detection:
 *
 *   CPUID.ASM         16-bit, and only there because MSVC 1.52c's inline
 *                     assembler cannot express a 32-bit register.  A 32-bit
 *                     compiler can, so CPUID and the EFLAGS tests are inline
 *                     here.  CPUID and RDTSC are EMITTED as opcode bytes:
 *                     MSVC 2.2's assembler predates both mnemonics.
 *   GetCpuType()      8086/186/286 cannot run Win32 code at all, so the only
 *                     question left is 386 or 486, and the AC flag answers it.
 *   INT 15h AX=C910h  a BIOS call; nothing in Win32 can issue one.  A part
 *                     without CPUID is named from its class alone.
 *   Cyrix DIR0/DIR1   port I/O at 22h/23h.  Not attempted from under a
 *                     running Windows; the Cyrix division test (no port
 *                     access, flags only) still says it is a Cyrix.
 *   GetFpuType()      never.  Its FWAIT is what faulted the Win16 build
 *                     under 386 enhanced mode.  CPUID says whether there is
 *                     an FPU; without CPUID, DosDevConfig does (it reports
 *                     the BIOS equipment word's coprocessor bit).
 *
 * A note on the inline assembly.  Every variable an __asm block names is
 * read or written while ESP is where the compiler left it: a push comes
 * AFTER the last load and the matching pop BEFORE the first store, because a
 * compiler that addresses locals off ESP would otherwise read the wrong
 * slot.  And every register CPUID or RDTSC writes is written by an ordinary
 * instruction too, so a compiler that tracks registers by reading the block
 * - it cannot read an _emit - still knows they are clobbered.
 *===========================================================================*/

#define INCL_DOSDEVICES
#define INCL_DOSERRORS

#include <os2.h>
#include <stdio.h>              // _snprintf
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "processr.hpp"

/*===========================================================================
 * The metal
 *=========================================================================*/

/*---------------------------------------------------------------------------
 * FlagToggles - can this EFLAGS bit be changed?
 *
 *   bit 18 (AC)  the 386 hardwires it to zero, so a 486 or later lets it move
 *   bit 21 (ID)  settable exactly when the CPUID instruction exists
 *
 * The original flags are put back before anything else runs.
 *-------------------------------------------------------------------------*/

static int FlagToggles( unsigned long flag_bit )
{
    unsigned long changed;

    __asm {
        mov     edx, flag_bit
        pushfd
        pop     eax
        mov     ecx, eax            // ecx = original flags
        xor     eax, edx            // flip the bit
        push    eax
        popfd
        pushfd
        pop     eax                 // eax = what stuck
        push    ecx
        popfd                       // restore the original flags
        xor     eax, ecx            // which bits moved
        mov     changed, eax
    }

    return ( changed & flag_bit ) != 0;
}

static int IsCpuIdSupported( void )  { return FlagToggles( 0x00200000UL ); }
static int Is486OrLater( void )      { return FlagToggles( 0x00040000UL ); }

/*---------------------------------------------------------------------------
 * CpuId - run CPUID at 'level' (subleaf 0) and return all four registers
 *-------------------------------------------------------------------------*/

static void CpuId( unsigned long level, unsigned long regs[ 4 ] )
{
    __asm {
        mov     eax, level
        mov     edx, regs           // both loads before the pushes
        push    ebx
        push    esi
        mov     esi, edx
        xor     ebx, ebx
        xor     ecx, ecx            // subleaf 0
        xor     edx, edx
        _emit   0x0F                // CPUID
        _emit   0xA2
        mov     [esi], eax
        mov     [esi+4], ebx
        mov     [esi+8], ecx
        mov     [esi+12], edx
        pop     esi
        pop     ebx
    }
}

/* The DOS build's CpuIdReg, kept so the code below reads the same. */
static unsigned long CpuIdReg( unsigned long level, int which )
{
    unsigned long regs[ 4 ];

    CpuId( level, regs );
    return regs[ which & 3 ];
}

/*---------------------------------------------------------------------------
 * IsCyrixCpu - the Cyrix division test
 *
 * A 5/2 divide leaves the arithmetic flags undefined.  Intel and AMD parts
 * change them; Cyrix parts leave them exactly as they were.  So clear them
 * with SAHF, divide, and see whether they are still clear.  No ports, no
 * privilege: it is only arithmetic.
 *-------------------------------------------------------------------------*/

static int IsCyrixCpu( void )
{
    unsigned long flags_after;

    __asm {
        xor     eax, eax
        sahf                        // SF ZF AF PF CF all clear
        mov     eax, 5
        mov     ecx, 2
        div     cl                  // AL = 2, AH = 1; flags "undefined"
        lahf
        shr     eax, 8
        and     eax, 0xFF
        mov     flags_after, eax
    }

    return flags_after == 0x02;     /* only the always-one bit 1 is set     */
}

/*---------------------------------------------------------------------------
 * HasFdivBug - the 1994 Pentium division erratum
 *
 * 4195835 - ( 4195835 / 3145727 ) * 3145727 is 0 on a correct FPU and 256
 * on a flawed one.  volatile, so the compiler cannot fold it at compile time
 * and quietly report the answer of the machine that built the program.
 *-------------------------------------------------------------------------*/

int HasFdivBug( void )
{
    volatile double x = 4195835.0;
    volatile double y = 3145727.0;
    volatile double r;

    r = x - ( x / y ) * y;
    return ( r > 1.0 || r < -1.0 ) ? 1 : 0;
}

/*---------------------------------------------------------------------------
 * FpuFromSystem - is there a coprocessor, for a CPU that cannot say?
 *
 * Only asked when there is no CPUID (a 386, or an early 486).  Nothing here
 * touches the coprocessor itself: OS/2 emulates one when it is absent, so
 * executing FPU instructions would get the emulator's answer.  DosDevConfig
 * reports what the hardware has.
 *-------------------------------------------------------------------------*/

static int FpuFromSystem( void )
{
    BYTE present = 0;

    if ( DosDevConfig( &present, DEVINFO_COPROCESSOR ) != NO_ERROR ) return 0;
    return present ? 1 : 0;
}

/*===========================================================================
 * Naming
 *=========================================================================*/

void RemovePreceedingSpaces( char* string )
{
    char* start = string;

    // find the first non-space character
    while ( *start == ' ' ) {
        start++;
    }

    // shift the string to the left
    if ( start != string ) {
        memmove( string, start, strlen( start ) + 1 );
    }
}

/***********************************************************************
;   Function:   int GetVendorString ()
;   Purpose:    Get CPU vendor string if supported
;   Inputs:     pointer to char array that will receive the string
;   Returns:    highest level CPUID instruction supported
;***********************************************************************/
static int GetVendorString( char* vendor )
{
    char          cpuid_string[13];
    unsigned long regs[ 4 ];

    if ( NULL == vendor ) return 0;

    // CPUID level 0: highest standard level (EAX) + vendor string (EBX,EDX,ECX)
    CpuId( 0, regs );
    memcpy( &cpuid_string[0], &regs[1], 4 );    // EBX
    memcpy( &cpuid_string[4], &regs[3], 4 );    // EDX
    memcpy( &cpuid_string[8], &regs[2], 4 );    // ECX
    cpuid_string[sizeof( cpuid_string ) - 1] = '\0';

    if ( strcmp( cpuid_string, "GenuineIntel" ) == 0 ) {
        strcpy( vendor, "Intel" );
    }
    else if ( strcmp( cpuid_string, "AuthenticAMD" ) == 0 ) {
        strcpy( vendor, "AMD" );
    }
    else if ( strcmp( cpuid_string, "CyrixInstead" ) == 0 ) {
        strcpy( vendor, "Cyrix" );
    }
    else if ( strcmp( cpuid_string, "CentaurHauls" ) == 0 ) {
        strcpy( vendor, "IDT" );
    }
    else if ( strcmp( cpuid_string, "VIA VIA VIA " ) == 0 ) {
        strcpy( vendor, "VIA" );
    }
    else if ( strcmp( cpuid_string, "NexGenDriven" ) == 0 ) {
        strcpy( vendor, "NexGen" );
    }
    else if ( strcmp( cpuid_string, "RiseRiseRise" ) == 0 ) {
        strcpy( vendor, "Rise" );
    }
    else if ( strcmp( cpuid_string, "UMC UMC UMC " ) == 0 ) {
        strcpy( vendor, "UMC" );
    }
    else if ( strcmp( cpuid_string, "TCGTCGTCGTCG" ) == 0 ) {
        strcpy( vendor, "QEMU" );
    }
    else if ( strcmp( cpuid_string, "VBoxVBoxVBox" ) == 0 ) {
        strcpy( vendor, "VirtualBox" );
    }
    else {
        strcpy( vendor, "Unknown" );
    }

    return (int)regs[0];
}

/*---------------------------------------------------------------------------
 * DescribeByModel
 *
 * Fill proc->description from a model-indexed table, with bounds checking and
 * optional OverDrive tagging.  Replaces the copy/pasted per-vendor blocks.
 *
 *   table          - array of model-indexed name strings
 *   min_model      - lowest valid model index (inclusive)
 *   max_model      - one past the highest valid model index (exclusive)
 *   allow_overdrive- when true, cpuid type==1 prints "<name> OverDrive"
 *
 * If the model is out of [min,max) the description is left untouched (the
 * caller has already defaulted it, or leaves it "Unknown").
 *-------------------------------------------------------------------------*/
static void DescribeByModel( processor_t* proc, const char* const table[],
                             int min_model, int max_model, int allow_overdrive )
{
    int model = (int)proc->cpuid.bits.model;

    if ( model < min_model || model >= max_model ) {
        return;                 // out of range -- leave description as-is
    }

    if ( allow_overdrive && proc->cpuid.bits.type == 1 ) {
        _snprintf( proc->description, sizeof( proc->description ),
                   "%s OverDrive", table[model] );
    }
    else {
        _snprintf( proc->description, sizeof( proc->description ),
                   "%s", table[model] );
    }
}

/*---------------------------------------------------------------------------
 * DescribeCpuId
 *
 * Turn the (already-read) vendor + cpuid family/model into a human-readable
 * proc->description.  This is the big switch, now free of the CPUID plumbing.
 *-------------------------------------------------------------------------*/
static void DescribeCpuId( processor_t* proc )
{
    static const char* Intel486Description[] = {
        "Unknown", "486DX", "486SX", "486DX/2", "486SL", "486 Unknown",
        "486 Unknown", "486DX/2", "486DX/4", "486 Unknown"
    };
    static const char* AMD486Description[] = {
        "Unknown", "486 Unknown", "486 Unknown", "486DX/2", "486 Unknown",
        "486 Unknown", "486 Unknown", "486DX/2", "Am486DX/4", "Am486DX/4",
        "486 Unknown", "486 Unknown", "486 Unknown", "486 Unknown",
        "Am5x86", "Am5x86"
    };
    static const char* Intel586Description[] = {
        "Unknown", "Pentium", "Pentium", "Pentium 63/83MHz", "Pentium MMX"
    };
    static const char* AMDKxDescription[] = {
        "K5", "K5", "K5", "K5", "K5 Unknown", "K5 Unknown", "K6", "K6",
        "K6-2", "K6-III", "K6-III Unknown", "K6-III Unknown",
        "K6-III Unknown", "K6-2+ or K6-III+"
    };
    static const char* IDTWinChipDescription[] = {
        "Unknown", "WinChip Unknown", "WinChip Unknown", "WinChip Unknown",
        "WinChip C6", "WinChip Unknown", "WinChip Unknown", "WinChip Unknown",
        "WinChip 2"
    };
    static const char* Intel686Description[] = {
        "686 Unknown", "Pentium Pro", "686 Unknown", "Pentium II",
        "686 Unknown", "Pentium II or Celeron", "Celeron", "Pentium III",
        "Pentium III or Celeron", "Pentium or Celeron M", "Pentium III Xeon",
        "Pentium III or Celeron", "Pentium III", "Pentium or Celeron M",
        "Core", "Core 2"
    };
    static const char* Cyrix686Description[] = {
        "6x86 Unknown", "6x86 Unknown", "6x86", "6x86"
    };
    static const char* Cyrix686MXDescription[] = {
        "6x86 MII", "6x86MX Unknown", "6x86MX Unknown", "6x86MX Unknown",
        "6x86MX Unknown", "III"
    };
    static const char* Via686Description[] = {
        "C3 Unknown", "C3 Unknown", "C3 Unknown", "C3 Unknown", "C3 Unknown",
        "C3 Unknown", "C3", "C3", "C3", "C3", "C7", "C7 Unknown",
        "C7 Unknown", "C7"
    };

    // Default; the vendor blocks below overwrite it on a hit.
    _snprintf( proc->description, sizeof( proc->description ), "Unknown" );

    if ( strcmp( proc->vendor, "Intel" ) == 0 ) {
        switch ( proc->processor_family ) {
            case 6:
                // SYSENTER/SYSEXIT not supported on the Pentium Pro (model 1)
                if ( proc->cpuid.bits.type != 1 && proc->cpuid.bits.model == 1 ) {
                    proc->intel_features1.bits.sep = 0;
                }
                DescribeByModel( proc, Intel686Description, 0, 16, 1 );
                break;
            case 5:
                DescribeByModel( proc, Intel586Description, 0, 5, 1 );
                break;
            case 4:
                DescribeByModel( proc, Intel486Description, 0, 9, 1 );
                break;
        }
    }
    else if ( strcmp( proc->vendor, "AMD" ) == 0 ) {
        switch ( proc->processor_family ) {
            case 5:  DescribeByModel( proc, AMDKxDescription, 0, 14, 0 );  break;
            case 4:  DescribeByModel( proc, AMD486Description, 0, 16, 0 );  break;
        }
    }
    else if ( strcmp( proc->vendor, "Cyrix" ) == 0 ) {
        switch ( proc->processor_family ) {
            case 6:
                // Only models 0 and 5 are named; both land in the MX table.
                if ( proc->cpuid.bits.model == 0 || proc->cpuid.bits.model == 5 ) {
                    _snprintf( proc->description, sizeof( proc->description ),
                               "%s", Cyrix686MXDescription[proc->cpuid.bits.model] );
                }
                break;
            case 5:
                DescribeByModel( proc, Cyrix686Description, 1, 4, 0 );
                break;
            case 4:
                strncpy( proc->description, "Cx5x86 or 486",
                         sizeof( proc->description ) );
                proc->description[sizeof( proc->description ) - 1] = '\0';
                break;
        }
    }
    else if ( strcmp( proc->vendor, "VIA" ) == 0 ) {
        if ( proc->processor_family == 6 ) {
            DescribeByModel( proc, Via686Description, 0, 14, 0 );
        }
    }
    else if ( strcmp( proc->vendor, "IDT" ) == 0 ) {
        if ( proc->processor_family == 5 ) {
            DescribeByModel( proc, IDTWinChipDescription, 1, 9, 0 );
        }
        proc->intel_features1.bits.cx8 = 1; // IDT parts have CMPXCHG8B, off by default
    }
    else if ( strcmp( proc->vendor, "NexGen" ) == 0 ) {
        if ( proc->processor_family == 5 ) {
            strncpy( proc->description, "Nx586", sizeof( proc->description ) );
            proc->description[sizeof( proc->description ) - 1] = '\0';
        }
    }
    else if ( strcmp( proc->vendor, "Rise" ) == 0 ) {
        if ( proc->processor_family == 5 ) {
            strncpy( proc->description, "mP6", sizeof( proc->description ) );
            proc->description[sizeof( proc->description ) - 1] = '\0';
        }
        proc->intel_features1.bits.cx8 = 1; // mP6 has CMPXCHG8B, off by default
    }
}

/*---------------------------------------------------------------------------
 * ReadCpuIdInfo
 *
 * The CPUID-capable path.  Always fills proc->vendor.  When CPUID level 1 is
 * available it also fills cpuid, the Intel/AMD feature unions,
 * processor_family, and the extended brand string.
 *
 * Returns the highest standard CPUID level supported (0 if none).
 *-------------------------------------------------------------------------*/
static int ReadCpuIdInfo( processor_t* proc )
{
    unsigned long highest_cpuid = (unsigned long)GetVendorString( proc->vendor );

    if ( highest_cpuid > 0 ) {
        unsigned long regs[ 4 ];

        // CPUID level 1: family/model/stepping (EAX) + feature flags (EDX/ECX)
        CpuId( 0x00000001UL, regs );
        proc->cpuid.full         = regs[0];
        proc->cpu_features1.full = proc->intel_features1.full = regs[3];
        proc->cpu_features2.full = proc->intel_features2.full = regs[2];

        // AMD has its own extended (0x80000001) feature set
        if ( strcmp( proc->vendor, "AMD" ) == 0 &&
             CpuIdReg( 0x80000000UL, 0 ) >= 0x80000001UL ) {
            CpuId( 0x80000001UL, regs );
            proc->amd_features1.full = regs[3];     // EDX
            proc->amd_features2.full = regs[2];     // ECX
        }

        proc->processor_family = (int)proc->cpuid.bits.family;

        // Extended brand string (leaves 0x80000002..4), when there is one
        if ( CpuIdReg( 0x80000000UL, 0 ) >= 0x80000004UL ) {
            char          brand_string[49];
            unsigned long leaf;

            for ( leaf = 0; leaf < 3; ++leaf ) {
                CpuId( 0x80000002UL + leaf, regs );
                memcpy( &brand_string[leaf * 16], regs, 16 );
            }
            brand_string[48] = '\0';

            RemovePreceedingSpaces( brand_string );

            // Keep only if it starts with an alphanumeric (sanity check).
            if ( isalnum( (unsigned char)brand_string[0] ) ) {
                strncpy( proc->brand, brand_string, sizeof( proc->brand ) );
                proc->brand[sizeof( proc->brand ) - 1] = '\0';
            }
        }
    }

    return (int)highest_cpuid;
}

/*---------------------------------------------------------------------------
 * DescribeCoprocessor
 *
 * Name the FPU and (on family-5 parts with an on-die FPU) run the Pentium
 * FDIV-bug test.
 *-------------------------------------------------------------------------*/
static void DescribeCoprocessor( processor_t* proc )
{
    static const char* CoprocessorDescriptions[] = {
        "None", "8087", "80287", "80387"
    };

    int has_coprocessor = ( proc->coprocessor_family != 0 );

    if ( proc->processor_family > 5 ) {
        _snprintf( proc->coprocessor_description,
                   sizeof( proc->coprocessor_description ), "Integrated" );
    }
    else if ( proc->processor_family == 5 && proc->intel_features1.bits.fpu == 1 ) {
        strncpy( proc->coprocessor_description,
                 HasFdivBug() ? "Integrated (Pentium FDIV bug)" : "Integrated",
                 sizeof( proc->coprocessor_description ) );
    }
    else if ( strcmp( proc->vendor, "AMD" ) != 0 && proc->processor_family == 5
              && proc->intel_features1.bits.fpu == 0 && has_coprocessor ) {
        _snprintf( proc->coprocessor_description,
                   sizeof( proc->coprocessor_description ), "Nx587" );
    }
    else if ( ( proc->processor_family == 5 || proc->processor_family == 4 )
              && ( proc->intel_features1.bits.fpu == 1
                   || proc->amd_features1.bits.fpu == 1 ) ) {
        strncpy( proc->coprocessor_description, "Integrated",
                 sizeof( proc->coprocessor_description ) );
    }
    else if ( proc->processor_family == 4 && has_coprocessor ) {
        // a 486 with no CPUID: a DX has it on the die; an SX with a 487
        // is, as far as software can tell, the same thing
        strncpy( proc->coprocessor_description, "Integrated",
                 sizeof( proc->coprocessor_description ) );
    }
    else {
        _snprintf( proc->coprocessor_description,
                   sizeof( proc->coprocessor_description ), "%s",
                   CoprocessorDescriptions[proc->coprocessor_family & 3] );
    }
    proc->coprocessor_description[sizeof( proc->coprocessor_description ) - 1] = '\0';
}

/*---------------------------------------------------------------------------
 * GetProcessorInfo
 *
 * Orchestrator -- Gets all processor related information
 *-------------------------------------------------------------------------*/
void GetProcessorInfo( processor_t* proc )
{
    if ( NULL == proc ) return;

    memset( proc, 0, sizeof( *proc ) );

    /* OS/2 2.x needs a 386, so the question starts there */
    proc->processor_family = Is486OrLater() ? 4 : 3;

    if ( proc->processor_family > 3 && IsCpuIdSupported() ) {
        ReadCpuIdInfo( proc );      // vendor + (if available) cpuid/features/brand
        DescribeCpuId( proc );      // vendor x family x model -> name
    }
    else if ( proc->processor_family > 3 && IsCyrixCpu() ) {
        strcpy( proc->vendor, "Cyrix" );
        strcpy( proc->description, "Cx486 or Cx5x86" );
    }
    else {
        strcpy( proc->vendor, "\"Classic\"" );
        strcpy( proc->description, ( proc->processor_family == 4 ) ? "80486" : "80386" );
    }

    /* 0 = none, 3 = 80387 or on the die: the 8087/80287 question needs a
       CPU that cannot run this program */
    if ( proc->cpu_features1.bits.fpu || proc->amd_features1.bits.fpu ) {
        proc->coprocessor_family = 3;
    }
    else {
        proc->coprocessor_family = FpuFromSystem() ? 3 : 0;
    }

    DescribeCoprocessor( proc );
}
