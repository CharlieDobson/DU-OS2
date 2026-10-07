/*===========================================================================
 * ZIPWRITE.C  -  Writing a .zip the way PKZIP 2.04g writes one
 * Target: MSVC 2.2  Win32s
 *
 * Every field is one PKUNZIP 2.04g reads: no data descriptors (the local
 * header is filled in afterwards instead, by seeking back), no extra fields,
 * no Zip64, no UTF-8 flag.  "Version made by" is 2.0 on MS-DOS, and a
 * deflated entry carries general-purpose bit 1, which is how PKZIP marks
 * -ex: anything listing the archive says "maximum" for it.
 *
 * STORE OR DEFLATE, decided twice.  Before: a file whose first 16 KB the
 * sniffer recognises as already compressed (a zip, a JPEG, a 7z, noise) is
 * stored without being tried.  After: a file that deflated to no smaller
 * than it started is written again, stored - the source is read a second
 * time and the stored copy lands over the deflated one.  Since stored is
 * never larger than what it replaces, the entries after it simply follow on,
 * and the file is cut to its final length at the end.
 *
 * NAMES are written in the OEM code page, as the zip format has always had
 * it for MS-DOS-made entries.  DOS and OS/2 names already are; a Win32 name
 * is ANSI and is converted (CharToOemBuff), which is what ZIPARC.C undoes
 * when it reads one back on Win32.  Folders end in '/', and '/' separates
 * them, as the format says.
 *===========================================================================*/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>             /* _chsize */
#include "arccompi.h"
#include "deflenc.h"
#include "crc32.h"

#define ZIP_VER_MADE     20         /* 2.0, host 0 = MS-DOS                 */
#define ZIP_VER_DEFLATE  20
#define ZIP_VER_STORE    10
#define ZIP_FLAG_MAX     0x0002     /* deflated with -ex                    */

typedef struct {
    FILE   *f;
    int     failed;
} ZipOut;

static int PutBytes( ZipOut *o, const void *p, UInt32 n )
{
    if ( !o->failed && n && fwrite( p, 1, n, o->f ) != n ) o->failed = 1;
    return !o->failed;
}

static void Put16( Byte *p, UInt32 v ) { p[0] = (Byte)v; p[1] = (Byte)( v >> 8 ); }
static void Put32( Byte *p, UInt32 v ) { Put16( p, v & 0xFFFF ); Put16( p + 2, v >> 16 ); }

static int DeflSink( void *user, const Byte *data, UInt32 len )
{
    return PutBytes( (ZipOut *)user, data, len );
}

/* The stored name, in the form a zip holds it. */
static void ZipName( ArcCompJob *j, const CompEntry *e, char *dst, int size )
{
    int n;

    lstrcpyn( dst, CompStoredName( j, e ), size - 1 );
#if defined(_WIN32)
    CharToOemBuff( dst, dst, (DWORD)strlen( dst ) );
#endif
    for ( n = 0; dst[n]; n++ )
        if ( dst[n] == '\\' ) dst[n] = '/';
    if ( e->isDir && n > 0 && dst[n - 1] != '/' )
    {
        dst[n++] = '/';
        dst[n] = '\0';
    }
}

static void LocalHeader( Byte *h, const CompEntry *e, unsigned nameLen )
{
    unsigned ver = ( e->method == 8 ) ? ZIP_VER_DEFLATE : ZIP_VER_STORE;
    if ( e->isDir ) ver = ZIP_VER_DEFLATE;

    Put32( h,      0x04034B50UL );
    Put16( h + 4,  ver );
    Put16( h + 6,  ( e->method == 8 ) ? ZIP_FLAG_MAX : 0 );
    Put16( h + 8,  e->method );
    Put16( h + 10, e->dosTime );
    Put16( h + 12, e->dosDate );
    Put32( h + 14, e->crc );
    Put32( h + 18, e->packed );
    Put32( h + 22, e->realSize );
    Put16( h + 26, nameLen );
    Put16( h + 28, 0 );
}

static int WriteLocal( ZipOut *o, const CompEntry *e, const char *name )
{
    Byte h[30];
    unsigned n = (unsigned)strlen( name );
    LocalHeader( h, e, n );
    PutBytes( o, h, 30 );
    return PutBytes( o, name, n );
}

