/*===========================================================================
 * RARARC.C  -  RAR (2.x/3.x, "RAR4") parsing and extraction
 * Target: MSVC 2.2  Win32s
 *
 * PHASE 1: container parsing, listing, and STORED (method 0x30) extraction.
 * Compressed entries are listed but return SZ_ERR_UNSUPPORTED on extraction
 * until the RAR unpack engine is ported (a later phase).  RAR5 is detected and
 * reported as unsupported.  No encryption, no multi-volume.
 *===========================================================================*/

#include <windows.h>     /* lstrcpyn */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>      /* _mkdir */

#include "rararc.h"
#include "rar3dec.h"
#include "crc32.h"
#include "platform.h"   /* SetFileDosMTime */

/*---- RAR4 block constants ------------------------------------------------ */
#define RAR_MARK      0x72   /* marker block type             */
#define RAR_MAIN      0x73   /* archive (main) header         */
#define RAR_FILE      0x74   /* file header                   */
#define RAR_ENDARC    0x7B   /* end-of-archive header         */

/* main-header flags */
#define MHD_SOLID       0x0008   /* solid archive               */
#define MHD_ENCRYPTVER  0x0080   /* block headers are encrypted */
/* file-header flags */
#define LHD_SPLIT_AFTER 0x0002
#define LHD_PASSWORD    0x0004   /* entry is encrypted          */
#define LHD_SOLID       0x0010   /* uses the previous file's window */
#define LHD_LARGE       0x0100   /* 64-bit size fields present  */
#define LHD_UNICODE     0x0200   /* name carries a unicode part */
#define LHD_DIRECTORY   0x00E0   /* all dictionary bits set      */
/* base-header flags */
#define LONG_BLOCK      0x8000   /* an ADD_SIZE/data area follows */

#define RAR_METHOD_STORE 0x30

static const unsigned char RAR4_SIG[7] =
    { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00 };

/*---- little-endian field readers ----------------------------------------- */
static UInt32 Rd16( const Byte *p )
{
    return (UInt32)p[0] | ( (UInt32)p[1] << 8 );
}
static UInt32 Rd32( const Byte *p )
{
    return (UInt32)p[0] | ( (UInt32)p[1] << 8 ) |
           ( (UInt32)p[2] << 16 ) | ( (UInt32)p[3] << 24 );
}

/*---- archive object ------------------------------------------------------ */
/* RAR has no up-front file count - the entries are discovered while walking
 * the block headers - so the parallel tables start small and double.  Fixed
 * SZ_MAX_FILES tables here would cost ~5 MB for every archive opened. */
struct RarArchive {
    FILE     *fp;
    int       numEntries;
    int       cap;                       /* entries the tables can hold      */
    int       solid;                     /* archive uses solid compression   */
    RarEntry *entries;
    long     *dataOffset;                /* file offset of the data area     */
    UInt16   *headFlags;                 /* file-header flags                */
};

/* Make room for one more entry.  Returns 0 (and leaves the archive usable)
 * when out of memory. */
static int RarGrow( RarArchive *z )
{
    int       n;
    RarEntry *ne;
    long     *nd;
    UInt16   *nf;

    if ( z->numEntries < z->cap ) return 1;

    n = z->cap ? z->cap * 2 : 32;
    if ( n > SZ_MAX_FILES ) n = SZ_MAX_FILES;
    if ( n <= z->numEntries ) return 0;

    ne = (RarEntry *)realloc( z->entries, (size_t)n * sizeof( RarEntry ) );
    if ( !ne ) return 0;
    z->entries = ne;

    nd = (long *)realloc( z->dataOffset, (size_t)n * sizeof( long ) );
    if ( !nd ) return 0;
    z->dataOffset = nd;

    nf = (UInt16 *)realloc( z->headFlags, (size_t)n * sizeof( UInt16 ) );
    if ( !nf ) return 0;
    z->headFlags = nf;

    z->cap = n;
    return 1;
}

