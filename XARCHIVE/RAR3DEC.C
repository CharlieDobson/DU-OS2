/*===========================================================================
 * RAR3DEC.C  -  RAR2 (unpack v20) and RAR3 (unpack v29/36) LZ decoder
 * Target: MSVC 2.2  Win32s
 *
 * The decompression logic here is adapted from the UnRAR source code by
 * Alexander L. Roshal (files unpack20.cpp, unpack30.cpp and unpack.cpp - the
 * functions Unpack20, Unpack29, ReadTables20, ReadTables30, ReadEndOfBlock,
 * MakeDecodeTables and DecodeNumber, together with their decode tables).
 *
 *   The UnRAR sources may be used in any software to handle RAR archives
 *   without limitations free of charge, but cannot be used to re-create the
 *   RAR compression algorithm, which is proprietary.  Distribution of modified
 *   UnRAR sources in separate form or as a part of other software is
 *   permitted, provided that it is clearly stated in the documentation and
 *   source comments that the code may not be used to develop a RAR (WinRAR)
 *   compatible archiver.
 *
 * >>> THIS CODE MAY NOT BE USED TO DEVELOP A RAR (WinRAR) COMPATIBLE ARCHIVER.
 *
 * Only the LZ path is implemented.  PPMd (model H) blocks and the RAR3 filter
 * virtual machine (symbol 257) are intentionally not supported and return
 * SZ_ERR_UNSUPPORTED, as do RAR2 multimedia/audio blocks.  The bounded
 * ring-window streaming (per-byte emit callback) and the Rar2Ctx/Rar3Ctx
 * context API wrapped around the decoder are original to XArchive.
 *===========================================================================*/

#include <stdlib.h>
#include <string.h>
#include "rar3dec.h"

/*---- code-table sizes (UnRAR: NC/DC/LDC/RC/BC for v2 "20" and v3 "30") ---- */
#define NC20  298
#define DC20   48
#define RC20   28
#define BC20   19
#define LT20_SIZE (NC20 + DC20 + RC20)          /* 374 */

#define NC30  299
#define DC30   60
#define LDC30  17
#define RC30   28
#define BC30   20
#define LT30_SIZE (NC30 + DC30 + LDC30 + RC30)  /* 404 */

#define LOW_DIST_REP_COUNT 16
#define HUFF_MAX_SYMS 306                        /* >= max single-code symbols */

/* Bounded ring window shared by both decoders.  It must exceed the largest
 * possible match distance: RAR2 offsets reach ~1 MB and RAR3 offsets reach
 * 4 MB-1 (0x3FFFFF), so a 4 MB window covers both exactly. */
#define RAR_WIN_BITS 22
#define RAR_WIN_SIZE (1UL << RAR_WIN_BITS)
#define RAR_WIN_MASK (RAR_WIN_SIZE - 1)

/*---- decode tables (values from UnRAR) ----------------------------------- */
/* Length bases/bits are shared by v2 and v3 (UnRAR LDecode/LBits). */
static const unsigned char LDecode[28] =
    {  0, 1, 2, 3, 4, 5, 6, 7, 8,10,12,14,16,20,
      24,28,32,40,48,56,64,80,96,112,128,160,192,224 };
static const unsigned char LBits[28] =
    { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5 };
/* Short-distance table, shared by v2 and v3 (UnRAR SDDecode/SDBits). */
static const unsigned char SDDecode[8] = { 0, 4, 8, 16, 32, 64, 128, 192 };
static const unsigned char SDBits[8]   = { 2, 2, 3, 4, 5, 6, 6, 6 };

/* RAR2 distance table (UnRAR DDecode/DBits in unpack20.cpp). */
static const UInt32 DDecode20[48] =
    {      0,      1,      2,      3,      4,      6,      8,     12,
          16,     24,     32,     48,     64,     96,    128,    192,
         256,    384,    512,    768,   1024,   1536,   2048,   3072,
        4096,   6144,   8192,  12288,  16384,  24576,  32768UL,49152UL,
       65536UL,98304UL,131072UL,196608UL,262144UL,327680UL,393216UL,458752UL,
      524288UL,589824UL,655360UL,720896UL,786432UL,851968UL,917504UL,983040UL };
