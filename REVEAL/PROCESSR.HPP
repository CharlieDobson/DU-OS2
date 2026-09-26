/*===========================================================================
 * PROCESSR.HPP  -  Processor identification structure and declarations
 *
 * The OS/2 copy of DUSOURCE\REVEAL\PROCESSR.HPP (by way of Win32s): the
 * structure is unchanged, so the page builders read it the same way in
 * every build.
 *
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *===========================================================================*/
// processor definition file

#ifndef PROCESSOR_HPP
#define PROCESSOR_HPP 1

union cpuid_t
{
    unsigned long full;
    struct {
        unsigned long stepping:4,
        model:4,
        family:4,
        type:2,
        reserved:2,
        extmodel:4,
        extfamily:8,
        extreserved:4;
    } bits;
};

union cpu_features1_t
{
    unsigned long full;
    struct {
        unsigned long fpu:1,
        vme:1,
        debug:1,
        pse:1,
        tsc:1,
        msr:1,
        pae:1,
        mce:1,
        cx8:1,
        apic:1,
        reserved1:1,
        sep:1,
        mtrr:1,
        pge:1,
        mca:1,
        cmov:1,
        pat:1,
        pse36:1,
        psn:1,
        clflush:1,
        reserved2:1,
        ds:1,
        acpi:1,
        mmx:1,
        fxsr:1,
        sse:1,
        sse2:1,
        selfsnoop:1,
        htt:1,
        tm:1,
        ia64:1,
        pbe:1;
    } bits;
};

union cpu_features2_t
{
    unsigned long full;
    struct {
        unsigned long sse3:1,
        pclmulqdq:1,
        dtes64:1,
        monitor:1,
        dscpl:1,
        vmx:1,
        smx:1,
        eist:1,
        tm2:1,
        ssse3:1,
        cnxtid:1,
        sdbg:1,
        fma:1,
        cx16:1,
        xtpr:1,
        pdcm:1,
        reserved0:1,
        pcid:1,
        dca:1,
        sse41:1,
        sse42:1,
        x2apic:1,
        movbe:1,
        popcnt:1,
        tscdeadline:1,
        aes:1,
        xsave:1,
        osxsave:1,
        avx:1,
        f16c:1,
        rdrnd:1,
        hypervisor:1;
    } bits;
};

// leaf 1 EDX
union intel_features1_t
{
    unsigned long full;
    struct {
        unsigned long fpu:1,
        vme:1,
        debug:1,
        pse:1,
        tsc:1,
        msr:1,
        pae:1,
        mce:1,
        cx8:1,
        apic:1,
        reserved1:1,
        sep:1,
        mtrr:1,
        pge:1,
        mca:1,
        cmov:1,
        pat:1,
        pse36:1,
        psn:1,
        clflush:1,
        reserved2:1,
        ds:1,
        acpi:1,
        mmx:1,
        fxsr:1,
        sse:1,
        sse2:1,
        selfsnoop:1,
        htt:1,
        tm:1,
        ia64:1,
        pbe:1;
    } bits;
};

// extended leaf 1 EDX
union amd_features1_t {
    unsigned long full;
    struct {
        unsigned long fpu:1,
        vme:1,
        debug:1,
        pse:1,
        tsc:1,
        msr:1,
        pae:1,
        mce:1,
        cx8:1,
        apic:1,
        reserved0:1,
        syscall:1,
        mtrr:1,
        pge:1,
        mca:1,
        cmov:1,
        pat:1,
        pse36:1,
        reserved1:1,
        reserved2:1,
        nx:1,
        reserved3:1,
        mmxext:1,
        mmx:1,
        fxsr:1,
        fxsr_opt:1,
        pdpe1gb:1,
        rdtscp:1,
        reserved4:1,
        lm:1,
        threednowext:1,
        threednow:1;
    } bits;
};

// leaf 1 ECX
union intel_features2_t
{
    unsigned long full;
    struct {
        unsigned long sse3:1,
        pclmulqdq:1,
        dtes64:1,
        monitor:1,
        dscpl:1,
        vmx:1,
        smx:1,
        eist:1,
        tm2:1,
        ssse3:1,
        cnxtid:1,
        sdbg:1,
        fma:1,
        cx16:1,
        xtpr:1,
        pdcm:1,
        reserved0:1,
        pcid:1,
        dca:1,
        sse41:1,
        sse42:1,
        x2apic:1,
        movbe:1,
        popcnt:1,
        tscdeadline:1,
        aes:1,
        xsave:1,
        osxsave:1,
        avx:1,
        f16c:1,
        rdrnd:1,
        hypervisor:1;
    } bits;
};

// Extended leaf 1 ECX
union amd_features2_t
{
    unsigned long full;
    struct {
        unsigned long lahf_lm:1,
        cmp_legacy:1,
        svm:1,
        extapic:1,
        cr8_legacy:1,
        cabm_lzcnt:1,
        sse4a:1,
        misalign_sse:1,
        threednow_prefetch:1,
        osvw:1,
        ibs:1,
        xop:1,
        skinit:1,
        wdt:1,
        reserved0:1,
        lwp:1,
        fma4:1,
        tce:1,
        reserved1:1,
        nodeid_msr:1,
        reserved2:1,
        tbm:1,
        topoext:1,
        perfctr_core:1,
        perfctr_nb:1,
        str_perfmon:1,
        dbx:1,
        perf_tsc:1,
        pcx_l2i:1,
        monitorx:1,
        addr_mask_ext:1,
        reserved3:1;
    } bits;
};

typedef struct processor_t
{
    char vendor[13];
    char description[40];
    char brand[49];
    char coprocessor_description[40];
    int processor_family;
    int coprocessor_family;
    unsigned long mhz;
    cpuid_t cpuid;
    cpu_features1_t cpu_features1;
    cpu_features2_t cpu_features2;
    intel_features1_t intel_features1;
    intel_features2_t intel_features2;
    amd_features1_t amd_features1;
    amd_features2_t amd_features2;
} processor_t;

int  HasFdivBug( void );          // Pentium FDIV erratum present
void GetProcessorInfo( processor_t* proc );

#endif // end PROCESSOR_HPP
