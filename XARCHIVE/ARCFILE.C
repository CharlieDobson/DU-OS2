/*===========================================================================
 * ARCFILE.C  -  Format-agnostic archive front door (7z / zip dispatch)
 * Target: MSVC 2.2  Win32s
 *===========================================================================*/

#include <windows.h>     /* FILETIME / SYSTEMTIME conversion + wsprintf */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcfile.h"
#include "szarc.h"
#include "ziparc.h"
#include "rararc.h"
#include "rar5arc.h"
#include "diskarc.h"

#define FMT_7Z    1
#define FMT_ZIP   2
#define FMT_RAR   3      /* RAR 2.x/3.x (RAR4) */
#define FMT_RAR5  4
#define FMT_DISK  5      /* FAT12/16 floppy image (.img/.dsk) */

struct ArcFile {
    int          fmt;
    SzArchive   *sz;
    ZipArchive  *zip;
    RarArchive  *rar;
    Rar5Archive *rar5;
    DiskArchive *disk;
};

static const unsigned char SIG_7Z[6]  = { 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C };
static const unsigned char SIG_RAR[6] = { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07 };

/*===========================================================================
 * How much memory this program may use (see ARCDEFS.H)
 *
 * The old build baked 32 MB into a header and refused anything above it.
 * That number was wrong in both directions at once: too big for a 386 with
 * 8 MB, and absurdly small for the 8 GB DOS machines people actually run.
 * So ask the machine instead.
 *
 * Two questions get asked, because either alone lies:
 *
 *   1. The system: how much does it say is free?  Cheap, and it stops the
 *      probe below from trying to grab a gigabyte on a machine that has
 *      nothing like it.
 *   2. The heap: what is the largest block malloc will really hand over?
 *      This is the question that matters, since the LZMA dictionary is one
 *      allocation.  Under a DOS extender the system figure and the heap
 *      figure can differ by a lot, and only the second one is a promise.
 *
 * A binary search between the two, then three quarters of the answer: the
 * dictionary is the largest single allocation but not the only one, and the
 * packed buffer, the entry table and the output file's buffering all still
 * have to fit alongside it.
 *===========================================================================*/

#if !defined(_WIN32) && !defined(__OS2__)
#include <i86.h>            /* int386x, segread - the DOS DPMI query below */
#endif

static UInt32 g_memBudget = 0;      /* 0 = not measured yet          */
static UInt32 g_memForced = 0;      /* MemoryLimitMB, 0 = auto       */

/* The two raw readings the budget is derived from, kept so a user can be
 * asked what their machine actually reported instead of only what the
 * program decided.  "1 GB of RAM but it says 46 MB" is not answerable from
 * the budget alone: it is one number with three quite different causes
 * behind it, and these two separate them.
 *
 *   g_memSysFree - what the system claims is free in one block (DPMI 0500h
 *                  under DOS, GlobalMemoryStatus under Win32, DosQuerySysInfo
 *                  under OS/2).  0 when it would not say.
 *   g_memProbed  - the largest block malloc actually produced.
 *
 * A g_memSysFree near 64 MB on a machine with far more than that is the
 * classic one: the XMS 2.0 "free extended memory" call reports KILOBYTES in
 * a 16-bit register, so it saturates at 65535 KB and no extender asking it
 * can ever see past 64 MB.  XMS 3.0 (function 88h, HIMEM.SYS 3.x) has the
 * 32-bit version of the same call.  See ArcMemReport. */
static UInt32 g_memSysFree = 0;
static UInt32 g_memProbed  = 0;

void ArcMemSetLimit( UInt32 mb )
{
    g_memForced = mb;
    g_memBudget = 0;                /* re-decide on the next question */
}

/* What the system claims is available, in bytes, or 0 when it will not say.
 * Only ever used as the upper bound of the heap probe, never as the answer. */
static UInt32 ArcMemSystemFree( void )
{
#if defined(_WIN32)
    MEMORYSTATUS ms;
    DWORD        total;

    ms.dwLength = sizeof( ms );
    GlobalMemoryStatus( &ms );

    /* Free RAM plus whatever the swapfile can still grow by, but never more
     * address space than is left - on Win32s the last of those is the one
     * that bites first. */
    total = ms.dwAvailPhys;
    if ( total > 0xFFFFFFFFUL - ms.dwAvailPageFile ) total = 0xFFFFFFFFUL;
    else                                             total += ms.dwAvailPageFile;
    if ( total > ms.dwAvailVirtual ) total = ms.dwAvailVirtual;
    return (UInt32)total;

#elif defined(__OS2__)
    /* OS2PLAT.C: DosQuerySysInfo's free-memory and largest-private-allocation
     * figures, whichever is smaller, or 0 when OS/2 will not say.  This one
     * is not optional the way the Win32 figure is: OS/2 would happily grant a
     * 768 MB probe by growing the swap file, so the bound has to come from
     * somewhere other than malloc's willingness. */
    extern unsigned int Os2MemFree( void );
    return (UInt32)Os2MemFree();

#else
    /* DPMI 0500h "get free memory information": a 48-byte block whose first
     * dword is the largest available free block in bytes.  DOS/32A answers
     * it whether it is running raw, under VCPI, or under a DPMI host; a host
     * that does not sets carry, and we fall through to the heap probe. */
    union  REGS  r;
    struct SREGS sr;
    UInt32       info[12];
    int          i;

    for ( i = 0; i < 12; i++ ) info[i] = 0;
    memset( &r, 0, sizeof( r ) );
    segread( &sr );
    r.x.eax = 0x0500;
    r.x.edi = (unsigned long)info;      /* flat offset; ES == DS in this model */
    sr.es   = sr.ds;
    int386x( 0x31, &r, &r, &sr );

    if ( r.x.cflag ) return 0;
    if ( info[0] == 0 || info[0] == 0xFFFFFFFFUL ) return 0;
    return info[0];
#endif
}

/* Largest block malloc will actually produce, at 1 MB resolution, searched
 * between 0 and 'hi'.  Each failed try costs nothing and each successful one
 * is freed immediately, so this is about a dozen allocations. */
static UInt32 ArcMemProbeHeap( UInt32 hi )
{
    UInt32 lo = 0, mid;
    void  *p;

    while ( hi - lo > 1024UL * 1024 )
    {
        mid = lo + ( hi - lo ) / 2;
        p   = malloc( mid );
        if ( p ) { free( p ); lo = mid; }
        else       hi = mid;
    }
    return lo;
}

/* The measured budget in bytes, decided once. */
static UInt32 ArcMemBudget( void )
{
    UInt32 sys, hi, got;

    if ( g_memBudget ) return g_memBudget;

    if ( g_memForced )                       /* the preference wins outright */
    {
        g_memBudget = ( g_memForced > ARC_MEM_CEILING / ( 1024UL * 1024 ) )
                    ? ARC_MEM_CEILING
                    : g_memForced * 1024UL * 1024;
        if ( g_memBudget < ARC_MEM_FLOOR ) g_memBudget = ARC_MEM_FLOOR;
        return g_memBudget;
    }

    sys = ArcMemSystemFree();

    /*---- What the system says is a HINT, not a lid ------------------------ *
     * The system figure exists to stop the probe measuring memory that is
     * not really there.  That is a live danger on Win32 and OS/2, where a
     * large malloc succeeds by growing the swap file and then measures
     * address space rather than RAM - so there it stays a hard cap.
     *
     * Under DOS it is neither necessary nor safe.  Nothing overcommits: a
     * malloc that succeeds has the memory, so the probe cannot be fooled and
     * needs no lid.  And the figure itself is not to be trusted downward -
     * the DPMI host is answering out of a pool it obtained through XMS, and
     * the XMS 2.0 free-memory call reports kilobytes in a 16-bit register, so
     * a machine with a gigabyte can be told 64 MB and nothing further up the
     * stack ever learns otherwise.  Capping the probe at that figure turned a
     * host's underestimate into OUR refusal, on a machine that would have
     * handed over the memory if it had simply been asked.
     *
     * So on DOS the probe runs to the ceiling and lets malloc give the
     * answer; sys is kept only for the diagnostics box, which is where an
     * underestimate is worth showing rather than acting on.
     *---------------------------------------------------------------------- */
#if defined(_WIN32) || defined(__OS2__)
    hi = sys ? sys : ARC_MEM_CEILING;
#else
    hi = ARC_MEM_CEILING + ARC_MEM_CEILING / 2;
#endif

    /* Never ask the heap for more than the largest dictionary that can exist,
     * plus the third we are about to give away - probing past that would only
     * measure address space nothing will ever want. */
    if ( hi > ARC_MEM_CEILING + ARC_MEM_CEILING / 2 )
        hi = ARC_MEM_CEILING + ARC_MEM_CEILING / 2;

    got = ArcMemProbeHeap( hi );

    g_memSysFree = sys;                      /* keep the two raw readings */
    g_memProbed  = got;

    got -= got / 4;                          /* leave a quarter for the rest */

    if ( got < ARC_MEM_FLOOR )   got = ARC_MEM_FLOOR;
    if ( got > ARC_MEM_CEILING ) got = ARC_MEM_CEILING;

    g_memBudget = got;
    return g_memBudget;
}

