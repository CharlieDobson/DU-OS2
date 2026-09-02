/*===========================================================================
 * RAR5ARC.C  -  RAR 5.x parsing and extraction
 * Target: MSVC 2.2  Win32s
 *
 * PHASE 1: container parsing, listing, and STORED (method 0) extraction.
 * Compressed entries return SZ_ERR_UNSUPPORTED until the RAR5 unpack engine is
 * ported.  No encryption, no multi-volume.
 *
 * RAR5 block: [HeadCRC 4][HeaderSize vint][header body: HeaderSize bytes]
 *             [data area: DataSize bytes].  vint = 7 bits/byte, little-endian,
 *             high bit = continuation.
 *===========================================================================*/

#include <windows.h>     /* lstrcpyn, SYSTEMTIME -> FILETIME */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>      /* _mkdir */

#include "rar5arc.h"
#include "crc32.h"
#include "platform.h"    /* SetFileMTime */

/* header types */
#define H5_MAIN     1
#define H5_FILE     2
#define H5_SERVICE  3
#define H5_ENCRYPT  4
#define H5_ENDARC   5

/* header flags */
#define H5F_EXTRA   0x0001   /* extra-area size present */
#define H5F_DATA    0x0002   /* data-area size present  */

/* file flags */
#define F5_DIR      0x0001
#define F5_MTIME    0x0002
#define F5_CRC      0x0004

static const unsigned char RAR5_SIG[8] =
    { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00 };

/*---- buffered reader over one header body -------------------------------- */
typedef struct {
    const Byte *p;
    const Byte *end;
    int         err;
} BRd;

static UInt32 BrVint( BRd *b )
{
    UInt32 val = 0;
    int    shift = 0;
    for ( ;; )
    {
        Byte c;
        if ( b->p >= b->end ) { b->err = 1; break; }
        c = *b->p++;
        if ( shift < 32 )
            val |= ( (UInt32)( c & 0x7F ) ) << shift;
        shift += 7;
        if ( !( c & 0x80 ) ) break;
    }
    return val;
}

static UInt32 BrU32( BRd *b )
{
    UInt32 v;
    if ( b->p + 4 > b->end ) { b->err = 1; return 0; }
    v = (UInt32)b->p[0] | ( (UInt32)b->p[1] << 8 ) |
        ( (UInt32)b->p[2] << 16 ) | ( (UInt32)b->p[3] << 24 );
    b->p += 4;
    return v;
}

/* Read a vint straight from the file (for the HeaderSize field). */
static int RdVintFile( FILE *fp, UInt32 *val, int *nBytes )
{
    UInt32 v = 0;
    int    shift = 0, n = 0, c;
    for ( ;; )
    {
        c = fgetc( fp );
        if ( c == EOF ) return -1;
        n++;
        if ( shift < 32 )
            v |= ( (UInt32)( c & 0x7F ) ) << shift;
        shift += 7;
        if ( !( c & 0x80 ) ) break;
    }
    *val = v; *nBytes = n;
    return 0;
}

/*---- Unix time -> FILETIME (UTC), no __int64 ----------------------------- */
static void UnixToFileTime( UInt32 t, UInt32 *lo, UInt32 *hi )
{
    SYSTEMTIME st;
    FILETIME   ft;
    long   days = (long)( t / 86400UL );
    UInt32 secs = t % 86400UL;
    long   z, era, doe, yoe, y, doy, mp, d, m;

    z   = days + 719468;
    era = ( z >= 0 ? z : z - 146096 ) / 146097;
    doe = z - era * 146097;
    yoe = ( doe - doe/1460 + doe/36524 - doe/146096 ) / 365;
    y   = yoe + era * 400;
    doy = doe - ( 365*yoe + yoe/4 - yoe/100 );
    mp  = ( 5*doy + 2 ) / 153;
    d   = doy - ( 153*mp + 2 )/5 + 1;
    m   = ( mp < 10 ) ? mp + 3 : mp - 9;
    y  += ( m <= 2 );

    st.wYear   = (WORD)y;   st.wMonth  = (WORD)m;   st.wDay    = (WORD)d;
    st.wHour   = (WORD)( secs / 3600 );
    st.wMinute = (WORD)( ( secs % 3600 ) / 60 );
    st.wSecond = (WORD)( secs % 60 );
    st.wMilliseconds = 0;   st.wDayOfWeek = 0;

    if ( SystemTimeToFileTime( &st, &ft ) )
    { *lo = ft.dwLowDateTime; *hi = ft.dwHighDateTime; }
    else
    { *lo = 0; *hi = 0; }
}