static const unsigned char DBits20[48] =
    {  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4,
       5, 5, 6, 6, 7, 7, 8, 8, 9, 9,10,10,
      11,11,12,12,13,13,14,14,15,15,16,16,
      16,16,16,16,16,16,16,16,16,16,16,16 };

/* RAR3 distance table (UnRAR builds DDecode/DBits from DBitLengthCounts). */
static UInt32        DDecode30[DC30];
static unsigned char DBits30[DC30];
static int           g_v3TablesReady = 0;

static void InitV3DistTables( void )
{
    static const int counts[19] =
        { 4,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,14,0,12 };
    UInt32 dist = 0;
    int    bitlen = 0, slot = 0, i, j;

    if ( g_v3TablesReady ) return;
    for ( i = 0; i < 19; i++, bitlen++ )
        for ( j = 0; j < counts[i]; j++, slot++, dist += ( 1UL << bitlen ) )
        {
            DDecode30[slot] = dist;
            DBits30[slot]   = (unsigned char)bitlen;
        }
    g_v3TablesReady = 1;
}

/*---- MSB-first bit reader ------------------------------------------------ *
 * Modelled on UnRAR's BitInput getbits()/addbits(): getbits() returns the
 * next 16 bits (MSB-first) without consuming, and addbits() advances.  Here
 * BitPeek16() is getbits() and BitAdd() is addbits(); BitGet(n) reads n<=16
 * bits, i.e. UnRAR's "getbits()>>(16-n); addbits(n)".
 *-------------------------------------------------------------------------- */
typedef struct {
    const Byte *buf;
    UInt32      size;
    UInt32      bytePos;
    int         bitPos;      /* 0..7, measured from the MSB */
    int         overrun;
} RBit;

static UInt32 BitPeek16( RBit *b )
{
    UInt32 v;
    UInt32 b0, b1, b2;

    if ( b->bytePos >= b->size ) b->overrun = 1;
    b0 = ( b->bytePos     < b->size ) ? b->buf[b->bytePos]     : 0;
    b1 = ( b->bytePos + 1 < b->size ) ? b->buf[b->bytePos + 1] : 0;
    b2 = ( b->bytePos + 2 < b->size ) ? b->buf[b->bytePos + 2] : 0;
    v = ( b0 << 16 ) | ( b1 << 8 ) | b2;      /* 24 bits, b0 most significant */
    return ( v >> ( 8 - b->bitPos ) ) & 0xFFFF;
}

static void BitAdd( RBit *b, int n )
{
    b->bitPos += n;
    b->bytePos += (UInt32)( b->bitPos >> 3 );
    b->bitPos &= 7;
}

static UInt32 BitGet( RBit *b, int n )        /* read n bits, 0 <= n <= 16 */
{
    UInt32 v;
    if ( n == 0 ) return 0;
    v = BitPeek16( b ) >> ( 16 - n );
    BitAdd( b, n );
    return v;
}

/*---- canonical Huffman (UnRAR MakeDecodeTables / DecodeNumber) ------------ */
typedef struct {
    int            maxNum;
    UInt32         decodeLen[16];
    UInt32         decodePos[16];
    unsigned short decodeNum[HUFF_MAX_SYMS];
} DecTbl;

/* Build the canonical decode table from a per-symbol bit-length array.
 * (Adapted verbatim from UnRAR Unpack::MakeDecodeTables, minus the optional
 * QuickBits lookup cache, which is only a speed optimisation.) */