/* Hand back the slack the doubling left over once the scan is done.  A shrink
 * that fails is harmless - the tables just stay as they are. */
static void RarTrim( RarArchive *z )
{
    int       n = z->numEntries ? z->numEntries : 1;
    RarEntry *ne;
    long     *nd;
    UInt16   *nf;

    if ( n >= z->cap ) return;

    ne = (RarEntry *)realloc( z->entries, (size_t)n * sizeof( RarEntry ) );
    if ( ne ) z->entries = ne;
    nd = (long *)realloc( z->dataOffset, (size_t)n * sizeof( long ) );
    if ( nd ) z->dataOffset = nd;
    nf = (UInt16 *)realloc( z->headFlags, (size_t)n * sizeof( UInt16 ) );
    if ( nf ) z->headFlags = nf;
    if ( ne && nd && nf ) z->cap = n;
}

/*---- path helpers (shared style with ZIPARC) ----------------------------- */
static void MakeDirs( const char *path, int includeLast )
{
    char  buf[SZ_MAX_NAME * 2];
    char *p;

    lstrcpyn( buf, path, sizeof( buf ) );
    p = buf;
    if ( p[0] && p[1] == ':' ) p += 2;
    if ( *p == '\\' ) p++;

    for ( ; *p; p++ )
        if ( *p == '\\' )
        {
            *p = '\0';
            _mkdir( buf );
            *p = '\\';
        }
    if ( includeLast )
        _mkdir( buf );
}

/* destDir\name with the name made filesystem-safe (ArcFsName, ARCFILE.C). */
static void BuildOut( char *dst, int dstSize,
                      const char *destDir, const char *name )
{
    char        fsname[SZ_MAX_NAME];
    const char *s = fsname;
    int         n = 0;

    ArcFsName( fsname, sizeof( fsname ), name );
    if ( destDir && destDir[0] )
    {
        while ( destDir[n] && n < dstSize - 2 ) { dst[n] = destDir[n]; n++; }
        if ( n > 0 && dst[n-1] != '\\' ) dst[n++] = '\\';
    }
    while ( *s && n < dstSize - 1 ) dst[n++] = *s++;
    dst[n] = '\0';
}

