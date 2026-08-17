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
    else if ( DiskProbe( path ) && DiskOpen( path, &a->disk ) == SZ_OK )
    {
        /* A raw FAT12/16 floppy image (.img/.dsk): a jump-opcode boot sector
         * with a sane BPB, distinct from zip ("PK") or MZ executables. */
        fmt = FMT_DISK;
        rc  = SZ_OK;
    }
    else
    {
        /* Try zip (the trailing central directory scan handles both plain and
         * self-extracting zips).  If that fails and the file is an .exe, try a
         * self-extracting 7z (SzOpen scans for the embedded 7z signature). */
        rc = ZipOpen( path, &a->zip );
        if ( rc == SZ_OK )
        {
            DiskArchive *dsk;
            if ( TryMountImz( a->zip, &dsk ) )   /* WinImage .imz -> FAT mount */
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
    g_owAll   = -1;                        /* new operation: forget "all" */
    g_names83 = NamesNeed83( destDir );    /* and re-read the destination  */
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
    g_owAll   = -1;                        /* new operation: forget "all" */
    g_names83 = NamesNeed83( destDir );    /* and re-read the destination  */
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
    return "Not enough memory for this archive.\n"
           "It was compressed with a dictionary larger than this program can "
           "hold in RAM (the limit is 32 MB).\n"
           "Re-create the archive with a smaller dictionary - in 7-Zip, a "
           "lower compression level, or Dictionary size 32 MB or less - and "
           "it will extract here.";
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
 *   - and, when the target only understands 8.3 names, mangles every
 *     component to the upper-case 8.3 form the way DOS itself would:
 *     spaces and extra dots are dropped, the rest is truncated to 8+3
 *     ("A Long File Name.txt" -> "ALONGFIL.TXT").
 *
 * Two long names CAN mangle to the same 8.3 name; the later entry then
 * overwrites the earlier one, exactly as copying them onto a FAT disk would.
 * A ~N uniquifier would need per-extraction state in all five backends and
 * is deliberately not attempted.
 *===========================================================================*/

#ifdef __OS2__
/* OS2PLAT.C: 1 when the drive holding 'path' (the current drive when path is
 * NULL or relative) carries an 8.3-only filesystem (FAT); 0 for HPFS and
 * other long-name filesystems. */
extern int Os2NamesNeed83( const char *path );
#endif

/* True when extracted names must fit the FAT 8.3 form, decided from the
 * extraction's destination:
 *   - DOS build: always (real-mode FAT).
 *   - Win32 build: only under Win32s, whatever the destination.  GetVersion()
 *     with the high bit set and a major version of 3 is Win32s - Windows 95
 *     reports major 4, and NT clears the high bit; both take long names.
 *   - OS/2 build: only when the destination drive's filesystem is FAT
 *     (DosQueryFSAttach via Os2NamesNeed83; HPFS takes long names).
 * ArcExtractAll / ArcExtractItems latch the answer per operation in
 * g_names83; ArcFsName probes the current drive if somehow asked outside
 * one. */
static int NamesNeed83( const char *destDir )
{
#if defined(_WIN32)
    DWORD v = GetVersion();
    (void)destDir;
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

/* 8.3 target: mangle to an upper-case 8.3 name the way DOS would.  The text
 * after the LAST dot becomes the extension; spaces and other dots are
 * dropped; anything else FAT cannot take becomes '_'.  In place; the result
 * is at most 13 characters, always shorter than the buffer 'comp' sits in. */
static void Clean83( char *comp )
{
    char base[10], ext[4];
    int  bi = 0, xi = 0, i, dot = -1;

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
        if ( c == ' ' || c == '.' ) continue;
        if ( c >= 'a' && c <= 'z' ) c = c - 'a' + 'A';
        if ( !Fs83Valid( c ) ) c = '_';
        if ( *n < max ) out[(*n)++] = (char)c;
    }
    base[bi] = '\0';
    ext[xi]  = '\0';

    if ( bi == 0 && xi > 0 )           /* nothing survived before the dot */
    {
        lstrcpy( base, ext );
        ext[0] = '\0';
    }
    if ( base[0] == '\0' )
    {
        base[0] = '_';
        base[1] = '\0';
    }

    lstrcpy( comp, base );
    if ( ext[0] )
    {
        lstrcat( comp, "." );
        lstrcat( comp, ext );
    }
    GuardDevice( comp );
}

/* One path component, in place.  "." vanishes (empty result = drop the
 * component); ".." becomes "__" so it cannot climb out of the destination. */
static void CleanComponent( char *comp, int e83 )
{
    if ( comp[0] == '.' && comp[1] == '\0' )
    {
        comp[0] = '\0';
        return;
    }
    if ( comp[0] == '.' && comp[1] == '.' && comp[2] == '\0' )
    {
        comp[0] = comp[1] = '_';
        return;
    }
    if ( e83 ) Clean83( comp );
    else       CleanLong( comp );
}

void ArcFsName( char *dst, int dstSize, const char *name )
{
    char        comp[SZ_MAX_NAME + 2];   /* +2: GuardDevice may grow it by one */
    const char *s  = name;
    int         di = 0, ci, e83;

    if ( dstSize <= 0 ) return;
    if ( g_names83 < 0 )
        g_names83 = NamesNeed83( NULL );
    e83 = g_names83;
    while ( s && *s )
    {
        while ( *s == '\\' || *s == '/' ) s++;
        ci = 0;
        while ( *s && *s != '\\' && *s != '/' )
        {
            if ( ci < SZ_MAX_NAME ) comp[ci++] = *s;
            s++;
        }
        comp[ci] = '\0';
        if ( ci == 0 ) break;

        CleanComponent( comp, e83 );
        if ( comp[0] == '\0' ) continue;

        if ( di > 0 && di < dstSize - 1 ) dst[di++] = '\\';
        for ( ci = 0; comp[ci] && di < dstSize - 1; ci++ )
            dst[di++] = comp[ci];
    }
    /* "Extract without paths": keep only the final component.  Done after the
     * per-component cleaning above, so ".." and device names are already
     * disarmed and the 8.3 mangling has already run. */
    if ( g_flatten && di > 0 )
    {
        int k, last = -1;
        for ( k = 0; k < di; k++ )
            if ( dst[k] == '\\' ) last = k;
        if ( last >= 0 )
        {
            int m = 0;
            for ( k = last + 1; k < di; k++ ) dst[m++] = dst[k];
            di = m;
        }
    }

    if ( di == 0 && dstSize > 1 )        /* never hand back an empty name */
        dst[di++] = '_';
    dst[di] = '\0';
}