/*---- archive object ------------------------------------------------------ */
/* Like RAR4, the entry count only emerges from the header walk, so the tables
 * start small and double instead of costing ~5 MB per archive up front. */
struct Rar5Archive {
    FILE      *fp;
    int        numEntries;
    int        cap;                   /* entries the tables can hold */
    Rar5Entry *entries;
    long      *dataOffset;
};

/* Make room for one more entry.  1 on success, 0 when out of memory, -1 when
 * the entry limit is what stopped us (see RarGrow in RARARC.C - the caller
 * has to report those two differently). */
static int Rar5Grow( Rar5Archive *z )
{
    int        n;
    Rar5Entry *ne;
    long      *nd;

    if ( z->numEntries < z->cap ) return 1;
    if ( (UInt32)z->numEntries >= SZ_MAX_FILES ) return -1;

    n = z->cap ? z->cap * 2 : 32;
    if ( (UInt32)n > SZ_MAX_FILES ) n = (int)SZ_MAX_FILES;
    if ( n <= z->numEntries ) return 0;

    ne = (Rar5Entry *)realloc( z->entries, (size_t)n * sizeof( Rar5Entry ) );
    if ( !ne ) return 0;
    z->entries = ne;

    nd = (long *)realloc( z->dataOffset, (size_t)n * sizeof( long ) );
    if ( !nd ) return 0;
    z->dataOffset = nd;

    z->cap = n;
    return 1;
}

/* Hand back the slack the doubling left over once the scan is done. */
static void Rar5Trim( Rar5Archive *z )
{
    int        n = z->numEntries ? z->numEntries : 1;
    Rar5Entry *ne;
    long      *nd;

    if ( n >= z->cap ) return;

    ne = (Rar5Entry *)realloc( z->entries, (size_t)n * sizeof( Rar5Entry ) );
    if ( ne ) z->entries = ne;
    nd = (long *)realloc( z->dataOffset, (size_t)n * sizeof( long ) );
    if ( nd ) z->dataOffset = nd;
    if ( ne && nd ) z->cap = n;
}

/*---- path helpers -------------------------------------------------------- */
static void MakeDirs( const char *path, int includeLast )
{
    char  buf[SZ_MAX_NAME * 2];
    char *p;
    lstrcpyn( buf, path, sizeof( buf ) );
    p = buf;
    if ( p[0] && p[1] == ':' ) p += 2;
    if ( *p == '\\' ) p++;
    for ( ; *p; p++ )
        if ( *p == '\\' ) { *p = '\0'; _mkdir( buf ); *p = '\\'; }
    if ( includeLast )
        _mkdir( buf );
}

/* destDir\name with the name made filesystem-safe (ArcFsName, ARCFILE.C). */
static void BuildOut( char *dst, int dstSize,
                      const char *destDir, const char *name, int isDir )
{
    char        fsname[SZ_MAX_NAME];
    const char *s = fsname;
    int         n = 0;

    ArcFsName( fsname, sizeof( fsname ), name, isDir );
    if ( destDir && destDir[0] )
    {
        while ( destDir[n] && n < dstSize - 2 ) { dst[n] = destDir[n]; n++; }
        /* '/' counts as a separator too - see BuildPath in SZARC.C. */
        if ( n > 0 && dst[n-1] != '\\' && dst[n-1] != '/' ) dst[n++] = '\\';
    }
    while ( *s && n < dstSize - 1 ) dst[n++] = *s++;
    dst[n] = '\0';
}