/*---- open + walk the block headers --------------------------------------- */
int RarOpen( const char *path, RarArchive **out )
{
    RarArchive *z;
    Byte        marker[7];
    long        pos;
    int         rc = SZ_OK;

    *out = NULL;

    z = (RarArchive *)calloc( 1, sizeof( RarArchive ) );
    if ( !z ) return SZ_ERR_MEMORY;

    z->fp = fopen( path, "rb" );
    if ( !z->fp ) { RarClose( z ); return SZ_ERR_OPEN; }

    if ( fread( marker, 1, 7, z->fp ) != 7 )
    { RarClose( z ); return SZ_ERR_SIG; }

    /* "Rar!\x1A\x07" then 0x00 = RAR4, 0x01 = RAR5 (not yet supported). */
    if ( memcmp( marker, RAR4_SIG, 6 ) != 0 )
    { RarClose( z ); return SZ_ERR_SIG; }
    if ( marker[6] != 0x00 )
    { RarClose( z ); return SZ_ERR_UNSUPPORTED; }  /* RAR5 */

    pos = 7;
    z->numEntries = 0;

    for ( ;; )
    {
        Byte   base[7];
        Byte   type;
        UInt16 flags, headSize;

        if ( fseek( z->fp, pos, SEEK_SET ) != 0 ) break;
        if ( fread( base, 1, 7, z->fp ) != 7 ) break;     /* clean EOF */

        type     = base[2];
        flags    = (UInt16)Rd16( base + 3 );
        headSize = (UInt16)Rd16( base + 5 );
        if ( headSize < 7 ) { rc = SZ_ERR_FORMAT; break; }

        if ( type == RAR_ENDARC ) break;
        if ( type == RAR_MAIN )
        {
            if ( flags & MHD_ENCRYPTVER )
            { rc = SZ_ERR_UNSUPPORTED; break; }            /* encrypted headers */
            z->solid = ( flags & MHD_SOLID ) ? 1 : 0;
        }

        if ( type == RAR_FILE )
        {
            Byte  *hdr = (Byte *)malloc( headSize );
            UInt32 packSize, unpSize, ftime, attr;
            UInt16 nameSize;
            int    nameOff;

            if ( !hdr ) { rc = SZ_ERR_MEMORY; break; }
            memcpy( hdr, base, 7 );
            if ( fread( hdr + 7, 1, (size_t)( headSize - 7 ), z->fp )
                 != (size_t)( headSize - 7 ) )
            { free( hdr ); rc = SZ_ERR_READ; break; }

            packSize = Rd32( hdr + 7 );
            unpSize  = Rd32( hdr + 11 );
            ftime    = Rd32( hdr + 20 );
            attr     = Rd32( hdr + 28 );
            nameSize = (UInt16)Rd16( hdr + 26 );
            nameOff  = 32;
            if ( flags & LHD_LARGE )           /* skip 8 bytes of high sizes */
                nameOff = 40;
            if ( nameOff + (int)nameSize > (int)headSize )
                nameSize = ( headSize > nameOff ) ? (UInt16)( headSize - nameOff ) : 0;

            if ( !RarGrow( z ) ) { free( hdr ); rc = SZ_ERR_MEMORY; break; }

            if ( z->numEntries < SZ_MAX_FILES )
            {
                RarEntry *e = &z->entries[z->numEntries];
                const Byte *np = hdr + nameOff;
                UInt16 j; int k = 0, last;

                for ( j = 0; j < nameSize && k < SZ_MAX_NAME - 1; j++ )
                {
                    char c = (char)np[j];
                    if ( c == '\0' ) break;    /* unicode: ascii part ends here */
                    e->name[k++] = ( c == '/' ) ? '\\' : c;
                }
                e->name[k] = '\0';

                e->size       = unpSize;
                e->packed     = packSize;
                e->crc        = Rd32( hdr + 16 );
                e->unpVer     = hdr[24];
                e->methodCode = hdr[25];
                e->modDate    = (UInt16)( ftime >> 16 );
                e->modTime    = (UInt16)( ftime & 0xFFFF );
                e->attrib     = attr;
                e->isDir      = ( ( flags & LHD_DIRECTORY ) == LHD_DIRECTORY );

                last = k - 1;
                if ( last >= 0 && e->name[last] == '\\' )
                { e->name[last] = '\0'; e->isDir = 1; }

                z->dataOffset[z->numEntries] = pos + headSize;
                z->headFlags[z->numEntries]  = flags;
                z->numEntries++;
            }

            free( hdr );
            pos = pos + headSize + (long)packSize;
        }
        else
        {
            UInt32 addSize = 0;
            if ( flags & LONG_BLOCK )
            {
                Byte a[4];
                if ( fread( a, 1, 4, z->fp ) != 4 ) break;  /* a sits at pos+7 */
                addSize = Rd32( a );
            }
            pos = pos + headSize + (long)addSize;
        }

        if ( z->numEntries >= SZ_MAX_FILES ) break;
    }

    if ( rc != SZ_OK ) { RarClose( z ); return rc; }

    RarTrim( z );
    *out = z;
    return SZ_OK;
}

int RarNumEntries( RarArchive *r )
{
    return r ? r->numEntries : 0;
}

const RarEntry *RarGetEntry( RarArchive *r, int index )
{
    if ( !r || index < 0 || index >= r->numEntries ) return NULL;
    return &r->entries[index];
}