static void MakeDecodeTables( const unsigned char *lengthTable,
                              DecTbl *dec, int size )
{
    UInt32 lengthCount[16];
    UInt32 copyDecodePos[16];
    UInt32 upperLimit = 0;
    int    i;

    dec->maxNum = size;
    memset( lengthCount, 0, sizeof( lengthCount ) );
    for ( i = 0; i < size; i++ )
        lengthCount[ lengthTable[i] & 0x0F ]++;
    lengthCount[0] = 0;

    for ( i = 0; i < size && i < HUFF_MAX_SYMS; i++ )
        dec->decodeNum[i] = 0;

    dec->decodePos[0] = 0;
    dec->decodeLen[0] = 0;
    for ( i = 1; i < 16; i++ )
    {
        upperLimit += lengthCount[i];
        dec->decodeLen[i] = upperLimit << ( 16 - i );
        upperLimit *= 2;
        dec->decodePos[i] = dec->decodePos[i-1] + lengthCount[i-1];
    }

    memcpy( copyDecodePos, dec->decodePos, sizeof( copyDecodePos ) );
    for ( i = 0; i < size; i++ )
    {
        int curLen = lengthTable[i] & 0x0F;
        if ( curLen != 0 )
            dec->decodeNum[ copyDecodePos[curLen]++ ] = (unsigned short)i;
    }
}

/* Decode one symbol.  (Adapted from UnRAR Unpack::DecodeNumber, slow path;
 * the QuickBits fast path is omitted.  Validated against real archives by the
 * RAR2 CRC regression, which exercises this same routine.) */
static int DecodeNumber( RBit *b, DecTbl *dec )
{
    UInt32 bitField = BitPeek16( b ) & 0xFFFE;
    UInt32 dist, pos;
    int    bits = 15, i;

    for ( i = 1; i < 15; i++ )
        if ( bitField < dec->decodeLen[i] ) { bits = i; break; }
    BitAdd( b, bits );

    dist = bitField - dec->decodeLen[bits-1];
    dist >>= ( 16 - bits );
    pos = dec->decodePos[bits] + dist;
    if ( pos >= (UInt32)dec->maxNum ) pos = 0;
    return (int)dec->decodeNum[pos];
}

/*---- shared buffer sink (non-solid wrappers write to one output buffer) --- */
typedef struct { Byte *buf; UInt32 pos; UInt32 cap; } RarBufSink;

static int RarBufEmit( void *user, Byte b )
{
    RarBufSink *s = (RarBufSink *)user;
    if ( s->pos < s->cap ) s->buf[s->pos] = b;
    s->pos++;
    return SZ_OK;
}

/*===========================================================================
 * RAR2  (unpack version 20)  -  UnRAR Unpack20 / ReadTables20
 *===========================================================================*/
struct Rar2Ctx {
    RBit          b;
    DecTbl        LD, DD, RD, BD;          /* main / dist / length / bit-len  */
    unsigned char lengthtable[LT20_SIZE];
    int           oldoffset[4];
    unsigned      oldoffsetindex;
    int           lastoffset, lastlength;
    Byte         *win;
    UInt32        wmask;
    UInt32        pos;
};

Rar2Ctx *Rar2Create( void )
{
    Rar2Ctx *c = (Rar2Ctx *)malloc( sizeof( Rar2Ctx ) );
    if ( !c ) return NULL;
    c->win = (Byte *)malloc( RAR_WIN_SIZE );
    if ( !c->win ) { free( c ); return NULL; }
    c->wmask = RAR_WIN_MASK;
    c->pos   = 0;
    memset( c->lengthtable, 0, sizeof( c->lengthtable ) );
    c->oldoffset[0] = c->oldoffset[1] = c->oldoffset[2] = c->oldoffset[3] = 0;
    c->oldoffsetindex = 0;
    c->lastoffset = 0;
    c->lastlength = 0;
    c->b.buf = NULL; c->b.size = 0;
    c->b.bytePos = 0; c->b.bitPos = 0; c->b.overrun = 0;
    return c;
}

void Rar2Free( Rar2Ctx *c ) { if ( c ) { free( c->win ); free( c ); } }