/*---- open + walk the blocks ---------------------------------------------- */
int Rar5Open( const char *path, Rar5Archive **out )
{
    Rar5Archive *z;
    Byte         sig[8];
    long         pos;
    int          rc = SZ_OK;

    *out = NULL;

    z = (Rar5Archive *)calloc( 1, sizeof( Rar5Archive ) );
    if ( !z ) return SZ_ERR_MEMORY;
    z->fp = fopen( path, "rb" );
    if ( !z->fp ) { Rar5Close( z ); return SZ_ERR_OPEN; }

    if ( fread( sig, 1, 8, z->fp ) != 8 || memcmp( sig, RAR5_SIG, 8 ) != 0 )
    { Rar5Close( z ); return SZ_ERR_SIG; }

    pos = 8;
    z->numEntries = 0;

    for ( ;; )
    {
        Byte   crc[4];
        UInt32 headSize, dataSize = 0;
        int    vLen;
        Byte  *hdr;
        BRd    b;
        UInt32 type, flags;

        if ( fseek( z->fp, pos, SEEK_SET ) != 0 ) break;
        if ( fread( crc, 1, 4, z->fp ) != 4 ) break;        /* clean EOF */
        if ( RdVintFile( z->fp, &headSize, &vLen ) != 0 ) break;
        if ( headSize == 0 || headSize > 0x100000UL )
        { rc = SZ_ERR_FORMAT; break; }

        hdr = (Byte *)malloc( headSize );
        if ( !hdr ) { rc = SZ_ERR_MEMORY; break; }
        if ( fread( hdr, 1, headSize, z->fp ) != headSize )
        { free( hdr ); rc = SZ_ERR_READ; break; }

        b.p = hdr; b.end = hdr + headSize; b.err = 0;
        type  = BrVint( &b );
        flags = BrVint( &b );
        if ( flags & H5F_EXTRA ) (void)BrVint( &b );        /* extra-area size */
        if ( flags & H5F_DATA )  dataSize = BrVint( &b );

        if ( type == H5_ENDARC )   { free( hdr ); break; }
        if ( type == H5_ENCRYPT )  { free( hdr ); rc = SZ_ERR_UNSUPPORTED; break; }

        if ( type == H5_FILE )
        {
            UInt32 fileFlags = BrVint( &b );
            UInt32 unpSize    = BrVint( &b );
            UInt32 attr       = BrVint( &b );
            UInt32 mtime = 0, dcrc = 0, compInfo, nameLen;
            int    hasMtime = 0, hasCrc = 0;

            if ( fileFlags & F5_MTIME ) { mtime = BrU32( &b ); hasMtime = 1; }
            if ( fileFlags & F5_CRC )   { dcrc  = BrU32( &b ); hasCrc = 1; }
            compInfo = BrVint( &b );
            (void)BrVint( &b );                              /* host OS */
            nameLen  = BrVint( &b );

            if ( !b.err )
            {
                Rar5Entry *e;
                UInt32 j; int k = 0, last;
                UInt32 avail = (UInt32)( b.end - b.p );
                int    g     = Rar5Grow( z );

                if ( g <= 0 )
                {
                    free( hdr );
                    rc = ( g < 0 )
                       ? ArcCheckEntryCount( (UInt32)z->numEntries + 1 )
                       : SZ_ERR_MEMORY;
                    break;
                }
                e = &z->entries[z->numEntries];
                if ( nameLen > avail ) nameLen = avail;

                for ( j = 0; j < nameLen && k < SZ_MAX_NAME - 1; j++ )
                {
                    char c = (char)b.p[j];
                    e->name[k++] = ( c == '/' ) ? '\\' : c;
                }
                e->name[k] = '\0';
                last = k - 1;

                e->size       = unpSize;
                e->packed     = dataSize;
                e->crc        = dcrc;
                e->hasCrc     = hasCrc;
                e->methodCode = (int)( ( compInfo >> 7 ) & 7 );
                e->attrib     = attr;
                e->isDir      = ( fileFlags & F5_DIR ) ? 1 : 0;
                if ( last >= 0 && e->name[last] == '\\' )
                { e->name[last] = '\0'; e->isDir = 1; }

                e->hasMtime = hasMtime;
                e->mtimeLo = e->mtimeHi = 0;
                if ( hasMtime )
                    UnixToFileTime( mtime, &e->mtimeLo, &e->mtimeHi );

                z->dataOffset[z->numEntries] = pos + 4 + vLen + (long)headSize;
                z->numEntries++;
            }
        }

        free( hdr );
        pos = pos + 4 + vLen + (long)headSize + (long)dataSize;
    }

    if ( rc != SZ_OK ) { Rar5Close( z ); return rc; }
    Rar5Trim( z );
    *out = z;
    return SZ_OK;
}

int Rar5NumEntries( Rar5Archive *r )
{
    return r ? r->numEntries : 0;
}

const Rar5Entry *Rar5GetEntry( Rar5Archive *r, int index )
{
    if ( !r || index < 0 || index >= r->numEntries ) return NULL;
    return &r->entries[index];
}