UInt32 ArcMaxDictSize( void )   { return ArcMemBudget(); }
UInt32 ArcMaxBufferSize( void ) { return ArcMemBudget(); }
UInt32 ArcMemLimitMB( void )    { return ArcMemBudget() / ( 1024UL * 1024 ); }

/*---- Where the budget came from ------------------------------------------- *
 * The three numbers the decision was made from, so a report of "1 GB of RAM
 * and it says 46 MB" can be settled rather than guessed at.  Reading them
 * forces the measurement if it has not happened yet, so they always describe
 * the budget in force.
 *-------------------------------------------------------------------------- */
void ArcMemReport( UInt32 *sysFreeMB, UInt32 *largestMB, UInt32 *budgetMB,
                   int *forced )
{
    UInt32 budget = ArcMemBudget();          /* measures on the first call */

    if ( sysFreeMB ) *sysFreeMB = g_memSysFree / ( 1024UL * 1024 );
    if ( largestMB ) *largestMB = g_memProbed / ( 1024UL * 1024 );
    if ( budgetMB )  *budgetMB  = budget / ( 1024UL * 1024 );
    if ( forced )    *forced    = ( g_memForced != 0 );
}

/*---- How many entries will fit (see ARCDEFS.H) ---------------------------- *
 * What one listed entry costs while an archive is open: the backend's entry
 * record - whichever of the five is largest, all of them dominated by their
 * fixed SZ_MAX_NAME name field - plus the parse-time scratch 7z keeps beside
 * it and the front end's per-row bookkeeping.  Measured from the structs
 * rather than guessed, so it stays right if one of them gains a field.
 *-------------------------------------------------------------------------- */
static UInt32 ArcEntryCost( void )
{
    UInt32 big = (UInt32)sizeof( SzEntry );

    if ( (UInt32)sizeof( ZipEntry )  > big ) big = (UInt32)sizeof( ZipEntry );
    if ( (UInt32)sizeof( RarEntry )  > big ) big = (UInt32)sizeof( RarEntry );
    if ( (UInt32)sizeof( Rar5Entry ) > big ) big = (UInt32)sizeof( Rar5Entry );
    if ( (UInt32)sizeof( DiskEntry ) > big ) big = (UInt32)sizeof( DiskEntry );

    /* Generous slack for 7z's ParseState scratch (a substream record plus
     * three bit arrays plus a CRC per entry, all live at once while the
     * header is read) and for the view row and mark byte the TUI keeps. */
    return big + 64;
}

UInt32 ArcMaxEntries( void )
{
    /* Half the budget, not all of it: the entry table has to coexist with
     * the dictionary during extraction, and the table lives for the whole
     * time the archive is open while the dictionary comes and goes. */
    UInt32 n = ( ArcMemBudget() / 2 ) / ArcEntryCost();

    if ( n < ARC_MIN_ENTRIES ) n = ARC_MIN_ENTRIES;
    if ( n > ARC_MAX_ENTRIES ) n = ARC_MAX_ENTRIES;
    return n;
}

int ArcCheckEntryCount( UInt32 n )
{
    if ( n > ARC_MAX_ENTRIES ) return SZ_ERR_TOOBIG;   /* corrupt, not big */
    if ( n > ArcMaxEntries() ) return SZ_ERR_NORAM;    /* real, but too big
                                                        * for this machine */
    return SZ_OK;
}

/* Cap on the decompressed image we will unwrap from a .imz and hold in memory
 * (a mounted image lives in RAM whole - see SZ_MAX_BUFFER_SIZE). */
#define IMZ_MAX_IMAGE  SZ_MAX_BUFFER_SIZE

/* A WinImage .imz is an ordinary zip holding a single raw disk image (.ima).
 * If 'zip' looks like one, decompress that image into memory and mount it as a
 * FAT volume so the user browses the files *inside* the image, not the .ima
 * entry.  Returns 1 and sets *outDisk on success; 0 to fall back to plain zip. */
static int TryMountImz( ZipArchive *zip, DiskArchive **outDisk )
{
    int             i, n, imgIdx = -1, fileCount = 0;
    const ZipEntry *e;
    unsigned char  *buf;
    UInt32          len;

    *outDisk = NULL;

    /* An .imz carries exactly one file (the image). */
    n = ZipNumEntries( zip );
    for ( i = 0; i < n; i++ )
    {
        e = ZipGetEntry( zip, i );
        if ( !e || e->isDir ) continue;
        fileCount++;
        imgIdx = i;
    }
    if ( fileCount != 1 || imgIdx < 0 ) return 0;

    /* Cheap pre-filter: a raw image is a whole number of 512-byte sectors in
     * the floppy / small-disk range.  Skip the decompress for anything else so
     * ordinary single-file zips are not needlessly inflated. */
    e = ZipGetEntry( zip, imgIdx );
    if ( !e || e->size < 512 || e->size > IMZ_MAX_IMAGE || ( e->size & 511 ) )
        return 0;

    if ( ZipExtractToMemory( zip, imgIdx, &buf, &len ) != SZ_OK )
        return 0;
    if ( !DiskProbeBuf( buf, len ) ) { free( buf ); return 0; }

    /* DiskOpenMemory takes ownership of buf (frees it, even on failure). */
    if ( DiskOpenMemory( buf, len, 1, outDisk ) != SZ_OK )
        return 0;
    return 1;
}