void Rar2SetInput( Rar2Ctx *c, const Byte *src, UInt32 srcLen )
{
    c->b.buf = src; c->b.size = srcLen;
    c->b.bytePos = 0; c->b.bitPos = 0; c->b.overrun = 0;
}

void Rar2Align( Rar2Ctx *c )
{
    if ( c->b.bitPos ) { c->b.bitPos = 0; c->b.bytePos++; }
}

/* UnRAR ReadTables20 (LZ path only; audio blocks are refused). */
int Rar2ReadTables( Rar2Ctx *c )
{
    RBit          *b = &c->b;
    unsigned char  bitLength[BC20];
    unsigned char  table[LT20_SIZE];
    UInt32         bitField;
    int            i, number, n;

    bitField = BitPeek16( b );
    if ( bitField & 0x8000 ) return SZ_ERR_UNSUPPORTED;   /* audio block */
    if ( !( bitField & 0x4000 ) )
        memset( c->lengthtable, 0, sizeof( c->lengthtable ) );
    BitAdd( b, 2 );

    for ( i = 0; i < BC20; i++ )
        bitLength[i] = (unsigned char)BitGet( b, 4 );
    if ( b->overrun ) return SZ_ERR_DATA;
    MakeDecodeTables( bitLength, &c->BD, BC20 );

    for ( i = 0; i < LT20_SIZE; )
    {
        number = DecodeNumber( b, &c->BD );
        if ( b->overrun ) return SZ_ERR_DATA;
        if ( number < 16 )
        {
            table[i] = (unsigned char)( ( number + c->lengthtable[i] ) & 0x0F );
            i++;
        }
        else if ( number == 16 )
        {
            if ( i == 0 ) return SZ_ERR_DATA;
            n = (int)BitGet( b, 2 ) + 3;
            while ( n-- > 0 && i < LT20_SIZE ) { table[i] = table[i-1]; i++; }
        }
        else
        {
            n = ( number == 17 ) ? (int)BitGet( b, 3 ) + 3
                                 : (int)BitGet( b, 7 ) + 11;
            while ( n-- > 0 && i < LT20_SIZE ) table[i++] = 0;
        }
    }
    if ( b->overrun ) return SZ_ERR_DATA;

    MakeDecodeTables( &table[0],            &c->LD, NC20 );
    MakeDecodeTables( &table[NC20],         &c->DD, DC20 );
    MakeDecodeTables( &table[NC20 + DC20],  &c->RD, RC20 );
    memcpy( c->lengthtable, table, LT20_SIZE );
    return SZ_OK;
}

/* Push a distance onto the recent-distance ring and emit a copied match
 * (UnRAR CopyString20 semantics: every match updates LastDist/LastLength). */
static int Rar2Match( Rar2Ctx *c, Rar2Emit emit, void *user,
                      int length, int distance, UInt32 endPos )
{
    RBit  *b = &c->b;
    int    rc = SZ_OK;

    c->lastoffset = distance;
    c->oldoffset[c->oldoffsetindex & 3] = distance;
    c->oldoffsetindex++;
    c->lastlength = length;

    if ( distance <= 0 || (UInt32)distance > c->pos ||
         (UInt32)distance > c->wmask + 1 ) return SZ_ERR_DATA;
    while ( length-- > 0 && c->pos < endPos )
    {
        Byte mb = c->win[( c->pos - (UInt32)distance ) & c->wmask];
        c->win[c->pos & c->wmask] = mb;
        c->pos++;
        if ( ( rc = emit( user, mb ) ) != SZ_OK ) return rc;
    }
    if ( b->overrun ) return SZ_ERR_DATA;
    return SZ_OK;
}

