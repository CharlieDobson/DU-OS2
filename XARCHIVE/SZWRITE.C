/*===========================================================================
 * SZWRITE.C  -  Writing a solid .7z
 * Target: MSVC 2.2  Win32s
 *
 * The layout is the one 7-Zip writes, checked field by field against an
 * archive it made (7-Zip 26.02, -mhc=off): a 32-byte start header pointing
 * at a header written last, the packed streams between them, and in the
 * header one FOLDER per coded stream.
 *
 * FOUR KINDS OF FOLDER, chosen per file by CompSniff:
 *
 *   LZMA2         everything that compresses.  Solid: the files run into
 *                 one another in a single stream, sorted by extension and
 *                 then by name so that similar files sit together and a
 *                 match can reach back into the last one.  LZMA2 rather
 *                 than plain LZMA because 7-Zip has used it by default
 *                 since 9.x, and because a stretch that will not compress -
 *                 something the sniff missed - goes in as stored chunks
 *                 instead of growing.
 *   BCJ2 + LZMA2  x86 executables.  Each CALL and JMP target is cut out of
 *                 the code into a side stream of its own as an absolute
 *                 address, so the code compresses without them and the
 *                 same call made from two places becomes the same bytes in
 *                 the side stream.  Four coders and four packed streams, in
 *                 7-Zip's own order.  An executables folder past
 *                 SZW_BCJ2_MAX gets the older BCJ filter instead (one
 *                 coder, in place), since BCJ2's side streams are held in
 *                 memory until the main pass is over.
 *   Delta + LZMA2 sampled data - PCM sound, true-colour pictures - where
 *                 each byte goes in as its difference from the byte one
 *                 frame back.  One folder per frame size.
 *   Copy          files that are already compressed.  Squeezing a JPEG
 *                 again costs time and comes out a little bigger.
 *
 * A folder is closed and the next of its kind begun at 1 GB, the largest
 * XArchive's extractor will take (SZ_MAX_UNPACK_SIZE).
 *
 * THE DICTIONARY decides both how well a big solid archive compresses and
 * how much memory anyone will need to extract it.  It is the largest power
 * of two whose ENCODER fits in this machine's measured budget (about 11.5
 * bytes per dictionary byte - LzmaEncMemNeeded), no larger than the folder's
 * data, no larger than 64 MB (7-Zip's own ultra setting), no smaller than
 * 64 KB.  On Win32 and OS/2 the budget is also held under the physical
 * memory free, since a paged match finder runs at the speed of the disk.
 *
 * NAMES are UTF-16 with '/' between folders, as 7-Zip stores them.
 * Attributes and modification times go in for every entry; folders come
 * first, then empty files, then the files with data, in folder order.
 *
 * THE HEADER is itself compressed when that makes it smaller (it always
 * does past a few dozen names) - with plain LZMA, as 7-Zip does its own.
 *===========================================================================*/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arccompi.h"
#include "lzmaenc.h"
#include "platform.h"
#include "crc32.h"

/*---- Property ids (see SZARC.C) ------------------------------------------- */
#define k7zEnd                0x00
#define k7zHeader             0x01
#define k7zMainStreamsInfo    0x04
#define k7zFilesInfo          0x05
#define k7zPackInfo           0x06
#define k7zUnpackInfo         0x07
#define k7zSubStreamsInfo     0x08
#define k7zSize               0x09
#define k7zCRC                0x0A
#define k7zFolder             0x0B
#define k7zCodersUnpackSize   0x0C
#define k7zNumUnpackStream    0x0D
#define k7zEmptyStream        0x0E
#define k7zEmptyFile          0x0F
#define k7zName               0x11
#define k7zMTime              0x14
#define k7zWinAttributes      0x15
#define k7zEncodedHeader      0x17

#define SZW_MAX_FOLDERS       64
#define SZW_DICT_MIN          ( (UInt32)1 << 16 )
#define SZW_DICT_MAX          ( (UInt32)1 << 26 )
#define SZW_BCJ_BUF           65536U
#define SZW_BCJ2_MAX          ( (UInt32)64 << 20 )  /* BCJ2 up to here      */
#define SZW_SIDE_DICT_MAX     ( (UInt32)1 << 20 )   /* CALL/JUMP streams    */

typedef struct {
    int    kind;              /* CK_DATA, CK_EXE, CK_PACKED or CK_DELTA      */
    UInt32 first, count;      /* range of j->order-style indices in 'files'  */
    UInt32 planned;           /* bytes the scan said                         */
    UInt32 packSize;          /* the main packed stream                      */
    UInt32 unpackSize;        /* bytes of file data in the folder            */
    UInt32 dictSize;
    UInt32 numSubs;           /* files that came out with data               */
    int    lzma2;             /* LZMA2 (the data folders) or LZMA (the header) */
    Byte   props[5];          /* LZMA: all five; LZMA2: the first            */
    unsigned delta;           /* CK_DELTA: the frame size                    */

    /* BCJ2: three more packed streams follow the main one, in this order. */
    int    bcj2;
    UInt32 mainLen;           /* the main stream, before LZMA2               */
    UInt32 rcSize;            /* the range-coded bits, written raw           */
    UInt32 callLen, callPack; /* CALL targets, before and after LZMA         */
    UInt32 jumpLen, jumpPack; /* JUMP targets                                */
    Byte   callProps[5];
    Byte   jumpProps[5];
} SzwFolder;

/*===========================================================================
 * A growable byte buffer, for building the header
 *===========================================================================*/
typedef struct {
    Byte  *p;
    UInt32 len, cap;
    int    failed;
} HBuf;

static void HPut( HBuf *h, const void *data, UInt32 n )
{
    if ( h->failed ) return;
    if ( h->len + n > h->cap )
    {
        UInt32 cap = ( h->cap ? h->cap * 2 : 4096 );
        Byte  *np;
        while ( cap < h->len + n ) cap *= 2;
        np = (Byte *)realloc( h->p, cap );
        if ( !np ) { h->failed = 1; return; }
        h->p = np;
        h->cap = cap;
    }
    memcpy( h->p + h->len, data, n );
    h->len += n;
}

static void HByte( HBuf *h, Byte b ) { HPut( h, &b, 1 ); }

static void HUInt32( HBuf *h, UInt32 v )
{
    Byte b[4];
    b[0] = (Byte)v; b[1] = (Byte)( v >> 8 ); b[2] = (Byte)( v >> 16 ); b[3] = (Byte)( v >> 24 );
    HPut( h, b, 4 );
}

/* 7z's variable-length number: the leading 1 bits of the first byte say how
 * many little-endian bytes follow. */