/*---- extract one stored entry -------------------------------------------- */
static int Rar5ExtractIndex( Rar5Archive *z, int idx, const char *destDir )
{
    Rar5Entry    *e = &z->entries[idx];
    char          outPath[SZ_MAX_NAME * 4];
    FILE         *out;
    UInt32        crc, remain;
    unsigned int  toRead;
    unsigned char buf[512];
    int           rc;

    /* destDir == NULL means "test only": read + CRC-check but write nothing. */
    if ( destDir )
    {
        BuildOut( outPath, sizeof( outPath ), destDir, e->name, e->isDir );
        /* Nothing to create, or skipped/cancelled at the 8.3 prompt - see
         * ArcNameVerdict in ARCDEFS.H. */
        if ( ArcNameVerdict() == ARC_NAME_ABORT ) return SZ_ERR_CANCEL;
        if ( ArcNameVerdict() == ARC_NAME_SKIP )  return SZ_OK;
    }

    if ( e->isDir )
    {
        if ( destDir && !ArcFlattenPaths() ) MakeDirs( outPath, 1 );
        return SZ_OK;
    }
    if ( destDir && !ArcWantWrite( outPath ) )
        return SZ_OK;                  /* exists and the user chose to keep it */
    if ( e->methodCode != 0 )
        return SZ_ERR_UNSUPPORTED;                 /* compressed: decoder TBD */

    if ( fseek( z->fp, z->dataOffset[idx], SEEK_SET ) != 0 )
        return SZ_ERR_READ;

    if ( destDir )
    {
        MakeDirs( outPath, 0 );
        out = fopen( outPath, "wb" );
        if ( !out ) return SZ_ERR_WRITE;
    }
    else
        out = NULL;

    rc     = SZ_OK;
    crc    = Crc32Init();
    remain = e->packed;                            /* == size for stored */
    while ( remain > 0 )
    {
        toRead = ( remain > 512UL ) ? 512U : (unsigned int)remain;
        if ( fread( buf, 1, toRead, z->fp ) != toRead ) { rc = SZ_ERR_READ;  break; }
        crc = Crc32Update( crc, buf, toRead );
        if ( out && fwrite( buf, 1, toRead, out ) != toRead ) { rc = SZ_ERR_WRITE; break; }
        remain -= toRead;
    }
    if ( out ) fclose( out );

    if ( rc == SZ_OK && e->hasCrc && Crc32Done( crc ) != e->crc )
        rc = SZ_ERR_CRC;
    if ( rc == SZ_OK )
    {
        if ( destDir && e->hasMtime )
        {
            FILETIME ft;
            ft.dwLowDateTime = e->mtimeLo; ft.dwHighDateTime = e->mtimeHi;
            SetFileMTime( outPath, &ft );
        }
    }
    else if ( destDir )
        remove( outPath );
    return rc;
}

int Rar5ExtractAll( Rar5Archive *r, const char *destDir,
                    SzProgress prog, void *user )
{
    int i, rc;
    if ( !r ) return SZ_ERR_FORMAT;
    for ( i = 0; i < r->numEntries; i++ )
    {
        if ( !r->entries[i].name[0] ) continue;
        if ( prog && !prog( user, i, r->numEntries, r->entries[i].name ) )
            return SZ_ERR_CANCEL;
        rc = Rar5ExtractIndex( r, i, destDir );
        if ( rc ) return rc;
    }
    return SZ_OK;
}

int Rar5ExtractItems( Rar5Archive *r, const int *indices, int count,
                      const char *destDir, SzProgress prog, void *user )
{
    int k, idx, rc;
    if ( !r ) return SZ_ERR_FORMAT;
    for ( k = 0; k < count; k++ )
    {
        idx = indices[k];
        if ( idx < 0 || idx >= r->numEntries ) continue;
        if ( !r->entries[idx].name[0] ) continue;
        if ( prog && !prog( user, idx, r->numEntries, r->entries[idx].name ) )
            return SZ_ERR_CANCEL;
        rc = Rar5ExtractIndex( r, idx, destDir );
        if ( rc ) return rc;
    }
    return SZ_OK;
}

void Rar5Close( Rar5Archive *r )
{
    if ( r )
    {
        if ( r->fp )         fclose( r->fp );
        if ( r->entries )    free( r->entries );
        if ( r->dataOffset ) free( r->dataOffset );
        free( r );
    }
}