int Rar2Decode2( Rar2Ctx *c, Rar2Emit emit, void *user, UInt32 endPos )
{
    RBit *b  = &c->b;
    int   rc = SZ_OK;

    while ( c->pos < endPos )
    {
        int number = DecodeNumber( b, &c->LD );
        int length, distance, bits, dn, ln;

        if ( b->overrun ) { rc = SZ_ERR_DATA; break; }

        if ( number < 256 )                              /* literal */
        {
            Byte lb = (Byte)number;
            c->win[c->pos & c->wmask] = lb;
            c->pos++;
            if ( ( rc = emit( user, lb ) ) != SZ_OK ) break;
            continue;
        }
        if ( number > 269 )                              /* normal match */
        {
            length = LDecode[ number - 270 ] + 3;
            bits   = LBits[ number - 270 ];
            if ( bits ) length += (int)BitGet( b, bits );

            dn = DecodeNumber( b, &c->DD );
            if ( dn < 0 || dn >= DC20 ) { rc = SZ_ERR_DATA; break; }
            distance = (int)DDecode20[dn] + 1;
            bits = DBits20[dn];
            if ( bits ) distance += (int)BitGet( b, bits );
            if ( distance >= 0x2000 )
            {
                length++;
                if ( distance >= 0x40000L ) length++;
            }
            if ( ( rc = Rar2Match( c, emit, user, length, distance, endPos ) )
                 != SZ_OK ) break;
            continue;
        }
        if ( number == 269 )                             /* new tables */
        {
            rc = Rar2ReadTables( c );
            if ( rc != SZ_OK ) break;
            continue;
        }
        if ( number == 256 )                             /* repeat last match */
        {
            if ( ( rc = Rar2Match( c, emit, user,
                                   c->lastlength, c->lastoffset, endPos ) )
                 != SZ_OK ) break;
            continue;
        }
        if ( number < 261 )                              /* 257..260 old dist */
        {
            distance = c->oldoffset[ ( c->oldoffsetindex - (unsigned)( number - 256 ) ) & 3 ];
            ln = DecodeNumber( b, &c->RD );
            if ( ln < 0 || ln >= RC20 ) { rc = SZ_ERR_DATA; break; }
            length = LDecode[ln] + 2;
            bits = LBits[ln];
            if ( bits ) length += (int)BitGet( b, bits );
            if ( distance >= 0x101 )
            {
                length++;
                if ( distance >= 0x2000 )
                {
                    length++;
                    if ( distance >= 0x40000L ) length++;
                }
            }
            if ( ( rc = Rar2Match( c, emit, user, length, distance, endPos ) )
                 != SZ_OK ) break;
            continue;
        }
        /* 261..268 short distance */
        distance = SDDecode[ number - 261 ] + 1;
        bits = SDBits[ number - 261 ];
        if ( bits ) distance += (int)BitGet( b, bits );
        if ( ( rc = Rar2Match( c, emit, user, 2, distance, endPos ) )
             != SZ_OK ) break;
    }
    return rc;
}

/* Push raw (stored) bytes through the window + emit (solid stored entries). */
int Rar2Feed( Rar2Ctx *c, Rar2Emit emit, void *user, const Byte *data, UInt32 len )
{
    int rc = SZ_OK;
    while ( len-- > 0 )
    {
        Byte bb = *data++;
        c->win[c->pos & c->wmask] = bb;
        c->pos++;
        if ( ( rc = emit( user, bb ) ) != SZ_OK ) break;
    }
    return rc;
}

/* Non-solid wrapper: decode a whole v2 stream into a caller-provided buffer. */
int Rar2Decode( const Byte *src, UInt32 srcLen, Byte *dst, UInt32 dstLen )
{
    Rar2Ctx   *c = Rar2Create();
    RarBufSink s;
    int        rc;

    if ( !c ) return SZ_ERR_MEMORY;
    s.buf = dst; s.pos = 0; s.cap = dstLen;
    Rar2SetInput( c, src, srcLen );
    rc = Rar2ReadTables( c );
    if ( rc == SZ_OK )
        rc = Rar2Decode2( c, RarBufEmit, &s, dstLen );
    Rar2Free( c );
    return rc;
}