/*---- extract one entry by index ------------------------------------------ */
static int RarExtractIndex( RarArchive *z, int idx, const char *destDir )
{
    RarEntry *e = &z->entries[idx];
    char      outPath[SZ_MAX_NAME * 4];
    FILE     *out;
    int       rc = SZ_OK;

    /* destDir == NULL means "test only": decode + CRC-check but write nothing. */
    if ( destDir ) BuildOut( outPath, sizeof( outPath ), destDir, e->name );

    if ( e->isDir )
    {
        if ( destDir && !ArcFlattenPaths() ) MakeDirs( outPath, 1 );
        return SZ_OK;
    }
    if ( destDir && !ArcWantWrite( outPath ) )
        return SZ_OK;                  /* exists and the user chose to keep it */
    if ( z->headFlags[idx] & LHD_PASSWORD )
        return SZ_ERR_UNSUPPORTED;                 /* encrypted */

    if ( fseek( z->fp, z->dataOffset[idx], SEEK_SET ) != 0 )
        return SZ_ERR_READ;
    if ( destDir ) MakeDirs( outPath, 0 );

    if ( e->methodCode == RAR_METHOD_STORE )
    {
        UInt32        crc = Crc32Init(), remain = e->packed;
        unsigned int  toRead;
        unsigned char buf[512];

        out = destDir ? fopen( outPath, "wb" ) : NULL;
        if ( destDir && !out ) return SZ_ERR_WRITE;
        while ( remain > 0 )
        {
            toRead = ( remain > 512UL ) ? 512U : (unsigned int)remain;
            if ( fread( buf, 1, toRead, z->fp ) != toRead ) { rc = SZ_ERR_READ;  break; }
            crc = Crc32Update( crc, buf, toRead );
            if ( out && fwrite( buf, 1, toRead, out ) != toRead ) { rc = SZ_ERR_WRITE; break; }
            remain -= toRead;
        }
        if ( out ) fclose( out );
        if ( rc == SZ_OK && Crc32Done( crc ) != e->crc )
            rc = SZ_ERR_CRC;
    }
    else                                           /* compressed */
    {
        Byte *packBuf, *outBuf;

        if ( z->headFlags[idx] & LHD_SOLID )
            return SZ_ERR_UNSUPPORTED;             /* needs cross-file window */
        if ( e->unpVer < 20 )
            return SZ_ERR_UNSUPPORTED;             /* RAR 1.x (v1)            */
        /* Non-solid RAR decodes to a buffer, so this entry has to fit in RAM
         * whole (unlike the 7z folder path, which streams). */
        if ( e->packed > SZ_MAX_BUFFER_SIZE || e->size > SZ_MAX_BUFFER_SIZE )
            return SZ_ERR_TOOBIG;

        packBuf = (Byte *)malloc( e->packed ? e->packed : 1 );
        if ( !packBuf ) return SZ_ERR_MEMORY;
        if ( fread( packBuf, 1, e->packed, z->fp ) != e->packed )
        { free( packBuf ); return SZ_ERR_READ; }

        outBuf = (Byte *)malloc( e->size ? e->size : 1 );
        if ( !outBuf ) { free( packBuf ); return SZ_ERR_MEMORY; }

        rc = ( e->unpVer >= 29 )
             ? Rar3Decode( packBuf, e->packed, outBuf, e->size )   /* RAR3 v3 */
             : Rar2Decode( packBuf, e->packed, outBuf, e->size );  /* RAR2 v2 */
        free( packBuf );

        if ( rc == SZ_OK && Crc32Calc( outBuf, e->size ) != e->crc )
            rc = SZ_ERR_CRC;
        if ( rc == SZ_OK && destDir )
        {
            out = fopen( outPath, "wb" );
            if ( !out ) { free( outBuf ); return SZ_ERR_WRITE; }
            if ( e->size && fwrite( outBuf, 1, e->size, out ) != e->size )
                rc = SZ_ERR_WRITE;
            fclose( out );
        }
        free( outBuf );
    }

    if ( rc == SZ_OK )
    {
        if ( destDir ) SetFileDosMTime( outPath, e->modDate, e->modTime );
    }
    else if ( destDir )
        remove( outPath );
    return rc;
}