static void HNum( HBuf *h, UInt32 v )
{
    if ( v < 0x80UL )             HByte( h, (Byte)v );
    else if ( v < 0x4000UL )
    {
        HByte( h, (Byte)( 0x80 | ( v >> 8 ) ) );
        HByte( h, (Byte)v );
    }
    else if ( v < 0x200000UL )
    {
        HByte( h, (Byte)( 0xC0 | ( v >> 16 ) ) );
        HByte( h, (Byte)v );
        HByte( h, (Byte)( v >> 8 ) );
    }
    else if ( v < 0x10000000UL )
    {
        HByte( h, (Byte)( 0xE0 | ( v >> 24 ) ) );
        HByte( h, (Byte)v );
        HByte( h, (Byte)( v >> 8 ) );
        HByte( h, (Byte)( v >> 16 ) );
    }
    else
    {
        HByte( h, 0xF0 );
        HUInt32( h, v );
    }
}

/* A bit vector, most significant bit first. */
static void HBits( HBuf *h, const Byte *bits, UInt32 n )
{
    Byte   cur = 0;
    UInt32 i;
    for ( i = 0; i < n; i++ )
    {
        if ( bits[i] ) cur |= (Byte)( 0x80 >> ( i & 7 ) );
        if ( ( i & 7 ) == 7 ) { HByte( h, cur ); cur = 0; }
    }
    if ( n & 7 ) HByte( h, cur );
}

/*===========================================================================
 * BCJ x86, encoding
 *
 * The mirror of SZARC.C's BcjX86Chunk with the sign of the address change
 * reversed - Igor Pavlov's public-domain x86 filter.  Returns how many of the
 * bytes are FINISHED; the last few are kept back until the bytes after them
 * arrive, and when the input ends they go out unconverted, as the decoder
 * expects.
 *===========================================================================*/
#define Test86MSByte( b )  ( ( ( (b) + 1 ) & 0xFE ) == 0 )

typedef struct {
    UInt32 base;
    UInt32 mask;
} BcjEnc;

static UInt32 BcjEncChunk( BcjEnc *b, Byte *data, UInt32 size )
{
    UInt32 pos = 0, mask = b->mask, lim;
    const UInt32 ip = 5;

    if ( size < 5 ) return 0;
    lim = size - 4;
    for ( ;; )
    {
        UInt32 p;
        for ( p = pos; p < lim; p++ )
            if ( ( data[p] & 0xFE ) == 0xE8 ) break;
        {
            UInt32 d = p - pos;
            pos = p;
            if ( p >= lim )
            {
                mask = ( d > 2 ) ? 0 : mask >> (unsigned)d;
                break;
            }
            if ( d > 2 )
                mask = 0;
            else
            {
                mask >>= (unsigned)d;
                if ( mask != 0 && ( mask > 4 || mask == 3 ||
                                    Test86MSByte( data[pos + ( mask >> 1 ) + 1] ) ) )
                {
                    mask = ( mask >> 1 ) | 4;
                    pos++;
                    continue;
                }
            }
        }
        if ( Test86MSByte( data[pos + 4] ) )
        {
            UInt32 v = ( (UInt32)data[pos + 4] << 24 ) | ( (UInt32)data[pos + 3] << 16 ) |
                       ( (UInt32)data[pos + 2] << 8 ) | (UInt32)data[pos + 1];
            UInt32 cur = ip + b->base + pos;
            v += cur;
            if ( mask != 0 )
            {
                unsigned sh = ( mask & 6 ) << 2;
                if ( Test86MSByte( (Byte)( v >> sh ) ) )
                {
                    v ^= ( ( (UInt32)0x100 << sh ) - 1 );
                    v += cur;
                }
                mask = 0;
            }
            data[pos + 1] = (Byte)v;
            data[pos + 2] = (Byte)( v >> 8 );
            data[pos + 3] = (Byte)( v >> 16 );
            data[pos + 4] = (Byte)( 0 - ( ( v >> 24 ) & 1 ) );
            pos += 5;
        }
        else
        {
            mask = ( mask >> 1 ) | 4;
            pos++;
        }
    }
    b->mask = mask;
    b->base += pos;
    return pos;
}

/*===========================================================================
 * BCJ2, encoding
 *
 * The mirror of SZARC.C's BCJ2 stage.  The code goes out as four streams:
 * the bytes with every converted branch target cut out (MAIN, which LZMA2
 * then codes as the folder's stream), the targets of E8 CALLs and of E9 and
 * 0F 8x jumps as absolute big-endian addresses (CALL and JUMP, each
 * LZMA-coded once the main pass is over), and one range-coded bit per
 * branch opcode saying whether its target was cut out (RC, written raw).
 * A target is cut out when the top byte of its displacement is 00 or FF -
 * within 16 MB either way, the BCJ filter's own test, and what 7-Zip's
 * encoder does for a file under 16 MB.
 *
 * Positions are counted from the start of the folder's stream, files run
 * together, exactly as the decoder counts what it emits; an opcode's
 * absolute target is its displacement plus the address just past the
 * operand.  The bit for an opcode in the last four bytes of the stream,
 * which cannot own an operand, is coded as 0, as the decoder expects one
 * bit for every opcode it meets.
 *
 * The range coder is LZMA's, with the 32-bit 'low' and carry flag of
 * LZMAENC.C, since MSVC 2.2 has no 64-bit integer.  Its first byte is the
 * zero every LZMA stream starts with, and the decoder skips it.
 *===========================================================================*/
#define SZW_RC_TOP        ( (UInt32)1 << 24 )
#define SZW_RC_BITS       11
#define SZW_RC_TOTAL      ( 1 << SZW_RC_BITS )
#define SZW_RC_MOVE       5

#define Bcj2IsJ( b0, b1 ) ( ( ( (b1) & 0xFE ) == 0xE8 ) || \
                            ( (b0) == 0x0F && ( (b1) & 0xF0 ) == 0x80 ) )

typedef struct {
    UInt32 low, range;
    UInt32 carry;
    Byte   cache;
    UInt32 cacheSize;
    HBuf  *out;
} Bcj2Rc;

static void Bcj2RcInit( Bcj2Rc *rc, HBuf *out )
{
    rc->low       = 0;
    rc->range     = 0xFFFFFFFFUL;
    rc->carry     = 0;
    rc->cache     = 0;
    rc->cacheSize = 1;
    rc->out       = out;
}

static void Bcj2RcShiftLow( Bcj2Rc *rc )
{
    if ( rc->low < 0xFF000000UL || rc->carry )
    {
        Byte temp = rc->cache;
        do {
            HByte( rc->out, (Byte)( temp + (Byte)rc->carry ) );
            temp = 0xFF;
        } while ( --rc->cacheSize != 0 );
        rc->cache = (Byte)( rc->low >> 24 );
    }
    rc->cacheSize++;
    rc->low = ( rc->low & 0x00FFFFFFUL ) << 8;
    rc->carry = 0;
}