/* Copy the source to the archive unchanged, 'want' bytes or to its end,
 * keeping the CRC and the count.  0 on a read error. */
static int CopyStored( ArcCompJob *j, ZipOut *o, FILE *src, Byte *buf,
                       CompEntry *e, int idx, const char *name )
{
    UInt32 crc = Crc32Init(), total = 0;
    size_t n;

    while ( ( n = fread( buf, 1, COMP_IOBUF, src ) ) > 0 )
    {
        crc = Crc32Update( crc, buf, (UInt32)n );
        total += (UInt32)n;
        j->doneBytes += (UInt32)n;
        if ( !PutBytes( o, buf, (UInt32)n ) ) break;
        if ( !CompProgress( j, idx, (int)j->nOrder, name ) ) break;
    }
    e->crc = Crc32Done( crc );
    e->realSize = total;
    e->packed = total;
    return !ferror( src );
}

int ZipWriteArchive( ArcCompJob *j, FILE *f )
{
    ZipOut   o;
    DeflEnc *d = 0;
    Byte    *buf;
    char     name[SZ_MAX_NAME + 2];
    UInt32   k, nCentral = 0, cdStart, cdSize, endPos;
    int      rc = SZ_OK;

    o.f = f;
    o.failed = 0;
    buf = (Byte *)malloc( COMP_IOBUF );
    if ( !buf ) return SZ_ERR_MEMORY;
    if ( DeflCreate( &d ) != SZ_OK ) { free( buf ); return SZ_ERR_MEMORY; }

    for ( k = 0; k < j->nOrder && rc == SZ_OK; k++ )
    {
        CompEntry *e = &j->ents[ j->order[k] ];
        FILE      *src;
        long       dataStart;
        size_t     got;
        int        isText = 0;

        ZipName( j, e, name, sizeof( name ) );
        if ( !CompProgress( j, (int)k, (int)j->nOrder, CompStoredName( j, e ) ) )
        {
            rc = SZ_ERR_CANCEL;
            break;
        }

        e->offset = (UInt32)ftell( f );

        if ( e->isDir )
        {
            e->method = 0;
            e->crc = e->packed = e->realSize = 0;
            WriteLocal( &o, e, name );
            e->state = CS_DONE;
            nCentral++;
            continue;
        }

        src = CompOpenSource( j, e );
        if ( !src ) continue;                       /* counted as unreadable */

        /* Look at the start of it, then decide. */
        got = fread( buf, 1, COMP_IOBUF, src );
        e->kind = (Byte)CompSniff( buf, (UInt32)got, &isText );
        e->text = (Byte)isText;
        e->method = ( got == 0 || e->kind == CK_PACKED ) ? 0 : 8;
        e->crc = e->packed = e->realSize = 0;
        WriteLocal( &o, e, name );
        dataStart = ftell( f );

        if ( e->method == 8 )
        {
            UInt32 crc = Crc32Init(), total = 0;

            DeflReset( d, DeflSink, &o );
            while ( got > 0 )
            {
                crc = Crc32Update( crc, buf, (UInt32)got );
                total += (UInt32)got;
                j->doneBytes += (UInt32)got;
                if ( DeflFeed( d, buf, (UInt32)got ) != SZ_OK ) break;
                if ( !CompProgress( j, (int)k, (int)j->nOrder, CompStoredName( j, e ) ) )
                {
                    rc = SZ_ERR_CANCEL;
                    break;
                }
                got = fread( buf, 1, COMP_IOBUF, src );
            }
            if ( rc == SZ_OK ) DeflFinish( d );
            e->crc = Crc32Done( crc );
            e->realSize = total;
            e->packed = DeflOutBytes( d );

            if ( rc == SZ_OK && !o.failed && !ferror( src ) && e->packed >= total )
            {
                /* It did not help: write it again, stored. */
                j->doneBytes -= total;
                fseek( f, dataStart, SEEK_SET );
                fseek( src, 0, SEEK_SET );
                e->method = 0;
                if ( !CopyStored( j, &o, src, buf, e, (int)k, CompStoredName( j, e ) ) )
                    rc = SZ_ERR_READ;
            }
            else if ( ferror( src ) )
                rc = SZ_ERR_READ;
        }
        else
        {
            /* Stored from the start: the first buffer is already read. */
            UInt32 crc = Crc32Init(), total = 0;
            while ( got > 0 )
            {
                crc = Crc32Update( crc, buf, (UInt32)got );
                total += (UInt32)got;
                j->doneBytes += (UInt32)got;
                if ( !PutBytes( &o, buf, (UInt32)got ) ) break;
                if ( !CompProgress( j, (int)k, (int)j->nOrder, CompStoredName( j, e ) ) )
                {
                    rc = SZ_ERR_CANCEL;
                    break;
                }
                got = fread( buf, 1, COMP_IOBUF, src );
            }
            e->crc = Crc32Done( crc );
            e->realSize = e->packed = total;
            if ( ferror( src ) ) rc = SZ_ERR_READ;
        }
        fclose( src );

        if ( rc == SZ_ERR_READ )
        {
            char path[SZ_MAX_NAME * 2];
            CompSourcePath( j, e, path, sizeof( path ) );
            CompSetProblem( j, path );
        }
        if ( rc != SZ_OK || o.failed ) break;

        /* Now the local header can say what happened. */
        {
            long end = ftell( f );
            fseek( f, (long)e->offset, SEEK_SET );
            WriteLocal( &o, e, name );
            fseek( f, end, SEEK_SET );
        }
        e->state = CS_DONE;
        j->nFiles++;
        j->inBytes += e->realSize;
        nCentral++;
    }

    if ( rc == SZ_OK && o.failed ) rc = SZ_ERR_WRITE;

    /* The central directory: every entry written, in the same order. */
    if ( rc == SZ_OK )
    {
        cdStart = (UInt32)ftell( f );
        for ( k = 0; k < j->nOrder; k++ )
        {
            CompEntry *e = &j->ents[ j->order[k] ];
            Byte       c[46];
            unsigned   n;

            if ( e->state != CS_DONE ) continue;
            ZipName( j, e, name, sizeof( name ) );
            n = (unsigned)strlen( name );
            Put32( c,      0x02014B50UL );
            Put16( c + 4,  ZIP_VER_MADE );
            Put16( c + 6,  ( e->method == 8 || e->isDir ) ? ZIP_VER_DEFLATE : ZIP_VER_STORE );
            Put16( c + 8,  ( e->method == 8 ) ? ZIP_FLAG_MAX : 0 );
            Put16( c + 10, e->method );
            Put16( c + 12, e->dosTime );
            Put16( c + 14, e->dosDate );
            Put32( c + 16, e->crc );
            Put32( c + 20, e->packed );
            Put32( c + 24, e->realSize );
            Put16( c + 28, n );
            Put16( c + 30, 0 );                       /* extra    */
            Put16( c + 32, 0 );                       /* comment  */
            Put16( c + 34, 0 );                       /* disk     */
            Put16( c + 36, e->text ? 1 : 0 );         /* internal: text */
            Put32( c + 38, e->attr & 0xFF );          /* external: DOS bits */
            Put32( c + 42, e->offset );
            PutBytes( &o, c, 46 );
            PutBytes( &o, name, n );
        }
        cdSize = (UInt32)ftell( f ) - cdStart;
        {
            Byte z[22];
            Put32( z,      0x06054B50UL );
            Put16( z + 4,  0 );
            Put16( z + 6,  0 );
            Put16( z + 8,  nCentral );
            Put16( z + 10, nCentral );
            Put32( z + 12, cdSize );
            Put32( z + 16, cdStart );
            Put16( z + 20, 0 );
            PutBytes( &o, z, 22 );
        }
        if ( fflush( f ) != 0 ) o.failed = 1;
        endPos = (UInt32)ftell( f );

        /* An entry written again as stored was shorter the second time, and
         * what came after it moved up: whatever lies past the end now is
         * left over, and a reader finds the end record by its position. */
        if ( !o.failed && _chsize( fileno( f ), (long)endPos ) != 0 )
            o.failed = 1;
        if ( o.failed ) rc = SZ_ERR_WRITE;
        j->outBytes = endPos;
    }

    DeflFree( d );
    free( buf );
    return rc;
}