/*===========================================================================
 * RAR3  (unpack version 29/36)  -  UnRAR Unpack29 / ReadTables30 /
 * ReadEndOfBlock.  Same streaming ring-window contract as the RAR2 context;
 * tables / distance history / window position persist across solid files.
 *===========================================================================*/
struct Rar3Ctx {
    RBit          b;
    DecTbl        LD, DD, LDD, RD, BD;     /* main/dist/lowdist/length/bit-len */
    unsigned char lengthtable[LT30_SIZE];
    int           oldoffset[4];
    int           lastlength;
    int           prevLowDist, lowDistRepCount;
    Byte         *win;
    UInt32        wmask;
    UInt32        pos;
};

Rar3Ctx *Rar3Create( void )
{
    Rar3Ctx *c;
    InitV3DistTables();
    c = (Rar3Ctx *)malloc( sizeof( Rar3Ctx ) );
    if ( !c ) return NULL;
    c->win = (Byte *)malloc( RAR_WIN_SIZE );
    if ( !c->win ) { free( c ); return NULL; }
    c->wmask = RAR_WIN_MASK;
    c->pos   = 0;
    memset( c->lengthtable, 0, sizeof( c->lengthtable ) );
    c->oldoffset[0] = c->oldoffset[1] = c->oldoffset[2] = c->oldoffset[3] = 0;
    c->lastlength = 0;
    c->prevLowDist = 0;
    c->lowDistRepCount = 0;
    c->b.buf = NULL; c->b.size = 0;
    c->b.bytePos = 0; c->b.bitPos = 0; c->b.overrun = 0;
    return c;
}

void Rar3Free( Rar3Ctx *c ) { if ( c ) { free( c->win ); free( c ); } }

void Rar3SetInput( Rar3Ctx *c, const Byte *src, UInt32 srcLen )
{
    c->b.buf = src; c->b.size = srcLen;
    c->b.bytePos = 0; c->b.bitPos = 0; c->b.overrun = 0;
}

/* UnRAR ReadTables30 (LZ path only; PPMd blocks are refused). */
int Rar3ReadTables( Rar3Ctx *c )
{
    RBit          *b = &c->b;
    unsigned char  bitLength[BC30];
    unsigned char  table[LT30_SIZE];
    UInt32         bitField;
    int            i, number, n;

    if ( b->bitPos ) { b->bitPos = 0; b->bytePos++; }     /* byte-align */

    bitField = BitPeek16( b );
    if ( bitField & 0x8000 ) return SZ_ERR_UNSUPPORTED;   /* PPMd block */

    c->prevLowDist = 0;
    c->lowDistRepCount = 0;
    if ( !( bitField & 0x4000 ) )
        memset( c->lengthtable, 0, sizeof( c->lengthtable ) );
    BitAdd( b, 2 );

    for ( i = 0; i < BC30; i++ )
    {
        int length = (int)BitGet( b, 4 );
        if ( length == 15 )
        {
            int zeroCount = (int)BitGet( b, 4 );
            if ( zeroCount == 0 )
                bitLength[i] = 15;
            else
            {
                for ( zeroCount += 2; zeroCount > 0 && i < BC30; zeroCount-- )
                    bitLength[i++] = 0;
                i--;
            }
        }
        else
            bitLength[i] = (unsigned char)length;
    }
    if ( b->overrun ) return SZ_ERR_DATA;
    MakeDecodeTables( bitLength, &c->BD, BC30 );

    for ( i = 0; i < LT30_SIZE; )
    {
        number = DecodeNumber( b, &c->BD );
        if ( b->overrun ) return SZ_ERR_DATA;
        if ( number < 16 )
        {
            table[i] = (unsigned char)( ( number + c->lengthtable[i] ) & 0x0F );
            i++;
        }
        else if ( number < 18 )
        {
            n = ( number == 16 ) ? (int)BitGet( b, 3 ) + 3
                                 : (int)BitGet( b, 7 ) + 11;
            if ( i == 0 ) return SZ_ERR_DATA;
            while ( n-- > 0 && i < LT30_SIZE ) { table[i] = table[i-1]; i++; }
        }
        else
        {
            n = ( number == 18 ) ? (int)BitGet( b, 3 ) + 3
                                 : (int)BitGet( b, 7 ) + 11;
            while ( n-- > 0 && i < LT30_SIZE ) table[i++] = 0;
        }
    }
    if ( b->overrun ) return SZ_ERR_DATA;

    MakeDecodeTables( &table[0],                    &c->LD,  NC30 );
    MakeDecodeTables( &table[NC30],                 &c->DD,  DC30 );
    MakeDecodeTables( &table[NC30 + DC30],          &c->LDD, LDC30 );
    MakeDecodeTables( &table[NC30 + DC30 + LDC30],  &c->RD,  RC30 );
    memcpy( c->lengthtable, table, LT30_SIZE );
    return SZ_OK;
}