/*---- solid extraction (streaming) ---------------------------------------- *
 * In a solid archive every file shares one continuous LZ window, so a file
 * can't be decoded without the ones before it.  We decode the data-bearing
 * files in archive order through one persistent RAR2 context whose bounded
 * ring window supplies match look-back, streaming each output byte through a
 * sink that writes to the current file and rolls over at file boundaries.
 * Memory stays flat (window + small buffers) regardless of archive size.
 *-------------------------------------------------------------------------- */
typedef struct {
    RarArchive *z;
    const char *destDir;
    const char *req;             /* 1 per entry: requested for output         */
    SzProgress  prog;
    void       *user;
    int         scanNext;        /* next entry index to scan for a chain file */
    int         curIdx;          /* current file's entry index (-1 = none)    */
    int         curReq;          /* current file is requested                 */
    UInt32      fileRemaining;
    UInt32      fileCrc;
    FILE       *out;
    char        outPath[SZ_MAX_NAME * 4];
    Byte        buf[8192];
    UInt32      bufLen;
    int         rc;
} SolidSink;

static void SolidFlush( SolidSink *s )
{
    if ( s->bufLen )
    {
        if ( s->curReq )
            s->fileCrc = Crc32Update( s->fileCrc, s->buf, s->bufLen );
        if ( s->out && fwrite( s->buf, 1, s->bufLen, s->out ) != s->bufLen )
            s->rc = SZ_ERR_WRITE;
        s->bufLen = 0;
    }
}

static void SolidFinishFile( SolidSink *s )
{
    if ( s->curIdx < 0 ) return;
    SolidFlush( s );
    if ( s->out ) { fclose( s->out ); s->out = NULL; }
    if ( s->rc == SZ_OK && s->curReq )
    {
        RarEntry *e = &s->z->entries[s->curIdx];
        if ( Crc32Done( s->fileCrc ) != e->crc )
            s->rc = SZ_ERR_CRC;
        else if ( s->destDir )
            SetFileDosMTime( s->outPath, e->modDate, e->modTime );
    }
    s->curIdx = -1;
}

static void SolidOpenNext( SolidSink *s )
{
    RarArchive *z = s->z;
    int i;
    for ( i = s->scanNext; i < z->numEntries; i++ )
        if ( !z->entries[i].isDir && z->entries[i].size > 0 )
        {
            RarEntry *e = &z->entries[i];
            s->scanNext      = i + 1;
            s->curIdx        = i;
            s->curReq        = s->req[i];
            s->fileRemaining = e->size;
            s->fileCrc       = Crc32Init();
            s->bufLen        = 0;
            s->out           = NULL;
            if ( s->curReq && s->destDir )    /* NULL destDir = test only */
            {
                BuildOut( s->outPath, sizeof( s->outPath ),
                          s->destDir, e->name );
                /* Declined overwrite: demote to "not requested" - the chain
                 * still decodes through it, but nothing is written, CRC-
                 * checked, or timestamped, and the kept file is never
                 * removed. */
                if ( !ArcWantWrite( s->outPath ) )
                    s->curReq = 0;
            }
            if ( s->curReq )
            {
                if ( s->destDir )
                {
                    MakeDirs( s->outPath, 0 );
                    s->out = fopen( s->outPath, "wb" );
                    if ( !s->out ) { s->rc = SZ_ERR_WRITE; return; }
                }
                if ( s->prog && !s->prog( s->user, i, z->numEntries, e->name ) )
                    s->rc = SZ_ERR_CANCEL;
            }
            return;
        }
    s->rc = SZ_ERR_DATA;             /* data left but no chain file to hold it */
}

static int SolidEmit( void *user, Byte b )
{
    SolidSink *s = (SolidSink *)user;
    if ( s->rc != SZ_OK ) return s->rc;
    if ( s->fileRemaining == 0 )
    {
        SolidFinishFile( s );
        if ( s->rc != SZ_OK ) return s->rc;
        SolidOpenNext( s );
        if ( s->rc != SZ_OK ) return s->rc;
    }
    s->buf[s->bufLen++] = b;
    s->fileRemaining--;
    if ( s->bufLen == sizeof( s->buf ) )
        SolidFlush( s );
    return s->rc;
}