static void Bcj2RcBit( Bcj2Rc *rc, UInt16 *prob, unsigned bit )
{
    UInt32 p     = *prob;
    UInt32 bound = ( rc->range >> SZW_RC_BITS ) * p;

    if ( bit == 0 )
    {
        rc->range = bound;
        p += ( SZW_RC_TOTAL - p ) >> SZW_RC_MOVE;
    }
    else
    {
        UInt32 nl = rc->low + bound;
        if ( nl < rc->low ) rc->carry = 1;
        rc->low = nl;
        rc->range -= bound;
        p -= p >> SZW_RC_MOVE;
    }
    *prob = (UInt16)p;
    if ( rc->range < SZW_RC_TOP )
    {
        rc->range <<= 8;
        Bcj2RcShiftLow( rc );
    }
}

static void Bcj2RcFlush( Bcj2Rc *rc )
{
    int i;
    for ( i = 0; i < 5; i++ ) Bcj2RcShiftLow( rc );
}

typedef struct {
    Byte   *in;               /* raw bytes waiting, SZW_BCJ_BUF              */
    UInt32  inHave, inPos;
    int     inEof;
    UInt32  nowPos;           /* stream offset of in[0]                      */
    Byte    prevByte;
    Byte   *out;              /* main-stream bytes ready, SZW_BCJ_BUF        */
    UInt32  outN, outPos;
    HBuf    call, jump, rcOut;
    Bcj2Rc  rc;
    UInt16  probs[2 + 256];   /* Jcc, E9, then E8 by the byte before it      */
} Bcj2Enc;

static Bcj2Enc *Bcj2EncCreate( void )
{
    Bcj2Enc *b = (Bcj2Enc *)calloc( 1, sizeof( Bcj2Enc ) );
    int      i;

    if ( !b ) return 0;
    b->in  = (Byte *)malloc( SZW_BCJ_BUF );
    b->out = (Byte *)malloc( SZW_BCJ_BUF );
    if ( !b->in || !b->out )
    {
        free( b->in );
        free( b->out );
        free( b );
        return 0;
    }
    for ( i = 0; i < 2 + 256; i++ ) b->probs[i] = SZW_RC_TOTAL >> 1;
    Bcj2RcInit( &b->rc, &b->rcOut );
    return b;
}

static void Bcj2EncFree( Bcj2Enc *b )
{
    if ( !b ) return;
    free( b->in );
    free( b->out );
    free( b->call.p );
    free( b->jump.p );
    free( b->rcOut.p );
    free( b );
}

/* Turn what is waiting in 'in' into main-stream bytes in 'out', stopping at
 * an opcode whose operand has not all arrived yet (unless the input is at
 * its end, when the opcode goes out unconverted with a 0 bit). */
static void Bcj2Process( Bcj2Enc *b )
{
    while ( b->inPos < b->inHave && b->outN < SZW_BCJ_BUF )
    {
        Byte     c = b->in[b->inPos];
        unsigned idx;

        if ( !Bcj2IsJ( b->prevByte, c ) )
        {
            b->out[b->outN++] = c;
            b->prevByte = c;
            b->inPos++;
            continue;
        }
        if ( b->inHave - b->inPos < 5 && !b->inEof ) break;

        idx = ( c == 0xE8 ) ? 2 + (unsigned)b->prevByte : ( c == 0xE9 ) ? 1 : 0;
        b->out[b->outN++] = c;
        if ( b->inHave - b->inPos >= 5 )
        {
            const Byte *p = b->in + b->inPos + 1;
            Byte nextByte = p[3];
            if ( Test86MSByte( nextByte ) )
            {
                UInt32 src  = (UInt32)p[0] | ( (UInt32)p[1] << 8 ) |
                              ( (UInt32)p[2] << 16 ) | ( (UInt32)p[3] << 24 );
                UInt32 dest = b->nowPos + b->inPos + 5 + src;
                HBuf  *s    = ( c == 0xE8 ) ? &b->call : &b->jump;

                Bcj2RcBit( &b->rc, &b->probs[idx], 1 );
                HByte( s, (Byte)( dest >> 24 ) );
                HByte( s, (Byte)( dest >> 16 ) );
                HByte( s, (Byte)( dest >> 8 ) );
                HByte( s, (Byte)dest );
                b->prevByte = nextByte;
                b->inPos += 5;
                continue;
            }
        }
        Bcj2RcBit( &b->rc, &b->probs[idx], 0 );
        b->prevByte = c;
        b->inPos++;
    }
}

/*===========================================================================
 * Reading a folder's files as one stream
 *===========================================================================*/
typedef struct {
    ArcCompJob *j;
    UInt32     *files;        /* entry indices, in folder order              */
    UInt32      count;
    UInt32      next;         /* the next one to open                        */
    UInt32      overall;      /* index of files[0] in the progress count     */
    FILE       *fp;
    CompEntry  *cur;
    UInt32      crc;
    int         readError;

    UInt32      rawIn;        /* file bytes read, whatever a stage made of them */

    /* BCJ stage, when this is a large executables folder */
    int         bcj;
    BcjEnc      bst;
    Byte       *bbuf;
    UInt32      bHave, bDone, bPos;
    int         bEof;

    /* BCJ2 stage, when this is an executables folder up to SZW_BCJ2_MAX */
    Bcj2Enc    *b2;

    /* Delta stage, when this is a sampled-data folder */
    unsigned    delta;        /* the frame size, or 0                        */
    unsigned    dPos;
    Byte        dHist[256];   /* the last frame's bytes, as they were        */
} FolderReader;

static void FinishFile( FolderReader *r )
{
    if ( !r->fp ) return;
    fclose( r->fp );
    r->fp = 0;
    r->cur->crc = Crc32Done( r->crc );
    r->cur->state = CS_DONE;
    r->j->inBytes += r->cur->realSize;
}

/* Raw file bytes, the files one after another. */
static UInt32 RawRead( FolderReader *r, Byte *buf, UInt32 len )
{
    ArcCompJob *j = r->j;

    for ( ;; )
    {
        size_t n;

        if ( !r->fp )
        {
            CompEntry *e;
            if ( r->next >= r->count || r->readError || j->cancelled ) return 0;
            e = &j->ents[ r->files[r->next] ];
            CompProgress( j, (int)( r->overall + r->next ), (int)j->nOrder,
                          CompStoredName( j, e ) );
            r->next++;
            r->fp = CompOpenSource( j, e );
            if ( !r->fp ) continue;              /* skipped, and counted     */
            r->cur = e;
            r->crc = Crc32Init();
            e->realSize = 0;
        }

        n = fread( buf, 1, len, r->fp );
        if ( n > 0 )
        {
            r->crc = Crc32Update( r->crc, buf, (UInt32)n );
            r->cur->realSize += (UInt32)n;
            r->rawIn += (UInt32)n;
            j->doneBytes += (UInt32)n;
            return (UInt32)n;
        }
        if ( ferror( r->fp ) )
        {
            char path[SZ_MAX_NAME * 2];
            CompSourcePath( j, r->cur, path, sizeof( path ) );
            CompSetProblem( j, path );
            r->readError = 1;
            fclose( r->fp );
            r->fp = 0;
            return 0;
        }
        FinishFile( r );
    }
}