/* UnRAR InsertOldDist + CopyString for the v3 context: shift the recent
 * distance ring, record LastLength, then emit the copied match. */
static int Rar3Match( Rar3Ctx *c, Rar2Emit emit, void *user,
                      int length, int distance, UInt32 endPos, int insert )
{
    RBit *b  = &c->b;
    int   rc = SZ_OK;

    if ( insert )
    {
        c->oldoffset[3] = c->oldoffset[2];
        c->oldoffset[2] = c->oldoffset[1];
        c->oldoffset[1] = c->oldoffset[0];
        c->oldoffset[0] = distance;
    }
    c->lastlength = length;

    if ( distance <= 0 || (UInt32)distance > c->pos ||
         (UInt32)distance > c->wmask + 1 ) return SZ_ERR_DATA;
    while ( length-- > 0 && c->pos < endPos )
    {
        Byte mb = c->win[( c->pos - (UInt32)distance ) & c->wmask];
        c->win[c->pos & c->wmask] = mb;
        c->pos++;
        if ( ( rc = emit( user, mb ) ) != SZ_OK ) return rc;
    }
    if ( b->overrun ) return SZ_ERR_DATA;
    return SZ_OK;
}

/* UnRAR ReadEndOfBlock (symbol 256): returns SZ_OK to continue decoding, a
 * positive "stop" sentinel at a new-file boundary, or an SZ_ERR_* code. */
#define RAR3_STOP 1

static int Rar3ReadEndOfBlock( Rar3Ctx *c )
{
    RBit  *b = &c->b;
    UInt32 bitField = BitPeek16( b );
    int    newTable, newFile = 0;

    if ( bitField & 0x8000 ) { newTable = 1; BitAdd( b, 1 ); }
    else { newFile = 1; newTable = ( bitField & 0x4000 ) != 0; BitAdd( b, 2 ); }

    if ( newFile ) return RAR3_STOP;         /* end of this file's data */
    if ( !newTable ) return SZ_OK;
    return Rar3ReadTables( c );
}