static int SolidExtract( RarArchive *z, const int *indices, int count,
                         const char *destDir, SzProgress prog, void *user )
{
    char       *req;                 /* per-entry "wanted" flags (heap: the  */
    SolidSink   sink;                /* stack cannot hold one per entry)     */
    Rar2Ctx    *ctx2 = NULL;
    Rar3Ctx    *ctx3 = NULL;
    Byte       *packBuf = NULL;
    UInt32      packCap = 0, cum = 0;
    int         i, k, lastReqChain = -1, needTables = 1, useV3 = 0, rc = SZ_OK;

    req = (char *)calloc( z->numEntries ? z->numEntries : 1, 1 );
    if ( !req ) return SZ_ERR_MEMORY;
    if ( indices )
    {
        for ( k = 0; k < count; k++ )
            if ( indices[k] >= 0 && indices[k] < z->numEntries )
                req[indices[k]] = 1;
    }
    else
        for ( i = 0; i < z->numEntries; i++ ) req[i] = 1;

    if ( destDir && !ArcFlattenPaths() )      /* NULL destDir = test only */
        for ( i = 0; i < z->numEntries; i++ ) /* requested directories */
            if ( req[i] && z->entries[i].isDir && z->entries[i].name[0] )
            {
                char outPath[SZ_MAX_NAME * 4];
                BuildOut( outPath, sizeof( outPath ), destDir, z->entries[i].name );
                MakeDirs( outPath, 1 );
            }

    for ( i = 0; i < z->numEntries; i++ )
        if ( req[i] && !z->entries[i].isDir && z->entries[i].size > 0 )
            lastReqChain = i;

    if ( lastReqChain >= 0 )
    {
        /* One solid chain is produced by a single RAR version, so pick the
         * decoder from the first compressed data-bearing entry.  A chain with
         * only STORED entries needs no real codec (Feed handles those); the
         * v2 context then just supplies the shared ring window. */
        for ( i = 0; i <= lastReqChain; i++ )
        {
            RarEntry *e = &z->entries[i];
            if ( e->isDir || e->size == 0 ) continue;
            if ( e->methodCode == RAR_METHOD_STORE ) continue;
            useV3 = ( e->unpVer >= 29 ) ? 1 : 0;
            break;
        }

        if ( useV3 ) { ctx3 = Rar3Create(); if ( !ctx3 ) { free( req ); return SZ_ERR_MEMORY; } }
        else         { ctx2 = Rar2Create(); if ( !ctx2 ) { free( req ); return SZ_ERR_MEMORY; } }

        sink.z = z; sink.destDir = destDir; sink.req = req;
        sink.prog = prog; sink.user = user;
        sink.scanNext = 0; sink.curIdx = -1; sink.curReq = 0;
        sink.fileRemaining = 0; sink.fileCrc = 0;
        sink.out = NULL; sink.bufLen = 0; sink.rc = SZ_OK;

        for ( i = 0; i <= lastReqChain && rc == SZ_OK; i++ )
        {
            RarEntry *e = &z->entries[i];

            if ( e->isDir || e->size == 0 ) continue;
            if ( z->headFlags[i] & LHD_PASSWORD ) { rc = SZ_ERR_UNSUPPORTED; break; }
            if ( e->unpVer < 20 ) { rc = SZ_ERR_UNSUPPORTED; break; }  /* v1 */
            if ( e->methodCode != RAR_METHOD_STORE &&
                 ( ( e->unpVer >= 29 ) ? 1 : 0 ) != useV3 )
            { rc = SZ_ERR_UNSUPPORTED; break; }        /* mixed-version solid */

            if ( e->packed > packCap )
            {
                Byte *nb = (Byte *)realloc( packBuf, e->packed );
                if ( !nb ) { rc = SZ_ERR_MEMORY; break; }
                packBuf = nb; packCap = e->packed;
            }
            if ( fseek( z->fp, z->dataOffset[i], SEEK_SET ) != 0 ||
                 fread( packBuf, 1, e->packed, z->fp ) != e->packed )
            { rc = SZ_ERR_READ; break; }

            cum += e->size;
            if ( e->methodCode == RAR_METHOD_STORE )
                rc = useV3
                     ? Rar3Feed( ctx3, SolidEmit, &sink, packBuf, e->packed )
                     : Rar2Feed( ctx2, SolidEmit, &sink, packBuf, e->packed );
            else if ( useV3 )
            {
                Rar3SetInput( ctx3, packBuf, e->packed );
                if ( needTables )
                { rc = Rar3ReadTables( ctx3 ); needTables = 0; if ( rc ) break; }
                rc = Rar3Decode2( ctx3, SolidEmit, &sink, cum );
            }
            else
            {
                Rar2SetInput( ctx2, packBuf, e->packed );
                if ( needTables )
                { rc = Rar2ReadTables( ctx2 ); needTables = 0; if ( rc ) break; }
                rc = Rar2Decode2( ctx2, SolidEmit, &sink, cum );
            }
        }

        if ( rc == SZ_OK ) { SolidFinishFile( &sink ); rc = sink.rc; }
        if ( sink.out ) fclose( sink.out );
        if ( packBuf ) free( packBuf );
        if ( ctx3 ) Rar3Free( ctx3 );
        if ( ctx2 ) Rar2Free( ctx2 );
        if ( rc != SZ_OK ) { free( req ); return rc; }
    }

    if ( destDir )                            /* NULL destDir = test only */
        for ( i = 0; i < z->numEntries; i++ ) /* requested empty files */
            if ( req[i] && !z->entries[i].isDir && z->entries[i].size == 0
                 && z->entries[i].name[0] )
            {
                char  outPath[SZ_MAX_NAME * 4];
                FILE *out;
                BuildOut( outPath, sizeof( outPath ), destDir, z->entries[i].name );
                if ( !ArcWantWrite( outPath ) ) continue;
                MakeDirs( outPath, 0 );
                out = fopen( outPath, "wb" );
                if ( out ) fclose( out );
            }

    free( req );
    return SZ_OK;
}