/* The raw stream through BCJ2: refill the window, convert, hand out main. */
static UInt32 Bcj2Read( FolderReader *r, Byte *buf, UInt32 len )
{
    Bcj2Enc *b = r->b2;

    for ( ;; )
    {
        if ( b->outPos < b->outN )
        {
            UInt32 n = b->outN - b->outPos;
            if ( n > len ) n = len;
            memcpy( buf, b->out + b->outPos, n );
            b->outPos += n;
            return n;
        }
        b->outN = b->outPos = 0;
        if ( !b->inEof )
        {
            UInt32 tail = b->inHave - b->inPos;
            if ( tail && b->inPos ) memmove( b->in, b->in + b->inPos, tail );
            b->nowPos += b->inPos;
            b->inHave  = tail;
            b->inPos   = 0;
            while ( !b->inEof && b->inHave < SZW_BCJ_BUF )
            {
                UInt32 got = RawRead( r, b->in + b->inHave, SZW_BCJ_BUF - b->inHave );
                if ( got == 0 ) b->inEof = 1;
                b->inHave += got;
            }
        }
        if ( b->inPos == b->inHave ) return 0;
        Bcj2Process( b );
        if ( b->outN == 0 ) return 0;             /* cannot happen: a guard */
    }
}

/* The Delta filter, in place: each byte minus the byte one frame back. */
static void DeltaEncodeRun( FolderReader *r, Byte *data, UInt32 n )
{
    unsigned k = r->dPos, d = r->delta;
    UInt32   i;

    for ( i = 0; i < n; i++ )
    {
        Byte cur = data[i];
        data[i] = (Byte)( cur - r->dHist[k] );
        r->dHist[k] = cur;
        if ( ++k == d ) k = 0;
    }
    r->dPos = k;
}

/* What the encoder reads: the raw stream, through this folder's filter. */
static UInt32 FolderRead( void *user, Byte *buf, UInt32 len )
{
    FolderReader *r = (FolderReader *)user;
    UInt32 n;

    if ( r->delta )
    {
        n = RawRead( r, buf, len );
        DeltaEncodeRun( r, buf, n );
        return n;
    }
    if ( r->b2 )   return Bcj2Read( r, buf, len );
    if ( !r->bcj ) return RawRead( r, buf, len );

    if ( r->bPos == r->bDone )
    {
        UInt32 tail = r->bHave - r->bDone;
        if ( tail ) memmove( r->bbuf, r->bbuf + r->bDone, tail );
        r->bHave = tail;
        r->bDone = r->bPos = 0;
        while ( !r->bEof && r->bHave < SZW_BCJ_BUF )
        {
            UInt32 got = RawRead( r, r->bbuf + r->bHave, SZW_BCJ_BUF - r->bHave );
            if ( got == 0 ) r->bEof = 1;
            r->bHave += got;
        }
        if ( r->bHave == 0 ) return 0;
        r->bDone = BcjEncChunk( &r->bst, r->bbuf, r->bHave );
        if ( r->bEof ) r->bDone = r->bHave;      /* the tail goes as it is */
        if ( r->bDone == 0 ) return 0;
    }
    n = r->bDone - r->bPos;
    if ( n > len ) n = len;
    memcpy( buf, r->bbuf + r->bPos, n );
    r->bPos += n;
    return n;
}

static int FileSink( void *user, const Byte *data, UInt32 len )
{
    FILE *f = (FILE *)user;
    return fwrite( data, 1, len, f ) == len;
}

typedef struct {
    ArcCompJob   *j;
    FolderReader *r;
} EncProg;

static int EncProgress( void *user, UInt32 inDone, UInt32 outDone )
{
    EncProg *p = (EncProg *)user;
    FolderReader *r = p->r;
    (void)inDone; (void)outDone;
    return CompProgress( p->j, (int)( r->overall + ( r->next ? r->next - 1 : 0 ) ),
                         (int)p->j->nOrder,
                         r->cur ? CompStoredName( p->j, r->cur ) : "" );
}

/*===========================================================================
 * Choosing the dictionary
 *===========================================================================*/
/* Asked afresh each time: in a GUI that stays up all day, the physical
 * memory free at the second archive is not what it was at the first. */
static UInt32 DictCap( void )
{
    UInt32 budget, phys, d;

    budget = ArcMaxDictSize();
    phys = PlatPhysFree();
    if ( phys )
    {
        phys -= phys / 4;
        if ( phys < budget ) budget = phys;
    }
    for ( d = SZW_DICT_MAX; d > SZW_DICT_MIN; d >>= 1 )
        if ( LzmaEncMemNeeded( d ) <= budget ) break;
    return d;
}

static UInt32 DictFor( UInt32 bytes )
{
    UInt32 d = SZW_DICT_MIN, cap = DictCap();
    while ( d < bytes && d < cap ) d <<= 1;
    return d;
}

/*===========================================================================
 * Planning: which file goes in which folder, in what order
 *===========================================================================*/
static ArcCompJob *g_sj;

static const char *ExtOf( const char *rel )
{
    const char *p, *leaf = rel, *dot = 0;
    for ( p = rel; *p; p++ )
    {
        if ( *p == '\\' || *p == '/' ) { leaf = p + 1; dot = 0; }
        else if ( *p == '.' ) dot = p;
    }
    (void)leaf;
    return dot ? dot + 1 : "";
}

static int StrICmp( const char *a, const char *b )
{
    for ( ;; )
    {
        int ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if ( ca >= 'A' && ca <= 'Z' ) ca += 32;
        if ( cb >= 'A' && cb <= 'Z' ) cb += 32;
        if ( ca != cb ) return ca - cb;
        if ( !ca ) return 0;
    }
}

static int StreamCmp( const void *a, const void *b )
{
    const CompEntry *x = &g_sj->ents[ *(const UInt32 *)a ];
    const CompEntry *y = &g_sj->ents[ *(const UInt32 *)b ];
    int c;
    if ( x->kind != y->kind ) return (int)x->kind - (int)y->kind;
    if ( x->kind == CK_DELTA && x->delta != y->delta )
        return (int)x->delta - (int)y->delta;
    c = StrICmp( ExtOf( x->rel ), ExtOf( y->rel ) );
    if ( c ) return c;
    c = StrICmp( CompStoredName( g_sj, x ), CompStoredName( g_sj, y ) );
    if ( c ) return c;
    return ( *(const UInt32 *)a < *(const UInt32 *)b ) ? -1 : 1;
}

/*===========================================================================
 * The header
 *===========================================================================*/