int Rar3Decode2( Rar3Ctx *c, Rar2Emit emit, void *user, UInt32 endPos )
{
    RBit *b  = &c->b;
    int   rc = SZ_OK;

    while ( c->pos < endPos )
    {
        int number = DecodeNumber( b, &c->LD );
        int length, distance, bits, dn, ln, i;

        if ( b->overrun ) { rc = SZ_ERR_DATA; break; }

        if ( number < 256 )                              /* literal */
        {
            Byte lb = (Byte)number;
            c->win[c->pos & c->wmask] = lb;
            c->pos++;
            if ( ( rc = emit( user, lb ) ) != SZ_OK ) break;
            continue;
        }
        if ( number >= 271 )                             /* normal match */
        {
            length = LDecode[ number - 271 ] + 3;
            bits   = LBits[ number - 271 ];
            if ( bits ) length += (int)BitGet( b, bits );

            dn = DecodeNumber( b, &c->DD );
            if ( dn < 0 || dn >= DC30 ) { rc = SZ_ERR_DATA; break; }
            distance = (int)DDecode30[dn] + 1;
            bits = DBits30[dn];
            if ( bits )
            {
                if ( dn > 9 )
                {
                    if ( bits > 4 )
                        distance += (int)( BitGet( b, bits - 4 ) << 4 );
                    if ( c->lowDistRepCount > 0 )
                    {
                        c->lowDistRepCount--;
                        distance += c->prevLowDist;
                    }
                    else
                    {
                        int lowDist = DecodeNumber( b, &c->LDD );
                        if ( lowDist == 16 )
                        {
                            c->lowDistRepCount = LOW_DIST_REP_COUNT - 1;
                            distance += c->prevLowDist;
                        }
                        else
                        {
                            distance += lowDist;
                            c->prevLowDist = lowDist;
                        }
                    }
                }
                else
                    distance += (int)BitGet( b, bits );
            }
            if ( distance >= 0x2000 )
            {
                length++;
                if ( distance >= 0x40000L ) length++;
            }
            if ( ( rc = Rar3Match( c, emit, user, length, distance, endPos, 1 ) )
                 != SZ_OK ) break;
            continue;
        }
        if ( number == 256 )                             /* end of block */
        {
            rc = Rar3ReadEndOfBlock( c );
            if ( rc == RAR3_STOP ) { rc = SZ_OK; break; }
            if ( rc != SZ_OK ) break;
            continue;
        }
        if ( number == 257 )                             /* filter/VM */
        { rc = SZ_ERR_UNSUPPORTED; break; }
        if ( number == 258 )                             /* repeat last match */
        {
            if ( c->lastlength != 0 &&
                 ( rc = Rar3Match( c, emit, user, c->lastlength,
                                   c->oldoffset[0], endPos, 0 ) ) != SZ_OK )
                break;
            continue;
        }
        if ( number < 263 )                              /* 259..262 old dist */
        {
            int distNum = number - 259;
            distance = c->oldoffset[distNum];
            for ( i = distNum; i > 0; i-- )
                c->oldoffset[i] = c->oldoffset[i-1];
            c->oldoffset[0] = distance;

            ln = DecodeNumber( b, &c->RD );
            if ( ln < 0 || ln >= RC30 ) { rc = SZ_ERR_DATA; break; }
            length = LDecode[ln] + 2;
            bits = LBits[ln];
            if ( bits ) length += (int)BitGet( b, bits );
            if ( ( rc = Rar3Match( c, emit, user, length, distance, endPos, 0 ) )
                 != SZ_OK ) break;
            continue;
        }
        /* 263..270 short distance */
        distance = SDDecode[ number - 263 ] + 1;
        bits = SDBits[ number - 263 ];
        if ( bits ) distance += (int)BitGet( b, bits );
        if ( ( rc = Rar3Match( c, emit, user, 2, distance, endPos, 1 ) )
             != SZ_OK ) break;
    }
    return rc;
}

int Rar3Feed( Rar3Ctx *c, Rar2Emit emit, void *user, const Byte *data, UInt32 len )
{
    int rc = SZ_OK;
    while ( len-- > 0 )
    {
        Byte bb = *data++;
        c->win[c->pos & c->wmask] = bb;
        c->pos++;
        if ( ( rc = emit( user, bb ) ) != SZ_OK ) break;
    }
    return rc;
}

/* Non-solid wrapper: decode a whole v3 stream into a caller-provided buffer. */
int Rar3Decode( const Byte *src, UInt32 srcLen, Byte *dst, UInt32 dstLen )
{
    Rar3Ctx   *c = Rar3Create();
    RarBufSink s;
    int        rc;

    if ( !c ) return SZ_ERR_MEMORY;
    s.buf = dst; s.pos = 0; s.cap = dstLen;
    Rar3SetInput( c, src, srcLen );
    rc = Rar3ReadTables( c );
    if ( rc == SZ_OK )
        rc = Rar3Decode2( c, RarBufEmit, &s, dstLen );
    Rar3Free( c );
    return rc;
}
