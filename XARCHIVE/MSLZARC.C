/*===========================================================================
 * MSLZARC.C  -  Files packed by Microsoft's COMPRESS.EXE (SZDD and KWAJ)
 * Target: MSVC 2.2  Win32s, and the Open Watcom DOS/32A and OS/2 builds
 *
 * See MSLZARC.H for the two formats.  Microsoft documented neither.  SZDD's
 * LZSS is simple and widely described; KWAJ's LZSS + Huffman method is known
 * from the libmspack project's reverse engineering.  Both are written fresh
 * here from those descriptions, and checked against the Windows 3.1 disks
 * and against files COMPRESS 1.11 itself wrote.
 *===========================================================================*/

#include <windows.h>     /* lstrcpyn (the DOS and OS/2 builds shim it) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <direct.h>      /* _mkdir */
#include <sys/types.h>
#include <sys/stat.h>

#include "mslzarc.h"
#include "mszipdec.h"
#include "platform.h"   /* SetFileDosMTime */

#define ML_SZDD       1         /* "SZDD": COMPRESS -r                    */
#define ML_SZQB       2         /* "SZ ": the older QBasic-era variant    */
#define ML_KWAJ       3

/* KWAJ's methods; SZDD is always ML_M_LZSS. */
#define ML_M_STORE    0
#define ML_M_XOR      1
#define ML_M_LZSS     2
#define ML_M_LZH      3
#define ML_M_MSZIP    4
#define ML_M_LAST     ML_M_MSZIP

/* KWAJ header flags: which optional fields follow the fixed 14 bytes, in
 * this order. */
#define KW_HAS_LENGTH 0x0001    /* 4 bytes: the uncompressed length       */
#define KW_HAS_UNK2   0x0002    /* 2 bytes nobody has identified           */
#define KW_HAS_DATA   0x0004    /* a 2-byte count, then that many bytes    */
#define KW_HAS_NAME   0x0008    /* the original base name, NUL-ended      */
#define KW_HAS_EXT    0x0010    /* the original extension, NUL-ended      */
#define KW_HAS_TEXT   0x0020    /* a 2-byte count, then free text         */

#define ML_WINDOW     4096U
#define ML_WMASK      ( ML_WINDOW - 1 )

/* KWAJ's MSZIP chunks are read out of one continuous stream; a block of 32 KB
 * of output never needs more than about 33 KB of input, so 64 KB always holds
 * a whole one. */
#define ML_ZBUF       65536UL