static void WriteCoder( HBuf *h, const SzwFolder *fo )
{
    if ( fo->kind == CK_PACKED )
    {
        HNum( h, 1 );                     /* one coder: Copy */
        HByte( h, 0x01 );
        HByte( h, 0x00 );
        return;
    }
    if ( fo->bcj2 )
    {
        /* 7-Zip's own order: the JUMP and CALL coders, the main LZMA2, then
         * BCJ2 with its four inputs.  In-streams count up coder by coder
         * (0, 1, 2, then BCJ2's 3..6: main, call, jump, rc), out-streams
         * likewise (0..3); the bind pairs and the packed-stream list say
         * what feeds what, and the four packed streams follow one another
         * in the archive in the order listed. */
        HNum( h, 4 );
        HByte( h, 0x23 );                 /* LZMA, 3-byte id, has properties */
        HByte( h, 0x03 ); HByte( h, 0x01 ); HByte( h, 0x01 );
        HNum( h, 5 );
        HPut( h, fo->jumpProps, 5 );
        HByte( h, 0x23 );
        HByte( h, 0x03 ); HByte( h, 0x01 ); HByte( h, 0x01 );
        HNum( h, 5 );
        HPut( h, fo->callProps, 5 );
        HByte( h, 0x21 );                 /* LZMA2 */
        HByte( h, 0x21 );
        HNum( h, 1 );
        HByte( h, fo->props[0] );
        HByte( h, 0x14 );                 /* BCJ2: 4-byte id, complex        */
        HByte( h, 0x03 ); HByte( h, 0x03 ); HByte( h, 0x01 ); HByte( h, 0x1B );
        HNum( h, 4 );                     /* four in, one out                */
        HNum( h, 1 );
        HNum( h, 5 ); HNum( h, 0 );       /* BCJ2's jump input <- coder 0    */
        HNum( h, 4 ); HNum( h, 1 );       /* BCJ2's call input <- coder 1    */
        HNum( h, 3 ); HNum( h, 2 );       /* BCJ2's main input <- LZMA2      */
        HNum( h, 2 ); HNum( h, 6 );       /* packed: main, the raw rc bits,  */
        HNum( h, 1 ); HNum( h, 0 );       /*         call, jump              */
        return;
    }

    HNum( h, ( fo->kind == CK_EXE || fo->kind == CK_DELTA ) ? 2 : 1 );
    if ( fo->lzma2 )
    {
        HByte( h, 0x21 );                 /* 1-byte id, has properties */
        HByte( h, 0x21 );
        HNum( h, 1 );
        HByte( h, fo->props[0] );
    }
    else
    {
        HByte( h, 0x23 );                 /* 3-byte id, has properties */
        HByte( h, 0x03 ); HByte( h, 0x01 ); HByte( h, 0x01 );
        HNum( h, 5 );
        HPut( h, fo->props, 5 );
    }
    if ( fo->kind == CK_EXE )
    {
        HByte( h, 0x04 );                 /* 4-byte id, simple, no properties */
        HByte( h, 0x03 ); HByte( h, 0x03 ); HByte( h, 0x01 ); HByte( h, 0x03 );
        HNum( h, 1 );                     /* bind: BCJ's input ...           */
        HNum( h, 0 );                     /*     ... is LZMA2's output       */
    }
    else if ( fo->kind == CK_DELTA )
    {
        HByte( h, 0x21 );                 /* 1-byte id, has properties       */
        HByte( h, 0x03 );
        HNum( h, 1 );
        HByte( h, (Byte)( fo->delta - 1 ) );
        HNum( h, 1 );                     /* bind: Delta's input ...         */
        HNum( h, 0 );                     /*     ... is LZMA2's output       */
    }
}

/* Packed streams a folder puts in the archive. */
static UInt32 FolderPacks( const SzwFolder *fo ) { return fo->bcj2 ? 4 : 1; }

static void WriteStreamsInfo( HBuf *h, UInt32 packPos, SzwFolder *fo, int nFo,
                              int withSubs, ArcCompJob *j, UInt32 *files,
                              const UInt32 *folderCrc )
{
    int i;
    UInt32 k, nPack = 0;

    for ( i = 0; i < nFo; i++ ) nPack += FolderPacks( &fo[i] );
    HByte( h, k7zPackInfo );
    HNum( h, packPos );
    HNum( h, nPack );
    HByte( h, k7zSize );
    for ( i = 0; i < nFo; i++ )
    {
        HNum( h, fo[i].packSize );
        if ( fo[i].bcj2 )
        {
            HNum( h, fo[i].rcSize );
            HNum( h, fo[i].callPack );
            HNum( h, fo[i].jumpPack );
        }
    }
    HByte( h, k7zEnd );

    HByte( h, k7zUnpackInfo );
    HByte( h, k7zFolder );
    HNum( h, (UInt32)nFo );
    HByte( h, 0 );                        /* not external */
    for ( i = 0; i < nFo; i++ ) WriteCoder( h, &fo[i] );
    HByte( h, k7zCodersUnpackSize );
    for ( i = 0; i < nFo; i++ )
    {
        /* One size per coder output, in coder order. */
        if ( fo[i].bcj2 )
        {
            HNum( h, fo[i].jumpLen );
            HNum( h, fo[i].callLen );
            HNum( h, fo[i].mainLen );
            HNum( h, fo[i].unpackSize );  /* BCJ2's: the code as it was */
            continue;
        }
        HNum( h, fo[i].unpackSize );      /* LZMA2's (or Copy's) output */
        if ( fo[i].kind == CK_EXE || fo[i].kind == CK_DELTA )
            HNum( h, fo[i].unpackSize );  /* the filter's: the same size */
    }
    if ( folderCrc )
    {
        HByte( h, k7zCRC );
        HByte( h, 1 );
        for ( i = 0; i < nFo; i++ ) HUInt32( h, folderCrc[i] );
    }
    HByte( h, k7zEnd );

    if ( withSubs )
    {
        int anyMulti = 0;
        HByte( h, k7zSubStreamsInfo );
        for ( i = 0; i < nFo; i++ ) if ( fo[i].numSubs != 1 ) anyMulti = 1;
        if ( anyMulti )
        {
            HByte( h, k7zNumUnpackStream );
            for ( i = 0; i < nFo; i++ ) HNum( h, fo[i].numSubs );
            HByte( h, k7zSize );
            for ( i = 0; i < nFo; i++ )
            {
                UInt32 left = fo[i].numSubs;
                for ( k = fo[i].first; k < fo[i].first + fo[i].count && left > 1; k++ )
                {
                    CompEntry *e = &j->ents[ files[k] ];
                    if ( e->state != CS_DONE || e->realSize == 0 ) continue;
                    HNum( h, e->realSize );
                    left--;
                }
            }
        }
        HByte( h, k7zCRC );
        HByte( h, 1 );                    /* every one defined */
        for ( i = 0; i < nFo; i++ )
            for ( k = fo[i].first; k < fo[i].first + fo[i].count; k++ )
            {
                CompEntry *e = &j->ents[ files[k] ];
                if ( e->state == CS_DONE && e->realSize > 0 ) HUInt32( h, e->crc );
            }
        HByte( h, k7zEnd );
    }
    HByte( h, k7zEnd );
}