int ArcOpen( const char *path, ArcFile **out )
{
    ArcFile      *a;
    FILE         *fp;
    unsigned char sig[7];
    int           fmt  = FMT_ZIP;
    int           isMZ = 0;
    int           rc;

    *out = NULL;

    /* Peek the signature to choose the backend.  7z and RAR have fixed magics
     * (RAR's 7th byte is 0x00 for RAR4, 0x01 for RAR5).  Anything else may be a
     * plain zip, a self-extracting zip, or a self-extracting 7z - the last two
     * are .exe files starting with "MZ". */
    fp = fopen( path, "rb" );
    if ( !fp ) return SZ_ERR_OPEN;
    if ( fread( sig, 1, 7, fp ) == 7 )
    {
        if ( memcmp( sig, SIG_7Z, 6 ) == 0 )
            fmt = FMT_7Z;
        else if ( memcmp( sig, SIG_RAR, 6 ) == 0 )
            fmt = ( sig[6] == 0x01 ) ? FMT_RAR5 : FMT_RAR;
        else if ( sig[0] == 'M' && sig[1] == 'Z' )
            isMZ = 1;
    }
    fclose( fp );

    a = (ArcFile *)calloc( 1, sizeof( ArcFile ) );
    if ( !a ) return SZ_ERR_MEMORY;

    if ( fmt == FMT_7Z )        rc = SzOpen( path, &a->sz );
    else if ( fmt == FMT_RAR )  rc = RarOpen( path, &a->rar );
    else if ( fmt == FMT_RAR5 ) rc = Rar5Open( path, &a->rar5 );
    else
    {
        /* A raw FAT12/16 floppy image (.img/.dsk): a jump-opcode boot sector
         * with a sane BPB, distinct from zip ("PK") or MZ executables. */
        int drc = DiskProbe( path ) ? DiskOpen( path, &a->disk ) : SZ_ERR_SIG;

        if ( drc == SZ_OK )
        {
            fmt = FMT_DISK;
            rc  = SZ_OK;
        }
        else if ( drc != SZ_ERR_SIG && drc != SZ_ERR_FORMAT )
        {
            /* The probe said FAT and the boot sector agreed, so this IS a disk
             * image and drc says what is actually wrong with it - too many
             * files to list, out of memory, unreadable.  Only a verdict about
             * its FORMAT may fall through to the scans below; anything else
             * would be buried under "not an archive", which is both wrong and
             * leaves the user nothing to act on. */
            free( a );
            return drc;
        }
        /* Try zip (the trailing central directory scan handles both plain and
         * self-extracting zips).  If that fails and the file is an .exe, try a
         * self-extracting 7z (SzOpen scans for the embedded 7z signature). */
        else
        {
            rc = ZipOpen( path, &a->zip );
            if ( rc == SZ_OK )
            {
                DiskArchive *dsk;
                if ( TryMountImz( a->zip, &dsk ) )  /* WinImage .imz -> FAT   */
                {
                    ZipClose( a->zip );
                    a->zip  = NULL;
                    a->disk = dsk;
                    fmt = FMT_DISK;
                }
                else
                    fmt = FMT_ZIP;
            }
            else if ( isMZ && SzOpen( path, &a->sz ) == SZ_OK )
            {
                fmt = FMT_7Z;
                rc  = SZ_OK;
            }
        }
    }

    a->fmt = fmt;
    if ( rc != SZ_OK ) { free( a ); return rc; }

    *out = a;
    return SZ_OK;
}

int ArcNumEntries( ArcFile *a )
{
    if ( !a ) return 0;
    if ( a->fmt == FMT_7Z )   return SzGetNumEntries( a->sz );
    if ( a->fmt == FMT_RAR )  return RarNumEntries( a->rar );
    if ( a->fmt == FMT_RAR5 ) return Rar5NumEntries( a->rar5 );
    if ( a->fmt == FMT_DISK ) return DiskNumEntries( a->disk );
    return ZipNumEntries( a->zip );
}

const char *ArcEntryName( ArcFile *a, int index )
{
    if ( !a ) return NULL;
    if ( a->fmt == FMT_7Z )
    {
        const SzEntry *e = SzGetEntry( a->sz, index );
        return e ? e->name : NULL;
    }
    if ( a->fmt == FMT_RAR )
    {
        const RarEntry *e = RarGetEntry( a->rar, index );
        return e ? e->name : NULL;
    }
    if ( a->fmt == FMT_RAR5 )
    {
        const Rar5Entry *e = Rar5GetEntry( a->rar5, index );
        return e ? e->name : NULL;
    }
    if ( a->fmt == FMT_DISK )
    {
        const DiskEntry *e = DiskGetEntry( a->disk, index );
        return e ? e->name : NULL;
    }
    {
        const ZipEntry *e = ZipGetEntry( a->zip, index );
        return e ? e->name : NULL;
    }
}

UInt32 ArcEntrySize( ArcFile *a, int index )
{
    if ( !a ) return 0;
    if ( a->fmt == FMT_7Z )
    {
        const SzEntry *e = SzGetEntry( a->sz, index );
        return e ? e->size : 0;
    }
    if ( a->fmt == FMT_RAR )
    {
        const RarEntry *e = RarGetEntry( a->rar, index );
        return e ? e->size : 0;
    }
    if ( a->fmt == FMT_RAR5 )
    {
        const Rar5Entry *e = Rar5GetEntry( a->rar5, index );
        return e ? e->size : 0;
    }
    if ( a->fmt == FMT_DISK )
    {
        const DiskEntry *e = DiskGetEntry( a->disk, index );
        return e ? e->size : 0;
    }
    {
        const ZipEntry *e = ZipGetEntry( a->zip, index );
        return e ? e->size : 0;
    }
}

int ArcEntryIsDir( ArcFile *a, int index )
{
    if ( !a ) return 0;
    if ( a->fmt == FMT_7Z )
    {
        const SzEntry *e = SzGetEntry( a->sz, index );
        return e ? e->isDir : 0;
    }
    if ( a->fmt == FMT_RAR )
    {
        const RarEntry *e = RarGetEntry( a->rar, index );
        return e ? e->isDir : 0;
    }
    if ( a->fmt == FMT_RAR5 )
    {
        const Rar5Entry *e = Rar5GetEntry( a->rar5, index );
        return e ? e->isDir : 0;
    }
    if ( a->fmt == FMT_DISK )
    {
        const DiskEntry *e = DiskGetEntry( a->disk, index );
        return e ? e->isDir : 0;
    }
    {
        const ZipEntry *e = ZipGetEntry( a->zip, index );
        return e ? e->isDir : 0;
    }
}

UInt32 ArcEntryPacked( ArcFile *a, int index )
{
    if ( !a ) return 0xFFFFFFFFUL;
    if ( a->fmt == FMT_7Z )
        return SzEntryPacked( a->sz, index );
    if ( a->fmt == FMT_RAR )
    {
        const RarEntry *e = RarGetEntry( a->rar, index );
        return e ? e->packed : 0xFFFFFFFFUL;
    }
    if ( a->fmt == FMT_RAR5 )
    {
        const Rar5Entry *e = Rar5GetEntry( a->rar5, index );
        return e ? e->packed : 0xFFFFFFFFUL;
    }
    if ( a->fmt == FMT_DISK )
    {
        const DiskEntry *e = DiskGetEntry( a->disk, index );
        return e ? e->packed : 0xFFFFFFFFUL;
    }
    {
        const ZipEntry *e = ZipGetEntry( a->zip, index );
        return e ? e->packed : 0xFFFFFFFFUL;
    }
}

const char *ArcEntryMethod( ArcFile *a, int index )
{
    if ( !a ) return "";
    if ( a->fmt == FMT_7Z )
        return SzEntryMethod( a->sz, index );
    if ( a->fmt == FMT_RAR )
    {
        const RarEntry *e = RarGetEntry( a->rar, index );
        if ( !e ) return "";
        return ( e->methodCode == 0x30 ) ? "Store" : "RAR";
    }
    if ( a->fmt == FMT_RAR5 )
    {
        const Rar5Entry *e = Rar5GetEntry( a->rar5, index );
        if ( !e ) return "";
        return ( e->methodCode == 0 ) ? "Store" : "RAR5";
    }
    if ( a->fmt == FMT_DISK )
        return "None";
    {
        const ZipEntry *e = ZipGetEntry( a->zip, index );
        if ( !e ) return "";
        if ( e->methodCode == 0 ) return "Store";
        if ( e->methodCode == 6 ) return "Implode";
        if ( e->methodCode == 8 ) return "Deflate";
        return "?";
    }
}