static const Byte SIG_SZDD[8] = { 'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33 };
static const Byte SIG_SZQB[8] = { 'S', 'Z', ' ', 0x88, 0xF0, 0x27, 0x33, 0xD1 };
static const Byte SIG_KWAJ[8] = { 'K', 'W', 'A', 'J', 0x88, 0xF0, 0x27, 0xD1 };

struct MslzArchive {
    FILE     *fp;
    char      path[SZ_MAX_NAME * 2];
    int       kind;             /* ML_SZDD / ML_SZQB / ML_KWAJ             */
    int       method;           /* ML_M_*, or what the header said         */
    long      dataStart;
    UInt32    dataLen;          /* from dataStart to the end of the file   */
    int       sizeKnown;
    int       hasLength;        /* sizeKnown from the header, not a decode */
    char     *comment;
    MslzEntry entry;
};

/*---- Input and output ------------------------------------------------------*/
typedef struct {
    FILE  *fp;
    UInt32 left;                /* bytes of the data not yet read          */
    Byte   buf[4096];
    UInt32 pos, len;
    int    err;                 /* fread came up short: a real read error  */
} MlIn;

typedef struct {
    FILE  *fp;                  /* NULL when testing, or measuring          */
    Byte   buf[8192];
    UInt32 len;
    UInt32 total;
    UInt32 limit;               /* the header's length, or 0xFFFFFFFF      */
    int    rc;
} MlOut;

typedef struct {
    UInt16 count[17];
    UInt16 symbol[256];
} MlHuff;

/* Everything one decode needs, on the heap: the DOS build's stack is small. */
typedef struct {
    MlIn   in;
    MlOut  out;
    Byte   window[ML_WINDOW];
    MlHuff trees[5];
} MlWork;

static int MlGet( MlIn *in )
{
    if ( in->pos == in->len )
    {
        UInt32 want = ( in->left < sizeof( in->buf ) )
                    ? in->left : (UInt32)sizeof( in->buf );

        if ( want == 0 ) return -1;
        in->len = (UInt32)fread( in->buf, 1, want, in->fp );
        in->pos = 0;
        if ( in->len < want ) in->err = 1;
        in->left = in->err ? 0 : in->left - in->len;
        if ( in->len == 0 ) return -1;
    }
    return in->buf[in->pos++];
}

static UInt32 MlRead( MlIn *in, Byte *dst, UInt32 n )
{
    UInt32 got = 0;

    while ( got < n )
    {
        int val = MlGet( in );

        if ( val < 0 ) break;
        dst[got++] = (Byte)val;
    }
    return got;
}

static int MlFlush( MlOut *out )
{
    if ( out->len && out->fp &&
         fwrite( out->buf, 1, out->len, out->fp ) != out->len )
        out->rc = SZ_ERR_WRITE;
    out->len = 0;
    return out->rc == SZ_OK;
}

/* 0 means stop: the length the header promised is reached, or writing
 * failed (out->rc says which). */
static int MlPut( MlOut *out, Byte val )
{
    if ( out->total >= out->limit ) return 0;
    out->buf[out->len++] = val;
    out->total++;
    if ( out->len == sizeof( out->buf ) && !MlFlush( out ) ) return 0;
    return out->total < out->limit;
}

/*---- LZSS: SZDD, and KWAJ method 2 -----------------------------------------
 * A flag byte, then eight items, least significant flag bit first: 1 is a
 * literal byte, 0 is a match of two bytes - a 12-bit ABSOLUTE position in the
 * 4 KB ring and a 4-bit length less three.  The ring starts full of spaces,
 * with its write position 16 bytes short of the end (18 for the QBasic
 * variant), and because match positions are absolute that starting point is
 * part of the format.  The stream simply ends; the header gives the length.
 *--------------------------------------------------------------------------- */
static void MlLzss( MlWork *work, UInt32 startPos )
{
    UInt32 pos = startPos;
    int    flags, bit;

    memset( work->window, ' ', ML_WINDOW );
    for ( ;; )
    {
        flags = MlGet( &work->in );
        if ( flags < 0 ) return;

        for ( bit = 0; bit < 8; bit++ )
        {
            if ( flags & ( 1 << bit ) )
            {
                int val = MlGet( &work->in );

                if ( val < 0 ) return;
                work->window[pos] = (Byte)val;
                pos = ( pos + 1 ) & ML_WMASK;
                if ( !MlPut( &work->out, (Byte)val ) ) return;
            }
            else
            {
                int    lo = MlGet( &work->in );
                int    hi = MlGet( &work->in );
                UInt32 from, len;

                if ( lo < 0 || hi < 0 ) return;
                from = (UInt32)lo | ( (UInt32)( hi & 0xF0 ) << 4 );
                len  = (UInt32)( hi & 0x0F ) + 3;
                while ( len-- )
                {
                    Byte val = work->window[from];

                    from = ( from + 1 ) & ML_WMASK;
                    work->window[pos] = val;
                    pos = ( pos + 1 ) & ML_WMASK;
                    if ( !MlPut( &work->out, val ) ) return;
                }
            }
        }
    }
}

/*---- KWAJ method 3: LZSS + Huffman -----------------------------------------
 * Bits most significant first.  Six 4-bit "how the lengths are sent" types
 * (the sixth only pads to a byte), then five trees:
 *
 *   0  match length after a literal run    16 symbols
 *   1  match length after a match          16
 *   2  literal run length                  32
 *   3  match offset, high six bits         64
 *   4  literal byte                       256
 *
 * Then: a match length from tree 0 or 1 - which one depends on whether the
 * last thing was a literal run that stopped short of 32.  Zero means a run
 * of 1..32 literals follows; anything else is a match of that length plus
 * two, at an offset of tree 3's symbol times 64 plus six raw bits, counted
 * back from the current position.  The ring is 4 KB of spaces to start.
 *
 * There is no end marker.  The data stops, and since a header length is
 * optional the end of the bits is the end of the file: a read that needs
 * bits past the last byte means "finished", not "corrupt".
 *--------------------------------------------------------------------------- */
typedef struct {
    MlIn  *in;
    UInt32 buf;                 /* the next bit is bit 31                  */
    int    cnt;
    int    fake;                /* zero bytes supplied past the end        */
} MlBits;

static UInt32 MlGetBits( MlBits *br, int n )        /* 1 <= n <= 16 */
{
    UInt32 val;

    while ( br->cnt < n )
    {
        int byte = MlGet( br->in );

        if ( byte < 0 )
        {
            byte = 0;
            br->fake++;
        }
        br->buf |= (UInt32)byte << ( 24 - br->cnt );
        br->cnt += 8;
    }
    val = br->buf >> ( 32 - n );
    br->buf <<= n;
    br->cnt -= n;
    return val;
}

/* Has anything read so far come from past the end of the data? */
static int MlBitsEnded( const MlBits *br )
{
    return br->fake * 8 > br->cnt;
}

static int MlBuild( MlHuff *tree, const Byte *lens, int n )
{
    UInt16 offs[17];
    int    len, sym, left = 1;

    memset( tree->count, 0, sizeof( tree->count ) );
    for ( sym = 0; sym < n; sym++ )
        tree->count[lens[sym]]++;
    tree->count[0] = 0;

    for ( len = 1; len <= 16; len++ )
    {
        left <<= 1;
        left -= tree->count[len];
        if ( left < 0 ) return -1;
    }

    offs[1] = 0;
    for ( len = 1; len < 16; len++ )
        offs[len + 1] = (UInt16)( offs[len] + tree->count[len] );
    for ( sym = 0; sym < n; sym++ )
        if ( lens[sym] ) tree->symbol[offs[lens[sym]]++] = (UInt16)sym;
    return 0;
}

/* Canonical codes a bit at a time - KWAJ files are floppy-sized, and this
 * keeps the decoder a page long. */
static int MlDecode( MlBits *br, const MlHuff *tree )
{
    int len, code = 0, first = 0, index = 0, count;

    for ( len = 1; len <= 16; len++ )
    {
        code |= (int)MlGetBits( br, 1 );
        count = tree->count[len];
        if ( code - first < count )
            return tree->symbol[index + code - first];
        index += count;
        first  = ( first + count ) << 1;
        code <<= 1;
    }
    return -1;
}

/* The four ways a tree's lengths are sent:
 *   0  none: every symbol has the same length (4 for 16 symbols ... 8 for 256)
 *   1  4 bits, then per symbol: 0 = same again, 10 = one longer, 11 = 4 new bits
 *   2  4 bits, then 2 bits per symbol: 0-2 add -1/0/+1, 3 = 4 new bits
 *   3  4 bits per symbol */
static int MlReadLens( MlBits *br, int type, int n, Byte *lens )
{
    int i, val, sel;

    switch ( type )
    {
    case 0:
        val = ( n == 16 ) ? 4 : ( n == 32 ) ? 5 : ( n == 64 ) ? 6 : 8;
        for ( i = 0; i < n; i++ ) lens[i] = (Byte)val;
        return SZ_OK;

    case 1:
        val = (int)MlGetBits( br, 4 );
        lens[0] = (Byte)val;
        for ( i = 1; i < n; i++ )
        {
            if ( MlGetBits( br, 1 ) == 0 )
                ;                                   /* the same again */
            else if ( MlGetBits( br, 1 ) == 0 )
                val++;
            else
                val = (int)MlGetBits( br, 4 );
            if ( val > 15 ) return SZ_ERR_DATA;
            lens[i] = (Byte)val;
        }
        return SZ_OK;

    case 2:
        val = (int)MlGetBits( br, 4 );
        lens[0] = (Byte)val;
        for ( i = 1; i < n; i++ )
        {
            sel = (int)MlGetBits( br, 2 );
            if ( sel == 3 ) val = (int)MlGetBits( br, 4 );
            else            val += sel - 1;
            if ( val < 0 || val > 15 ) return SZ_ERR_DATA;
            lens[i] = (Byte)val;
        }
        return SZ_OK;

    case 3:
        for ( i = 0; i < n; i++ )
            lens[i] = (Byte)MlGetBits( br, 4 );
        return SZ_OK;

    default:
        return SZ_ERR_DATA;
    }
}

static int MlLzh( MlWork *work )
{
    static const int counts[5] = { 16, 16, 32, 64, 256 };
    MlBits br;
    Byte   lens[256];
    int    types[6], i, afterRun = 0;
    UInt32 pos = 0;

    br.in   = &work->in;
    br.buf  = 0;
    br.cnt  = 0;
    br.fake = 0;
    memset( work->window, ' ', ML_WINDOW );

    /* An empty file is written as no data at all - not even the trees. */
    if ( work->in.left == 0 ) return SZ_OK;

    for ( i = 0; i < 6; i++ )
        types[i] = (int)MlGetBits( &br, 4 );
    for ( i = 0; i < 5; i++ )
        if ( MlReadLens( &br, types[i], counts[i], lens ) != SZ_OK ||
             MlBuild( &work->trees[i], lens, counts[i] ) )
            return SZ_ERR_DATA;
    if ( MlBitsEnded( &br ) ) return SZ_ERR_DATA;

    for ( ;; )
    {
        int len = MlDecode( &br, &work->trees[afterRun ? 1 : 0] );

        if ( MlBitsEnded( &br ) ) return SZ_OK;
        if ( len < 0 ) return SZ_ERR_DATA;

        if ( len > 0 )
        {
            UInt32 offset, from;
            int    high = MlDecode( &br, &work->trees[3] );

            if ( MlBitsEnded( &br ) ) return SZ_OK;
            if ( high < 0 ) return SZ_ERR_DATA;
            offset = ( (UInt32)high << 6 ) | MlGetBits( &br, 6 );
            if ( MlBitsEnded( &br ) ) return SZ_OK;

            afterRun = 0;
            len += 2;
            from = ( pos - offset ) & ML_WMASK;
            while ( len-- )
            {
                Byte val = work->window[from];

                from = ( from + 1 ) & ML_WMASK;
                work->window[pos] = val;
                pos = ( pos + 1 ) & ML_WMASK;
                if ( !MlPut( &work->out, val ) ) return SZ_OK;
            }
        }
        else
        {
            int run = MlDecode( &br, &work->trees[2] );

            if ( MlBitsEnded( &br ) ) return SZ_OK;
            if ( run < 0 ) return SZ_ERR_DATA;
            run++;
            /* A run of the full 32 may be followed by another run, so the
             * next length comes from the "after a match" tree as usual. */
            afterRun = ( run == 32 ) ? 0 : 1;
            while ( run-- )
            {
                int sym = MlDecode( &br, &work->trees[4] );

                if ( MlBitsEnded( &br ) ) return SZ_OK;
                if ( sym < 0 ) return SZ_ERR_DATA;
                work->window[pos] = (Byte)sym;
                pos = ( pos + 1 ) & ML_WMASK;
                if ( !MlPut( &work->out, (Byte)sym ) ) return SZ_OK;
            }
        }
    }
}

/*---- KWAJ method 4: MSZIP --------------------------------------------------
 * A two-byte length, then an MSZIP block ("CK" and a deflate stream), over
 * and over, until a length of zero.  The length is only ever tested against
 * zero: each block is read up to where its deflate stream ends and the next
 * length starts on the following byte, so this does not depend on what the
 * length was meant to count.
 *--------------------------------------------------------------------------- */
static int MlMszip( MlWork *work )
{
    MszipDec *dec   = NULL;
    Byte     *buf   = NULL;
    Byte     *frame = NULL;
    UInt32    have  = 0, pos = 0;
    int       rc;

    rc = MszipCreate( &dec );
    if ( rc != SZ_OK ) return rc;
    buf   = (Byte *)malloc( ML_ZBUF );
    frame = (Byte *)malloc( MSZIP_BLOCK_MAX );
    if ( !buf || !frame ) rc = SZ_ERR_MEMORY;

    while ( rc == SZ_OK )
    {
        UInt32 got, used, i;

        if ( pos > 0 )
        {
            memmove( buf, buf + pos, have - pos );
            have -= pos;
            pos   = 0;
        }
        have += MlRead( &work->in, buf + have, ML_ZBUF - have );

        if ( have < 2 ) break;                 /* ended without the zero */
        if ( ( buf[0] | ( buf[1] << 8 ) ) == 0 ) break;
        pos = 2;

        rc = MszipDecodeBlock( dec, buf + pos, have - pos, frame,
                               MSZIP_BLOCK_MAX, &got, &used );
        if ( rc != SZ_OK ) break;
        pos += used;

        for ( i = 0; i < got; i++ )
            if ( !MlPut( &work->out, frame[i] ) ) break;
        if ( i < got || work->out.total >= work->out.limit ) break;
    }

    MszipFree( dec );
    if ( buf )   free( buf );
    if ( frame ) free( frame );
    return rc;
}

/*---- Decode the data to a file, or to nowhere ------------------------------*/
static int MlDecodeTo( MslzArchive *arc, FILE *outFp, UInt32 limit,
                       UInt32 *produced )
{
    MlWork *work;
    int     rc = SZ_OK;

    *produced = 0;
    work = (MlWork *)calloc( 1, sizeof( MlWork ) );
    if ( !work ) return SZ_ERR_MEMORY;

    if ( fseek( arc->fp, arc->dataStart, SEEK_SET ) != 0 )
    {
        free( work );
        return SZ_ERR_READ;
    }
    work->in.fp     = arc->fp;
    work->in.left   = arc->dataLen;
    work->out.fp    = outFp;
    work->out.limit = limit;
    work->out.rc    = SZ_OK;

    switch ( arc->method )
    {
    case ML_M_STORE:
    case ML_M_XOR:
    {
        Byte mask = (Byte)( ( arc->method == ML_M_XOR ) ? 0xFF : 0x00 );
        int  val;

        while ( ( val = MlGet( &work->in ) ) >= 0 )
            if ( !MlPut( &work->out, (Byte)( val ^ mask ) ) ) break;
        break;
    }
    case ML_M_LZSS:
        /* SZDD starts the ring 16 bytes from its end; the QBasic-era "SZ"
         * variant and KWAJ's method 2 start it 18 from the end.  The KWAJ
         * figure is not the one published for the format - that says KWAJ
         * method 2 is SZDD's LZSS exactly - but it is what COMPRESS 1.11
         * writes: every file it packed with -a2 decodes right from 18 and
         * goes wrong at its first match from 16. */
        MlLzss( work, ( arc->kind == ML_SZDD ) ? ML_WINDOW - 16 : ML_WINDOW - 18 );
        break;
    case ML_M_LZH:
        rc = MlLzh( work );
        break;
    case ML_M_MSZIP:
        rc = MlMszip( work );
        break;
    default:
        rc = SZ_ERR_UNSUPPORTED;
        break;
    }

    MlFlush( &work->out );
    if ( work->out.rc != SZ_OK ) rc = work->out.rc;
    else if ( rc == SZ_OK && work->in.err ) rc = SZ_ERR_READ;
    *produced = work->out.total;
    free( work );
    return rc;
}

/*---- Names and dates -------------------------------------------------------*/
static const char *MlLeaf( const char *path )
{
    const char *leaf = path;
    const char *scan;

    for ( scan = path; *scan; scan++ )
        if ( *scan == '\\' || *scan == '/' || *scan == ':' ) leaf = scan + 1;
    return leaf;
}

/* COMPRESS -r swaps the last character of the name for '_' and keeps the
 * original in the header.  Put it back in the same case as the name around
 * it: the header has 'E', and a copy of SETUP.EX_ that a CD-ROM driver or a
 * Unix box turned into setup.ex_ should give setup.exe, not setup.exE.
 *
 * With no character kept - ten of the Windows 3.1 files, the mouse driver's
 * among them - the '_' is simply dropped, so README.EX_ becomes README.EX.
 * That is what Microsoft's EXPAND -r does with them, checked against it. */
static void MlSzddName( MslzArchive *arc, int missing )
{
    char *name = arc->entry.name;
    int   len  = (int)strlen( name );

    if ( len < 2 || name[len - 1] != '_' ) return;
    if ( !missing )
    {
        name[len - 1] = '\0';
        return;
    }
    if ( islower( (unsigned char)name[len - 2] ) )
        missing = tolower( missing );
    else if ( isupper( (unsigned char)name[len - 2] ) )
        missing = toupper( missing );
    name[len - 1] = (char)missing;
}

/* The compressed file's own date, which is the only one there is. */
static void MlFileDate( MslzArchive *arc )
{
    struct stat st;
    struct tm  *when;

    if ( stat( arc->path, &st ) != 0 ) return;
    when = localtime( &st.st_mtime );
    if ( !when || when->tm_year < 80 ) return;
    arc->entry.modDate = (UInt16)( ( ( when->tm_year - 80 ) << 9 ) |
                                   ( ( when->tm_mon + 1 ) << 5 ) |
                                   when->tm_mday );
    arc->entry.modTime = (UInt16)( ( when->tm_hour << 11 ) |
                                   ( when->tm_min << 5 ) |
                                   ( when->tm_sec / 2 ) );
}

static UInt32 MlLe16( const Byte *ptr )
{
    return ptr[0] | ( (UInt32)ptr[1] << 8 );
}

static UInt32 MlLe32( const Byte *ptr )
{
    return ptr[0] | ( (UInt32)ptr[1] << 8 ) |
           ( (UInt32)ptr[2] << 16 ) | ( (UInt32)ptr[3] << 24 );
}

/* A NUL-ended field of at most 'max' bytes.  A field that fills all of them
 * has no terminator; that is malformed, but the header's data offset still
 * says where the data is, so the name is kept rather than the file refused. */
static void MlField( FILE *fp, char *dst, int max, int keep )
{
    int i, n = 0;

    for ( i = 0; i < max; i++ )
    {
        int ch = fgetc( fp );

        if ( ch == EOF || ch == 0 ) break;
        if ( n < keep ) dst[n++] = (char)ch;
    }
    dst[n] = '\0';
}

/* The KWAJ header's optional fields, in their fixed order. */
static int MlKwajFields( MslzArchive *arc, UInt32 flags )
{
    FILE *fp = arc->fp;
    Byte  num[4];
    char  base[9], ext[4];

    base[0] = ext[0] = '\0';
    if ( fseek( fp, 14L, SEEK_SET ) != 0 ) return SZ_ERR_READ;

    if ( flags & KW_HAS_LENGTH )
    {
        if ( fread( num, 1, 4, fp ) != 4 ) return SZ_ERR_FORMAT;
        /* COMPRESS 1.11 writes 0xFFFFFFFF here for an EMPTY file.  Taken at
         * its word that is a file of 4 GB that decodes short - "corrupt" -
         * so it is read as no length at all, and the decode finds the 0. */
        if ( MlLe32( num ) != 0xFFFFFFFFUL )
        {
            arc->entry.size = MlLe32( num );
            arc->sizeKnown  = 1;
        }
    }
    if ( flags & KW_HAS_UNK2 )
        if ( fread( num, 1, 2, fp ) != 2 ) return SZ_ERR_FORMAT;
    if ( flags & KW_HAS_DATA )
    {
        if ( fread( num, 1, 2, fp ) != 2 ) return SZ_ERR_FORMAT;
        if ( fseek( fp, (long)MlLe16( num ), SEEK_CUR ) != 0 )
            return SZ_ERR_FORMAT;
    }
    if ( flags & KW_HAS_NAME ) MlField( fp, base, 9, 8 );
    if ( flags & KW_HAS_EXT )  MlField( fp, ext, 4, 3 );
    if ( flags & KW_HAS_TEXT )
    {
        UInt32 len;

        if ( fread( num, 1, 2, fp ) != 2 ) return SZ_ERR_FORMAT;
        len = MlLe16( num );
        if ( len )
        {
            arc->comment = (char *)malloc( len + 1 );
            if ( arc->comment )               /* best effort, like a zip's */
            {
                if ( fread( arc->comment, 1, len, fp ) != len )
                {
                    free( arc->comment );
                    arc->comment = NULL;
                }
                else
                    arc->comment[len] = '\0';
            }
        }
    }

    /* The original name, when the header kept any of it.  A missing half
     * comes from the compressed file's own name. */
    if ( base[0] || ext[0] )
    {
        char        own[SZ_MAX_NAME];
        char       *dot;
        const char *useBase = base;

        lstrcpyn( own, MlLeaf( arc->path ), sizeof( own ) );
        dot = strrchr( own, '.' );
        if ( dot ) *dot = '\0';
        if ( !base[0] ) useBase = own;
        if ( ext[0] )
            wsprintf( arc->entry.name, "%s.%s", useBase, ext );
        else
            lstrcpyn( arc->entry.name, useBase, SZ_MAX_NAME );
    }
    return SZ_OK;
}

/*---- Paths -----------------------------------------------------------------*/
/* Create the folders leading to 'path' (not 'path' itself). */
static void MakeDirs( const char *path )
{
    char  buf[SZ_MAX_NAME * 4];
    char *scan;

    lstrcpyn( buf, path, sizeof( buf ) );
    scan = buf;
    if ( scan[0] && scan[1] == ':' ) scan += 2;
    if ( *scan == '\\' ) scan++;
    for ( ; *scan; scan++ )
        if ( *scan == '\\' )
        {
            *scan = '\0';
            _mkdir( buf );
            *scan = '\\';
        }
}

/* destDir\name, the name made safe for the target (see ArcFsName). */
static void BuildOut( char *dst, int dstSize,
                      const char *destDir, const char *name )
{
    char        fsname[SZ_MAX_NAME];
    const char *src = fsname;
    int         len = 0;

    ArcFsName( fsname, sizeof( fsname ), name, 0 );
    if ( destDir && destDir[0] )
    {
        while ( destDir[len] && len < dstSize - 2 )
        {
            dst[len] = destDir[len];
            len++;
        }
        /* "C:\" and "C:/" already end in one; doubling it makes a path DOS
         * reads as a network name (see BuildPath in SZARC.C). */
        if ( len > 0 && dst[len - 1] != '\\' && dst[len - 1] != '/' )
            dst[len++] = '\\';
    }
    while ( *src && len < dstSize - 1 ) dst[len++] = *src++;
    dst[len] = '\0';
}

/* Would writing outPath write over the compressed file itself?  It can: a
 * file packed without -r keeps its own name, so extracting it into its own
 * folder names the file being read - and an overwrite prompt answered "yes"
 * would truncate it before a byte had been decoded.  The compare is written
 * out because the three compilers spell stricmp differently (as VOLIO.C
 * found before this). */
static int MlSameFile( const char *outPath, const char *arcPath )
{
    char fullOut[SZ_MAX_NAME * 4], fullArc[SZ_MAX_NAME * 4];
    int  i;

    if ( !_fullpath( fullOut, outPath, sizeof( fullOut ) ) ) return 0;
    if ( !_fullpath( fullArc, arcPath, sizeof( fullArc ) ) ) return 0;
    for ( i = 0; fullOut[i] || fullArc[i]; i++ )
        if ( toupper( (unsigned char)fullOut[i] ) !=
             toupper( (unsigned char)fullArc[i] ) )
            return 0;
    return 1;
}

/*---- Public ----------------------------------------------------------------*/
int MslzProbe( const unsigned char *sig, int len )
{
    if ( len < 8 ) return 0;
    return memcmp( sig, SIG_SZDD, 8 ) == 0 ||
           memcmp( sig, SIG_SZQB, 8 ) == 0 ||
           memcmp( sig, SIG_KWAJ, 8 ) == 0;
}

int MslzOpen( const char *path, MslzArchive **out )
{
    MslzArchive *arc;
    Byte         hdr[14];
    size_t       got;
    long         fileLen;
    int          missing = 0;
    int          rc      = SZ_OK;

    *out = NULL;
    arc = (MslzArchive *)calloc( 1, sizeof( MslzArchive ) );
    if ( !arc ) return SZ_ERR_MEMORY;
    lstrcpyn( arc->path, path, sizeof( arc->path ) );

    arc->fp = fopen( path, "rb" );
    if ( !arc->fp ) { MslzClose( arc ); return SZ_ERR_OPEN; }
    if ( fseek( arc->fp, 0L, SEEK_END ) != 0 ||
         ( fileLen = ftell( arc->fp ) ) < 0 ||
         fseek( arc->fp, 0L, SEEK_SET ) != 0 )
    { MslzClose( arc ); return SZ_ERR_READ; }

    got = fread( hdr, 1, sizeof( hdr ), arc->fp );
    lstrcpyn( arc->entry.name, MlLeaf( path ), SZ_MAX_NAME );

    if ( got >= 14 && memcmp( hdr, SIG_SZDD, 8 ) == 0 )
    {
        arc->kind       = ML_SZDD;
        /* 'A' is the only method SZDD ever had.  Anything else still lists,
         * and says "unsupported" when asked for its data. */
        arc->method     = ( hdr[8] == 'A' ) ? ML_M_LZSS : -1;
        missing       = hdr[9];
        arc->entry.size = MlLe32( hdr + 10 );
        arc->sizeKnown  = 1;
        arc->dataStart  = 14;
    }
    else if ( got >= 12 && memcmp( hdr, SIG_SZQB, 8 ) == 0 )
    {
        arc->kind       = ML_SZQB;
        arc->method     = ML_M_LZSS;
        arc->entry.size = MlLe32( hdr + 8 );
        arc->sizeKnown  = 1;
        arc->dataStart  = 12;
    }
    else if ( got >= 14 && memcmp( hdr, SIG_KWAJ, 8 ) == 0 )
    {
        arc->kind      = ML_KWAJ;
        arc->method    = (int)MlLe16( hdr + 8 );
        arc->dataStart = (long)MlLe16( hdr + 10 );
        rc = MlKwajFields( arc, MlLe16( hdr + 12 ) );
        if ( rc == SZ_OK && arc->dataStart < 14 ) rc = SZ_ERR_FORMAT;
    }
    else
        rc = SZ_ERR_SIG;

    if ( rc == SZ_OK && arc->dataStart > fileLen ) rc = SZ_ERR_FORMAT;
    if ( rc != SZ_OK ) { MslzClose( arc ); return rc; }

    arc->dataLen      = (UInt32)( fileLen - arc->dataStart );
    arc->entry.packed = arc->dataLen;
    if ( arc->kind == ML_SZDD ) MlSzddName( arc, missing );
    MlFileDate( arc );
    arc->hasLength = arc->sizeKnown;

    /* A KWAJ header need not say how long the file is.  Stored and XORed
     * data are as long as they are; anything else is decoded once, to
     * nowhere, to find out - these are floppy-era files, so it is quick. */
    if ( !arc->sizeKnown )
    {
        if ( arc->method == ML_M_STORE || arc->method == ML_M_XOR )
        {
            arc->entry.size = arc->dataLen;
            arc->sizeKnown  = 1;
        }
        else if ( arc->method >= 0 && arc->method <= ML_M_LAST )
        {
            UInt32 produced;

            if ( MlDecodeTo( arc, NULL, 0xFFFFFFFFUL, &produced ) == SZ_OK )
            {
                arc->entry.size = produced;
                arc->sizeKnown  = 1;
            }
        }
    }

    *out = arc;
    return SZ_OK;
}

int MslzNumEntries( MslzArchive *arc )
{
    return arc ? 1 : 0;
}

const MslzEntry *MslzGetEntry( MslzArchive *arc, int index )
{
    if ( !arc || index != 0 ) return NULL;
    return &arc->entry;
}

const char *MslzFormatName( MslzArchive *arc )
{
    if ( !arc ) return "?";
    return ( arc->kind == ML_KWAJ ) ? "KWAJ" : "SZDD";
}

const char *MslzMethod( MslzArchive *arc )
{
    if ( !arc ) return "";
    switch ( arc->method )
    {
    case ML_M_STORE: return "Store";
    case ML_M_XOR:   return "XOR";
    case ML_M_LZSS:  return "LZSS";
    case ML_M_LZH:   return "LZH";
    case ML_M_MSZIP: return "MSZIP";
    default:         return "?";
    }
}

const char *MslzComment( MslzArchive *arc )
{
    return ( arc && arc->comment && arc->comment[0] ) ? arc->comment : NULL;
}

int MslzHasLength( MslzArchive *arc )
{
    return arc ? arc->hasLength : 0;
}

static int MslzExtractOne( MslzArchive *arc, const char *destDir )
{
    char   outPath[SZ_MAX_NAME * 4];
    FILE  *outFp = NULL;
    UInt32 produced;
    int    rc;

    if ( arc->method < 0 || arc->method > ML_M_LAST ) return SZ_ERR_UNSUPPORTED;

    /* destDir == NULL means "test only": decode, write nothing. */
    if ( destDir )
    {
        BuildOut( outPath, sizeof( outPath ), destDir, arc->entry.name );
        if ( ArcNameVerdict() == ARC_NAME_ABORT ) return SZ_ERR_CANCEL;
        if ( ArcNameVerdict() == ARC_NAME_SKIP )  return SZ_OK;
        if ( MlSameFile( outPath, arc->path ) )     return SZ_ERR_WRITE;
        if ( !ArcWantWrite( outPath ) )           return SZ_OK;
        MakeDirs( outPath );
        outFp = fopen( outPath, "wb" );
        if ( !outFp ) return SZ_ERR_WRITE;
    }

    rc = MlDecodeTo( arc, outFp,
                     arc->sizeKnown ? arc->entry.size : 0xFFFFFFFFUL, &produced );
    if ( outFp ) fclose( outFp );

    /* No checksum in either format: the length is the only test there is,
     * and a file that decodes short of it was cut off. */
    if ( rc == SZ_OK && arc->sizeKnown && produced != arc->entry.size )
        rc = SZ_ERR_DATA;

    if ( rc == SZ_OK )
    {
        if ( destDir )
            SetFileDosMTime( outPath, arc->entry.modDate, arc->entry.modTime );
    }
    else if ( destDir )
        remove( outPath );            /* don't leave a partial file */
    return rc;
}

int MslzExtractAll( MslzArchive *arc, const char *destDir,
                    SzProgress prog, void *user )
{
    if ( !arc ) return SZ_ERR_FORMAT;
    if ( prog && !prog( user, 0, 1, arc->entry.name ) ) return SZ_ERR_CANCEL;
    return MslzExtractOne( arc, destDir );
}

int MslzExtractItems( MslzArchive *arc, const int *indices, int count,
                      const char *destDir, SzProgress prog, void *user )
{
    int k;

    if ( !arc ) return SZ_ERR_FORMAT;
    for ( k = 0; k < count; k++ )
        if ( indices[k] == 0 )
            return MslzExtractAll( arc, destDir, prog, user );
    return SZ_OK;
}

UInt32 MslzMemNeeded( MslzArchive *arc )
{
    UInt32 need = (UInt32)sizeof( MlWork );

    if ( arc && arc->method == ML_M_MSZIP )
        need += MszipMemNeeded() + ML_ZBUF + MSZIP_BLOCK_MAX;
    return need;
}

void MslzClose( MslzArchive *arc )
{
    if ( !arc ) return;
    if ( arc->fp )      fclose( arc->fp );
    if ( arc->comment ) free( arc->comment );
    free( arc );
}