int RarExtractAll( RarArchive *r, const char *destDir,
                   SzProgress prog, void *user )
{
    int i, rc;
    if ( !r ) return SZ_ERR_FORMAT;
    if ( r->solid )
        return SolidExtract( r, NULL, 0, destDir, prog, user );
    for ( i = 0; i < r->numEntries; i++ )
    {
        if ( !r->entries[i].name[0] ) continue;
        if ( prog && !prog( user, i, r->numEntries, r->entries[i].name ) )
            return SZ_ERR_CANCEL;
        rc = RarExtractIndex( r, i, destDir );
        if ( rc ) return rc;
    }
    return SZ_OK;
}

int RarExtractItems( RarArchive *r, const int *indices, int count,
                     const char *destDir, SzProgress prog, void *user )
{
    int k, idx, rc;
    if ( !r ) return SZ_ERR_FORMAT;
    if ( r->solid )
        return SolidExtract( r, indices, count, destDir, prog, user );
    for ( k = 0; k < count; k++ )
    {
        idx = indices[k];
        if ( idx < 0 || idx >= r->numEntries ) continue;
        if ( !r->entries[idx].name[0] ) continue;
        if ( prog && !prog( user, idx, r->numEntries, r->entries[idx].name ) )
            return SZ_ERR_CANCEL;
        rc = RarExtractIndex( r, idx, destDir );
        if ( rc ) return rc;
    }
    return SZ_OK;
}

void RarClose( RarArchive *r )
{
    if ( r )
    {
        if ( r->fp )         fclose( r->fp );
        if ( r->entries )    free( r->entries );
        if ( r->dataOffset ) free( r->dataOffset );
        if ( r->headFlags )  free( r->headFlags );
        free( r );
    }
}