void ArcEntryDate( ArcFile *a, int index, char *buf, int buflen )
{
    buf[0] = '\0';
    if ( !a || buflen < 20 ) return;

    if ( a->fmt == FMT_7Z || a->fmt == FMT_RAR5 )   /* stored as UTC FILETIME */
    {
        UInt32     lo = 0, hi = 0;
        int        has = 0;
        FILETIME   ft, lf;
        SYSTEMTIME st;

        if ( a->fmt == FMT_7Z )
        {
            const SzEntry *e = SzGetEntry( a->sz, index );
            if ( e ) { lo = e->mtimeLo; hi = e->mtimeHi; has = e->hasMtime; }
        }
        else
        {
            const Rar5Entry *e = Rar5GetEntry( a->rar5, index );
            if ( e ) { lo = e->mtimeLo; hi = e->mtimeHi; has = e->hasMtime; }
        }
        if ( !has ) return;

        ft.dwLowDateTime = lo; ft.dwHighDateTime = hi;
        if ( !FileTimeToLocalFileTime( &ft, &lf ) ) lf = ft;
        if ( !FileTimeToSystemTime( &lf, &st ) ) return;
        wsprintf( buf, "%04d-%02d-%02d %02d:%02d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute );
    }
    else      /* zip and RAR4 store MS-DOS packed date/time */
    {
        unsigned d = 0, t = 0;
        if ( a->fmt == FMT_RAR )
        {
            const RarEntry *e = RarGetEntry( a->rar, index );
            if ( !e ) return;
            d = e->modDate; t = e->modTime;
        }
        else if ( a->fmt == FMT_DISK )
        {
            const DiskEntry *e = DiskGetEntry( a->disk, index );
            if ( !e ) return;
            d = e->modDate; t = e->modTime;
        }
        else
        {
            const ZipEntry *e = ZipGetEntry( a->zip, index );
            if ( !e ) return;
            d = e->modDate; t = e->modTime;
        }
        if ( d == 0 && t == 0 ) return;
        wsprintf( buf, "%04d-%02d-%02d %02d:%02d",
                  (int)( ( ( d >> 9 ) & 0x7F ) + 1980 ),
                  (int)( ( d >> 5 ) & 0x0F ),
                  (int)( d & 0x1F ),
                  (int)( ( t >> 11 ) & 0x1F ),
                  (int)( ( t >> 5 ) & 0x3F ) );
    }
}

/* DOS attribute bits (shared low byte of Win attrs and zip external attrs). */
static void FormatDosAttr( UInt32 attr, char *buf )
{
    char *p = buf;
    if ( attr & 0x10 ) *p++ = 'D';   /* directory */
    if ( attr & 0x01 ) *p++ = 'R';   /* read-only */
    if ( attr & 0x02 ) *p++ = 'H';   /* hidden    */
    if ( attr & 0x04 ) *p++ = 'S';   /* system    */
    if ( attr & 0x20 ) *p++ = 'A';   /* archive   */
    *p = '\0';
}

void ArcEntryAttr( ArcFile *a, int index, char *buf, int buflen )
{
    buf[0] = '\0';
    if ( !a || buflen < 8 ) return;

    if ( a->fmt == FMT_7Z )
    {
        const SzEntry *e = SzGetEntry( a->sz, index );
        if ( !e || !e->hasAttrib ) return;
        FormatDosAttr( e->attrib, buf );
    }
    else if ( a->fmt == FMT_RAR )
    {
        const RarEntry *e = RarGetEntry( a->rar, index );
        if ( !e ) return;
        FormatDosAttr( e->attrib, buf );
    }
    else if ( a->fmt == FMT_RAR5 )
    {
        const Rar5Entry *e = Rar5GetEntry( a->rar5, index );
        if ( !e ) return;
        FormatDosAttr( e->attrib, buf );
    }
    else if ( a->fmt == FMT_DISK )
    {
        const DiskEntry *e = DiskGetEntry( a->disk, index );
        if ( !e ) return;
        FormatDosAttr( e->attrib, buf );
    }
    else
    {
        const ZipEntry *e = ZipGetEntry( a->zip, index );
        if ( !e ) return;
        FormatDosAttr( e->attrib, buf );
    }
}

/* Defined with ArcFsName at the end of this file. */
static int NamesNeed83( const char *destDir );
static int g_names83 = -1;             /* per-operation latch, -1 = undecided */
static int g_flatten = 0;              /* 1 = extract without folder names   */

void ArcSetFlattenPaths( int on ) { g_flatten = on ? 1 : 0; }
int  ArcFlattenPaths( void )      { return g_flatten; }

/*---- Overwrite confirmation (see ARCDEFS.H) ------------------------------- */
static ArcOverwriteFn g_owFn   = NULL;
static void          *g_owUser = NULL;
static int            g_owAll  = -1;   /* latched ARC_OW_*ALL, -1 = ask */

void ArcSetOverwritePrompt( ArcOverwriteFn fn, void *user )
{
    g_owFn   = fn;
    g_owUser = user;
    g_owAll  = -1;
}

int ArcWantWrite( const char *path )
{
    FILE *f;
    int   ans;

    f = fopen( path, "rb" );
    if ( !f ) return 1;                    /* nothing there - just write   */
    fclose( f );

    if ( g_owAll == ARC_OW_YESALL ) return 1;
    if ( g_owAll == ARC_OW_NOALL )  return 0;
    if ( !g_owFn ) return 1;               /* no prompt hook: old behaviour */

    ans = g_owFn( g_owUser, path );
    if ( ans == ARC_OW_YESALL ) { g_owAll = ans; return 1; }
    if ( ans == ARC_OW_NOALL )  { g_owAll = ans; return 0; }
    return ( ans == ARC_OW_YES );
}

int ArcExtractAll( ArcFile *a, const char *destDir,
                   SzProgress prog, void *user )
{
    g_owAll   = -1;                        /* new operation: forget "all"   */
    g_names83 = NamesNeed83( destDir );    /* and re-read the destination   */
    ArcResetNames();                       /* and start the ~N count over   */
    if ( !a ) return SZ_ERR_FORMAT;
    if ( a->fmt == FMT_7Z )   return SzExtractAll( a->sz, destDir, prog, user );
    if ( a->fmt == FMT_RAR )  return RarExtractAll( a->rar, destDir, prog, user );
    if ( a->fmt == FMT_RAR5 ) return Rar5ExtractAll( a->rar5, destDir, prog, user );
    if ( a->fmt == FMT_DISK ) return DiskExtractAll( a->disk, destDir, prog, user );
    return ZipExtractAll( a->zip, destDir, prog, user );
}

int ArcExtractItems( ArcFile *a, const int *indices, int count,
                     const char *destDir, SzProgress prog, void *user )
{
    g_owAll   = -1;                        /* new operation: forget "all"   */
    g_names83 = NamesNeed83( destDir );    /* and re-read the destination   */
    ArcResetNames();                       /* and start the ~N count over   */
    if ( !a ) return SZ_ERR_FORMAT;
    if ( a->fmt == FMT_7Z )
        return SzExtractItems( a->sz, indices, count, destDir, prog, user );
    if ( a->fmt == FMT_RAR )
        return RarExtractItems( a->rar, indices, count, destDir, prog, user );
    if ( a->fmt == FMT_RAR5 )
        return Rar5ExtractItems( a->rar5, indices, count, destDir, prog, user );
    if ( a->fmt == FMT_DISK )
        return DiskExtractItems( a->disk, indices, count, destDir, prog, user );
    return ZipExtractItems( a->zip, indices, count, destDir, prog, user );
}

/* Test integrity: run each backend's extractor with destDir == NULL, which the
 * backends treat as "decode + CRC-check, write nothing." */
int ArcTestAll( ArcFile *a, SzProgress prog, void *user )
{
    if ( !a ) return SZ_ERR_FORMAT;
    if ( a->fmt == FMT_7Z )   return SzExtractAll( a->sz, NULL, prog, user );
    if ( a->fmt == FMT_RAR )  return RarExtractAll( a->rar, NULL, prog, user );
    if ( a->fmt == FMT_RAR5 ) return Rar5ExtractAll( a->rar5, NULL, prog, user );
    if ( a->fmt == FMT_DISK ) return DiskExtractAll( a->disk, NULL, prog, user );
    return ZipExtractAll( a->zip, NULL, prog, user );
}

void ArcClose( ArcFile *a )
{
    if ( !a ) return;
    if ( a->fmt == FMT_7Z )        SzClose( a->sz );
    else if ( a->fmt == FMT_RAR )  RarClose( a->rar );
    else if ( a->fmt == FMT_RAR5 ) Rar5Close( a->rar5 );
    else if ( a->fmt == FMT_DISK ) DiskClose( a->disk );
    else                           ZipClose( a->zip );
    free( a );
}

const char *ArcErrorText( int code )
{
    return SzErrorText( code );      /* shared SZ_ERR_* table */
}

const char *ArcFormatName( ArcFile *a )
{
    int fmt = a ? a->fmt : 0;
    switch ( fmt )
    {
    case FMT_7Z:   return "7z";
    case FMT_ZIP:  return "Zip";
    case FMT_RAR:  return "RAR (2.x/3.x)";
    case FMT_RAR5: return "RAR5";
    case FMT_DISK: return DiskFsName( a->disk );   /* "FAT12" / "FAT16" */
    default:       return "?";
    }
}

const char *ArcNoRamHint( void )
{
    static char text[448];

    /* The limit is measured from the machine, so it has to be read out of
     * ArcMemLimitMB rather than written into the sentence.
     *
     * Worded for the shortage rather than for the dictionary: a big
     * dictionary is much the commonest way to get here, but a single huge
     * RAR entry or a mounted disk image can do it too, and telling someone
     * to shrink a dictionary that was never the problem wastes their time. */
    sprintf( text,
             "Not enough memory for this archive.\n"
             "It needs more than the %lu MB this machine can spare in one "
             "block, usually because it was compressed with a large "
             "dictionary.\n"
             "Free some memory, or raise MemoryLimitMB in XARCHIVE.INI if "
             "there is more to give.\n"
             "Failing that, re-create the archive with a dictionary of %lu MB "
             "or less (in 7-Zip, the Dictionary size setting, or simply a "
             "lower compression level).",
             (unsigned long)ArcMemLimitMB(), (unsigned long)ArcMemLimitMB() );
    return text;
}

const char *ArcMemFailHint( void )
{
    static char text[448];

    /* SZ_ERR_MEMORY is the OTHER shortage, and it needs different advice.
     * Here the archive asked for LESS than the budget and the system still
     * said no, so quoting the budget as a dictionary size to aim for would
     * be nonsense: a 64 MB dictionary that failed under a 200 MB budget is
     * not fixed by re-compressing to 200 MB. */
    sprintf( text,
             "Not enough memory for this archive.\n"
             "It asked for less than the %lu MB this program allows itself, "
             "and the system still could not provide it in one block.\n"
             "Close other programs and try again.  If the limit is set by "
             "hand in XARCHIVE.INI, lower MemoryLimitMB to match what this "
             "machine really has.\n"
             "Failing that, re-create the archive with a smaller dictionary "
             "(in 7-Zip, the Dictionary size setting, or simply a lower "
             "compression level).",
             (unsigned long)ArcMemLimitMB() );
    return text;
}

const char *ArcUnsupportedHint( ArcFile *a )
{
    int fmt = a ? a->fmt : 0;
    switch ( fmt )
    {
    case FMT_7Z:
        return "Some entries use a feature this extractor does not support, "
               "and were skipped.  Everything else was extracted.\n"
               "Supported: LZMA, LZMA2, stored, the BCJ x86 filter, and BCJ2.\n"
               "Not supported: other codecs (PPMd, BZip2, Deflate, ARM/other "
               "filters) and encryption.";
    case FMT_RAR:
        return "This RAR entry uses a feature this extractor does not support.\n"
               "Supported: stored and \"Normal\" LZ compression for RAR2 and "
               "RAR3, including solid archives.\n"
               "Not supported: PPMd compression, compression filters, RAR2 "
               "audio, encryption, and multi-volume archives.";
    case FMT_RAR5:
        return "This is a RAR5 archive.\n"
               "Only stored (uncompressed) entries can be extracted.\n"
               "RAR5 compression, encryption, and multi-volume archives are "
               "not supported.";
    case FMT_ZIP:
        return "This zip entry uses a method this extractor does not support.\n"
               "Supported: stored, Deflate, and Implode (PKZIP 1.x method 6).\n"
               "Not supported: encryption, Deflate64, BZip2/LZMA/PPMd, Zip64, "
               "and split archives.";
    case FMT_DISK:
        return "This disk image uses a feature this extractor does not support.\n"
               "Supported: raw FAT12 and FAT16 floppy/disk images with 8.3 and "
               "long file names.\n"
               "Not supported: FAT32, NTFS, other filesystems, and disk-image "
               "container formats other than a raw sector dump.";
    default:
        return "This archive uses a feature, compression method, or encryption "
               "that this extractor does not support.";
    }
}

/*===========================================================================
 * Filesystem-safe output names (ArcFsName)
 *
 * Entry names come out of the archive and cannot be trusted to be legal - or
 * safe - filenames on the extraction target.  Every backend's output-path
 * builder passes the stored name through ArcFsName before appending it to the
 * destination folder, which
 *
 *   - normalises '/' to '\' and strips any leading separators;
 *   - drops "." components and rewrites ".." to "__", so a hostile name
 *     cannot climb out of the destination folder (a ':' is replaced too,
 *     which also disarms "C:..." drive prefixes);
 *   - replaces characters the filesystem cannot take (control characters
 *     and  " * : < > ? |  ) with '_', and trims the trailing dots/spaces
 *     FAT and NTFS refuse to store;
 *   - appends '_' to a base name that matches a DOS device (CON, PRN, AUX,
 *     NUL, CLOCK$, COM1-9, LPT1-9 - with or without an extension), which
 *     would otherwise open the device instead of a file;
 *   - and, when the target only understands 8.3 names, reduces every
 *     component to the upper-case 8.3 form the way DOS 7 does it (below).
 *
 *===========================================================================
 * The numeric tail (~1, ~2, ~3 ...)
 *
 * Plain truncation is what this used to do, and it quietly ate files: "Long
 * filename 1.txt" and "Long filename 2.txt" both truncate to LONGFILE.TXT, so
 * the second entry landed on top of the first and the only warning was an
 * overwrite prompt naming a file the user had never heard of.
 *
 * DOS 7 / VFAT solved this years ago and we now do the same thing:
 *
 *   - a component that ALREADY fits 8.3 keeps itself, only upper-cased.
 *     README.TXT stays README.TXT; it never grows a tail it did not need.
 *   - a component that has to give something up - length, a space, a second
 *     dot, a character FAT will not store - gets its base cut to make room
 *     for "~N", and N counts up from 1 until the name is free.  So the pair
 *     above becomes LONGFI~1.TXT and LONGFI~2.TXT.
 *
 * "Free" means free within ONE destination folder, for the length of ONE
 * extraction, which is what the two little hash tables below remember:
 *
 *   g_snMap   (parent folder + stored name) -> the short name it was given.
 *             A folder mentioned by four hundred entries is shortened once
 *             and every entry underneath agrees about where it went.
 *   g_snUsed  (parent folder + short name) -> taken.  What makes N count up.
 *
 * Both are cleared by ArcResetNames at the start of every extraction, which
 * is deliberate: extracting the same archive twice must produce the same
 * names both times, so that the second run recognises the first run's files
 * and asks about overwriting them rather than laying down a parallel set of
 * ~2s.  The tables are only built on the 8.3 path, so long-name targets pay
 * nothing for any of this.
 *
 * Two things this does not do, both matching DOS rather than fighting it: a
 * stored name that IS already an 8.3 name can still collide with another
 * entry's tail (an archive holding both "LONGFI~1.TXT" and "Long filename
 * 1.txt"), which lands in the overwrite prompt as before; and extracting a
 * marked subset numbers only what it extracts, so a subset's names need not
 * match the same entries' names in a whole-archive extraction.
 *===========================================================================*/

#ifdef __OS2__
/* OS2PLAT.C: 1 when the drive holding 'path' (the current drive when path is
 * NULL or relative) carries an 8.3-only filesystem (FAT); 0 for HPFS and
 * other long-name filesystems. */
extern int Os2NamesNeed83( const char *path );
#endif

#if defined(_WIN32)
/*---- Ask the destination VOLUME, not the operating system ----------------- *
 * GetVolumeInformation reports the longest component name a volume will
 * store: 255 on NTFS and on a VFAT drive, 12 on plain FAT.  That is the
 * question actually being asked, and it is not the same question as "which
 * Windows is this".  A Win32s machine reached an HPFS or NTFS share over the
 * network can hold long names perfectly well, and deciding from GetVersion
 * threw them away; an NT machine extracting onto a FAT floppy cannot, and
 * deciding from GetVersion would have written names it could not store.
 *
 * Called through GetProcAddress rather than imported.  A static import of a
 * function Win32s might not export would stop the program LOADING on the
 * systems that lack it, which is exactly the platform this has to keep
 * working on (the same lesson as GetScrollInfo, in BUILD-NOTES.txt).
 *
 * Returns 1 for 8.3-only, 0 for long names, and -1 for "could not tell" so
 * the caller can fall back.
 *-------------------------------------------------------------------------- */
typedef BOOL (WINAPI *ArcPfnGetVolInfo)( LPCSTR, LPSTR, DWORD, LPDWORD,
                                         LPDWORD, LPDWORD, LPSTR, DWORD );

static int Win32VolumeNeeds83( const char *destDir )
{
    char             root[MAX_PATH];
    const char      *rootArg = NULL;
    DWORD            serial = 0, maxComp = 0, flags = 0;
    HMODULE          k32;
    ArcPfnGetVolInfo pGetVolInfo;
    UINT             oldMode;
    BOOL             ok;

    k32 = GetModuleHandle( "KERNEL32.DLL" );
    if ( !k32 ) return -1;
    pGetVolInfo = (ArcPfnGetVolInfo)GetProcAddress( k32, "GetVolumeInformationA" );
    if ( !pGetVolInfo )      /* an ANSI-only Win32s may export the bare name */
        pGetVolInfo = (ArcPfnGetVolInfo)GetProcAddress( k32, "GetVolumeInformation" );
    if ( !pGetVolInfo ) return -1;

    /* GetVolumeInformation wants a volume ROOT, or NULL for the current
     * drive - which is the right answer for a relative destination. */
    if ( destDir && destDir[0] && destDir[1] == ':' )
    {
        root[0] = destDir[0];
        root[1] = ':';
        root[2] = '\\';
        root[3] = '\0';
        rootArg = root;
    }
    else if ( destDir && destDir[0] == '\\' && destDir[1] == '\\' )
    {
        /* "\\server\share\..." - keep everything up to and including the
         * separator after the share name. */
        int i, seps = 0;
        for ( i = 2; destDir[i] && i < MAX_PATH - 2; i++ )
        {
            root[i] = destDir[i];
            if ( destDir[i] == '\\' && ++seps == 2 ) { i++; break; }
        }
        if ( seps < 2 ) { root[i] = '\\'; i++; }   /* "\\server\share" bare */
        root[0] = root[1] = '\\';
        root[i] = '\0';
        rootArg = root;
    }

    /* A destination on a drive with no disk in it would otherwise raise the
     * system's "drive not ready" box from inside a backend. */
    oldMode = SetErrorMode( SEM_FAILCRITICALERRORS );
    ok = pGetVolInfo( (LPCSTR)rootArg, NULL, 0, &serial, &maxComp, &flags,
                      NULL, 0 );
    SetErrorMode( oldMode );

    if ( !ok || maxComp == 0 ) return -1;
    return ( maxComp <= 12 );
}
#endif /* _WIN32 */

/* True when extracted names must fit the FAT 8.3 form, decided from the
 * extraction's destination:
 *   - DOS build: always (real-mode FAT; the LFN INT 21h/71h API is a
 *     Windows 95 service and is not there under plain DOS).
 *   - Win32 build: whatever the destination VOLUME says it can store
 *     (Win32VolumeNeeds83 above).  Only if the volume will not answer does
 *     it fall back to the old rule of thumb - GetVersion with the high bit
 *     set and a major version of 3 is Win32s, and a Win32s machine is on
 *     FAT far more often than not.
 *   - OS/2 build: whatever the destination drive's filesystem is
 *     (DosQueryFSAttach via Os2NamesNeed83): FAT is 8.3, HPFS and JFS and
 *     everything else take long names.
 * ArcExtractAll / ArcExtractItems latch the answer per operation in
 * g_names83; ArcFsName probes the current drive if somehow asked outside
 * one. */
static int NamesNeed83( const char *destDir )
{
#if defined(_WIN32)
    DWORD v;
    int   probed = Win32VolumeNeeds83( destDir );

    if ( probed >= 0 ) return probed;
    v = GetVersion();
    return ( ( v & 0x80000000UL ) != 0 && ( v & 0xFFUL ) == 3 );
#elif defined(__OS2__)
    return Os2NamesNeed83( destDir );
#else
    (void)destDir;
    return 1;
#endif
}

/* Characters FAT accepts inside an 8.3 name (input already upper-cased). */
static int Fs83Valid( int c )
{
    if ( c >= 'A' && c <= 'Z' ) return 1;
    if ( c >= '0' && c <= '9' ) return 1;
    if ( c >= 0x80 ) return 1;                   /* code-page characters */
    return c != 0 && strchr( "!#$%&'()-@^_`{}~", c ) != NULL;
}

/* If the part of 'comp' before the extension names a DOS device, append '_'
 * to it: "CON" -> "CON_", "con.txt" -> "con_.txt".  DOS resolves device
 * names BEFORE extensions, so "CON.TXT" is still the console. */
static void GuardDevice( char *comp )
{
    static const char *dev[] = { "CON", "PRN", "AUX", "NUL", "CLOCK$", 0 };
    char up[8];
    int  i, n, match = 0;

    for ( n = 0; n < 7 && comp[n] && comp[n] != '.'; n++ )
    {
        char c = comp[n];
        if ( c >= 'a' && c <= 'z' ) c = (char)( c - 'a' + 'A' );
        up[n] = c;
    }
    if ( comp[n] && comp[n] != '.' ) return;     /* base > 7 chars: safe */
    up[n] = '\0';

    for ( i = 0; dev[i]; i++ )
        if ( !strcmp( up, dev[i] ) ) match = 1;
    if ( !match && n == 4 && up[3] >= '1' && up[3] <= '9' &&
         ( !memcmp( up, "COM", 3 ) || !memcmp( up, "LPT", 3 ) ) )
        match = 1;
    if ( !match ) return;

    memmove( comp + n + 1, comp + n, strlen( comp + n ) + 1 );
    comp[n] = '_';
}

/* Long-name target: substitute forbidden characters, trim the trailing dots
 * and spaces the filesystem will not store.  In place; may shrink. */
static void CleanLong( char *comp )
{
    int r, w = 0;

    for ( r = 0; comp[r]; r++ )
    {
        int c = (unsigned char)comp[r];
        if ( c < 0x20 || strchr( "\"*:<>?|", c ) ) c = '_';
        comp[w++] = (char)c;
    }
    while ( w > 0 && ( comp[w-1] == '.' || comp[w-1] == ' ' ) ) w--;
    comp[w] = '\0';
    GuardDevice( comp );
}

/* 8.3 target: split into the upper-case base and extension DOS would use.
 * The text after the LAST dot becomes the extension; spaces and other dots
 * are dropped; anything else FAT cannot take becomes '_'.  'base' takes at
 * least 9 bytes and 'ext' at least 4.
 *
 * Returns 1 when nothing had to be given up - the stored name already WAS an
 * 8.3 name and only its case changed - and 0 when characters were dropped,
 * substituted, or truncated away.  That 0 is what earns the name a numeric
 * tail; a name that survives intact never gets one.
 *
 * 'wantExt' 0 means there is no such thing as an extension in this name: no
 * dot is looked for, and every character that survives goes into the base.
 * That is how a FOLDER is shortened - see ArcFsName in ARCDEFS.H.  The dots
 * are still dropped and still cost the name its exactness, which is right:
 * the name did lose something. */
static int Clean83( const char *comp, char *base, char *ext, int wantExt )
{
    int bi = 0, xi = 0, i, dot = -1, exact = 1;

    if ( wantExt )
        for ( i = 0; comp[i]; i++ )
            if ( comp[i] == '.' ) dot = i;
    if ( dot == 0 ) dot = -1;          /* ".profile": the whole name is the base */

    for ( i = 0; comp[i]; i++ )
    {
        int   c     = (unsigned char)comp[i];
        int   isExt = ( dot >= 0 && i > dot );
        char *out   = isExt ? ext  : base;
        int  *n     = isExt ? &xi  : &bi;
        int   max   = isExt ? 3    : 8;

        if ( i == dot ) continue;
        if ( c == ' ' || c == '.' ) { exact = 0; continue; }
        if ( c >= 'a' && c <= 'z' ) c = c - 'a' + 'A';
        if ( !Fs83Valid( c ) ) { c = '_'; exact = 0; }
        if ( *n < max ) out[(*n)++] = (char)c;
        else            exact = 0;                     /* did not fit 8 or 3 */
    }
    base[bi] = '\0';
    ext[xi]  = '\0';

    /* "name." renders as NAME, which is a perfectly good 8.3 name but is not
     * the SAME name - a trailing dot is one of the things FAT will not store.
     * Left unsaid, "name." and "name" in one folder would both come out NAME
     * and the second would land on the first. */
    if ( dot >= 0 && comp[dot + 1] == '\0' ) exact = 0;

    if ( bi == 0 && xi > 0 )           /* nothing survived before the dot */
    {
        lstrcpy( base, ext );
        ext[0] = '\0';
        exact  = 0;
    }
    if ( base[0] == '\0' )
    {
        base[0] = '_';
        base[1] = '\0';
        exact   = 0;
    }
    return exact;
}

/* base + ext -> "BASE.EXT" (or just "BASE"). */
static void Join83( char *comp, const char *base, const char *ext )
{
    lstrcpy( comp, base );
    if ( ext[0] )
    {
        lstrcat( comp, "." );
        lstrcat( comp, ext );
    }
}

/* "LONGFI~2.TXT": the base cut back far enough to make room for the tail, so
 * the whole name still fits in eight characters however long N grows. */
static void Tail83( char *comp, const char *base, const char *ext, int n )
{
    char num[10];
    int  room, i;

    sprintf( num, "~%d", n );
    room = 8 - (int)strlen( num );
    if ( room < 1 ) room = 1;

    for ( i = 0; i < room && base[i]; i++ ) comp[i] = base[i];
    comp[i] = '\0';
    lstrcat( comp, num );
    if ( ext[0] )
    {
        lstrcat( comp, "." );
        lstrcat( comp, ext );
    }
}

/*---- The per-extraction short-name registry (see the block comment above) -- */

#define SN_BUCKETS 1024                /* power of two: SnHash masks with it */

typedef struct SnNode {
    struct SnNode *next;
    char          *key;
    char          *val;
} SnNode;

/* g_snMap is the fast path and g_snUsed is the authority.  g_snUsed records
 * which STORED name each short name was handed to, not merely that it is
 * taken, so a component whose memo failed to allocate still recognises its
 * own name on the second look instead of counting past it - which would put
 * half of a folder's files in FOLDER~1 and half in FOLDER~2. */
static SnNode *g_snMap[SN_BUCKETS];    /* (parent, stored name) -> short name */
static SnNode *g_snUsed[SN_BUCKETS];   /* (parent, short name)  -> stored name*/

static int SnUpper( int c )
{
    return ( c >= 'a' && c <= 'z' ) ? c - 'a' + 'A' : c;
}

/* "PARENT\PATH" + a separator FAT cannot contain + "NAME", upper-cased,
 * which is what makes the lookup per-folder and case-blind the way FAT is. */
static void SnKey( char *key, int keySize, const char *parent, const char *name )
{
    int i = 0;

    while ( parent && parent[i] && i < keySize - 3 )
    {
        key[i] = (char)SnUpper( (unsigned char)parent[i] );
        i++;
    }
    key[i++] = '\x01';
    for ( ; *name && i < keySize - 1; name++ )
        key[i++] = (char)SnUpper( (unsigned char)*name );
    key[i] = '\0';
}

static unsigned SnHash( const char *s )
{
    unsigned h = 5381;
    while ( *s ) h = h * 33u + (unsigned char)*s++;
    return h & ( SN_BUCKETS - 1 );
}

static SnNode *SnFind( SnNode **tab, const char *key )
{
    SnNode *n;
    for ( n = tab[SnHash( key )]; n; n = n->next )
        if ( strcmp( n->key, key ) == 0 ) return n;
    return NULL;
}

/* Best effort: a failed allocation costs the memory of the decision, not the
 * extraction - the worst that happens is a name gets decided twice. */
static void SnAdd( SnNode **tab, const char *key, const char *val )
{
    unsigned h = SnHash( key );
    SnNode  *n = (SnNode *)malloc( sizeof( SnNode ) );

    if ( !n ) return;
    n->key = (char *)malloc( strlen( key ) + 1 );
    n->val = val ? (char *)malloc( strlen( val ) + 1 ) : NULL;
    if ( !n->key || ( val && !n->val ) )
    {
        free( n->key );
        free( n->val );
        free( n );
        return;
    }
    strcpy( n->key, key );
    if ( val ) strcpy( n->val, val );
    n->next = tab[h];
    tab[h]  = n;
}

static void SnClear( SnNode **tab )
{
    int i;
    for ( i = 0; i < SN_BUCKETS; i++ )
    {
        SnNode *n = tab[i];
        while ( n )
        {
            SnNode *next = n->next;
            free( n->key );
            free( n->val );
            free( n );
            n = next;
        }
        tab[i] = NULL;
    }
}

/* The verdict on the name ArcFsName was last given.  g_snAbort is separate and
 * STICKY: once the user cancels, every later name reports ABORT without the
 * prompt going up again, so a backend unwinding a solid folder entry by entry
 * does not ask four hundred more times on the way out. */
static int g_nameVerdict = ARC_NAME_OK;
static int g_snAbort     = 0;

int ArcNameVerdict( void )
{
    return g_snAbort ? ARC_NAME_ABORT : g_nameVerdict;
}

void ArcResetNames( void )
{
    SnClear( g_snMap );
    SnClear( g_snUsed );
    g_nameVerdict = ARC_NAME_OK;
    g_snAbort     = 0;
}

/* The 8.3 name one stored component gets inside one destination folder.
 *   parent - the already-shortened path of the folders above it, "" at the
 *            top of the destination;
 *   orig   - the component exactly as the archive stored it;
 *   comp   - receives the name to create (at least 14 bytes).
 * Asking twice for the same component in the same folder gives the same
 * answer both times, which is what keeps a folder's hundred entries agreeing
 * about where the folder went. */
static int SnSameName( const char *a, const char *b )
{
    while ( *a && *b )
    {
        if ( SnUpper( (unsigned char)*a ) != SnUpper( (unsigned char)*b ) )
            return 0;
        a++; b++;
    }
    return *a == *b;
}

/*---- Letting the user name it instead ------------------------------------- *
 * With no prompt installed nothing below changes: a name that will not fit
 * 8.3 gets the ~N tail and the extraction never stops.  That stays the
 * default, because it is the only behaviour that can run unattended.
 *
 * A front end that installs one is asked ONCE per (folder, stored name) -
 * the memo table above sees to that, so a folder mentioned by four hundred
 * entries is still one question - and only for names that actually have to
 * lose something.  A name that already fits 8.3 is never worth asking about.
 *
 * Whatever comes back is run through Clean83 like any other name: the user
 * is choosing between legal names, not being handed the ability to write
 * "my file.txt" onto a FAT volume.  If their choice is already taken by a
 * DIFFERENT stored name, it gets the ~N treatment on top - the alternative
 * is letting one entry silently land on another, which is the bug the tail
 * exists to prevent.
 *-------------------------------------------------------------------------- */
static ArcShortNamePrompt g_snAsk     = 0;
static void              *g_snAskUser = 0;

void ArcSetShortNamePrompt( ArcShortNamePrompt fn, void *user )
{
    g_snAsk     = fn;
    g_snAskUser = user;
}

/* Is 'comp' free in 'parent', or already ours?  The one question the
 * numbering loop and the user's answer both have to pass. */
static int SnNameFree( const char *parent, const char *comp, const char *orig )
{
    char    key[SZ_MAX_NAME * 2 + 8];
    SnNode *hit;

    SnKey( key, sizeof( key ), parent, comp );
    hit = SnFind( g_snUsed, key );
    return ( !hit || SnSameName( hit->val, orig ) );
}

static void Resolve83( const char *parent, const char *orig, char *comp,
                       int isDir )
{
    char    base[10], ext[4];
    char    key[SZ_MAX_NAME * 2 + 8];
    SnNode *hit;
    int     n;

    SnKey( key, sizeof( key ), parent, orig );
    hit = SnFind( g_snMap, key );
    if ( hit ) { lstrcpy( comp, hit->val ); return; }

    if ( Clean83( orig, base, ext, 1 ) )
    {
        Join83( comp, base, ext );      /* already an 8.3 name: leave it alone */
        GuardDevice( comp );
    }
    else
    {
        /* It has to be shortened, and for a FOLDER that means the extension
         * goes: the dots in "...(3.5-720k)(5.25-360k)" are disk capacities,
         * and three characters of one presented as a file type is worse than
         * no extension at all.  Re-clean with no extension so the whole name
         * competes for the eight characters there are.  Only reached when the
         * name did not already fit, so DATA.OLD is never touched. */
        if ( isDir ) Clean83( orig, base, ext, 0 );

        /* Count up until the name is either free or already ours.  "Already
         * ours" is what makes this safe to re-run for a component we have met
         * before but could not afford to memo. */
        for ( n = 1; n < 1000000; n++ )
        {
            Tail83( comp, base, ext, n );
            if ( SnNameFree( parent, comp, orig ) ) break;
        }

        /* The auto-renamed name is now the SUGGESTION.  Ask, and if the user
         * offers something else, legalise it and make room for it. */
        if ( g_snAsk && !g_snAbort )
        {
            char chosen[16];
            int  said;

            lstrcpyn( chosen, comp, sizeof( chosen ) );
            said = g_snAsk( g_snAskUser, parent, orig, chosen, sizeof( chosen ) );

            if ( said == ARC_SN_ABORT )
            {
                g_snAbort = 1;
                return;                 /* nothing memoed: the run is over */
            }
            if ( said == ARC_SN_SKIP )
            {
                g_nameVerdict = ARC_NAME_SKIP;
                return;                 /* likewise - do not reserve a name */
            }
            if ( said == ARC_SN_USE && chosen[0] )
            {
                char ubase[10], uext[4], cand[16];

                /* A name the USER typed keeps its extension whatever it names:
                 * they typed the dot on purpose.  The folder rule above is
                 * about what to discard when a name is cut down automatically,
                 * and nothing here is automatic. */
                Clean83( chosen, ubase, uext, 1 );
                Join83( cand, ubase, uext );
                GuardDevice( cand );

                if ( SnNameFree( parent, cand, orig ) )
                    lstrcpy( comp, cand );
                else
                    for ( n = 1; n < 1000000; n++ )
                    {
                        Tail83( comp, ubase, uext, n );
                        if ( SnNameFree( parent, comp, orig ) ) break;
                    }
            }
        }
    }

    SnKey( key, sizeof( key ), parent, comp );
    if ( !SnFind( g_snUsed, key ) ) SnAdd( g_snUsed, key, orig );
    SnKey( key, sizeof( key ), parent, orig );
    SnAdd( g_snMap, key, comp );
}

void ArcFsName( char *dst, int dstSize, const char *name, int isDir )
{
    char        orig[SZ_MAX_NAME + 2];
    char        comp[SZ_MAX_NAME + 2];   /* +2: GuardDevice may grow it by one */
    const char *s  = name;
    int         di = 0, ci, e83;
    int         real = 0;        /* components that were more than "." */
    int         compIsDir;       /* this component, not the whole entry     */

    if ( dstSize <= 0 ) return;
    g_nameVerdict = ARC_NAME_OK;
    if ( g_snAbort ) { dst[0] = '\0'; return; }
    if ( g_names83 < 0 )
        g_names83 = NamesNeed83( NULL );
    e83 = g_names83;
    while ( s && *s )
    {
        while ( *s == '\\' || *s == '/' ) s++;
        ci = 0;
        while ( *s && *s != '\\' && *s != '/' )
        {
            if ( ci < SZ_MAX_NAME ) orig[ci++] = *s;
            s++;
        }
        orig[ci] = '\0';
        if ( ci == 0 ) break;

        /* Anything with a path component after it is a folder whatever the
         * entry as a whole is; only the last one has to be taken on trust
         * from the caller.  Trailing separators are skipped first, so
         * "FOLDER/" is the last component and not an empty one after it. */
        {
            const char *look = s;
            while ( *look == '\\' || *look == '/' ) look++;
            compIsDir = ( *look != '\0' ) ? 1 : isDir;
        }

        /* "Extract without paths": every component lands at the top of the
         * destination, so the one before it is dropped instead of kept as a
         * parent.  Done here rather than to the finished path, because the
         * numbering below has to see the folder the file is REALLY going
         * into: two "readme.txt" from two archive folders collide once they
         * are flattened, and have to be numbered apart. */
        if ( g_flatten ) di = 0;

        if ( orig[0] == '.' && orig[1] == '\0' )
            continue;                           /* "." drops out entirely */
        ++real;
        if ( orig[0] == '.' && orig[1] == '.' && orig[2] == '\0' )
        {
            comp[0] = comp[1] = '_';            /* cannot climb out of dest */
            comp[2] = '\0';
        }
        else if ( e83 )
        {
            dst[di] = '\0';                     /* the parent, for the registry */
            Resolve83( dst, orig, comp, compIsDir );
            if ( g_snAbort || g_nameVerdict == ARC_NAME_SKIP )
            {
                dst[0] = '\0';                  /* caller checks the verdict */
                return;
            }
        }
        else
        {
            lstrcpy( comp, orig );
            CleanLong( comp );
        }
        if ( comp[0] == '\0' ) continue;

        if ( di > 0 && di < dstSize - 1 ) dst[di++] = '\\';
        for ( ci = 0; comp[ci] && di < dstSize - 1; ci++ )
            dst[di++] = comp[ci];
    }

    /* Nothing left.  Two different reasons, and they need opposite answers:
     *
     *   every component was "."  - the entry names the destination itself
     *     ("." is the whole of it).  There is nothing to create, and the "_"
     *     this used to fall back on produced a stray directory called _ in
     *     every extraction of a Unix-made archive.  Say SKIP.
     *
     *   there were real components and they all vanished - the name was made
     *     entirely of characters the filesystem forbids.  That IS an entry;
     *     dropping it silently would lose data, so it keeps the "_" and the
     *     collision numbering sorts out the second one. */
    if ( di == 0 )
    {
        if ( !real ) g_nameVerdict = ARC_NAME_SKIP;
        else if ( dstSize > 1 ) dst[di++] = '_';
    }
    dst[di] = '\0';
}