/* The name as UTF-16, '/' between folders. */
static void PutName( HBuf *h, const char *name )
{
    WCHAR w[SZ_MAX_NAME + 1];
    int   n, i;

    n = MultiByteToWideChar( CP_ACP, 0, name, -1, w, SZ_MAX_NAME + 1 );
    if ( n <= 0 ) { w[0] = 0; n = 1; }
    w[SZ_MAX_NAME] = 0;
    for ( i = 0; i < n && w[i]; i++ )
    {
        WCHAR c = ( w[i] == '\\' ) ? (WCHAR)'/' : w[i];
        HByte( h, (Byte)c );
        HByte( h, (Byte)( c >> 8 ) );
    }
    HByte( h, 0 );
    HByte( h, 0 );
}

static UInt32 NameBytes( const char *name )
{
    WCHAR w[SZ_MAX_NAME + 1];
    int   n = MultiByteToWideChar( CP_ACP, 0, name, -1, w, SZ_MAX_NAME + 1 );
    if ( n <= 0 ) n = 1;
    return (UInt32)n * 2;
}

/* Compress the header itself.  Returns the packed bytes in a malloc'd
 * buffer, or NULL when that would not save anything (or memory ran out -
 * either way the plain header is written). */
typedef struct { const Byte *p; UInt32 len, pos; } MemIn;

static UInt32 MemRead( void *user, Byte *buf, UInt32 len )
{
    MemIn *m = (MemIn *)user;
    UInt32 n = m->len - m->pos;
    if ( n > len ) n = len;
    if ( n ) memcpy( buf, m->p + m->pos, n );
    m->pos += n;
    return n;
}

/* A BCJ2 side stream (CALL or JUMP targets) as plain LZMA, straight into the
 * archive after the main stream: lc 0, lp 2, pb 2 for 4-byte big-endian
 * addresses, as 7-Zip codes them, and a dictionary no bigger than the
 * stream or 1 MB. */
static int WriteSideStream( FILE *f, const Byte *p, UInt32 len, Byte props[5], UInt32 *packed )
{
    LzmaEncProps lp;
    MemIn        m;
    UInt32       inT = 0, outT = 0, d = SZW_DICT_MIN, cap = DictCap();
    int          rc;

    while ( d < len && d < SZW_SIDE_DICT_MAX ) d <<= 1;
    if ( d > cap ) d = cap;
    LzmaEncPropsInit( &lp, d );
    lp.lc = 0;
    lp.lp = 2;
    lp.pb = 2;
    LzmaEncPropsBytes( &lp, props );
    m.p = p; m.len = len; m.pos = 0;
    rc = LzmaEncode( &lp, MemRead, &m, FileSink, f, 0, 0, &inT, &outT );
    *packed = outT;
    return rc;
}

/* The main pass is over: the range-coder bits go out raw, then the two
 * target streams through LZMA.  Fills in the folder's sizes and props. */
static int Bcj2Finish( Bcj2Enc *b, FILE *f, SzwFolder *fo )
{
    int rc;

    Bcj2RcFlush( &b->rc );
    if ( b->call.failed || b->jump.failed || b->rcOut.failed ) return SZ_ERR_MEMORY;
    if ( fwrite( b->rcOut.p, 1, b->rcOut.len, f ) != b->rcOut.len ) return SZ_ERR_WRITE;
    fo->rcSize  = b->rcOut.len;
    fo->callLen = b->call.len;
    fo->jumpLen = b->jump.len;
    rc = WriteSideStream( f, b->call.p, b->call.len, fo->callProps, &fo->callPack );
    if ( rc == SZ_OK )
        rc = WriteSideStream( f, b->jump.p, b->jump.len, fo->jumpProps, &fo->jumpPack );
    return rc;
}

static int MemWrite( void *user, const Byte *data, UInt32 len )
{
    HBuf *h = (HBuf *)user;
    HPut( h, data, len );
    return !h->failed;
}

/*===========================================================================
 * The writer
 *===========================================================================*/
