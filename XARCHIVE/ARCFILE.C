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

int ArcExtractAll( ArcFile *a, const char *destDir,
                   SzProgress prog, void *user )
{
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

const char *ArcUnsupportedHint( ArcFile *a )
{
    int fmt = a ? a->fmt : 0;
    switch ( fmt )
    {
    case FMT_7Z:
        return "This 7z entry uses a feature this extractor does not support.\n"
               "Supported: LZMA, LZMA2, stored, and the BCJ x86 filter.\n"
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