int SzWriteArchive( ArcCompJob *j, FILE *f )
{
    SzwFolder  fo[SZW_MAX_FOLDERS];
    int        nFo = 0, rc = SZ_OK, i;
    UInt32    *files = 0, nFiles = 0, k;
    UInt32    *dirs = 0, nDirs = 0;
    UInt32     nEmpty = 0, nStream = 0, nAll;
    HBuf       h;
    Byte       start[32];
    long       hdrPos;
    Byte      *sniff = 0;

    memset( &h, 0, sizeof( h ) );
    memset( fo, 0, sizeof( fo ) );
    j->dictSize = 0;

    files = (UInt32 *)malloc( ( j->nOrder ? j->nOrder : 1 ) * sizeof( UInt32 ) );
    dirs  = (UInt32 *)malloc( ( j->nOrder ? j->nOrder : 1 ) * sizeof( UInt32 ) );
    sniff = (Byte *)malloc( 16384 );
    if ( !files || !dirs || !sniff ) { rc = SZ_ERR_MEMORY; goto done; }

    /*---- Look at each file, and sort them into folders ----------------- */
    for ( k = 0; k < j->nOrder; k++ )
    {
        CompEntry *e = &j->ents[ j->order[k] ];
        if ( e->isDir ) { dirs[nDirs++] = j->order[k]; continue; }
        if ( e->size == 0 ) { e->kind = CK_DATA; files[nFiles++] = j->order[k]; continue; }
        if ( !CompProgress( j, (int)k, -1, CompStoredName( j, e ) ) )
        {
            rc = SZ_ERR_CANCEL;
            goto done;
        }
        {
            char  path[SZ_MAX_NAME * 2];
            FILE *s;
            int   isText = 0;
            CompSourcePath( j, e, path, sizeof( path ) );
            s = fopen( path, "rb" );
            if ( s )
            {
                size_t   got = fread( sniff, 1, 16384, s );
                unsigned frame = 0;
                e->kind  = (Byte)CompSniff( sniff, (UInt32)got, e->size, &isText, &frame );
                e->delta = (Byte)frame;
                fclose( s );
            }
            else
                e->kind = CK_DATA;      /* found again, and counted, below */
        }
        files[nFiles++] = j->order[k];
    }

    g_sj = j;
    qsort( files, nFiles, sizeof( UInt32 ), StreamCmp );

    /* Empty files lead (they need no folder); the rest are grouped by kind
     * (and, for sampled data, by frame size), a new folder at each change
     * and at the 1 GB mark. */
    {
        UInt32 a = 0, b;
        UInt32 *tmp = (UInt32 *)malloc( ( nFiles ? nFiles : 1 ) * sizeof( UInt32 ) );
        if ( !tmp ) { rc = SZ_ERR_MEMORY; goto done; }
        for ( b = 0; b < nFiles; b++ )
            if ( j->ents[ files[b] ].size == 0 ) tmp[a++] = files[b];
        nEmpty = a;
        for ( b = 0; b < nFiles; b++ )
            if ( j->ents[ files[b] ].size != 0 ) tmp[a++] = files[b];
        memcpy( files, tmp, nFiles * sizeof( UInt32 ) );
        free( tmp );
    }
    for ( k = nEmpty; k < nFiles; k++ )
    {
        CompEntry *e = &j->ents[ files[k] ];
        SzwFolder *cur = nFo ? &fo[nFo - 1] : 0;
        if ( !cur || cur->kind != e->kind ||
             ( e->kind == CK_DELTA && cur->delta != (unsigned)e->delta ) ||
             ( cur->count && cur->planned + e->size > SZ_MAX_UNPACK_SIZE ) )
        {
            if ( nFo == SZW_MAX_FOLDERS ) { rc = SZ_ERR_TOOBIG; goto done; }
            cur = &fo[nFo++];
            cur->kind  = e->kind;
            cur->delta = e->delta;
            cur->first = k;
        }
        cur->count++;
        cur->planned += e->size;
        e->folder = (Byte)( nFo - 1 );
    }
    nStream = nFiles - nEmpty;

    /*---- The packed streams -------------------------------------------- */
    memset( start, 0, sizeof( start ) );
    if ( fwrite( start, 1, 32, f ) != 32 ) { rc = SZ_ERR_WRITE; goto done; }

    for ( i = 0; i < nFo && rc == SZ_OK; i++ )
    {
        FolderReader r;
        long         startPos = ftell( f );
        UInt32       inT = 0, outT = 0;

        memset( &r, 0, sizeof( r ) );
        r.j       = j;
        r.files   = files + fo[i].first;
        r.count   = fo[i].count;
        r.overall = nDirs + fo[i].first;

        if ( fo[i].kind == CK_PACKED )
        {
            Byte *buf = (Byte *)malloc( COMP_IOBUF );
            UInt32 n;
            if ( !buf ) { rc = SZ_ERR_MEMORY; break; }
            while ( ( n = RawRead( &r, buf, COMP_IOBUF ) ) > 0 )
            {
                if ( fwrite( buf, 1, n, f ) != n ) { rc = SZ_ERR_WRITE; break; }
                inT += n;
                outT += n;
                if ( !CompProgress( j, (int)( r.overall + r.next - 1 ), (int)j->nOrder,
                                    r.cur ? CompStoredName( j, r.cur ) : "" ) )
                {
                    rc = SZ_ERR_CANCEL;
                    break;
                }
            }
            free( buf );
        }
        else
        {
            LzmaEncProps p;
            EncProg      ep;

            fo[i].dictSize = DictFor( fo[i].planned );
            if ( fo[i].dictSize > j->dictSize ) j->dictSize = fo[i].dictSize;
            LzmaEncPropsInit( &p, fo[i].dictSize );
            fo[i].lzma2    = 1;
            fo[i].props[0] = Lzma2EncPropByte( &p );
            fo[i].bcj2     = ( fo[i].kind == CK_EXE && fo[i].planned <= SZW_BCJ2_MAX );
            if ( fo[i].bcj2 )
            {
                r.b2 = Bcj2EncCreate();
                if ( !r.b2 ) { rc = SZ_ERR_MEMORY; break; }
            }
            else if ( fo[i].kind == CK_EXE )
            {
                r.bcj  = 1;
                r.bbuf = (Byte *)malloc( SZW_BCJ_BUF );
                if ( !r.bbuf ) { rc = SZ_ERR_MEMORY; break; }
            }
            else if ( fo[i].kind == CK_DELTA )
                r.delta = fo[i].delta;            /* dHist and dPos are 0 */
            ep.j = j;
            ep.r = &r;
            rc = Lzma2Encode( &p, FolderRead, &r, FileSink, f, EncProgress, &ep, &inT, &outT );
            free( r.bbuf );
            if ( r.b2 )
            {
                /* The main stream is down; now the three cut out of it.
                 * Nothing read means nothing to follow: the folder goes. */
                if ( rc == SZ_OK && !j->cancelled && !r.readError && r.rawIn > 0 )
                    rc = Bcj2Finish( r.b2, f, &fo[i] );
                Bcj2EncFree( r.b2 );
            }
        }
        if ( r.fp ) { fclose( r.fp ); r.fp = 0; }
        if ( rc == SZ_OK && j->cancelled ) rc = SZ_ERR_CANCEL;
        if ( rc == SZ_OK && r.readError ) rc = SZ_ERR_READ;
        if ( rc != SZ_OK ) break;

        fo[i].packSize   = outT;
        fo[i].mainLen    = inT;               /* BCJ2: less than went in */
        fo[i].unpackSize = r.rawIn;
        fo[i].numSubs = 0;
        for ( k = fo[i].first; k < fo[i].first + fo[i].count; k++ )
        {
            CompEntry *e = &j->ents[ files[k] ];
            if ( e->state == CS_DONE && e->realSize > 0 ) fo[i].numSubs++;
        }
        if ( fo[i].unpackSize == 0 )
        {
            /* Everything in it vanished or was empty by the time it was
             * read: take the folder out, and its few bytes with it. */
            fseek( f, startPos, SEEK_SET );
            fo[i].packSize = 0;
            fo[i].numSubs = 0;
        }
    }
    if ( rc != SZ_OK ) goto done;

    /* Drop folders that came out empty, keeping the order of the rest. */
    {
        int w = 0;
        for ( i = 0; i < nFo; i++ )
            if ( fo[i].unpackSize > 0 ) fo[w++] = fo[i];
        nFo = w;
    }

    /* Empty files never went through a folder: they are done as they are. */
    for ( k = 0; k < nEmpty; k++ )
    {
        CompEntry *e = &j->ents[ files[k] ];
        FILE *s = CompOpenSource( j, e );
        if ( s ) { fclose( s ); e->state = CS_DONE; e->realSize = 0; e->crc = 0; }
    }

    /*---- The header ---------------------------------------------------- */
    nAll = 0;
    for ( k = 0; k < nDirs; k++ )  j->ents[ dirs[k] ].state = CS_DONE;
    {
        /* Entry order: folders, then everything without data, then the
         * files with data in folder order - the order their substreams
         * are in, which is what ties a file to its bytes. */
        UInt32 *list = (UInt32 *)malloc( ( nDirs + nFiles + 1 ) * sizeof( UInt32 ) );
        Byte   *bits = 0;
        UInt32  nEmptyStream = 0, nEmptyFile = 0, n = 0, namesLen = 1;

        if ( !list ) { rc = SZ_ERR_MEMORY; goto done; }
        for ( k = 0; k < nDirs; k++ ) list[n++] = dirs[k];
        for ( k = 0; k < nFiles; k++ )
        {
            CompEntry *e = &j->ents[ files[k] ];
            if ( e->state == CS_DONE && e->realSize == 0 ) list[n++] = files[k];
        }
        nEmptyStream = n;
        nEmptyFile = n - nDirs;
        for ( k = 0; k < nFiles; k++ )
        {
            CompEntry *e = &j->ents[ files[k] ];
            if ( e->state == CS_DONE && e->realSize > 0 ) list[n++] = files[k];
        }
        nAll = n;
        j->nFiles = (int)( nAll - nDirs );

        HByte( &h, k7zHeader );
        if ( nFo > 0 )
        {
            HByte( &h, k7zMainStreamsInfo );
            WriteStreamsInfo( &h, 0, fo, nFo, 1, j, files, 0 );
        }

        HByte( &h, k7zFilesInfo );
        HNum( &h, nAll );

        bits = (Byte *)calloc( nAll + 1, 1 );
        if ( !bits ) { free( list ); rc = SZ_ERR_MEMORY; goto done; }
        if ( nEmptyStream > 0 )
        {
            for ( k = 0; k < nAll; k++ ) bits[k] = (Byte)( k < nEmptyStream );
            HByte( &h, k7zEmptyStream );
            HNum( &h, ( nAll + 7 ) / 8 );
            HBits( &h, bits, nAll );
            if ( nEmptyFile > 0 )
            {
                for ( k = 0; k < nEmptyStream; k++ ) bits[k] = (Byte)( k >= nDirs );
                HByte( &h, k7zEmptyFile );
                HNum( &h, ( nEmptyStream + 7 ) / 8 );
                HBits( &h, bits, nEmptyStream );
            }
        }
        free( bits );

        for ( k = 0; k < nAll; k++ )
            namesLen += NameBytes( CompStoredName( j, &j->ents[ list[k] ] ) );
        HByte( &h, k7zName );
        HNum( &h, namesLen );
        HByte( &h, 0 );
        for ( k = 0; k < nAll; k++ )
            PutName( &h, CompStoredName( j, &j->ents[ list[k] ] ) );

        HByte( &h, k7zMTime );
        HNum( &h, 2 + 8 * nAll );
        HByte( &h, 1 );
        HByte( &h, 0 );
        for ( k = 0; k < nAll; k++ )
        {
            HUInt32( &h, j->ents[ list[k] ].mtLo );
            HUInt32( &h, j->ents[ list[k] ].mtHi );
        }

        HByte( &h, k7zWinAttributes );
        HNum( &h, 2 + 4 * nAll );
        HByte( &h, 1 );
        HByte( &h, 0 );
        for ( k = 0; k < nAll; k++ )
            HUInt32( &h, j->ents[ list[k] ].attr & 0xFF );

        HByte( &h, k7zEnd );
        HByte( &h, k7zEnd );
        free( list );
    }
    if ( h.failed ) { rc = SZ_ERR_MEMORY; goto done; }

    /* Pack the header when that saves anything: 7-Zip's default, and a
     * big saving for an archive of many small files (UTF-16 names). */
    hdrPos = ftell( f );
    {
        HBuf  packed, shdr;
        Byte *hdr = h.p;
        UInt32 hdrLen = h.len, hdrCrc = Crc32Calc( h.p, h.len );

        memset( &packed, 0, sizeof( packed ) );
        memset( &shdr, 0, sizeof( shdr ) );
        if ( hdrLen > 128 )
        {
            LzmaEncProps p;
            MemIn        m;
            SzwFolder    hf;
            UInt32       inT, outT;

            memset( &hf, 0, sizeof( hf ) );
            m.p = h.p; m.len = h.len; m.pos = 0;
            hf.dictSize = DictFor( hdrLen );
            LzmaEncPropsInit( &p, hf.dictSize );
            LzmaEncPropsBytes( &p, hf.props );
            if ( LzmaEncode( &p, MemRead, &m, MemWrite, &packed, 0, 0, &inT, &outT ) == SZ_OK &&
                 !packed.failed && outT + 32 < hdrLen )
            {
                hf.kind = CK_DATA;
                hf.packSize = outT;
                hf.unpackSize = hdrLen;
                HByte( &shdr, k7zEncodedHeader );
                WriteStreamsInfo( &shdr, (UInt32)( hdrPos - 32 ), &hf, 1, 0, j, files, &hdrCrc );
                if ( !shdr.failed &&
                     fwrite( packed.p, 1, packed.len, f ) == packed.len )
                {
                    hdrPos = ftell( f );
                    hdr = shdr.p;
                    hdrLen = shdr.len;
                }
                else
                    rc = SZ_ERR_WRITE;
            }
        }
        if ( rc == SZ_OK && fwrite( hdr, 1, hdrLen, f ) != hdrLen ) rc = SZ_ERR_WRITE;

        if ( rc == SZ_OK )
        {
            UInt32 nextOff = (UInt32)( hdrPos - 32 );
            UInt32 nextCrc = Crc32Calc( hdr, hdrLen );
            start[0] = 0x37; start[1] = 0x7A; start[2] = 0xBC;
            start[3] = 0xAF; start[4] = 0x27; start[5] = 0x1C;
            start[6] = 0;    start[7] = 4;
            start[12] = (Byte)nextOff;          start[13] = (Byte)( nextOff >> 8 );
            start[14] = (Byte)( nextOff >> 16 ); start[15] = (Byte)( nextOff >> 24 );
            memset( start + 16, 0, 4 );
            start[20] = (Byte)hdrLen;          start[21] = (Byte)( hdrLen >> 8 );
            start[22] = (Byte)( hdrLen >> 16 ); start[23] = (Byte)( hdrLen >> 24 );
            memset( start + 24, 0, 4 );
            start[28] = (Byte)nextCrc;          start[29] = (Byte)( nextCrc >> 8 );
            start[30] = (Byte)( nextCrc >> 16 ); start[31] = (Byte)( nextCrc >> 24 );
            {
                UInt32 sc = Crc32Calc( start + 12, 20 );
                start[8]  = (Byte)sc;          start[9]  = (Byte)( sc >> 8 );
                start[10] = (Byte)( sc >> 16 ); start[11] = (Byte)( sc >> 24 );
            }
            j->outBytes = (UInt32)ftell( f );
            if ( fseek( f, 0, SEEK_SET ) != 0 || fwrite( start, 1, 32, f ) != 32 ||
                 fflush( f ) != 0 )
                rc = SZ_ERR_WRITE;
        }
        free( packed.p );
        free( shdr.p );
    }

done:
    free( h.p );
    free( files );
    free( dirs );
    free( sniff );
    (void)nStream;
    return rc;
}
