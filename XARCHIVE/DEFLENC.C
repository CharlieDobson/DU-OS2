/*===========================================================================
 * DEFLENC.C  -  Deflate encoder (RFC 1951), PKZIP 2.04g -ex strength
 * Target: MSVC 2.2  Win32s
 *
 * Written from RFC 1951.  The parse follows the scheme every deflate encoder
 * since PKZIP 2 has used, because the format rewards it and nothing simpler
 * does as well:
 *
 *   - a 32 KB sliding window held in a 64 KB buffer, slid down by half when
 *     the read position nears the top;
 *   - a hash of the next three bytes indexes 'head', the newest position with
 *     that hash, and 'prev' chains each position to the one before it with
 *     the same hash - so the chain for a position is every earlier place the
 *     same three bytes might begin;
 *   - LAZY matching: when a match is found, the next position is tried as
 *     well, and if that one starts a longer match the first byte goes out as
 *     a literal instead.  Most of the gain of the "maximum" setting is here;
 *   - symbols are collected for one block (16 K of them), then the block is
 *     sent whichever way is smallest: stored, the fixed codes, or Huffman
 *     codes built for exactly this block.
 *
 * The "maximum" settings are the ones PKZIP's -ex and zlib's level 9 share in
 * spirit: chains followed 4096 deep (a quarter of that once a match of 32 is
 * in hand), no lazy cut-off, and a three-byte match more than 4 KB back
 * thrown away because its distance code costs more than three literals.
 *
 * NO __int64 and no 64-bit arithmetic anywhere, for the MSVC 2.2 build.
 *===========================================================================*/

#include <stdlib.h>
#include <string.h>
#include "deflenc.h"

/*---- Format constants ----------------------------------------------------- */
#define WSIZE         32768U          /* the deflate window                   */
#define WMASK         ( WSIZE - 1 )
#define MIN_MATCH     3
#define MAX_MATCH     258
#define MIN_LOOKAHEAD ( MAX_MATCH + MIN_MATCH + 1 )
#define MAX_DIST      ( WSIZE - MIN_LOOKAHEAD )
#define TOO_FAR       4096            /* a 3-byte match further back is a loss */

#define HASH_BITS     15
#define HASH_SIZE     ( 1U << HASH_BITS )
#define HASH_MASK     ( HASH_SIZE - 1 )
#define NIL           0

#define L_CODES       286             /* literals, end of block, lengths      */
#define D_CODES       30
#define BL_CODES      19
#define HEAP_MAX      ( 2 * L_CODES + 1 )
#define END_BLOCK     256
#define MAX_BITS      15
#define MAX_BL_BITS   7
#define REP_3_6       16
#define REPZ_3_10     17
#define REPZ_11_138   18

#define LIT_BUFSIZE   16384U          /* symbols per block                    */
#define OUT_BUFSIZE   16384U

/* The maximum-compression parse. */
#define MAX_CHAIN     4096
#define GOOD_MATCH    32
#define NICE_MATCH    258

static const int extraLBits[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const int extraDBits[D_CODES] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
static const int extraBlBits[BL_CODES] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,3,7 };
static const Byte blOrder[BL_CODES] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };

/*---- Code tables built once ----------------------------------------------- */
static Byte    g_lengthCode[256];     /* match length - 3  ->  length code - 257 */
static int     g_baseLength[29];
static Byte    g_distCode[512];       /* see DCode                              */
static int     g_baseDist[D_CODES];
static UInt16  g_staticLCode[L_CODES + 2];
static Byte    g_staticLLen[L_CODES + 2];
static UInt16  g_staticDCode[D_CODES];
static Byte    g_staticDLen[D_CODES];
static int     g_tablesBuilt = 0;

/* Scratch for one Huffman build, kept off the stack (about 9 KB). */
typedef struct {
    UInt32 freq;
    int    sym;
} HSym;

typedef struct {
    HSym   sym[L_CODES + 2];
    UInt32 nodeFreq[2 * ( L_CODES + 2 )];
    int    parent[2 * ( L_CODES + 2 )];
    int    depth[2 * ( L_CODES + 2 )];
} HuffScratch;

struct DeflEnc {
    /* The window and the chains. */
    Byte   *window;                   /* 2 * WSIZE + MAX_MATCH slack           */
    UInt16 *head;                     /* HASH_SIZE                             */
    UInt16 *prev;                     /* WSIZE                                 */
    unsigned strstart;                /* the next position to be coded         */
    unsigned lookahead;               /* valid bytes from strstart on          */
    long     blockStart;              /* window index where the block began,
                                         negative once it has slid out        */
    unsigned matchStart;
    unsigned matchLength;
    unsigned prevLength;
    unsigned prevMatch;
    int      matchAvailable;

    /* One block's symbols: a literal, or a length/distance pair. */
    Byte   *lBuf;                     /* literal, or length - 3                */
    UInt16 *dBuf;                     /* 0 for a literal, else the distance    */
    unsigned lastLit;
    UInt32  lFreq[L_CODES + 2];
    UInt32  dFreq[D_CODES];
    UInt32  blFreq[BL_CODES];
    Byte    lLen[L_CODES + 2];
    Byte    dLen[D_CODES];
    Byte    blLen[BL_CODES];
    UInt16  lCode[L_CODES + 2];
    UInt16  dCode[D_CODES];
    UInt16  blCode[BL_CODES];
    HuffScratch hs;

    /* The bit writer. */
    UInt32  bitBuf;
    int     bitCount;
    Byte   *out;
    unsigned outPos;
    UInt32  outTotal;

    DeflWrite write;
    void     *user;
    int       failed;
};

/*===========================================================================
 * Static tables
 *===========================================================================*/
static UInt16 BitReverse( unsigned code, int len )
{
    unsigned res = 0;
    do {
        res |= code & 1;
        code >>= 1;
        res <<= 1;
    } while ( --len > 0 );
    return (UInt16)( res >> 1 );
}

/* Canonical codes for a set of lengths, bit-reversed for LSB-first output. */
static void GenCodes( const Byte *lens, int n, UInt16 *codes )
{
    UInt16 blCount[MAX_BITS + 1];
    UInt16 nextCode[MAX_BITS + 1];
    unsigned code = 0;
    int bits, i;

    memset( blCount, 0, sizeof( blCount ) );
    for ( i = 0; i < n; i++ ) blCount[ lens[i] ]++;
    blCount[0] = 0;
    for ( bits = 1; bits <= MAX_BITS; bits++ )
    {
        code = ( code + blCount[bits - 1] ) << 1;
        nextCode[bits] = (UInt16)code;
    }
    for ( i = 0; i < n; i++ )
    {
        int len = lens[i];
        codes[i] = len ? BitReverse( nextCode[len]++, len ) : 0;
    }
}

static void BuildTables( void )
{
    int code, n, length = 0, dist = 0;

    if ( g_tablesBuilt ) return;

    for ( code = 0; code < 28; code++ )
    {
        g_baseLength[code] = length;
        for ( n = 0; n < ( 1 << extraLBits[code] ); n++ )
            g_lengthCode[length++] = (Byte)code;
    }
    /* Length 258 has a code of its own (285) with no extra bits, rather than
     * being the last value of code 284's range. */
    g_lengthCode[length - 1] = (Byte)code;
    g_baseLength[28] = 255;

    for ( code = 0; code < 16; code++ )
    {
        g_baseDist[code] = dist;
        for ( n = 0; n < ( 1 << extraDBits[code] ); n++ )
            g_distCode[dist++] = (Byte)code;
    }
    dist >>= 7;                       /* from here on, in units of 128 */
    for ( ; code < D_CODES; code++ )
    {
        g_baseDist[code] = dist << 7;
        for ( n = 0; n < ( 1 << ( extraDBits[code] - 7 ) ); n++ )
            g_distCode[256 + dist++] = (Byte)code;
    }

    for ( n = 0;   n <= 143; n++ ) g_staticLLen[n] = 8;
    for ( ;        n <= 255; n++ ) g_staticLLen[n] = 9;
    for ( ;        n <= 279; n++ ) g_staticLLen[n] = 7;
    for ( ;        n <= 287; n++ ) g_staticLLen[n] = 8;
    GenCodes( g_staticLLen, L_CODES + 2, g_staticLCode );
    for ( n = 0; n < D_CODES; n++ )
    {
        g_staticDCode[n] = BitReverse( (unsigned)n, 5 );
        g_staticDLen[n]  = 5;
    }

    g_tablesBuilt = 1;
}

/* Distance code for a distance - 1 ('dist' here is 0-based). */
#define DCode( dist ) \
    ( (dist) < 256 ? g_distCode[dist] : g_distCode[256 + ( (dist) >> 7 )] )

/*===========================================================================
 * Output
 *===========================================================================*/
static void FlushOut( DeflEnc *d )
{
    if ( d->outPos == 0 ) return;
    if ( !d->failed && !d->write( d->user, d->out, d->outPos ) )
        d->failed = 1;
    d->outTotal += d->outPos;
    d->outPos = 0;
}

static void PutByte( DeflEnc *d, Byte b )
{
    d->out[d->outPos++] = b;
    if ( d->outPos == OUT_BUFSIZE ) FlushOut( d );
}

/* Up to 16 bits, LSB first. */
static void SendBits( DeflEnc *d, unsigned value, int len )
{
    d->bitBuf |= (UInt32)value << d->bitCount;
    d->bitCount += len;
    while ( d->bitCount >= 8 )
    {
        PutByte( d, (Byte)d->bitBuf );
        d->bitBuf >>= 8;
        d->bitCount -= 8;
    }
}

static void AlignToByte( DeflEnc *d )
{
    if ( d->bitCount > 0 ) PutByte( d, (Byte)d->bitBuf );
    d->bitBuf = 0;
    d->bitCount = 0;
}

/*===========================================================================
 * Huffman code lengths, limited to 'maxBits'
 *
 * An ordinary Huffman build first, from a sorted list with two queues.  If
 * the deepest leaf is past the limit, the length COUNTS are repaired the
 * usual way - a code at the limit is pulled up and an overflowing pair put
 * beside it - and the lengths are then handed out again, the longest to the
 * rarest symbols.  That keeps the code complete and costs very little.
 *
 * At least two symbols always get a code.  RFC 1951 allows a single used
 * code, but PKZIP's own inflater of the period wants a complete tree, and two
 * codes of length 1 cost one bit per symbol either way.
 *===========================================================================*/
static int HSymCmp( const void *a, const void *b )
{
    const HSym *x = (const HSym *)a, *y = (const HSym *)b;
    if ( x->freq != y->freq ) return ( x->freq < y->freq ) ? -1 : 1;
    return x->sym - y->sym;
}

static void BuildLengths( HuffScratch *hs, const UInt32 *freq, int n,
                          int maxBits, Byte *lens )
{
    HSym    *sym      = hs->sym;
    UInt32  *nodeFreq = hs->nodeFreq;
    int     *parent   = hs->parent;
    int     *depth    = hs->depth;
    int      blCount[MAX_BITS + 2];
    int      count = 0, i, leafHead, nodeHead, nodeTail, next;
    int      overflow = 0;

    memset( lens, 0, (size_t)n );
    for ( i = 0; i < n; i++ )
        if ( freq[i] ) { sym[count].freq = freq[i]; sym[count].sym = i; count++; }

    /* Fewer than two used symbols: make it two, at length 1. */
    if ( count < 2 )
    {
        int a = ( count == 1 ) ? sym[0].sym : 0;
        int b = ( a == 0 ) ? 1 : 0;
        lens[a] = 1;
        lens[b] = 1;
        return;
    }

    qsort( sym, (size_t)count, sizeof( HSym ), HSymCmp );

    /* Leaves are 0..count-1 (in frequency order), internal nodes count.. */
    for ( i = 0; i < count; i++ ) nodeFreq[i] = sym[i].freq;
    leafHead = 0;
    nodeHead = nodeTail = count;
    for ( next = count; next < 2 * count - 1; next++ )
    {
        int pick[2], k;
        for ( k = 0; k < 2; k++ )
        {
            if ( leafHead < count &&
                 ( nodeHead >= nodeTail || nodeFreq[leafHead] <= nodeFreq[nodeHead] ) )
                pick[k] = leafHead++;
            else
                pick[k] = nodeHead++;
        }
        nodeFreq[next] = nodeFreq[pick[0]] + nodeFreq[pick[1]];
        parent[pick[0]] = next;
        parent[pick[1]] = next;
        nodeTail = next + 1;
    }

    /* Depths, from the root down (parents always have higher numbers). */
    depth[2 * count - 2] = 0;
    for ( i = 2 * count - 3; i >= 0; i-- )
        depth[i] = depth[ parent[i] ] + 1;

    memset( blCount, 0, sizeof( blCount ) );
    for ( i = 0; i < count; i++ )
    {
        int len = depth[i];
        if ( len > maxBits ) { len = maxBits; overflow++; }
        blCount[len]++;
    }

    /* Repair the counts until the code is complete again. */
    while ( overflow > 0 )
    {
        int bits = maxBits - 1;
        while ( blCount[bits] == 0 ) bits--;
        blCount[bits]--;
        blCount[bits + 1] += 2;
        blCount[maxBits]--;
        overflow -= 2;
    }

    /* Hand the lengths out again: the rarest symbols (front of the sorted
     * list) get the longest codes. */
    {
        int len = maxBits, idx = 0;
        for ( ; len >= 1; len-- )
        {
            int k;
            for ( k = 0; k < blCount[len]; k++ )
                lens[ sym[idx++].sym ] = (Byte)len;
        }
    }
}

/*===========================================================================
 * The dynamic block header: the two code-length sequences, run-length coded
 * with the code-length alphabet (16 = repeat the last 3-6 times, 17 / 18 =
 * 3-10 / 11-138 zeros).  Done per tree, never across the boundary between
 * them - legal, but old inflaters have been known to object.
 *===========================================================================*/
static void ScanTree( DeflEnc *d, const Byte *lens, int maxCode )
{
    int n, prevLen = -1, curLen, nextLen = lens[0];
    int count = 0, maxCount = 7, minCount = 4;

    if ( nextLen == 0 ) { maxCount = 138; minCount = 3; }
    for ( n = 0; n <= maxCode; n++ )
    {
        curLen  = nextLen;
        nextLen = ( n + 1 <= maxCode ) ? lens[n + 1] : -1;
        if ( ++count < maxCount && curLen == nextLen ) continue;
        if ( count < minCount )      d->blFreq[curLen] += (UInt32)count;
        else if ( curLen != 0 )
        {
            if ( curLen != prevLen ) d->blFreq[curLen]++;
            d->blFreq[REP_3_6]++;
        }
        else if ( count <= 10 )      d->blFreq[REPZ_3_10]++;
        else                         d->blFreq[REPZ_11_138]++;
        count = 0;
        prevLen = curLen;
        if ( nextLen == 0 )          { maxCount = 138; minCount = 3; }
        else if ( curLen == nextLen ){ maxCount = 6;   minCount = 3; }
        else                         { maxCount = 7;   minCount = 4; }
    }
}

#define SendCode( d, c, codes, lens )  SendBits( (d), (codes)[c], (lens)[c] )

static void SendTree( DeflEnc *d, const Byte *lens, int maxCode )
{
    int n, prevLen = -1, curLen, nextLen = lens[0];
    int count = 0, maxCount = 7, minCount = 4;

    if ( nextLen == 0 ) { maxCount = 138; minCount = 3; }
    for ( n = 0; n <= maxCode; n++ )
    {
        curLen  = nextLen;
        nextLen = ( n + 1 <= maxCode ) ? lens[n + 1] : -1;
        if ( ++count < maxCount && curLen == nextLen ) continue;
        if ( count < minCount )
        {
            do { SendCode( d, curLen, d->blCode, d->blLen ); } while ( --count != 0 );
        }
        else if ( curLen != 0 )
        {
            if ( curLen != prevLen )
            {
                SendCode( d, curLen, d->blCode, d->blLen );
                count--;
            }
            SendCode( d, REP_3_6, d->blCode, d->blLen );
            SendBits( d, (unsigned)( count - 3 ), 2 );
        }
        else if ( count <= 10 )
        {
            SendCode( d, REPZ_3_10, d->blCode, d->blLen );
            SendBits( d, (unsigned)( count - 3 ), 3 );
        }
        else
        {
            SendCode( d, REPZ_11_138, d->blCode, d->blLen );
            SendBits( d, (unsigned)( count - 11 ), 7 );
        }
        count = 0;
        prevLen = curLen;
        if ( nextLen == 0 )          { maxCount = 138; minCount = 3; }
        else if ( curLen == nextLen ){ maxCount = 6;   minCount = 3; }
        else                         { maxCount = 7;   minCount = 4; }
    }
}

/*===========================================================================
 * Sending a block
 *===========================================================================*/
static void CompressBlock( DeflEnc *d, const UInt16 *lc, const Byte *ll,
                           const UInt16 *dc, const Byte *dl )
{
    unsigned i;

    for ( i = 0; i < d->lastLit; i++ )
    {
        unsigned dist = d->dBuf[i];
        unsigned lit  = d->lBuf[i];

        if ( dist == 0 )
            SendCode( d, lit, lc, ll );
        else
        {
            unsigned code = g_lengthCode[lit];
            int      extra;

            SendCode( d, code + 257, lc, ll );
            extra = extraLBits[code];
            if ( extra ) SendBits( d, lit - (unsigned)g_baseLength[code], extra );
            dist--;
            code = DCode( dist );
            SendCode( d, code, dc, dl );
            extra = extraDBits[code];
            if ( extra ) SendBits( d, dist - (unsigned)g_baseDist[code], extra );
        }
    }
    SendCode( d, END_BLOCK, lc, ll );
}

/* Bits the block's symbols would cost with these lengths (not counting the
 * block header). */
static UInt32 BlockBits( DeflEnc *d, const Byte *ll, const Byte *dl )
{
    UInt32 bits = 0;
    int    i;

    for ( i = 0; i < L_CODES; i++ )
        if ( d->lFreq[i] )
        {
            bits += d->lFreq[i] * ll[i];
            if ( i >= 257 ) bits += d->lFreq[i] * (UInt32)extraLBits[i - 257];
        }
    for ( i = 0; i < D_CODES; i++ )
        if ( d->dFreq[i] )
            bits += d->dFreq[i] * ( (UInt32)dl[i] + (UInt32)extraDBits[i] );
    return bits;
}

static void InitBlock( DeflEnc *d )
{
    memset( d->lFreq, 0, sizeof( d->lFreq ) );
    memset( d->dFreq, 0, sizeof( d->dFreq ) );
    d->lFreq[END_BLOCK] = 1;
    d->lastLit = 0;
}

static void FlushBlock( DeflEnc *d, int last )
{
    UInt32 storedLen, dynBits, statBits, dynBytes, statBytes;
    int    maxL, maxD, maxBl, i;
    const Byte *buf;

    storedLen = ( d->blockStart >= 0 )
              ? (UInt32)( (long)d->strstart - d->blockStart ) : 0;
    buf = ( d->blockStart >= 0 ) ? d->window + d->blockStart : 0;

    /* The block's own codes. */
    BuildLengths( &d->hs, d->lFreq, L_CODES, MAX_BITS, d->lLen );
    BuildLengths( &d->hs, d->dFreq, D_CODES, MAX_BITS, d->dLen );
    GenCodes( d->lLen, L_CODES, d->lCode );
    GenCodes( d->dLen, D_CODES, d->dCode );

    for ( maxL = L_CODES - 1; maxL >= 257 && d->lLen[maxL] == 0; maxL-- ) ;
    for ( maxD = D_CODES - 1; maxD >= 1 && d->dLen[maxD] == 0; maxD-- ) ;

    memset( d->blFreq, 0, sizeof( d->blFreq ) );
    ScanTree( d, d->lLen, maxL );
    ScanTree( d, d->dLen, maxD );
    BuildLengths( &d->hs, d->blFreq, BL_CODES, MAX_BL_BITS, d->blLen );
    GenCodes( d->blLen, BL_CODES, d->blCode );
    for ( maxBl = BL_CODES - 1; maxBl >= 3 && d->blLen[ blOrder[maxBl] ] == 0; maxBl-- ) ;

    dynBits = BlockBits( d, d->lLen, d->dLen ) + 3 + 5 + 5 + 4
            + 3 * (UInt32)( maxBl + 1 );
    for ( i = 0; i < BL_CODES; i++ )
        dynBits += d->blFreq[i] * ( (UInt32)d->blLen[i] + (UInt32)extraBlBits[i] );
    statBits  = BlockBits( d, g_staticLLen, g_staticDLen ) + 3;
    dynBytes  = ( dynBits + 7 ) >> 3;
    statBytes = ( statBits + 7 ) >> 3;
    if ( statBytes <= dynBytes ) dynBytes = statBytes;

    if ( buf && storedLen + 4 <= dynBytes && storedLen <= 0xFFFFU )
    {
        /* Stored: header bits, then aligned LEN / NLEN and the raw bytes. */
        UInt32 k;
        SendBits( d, (unsigned)( 0 + ( last ? 1 : 0 ) ), 3 );
        AlignToByte( d );
        PutByte( d, (Byte)storedLen );
        PutByte( d, (Byte)( storedLen >> 8 ) );
        PutByte( d, (Byte)~storedLen );
        PutByte( d, (Byte)( ~storedLen >> 8 ) );
        for ( k = 0; k < storedLen; k++ ) PutByte( d, buf[k] );
    }
    else if ( statBytes == dynBytes )
    {
        SendBits( d, (unsigned)( 2 + ( last ? 1 : 0 ) ), 3 );
        CompressBlock( d, g_staticLCode, g_staticLLen, g_staticDCode, g_staticDLen );
    }
    else
    {
        SendBits( d, (unsigned)( 4 + ( last ? 1 : 0 ) ), 3 );
        SendBits( d, (unsigned)( maxL + 1 - 257 ), 5 );
        SendBits( d, (unsigned)( maxD + 1 - 1 ), 5 );
        SendBits( d, (unsigned)( maxBl + 1 - 4 ), 4 );
        for ( i = 0; i <= maxBl; i++ )
            SendBits( d, d->blLen[ blOrder[i] ], 3 );
        SendTree( d, d->lLen, maxL );
        SendTree( d, d->dLen, maxD );
        CompressBlock( d, d->lCode, d->lLen, d->dCode, d->dLen );
    }

    if ( last ) AlignToByte( d );
    d->blockStart = (long)d->strstart;
    InitBlock( d );
}

/*===========================================================================
 * The parse
 *===========================================================================*/
static unsigned Hash3( const Byte *p )
{
    return ( ( (unsigned)p[0] << 10 ) ^ ( (unsigned)p[1] << 5 ) ^ p[2] ) & HASH_MASK;
}

/* Put position 'pos' at the head of its hash chain; returns the old head. */
static unsigned InsertString( DeflEnc *d, unsigned pos )
{
    unsigned h = Hash3( d->window + pos );
    unsigned old = d->head[h];
    d->prev[pos & WMASK] = (UInt16)old;
    d->head[h] = (UInt16)pos;
    return old;
}

static unsigned LongestMatch( DeflEnc *d, unsigned curMatch )
{
    unsigned chain = MAX_CHAIN;
    const Byte *scan = d->window + d->strstart;
    const Byte *strend = scan + MAX_MATCH;
    unsigned bestLen = d->prevLength;
    unsigned nice = NICE_MATCH;
    unsigned limit = ( d->strstart > MAX_DIST ) ? d->strstart - MAX_DIST : NIL;
    Byte scanEnd1 = scan[bestLen - 1];
    Byte scanEnd  = scan[bestLen];

    if ( d->prevLength >= GOOD_MATCH ) chain >>= 2;
    if ( nice > d->lookahead ) nice = d->lookahead;

    do {
        const Byte *match = d->window + curMatch;
        const Byte *s;
        unsigned    len;

        if ( match[bestLen] != scanEnd || match[bestLen - 1] != scanEnd1 ||
             match[0] != scan[0] || match[1] != scan[1] )
            continue;

        s = scan + 2;
        match += 2;
        while ( s < strend && *s == *match ) { s++; match++; }
        len = (unsigned)( s - scan );

        if ( len > bestLen )
        {
            d->matchStart = curMatch;
            bestLen = len;
            if ( len >= nice ) break;
            scanEnd1 = scan[bestLen - 1];
            scanEnd  = scan[bestLen];
        }
    } while ( ( curMatch = d->prev[curMatch & WMASK] ) > limit && --chain != 0 );

    return ( bestLen <= d->lookahead ) ? bestLen : d->lookahead;
}

/* Record a symbol; returns 1 when the block is full. */
static int TallyLit( DeflEnc *d, unsigned c )
{
    d->dBuf[d->lastLit] = 0;
    d->lBuf[d->lastLit++] = (Byte)c;
    d->lFreq[c]++;
    return d->lastLit == LIT_BUFSIZE - 1;
}

static int TallyDist( DeflEnc *d, unsigned dist, unsigned lenMinus3 )
{
    d->dBuf[d->lastLit] = (UInt16)dist;
    d->lBuf[d->lastLit++] = (Byte)lenMinus3;
    dist--;
    d->lFreq[ g_lengthCode[lenMinus3] + 257 ]++;
    d->dFreq[ DCode( dist ) ]++;
    return d->lastLit == LIT_BUFSIZE - 1;
}

/* Run the lazy parse over what is in the window.  With 'finish' clear it
 * stops while MIN_LOOKAHEAD bytes are still unparsed, since a match at those
 * positions could run on into input that has not arrived yet. */
static void Parse( DeflEnc *d, int finish )
{
    for ( ;; )
    {
        unsigned hashHead = NIL;

        if ( d->lookahead < MIN_LOOKAHEAD )
        {
            if ( !finish || d->lookahead == 0 ) break;
        }

        if ( d->lookahead >= MIN_MATCH )
            hashHead = InsertString( d, d->strstart );

        d->prevLength  = d->matchLength;
        d->prevMatch   = d->matchStart;
        d->matchLength = MIN_MATCH - 1;

        if ( hashHead != NIL && d->prevLength < MAX_MATCH &&
             d->strstart - hashHead <= MAX_DIST )
        {
            d->matchLength = LongestMatch( d, hashHead );
            if ( d->matchLength == MIN_MATCH &&
                 d->strstart - d->matchStart > TOO_FAR )
                d->matchLength = MIN_MATCH - 1;
        }

        if ( d->prevLength >= MIN_MATCH && d->matchLength <= d->prevLength )
        {
            /* The match that started one position back is the better one. */
            unsigned maxInsert = d->strstart + d->lookahead - MIN_MATCH;
            int      full;

            full = TallyDist( d, d->strstart - 1 - d->prevMatch,
                              d->prevLength - MIN_MATCH );
            d->lookahead -= d->prevLength - 1;
            d->prevLength -= 2;
            do {
                if ( ++d->strstart <= maxInsert )
                    InsertString( d, d->strstart );
            } while ( --d->prevLength != 0 );
            d->matchAvailable = 0;
            d->matchLength = MIN_MATCH - 1;
            d->strstart++;
            if ( full ) FlushBlock( d, 0 );
        }
        else if ( d->matchAvailable )
        {
            /* No better match here: the previous byte goes as a literal. */
            if ( TallyLit( d, d->window[d->strstart - 1] ) ) FlushBlock( d, 0 );
            d->strstart++;
            d->lookahead--;
        }
        else
        {
            d->matchAvailable = 1;
            d->strstart++;
            d->lookahead--;
        }
    }
}

/* Move the upper half of the window down when the parse nears the top, and
 * every chain entry with it.  Entries that would fall off the bottom become
 * NIL, which ends a chain. */
static void Slide( DeflEnc *d )
{
    unsigned n;

    memcpy( d->window, d->window + WSIZE, WSIZE );
    d->matchStart -= WSIZE;
    d->prevMatch  -= WSIZE;
    d->strstart   -= WSIZE;
    d->blockStart -= (long)WSIZE;
    for ( n = 0; n < HASH_SIZE; n++ )
    {
        unsigned m = d->head[n];
        d->head[n] = (UInt16)( m >= WSIZE ? m - WSIZE : NIL );
    }
    for ( n = 0; n < WSIZE; n++ )
    {
        unsigned m = d->prev[n];
        d->prev[n] = (UInt16)( m >= WSIZE ? m - WSIZE : NIL );
    }
}

/*===========================================================================
 * Public
 *===========================================================================*/
UInt32 DeflMemNeeded( void )
{
    return (UInt32)sizeof( DeflEnc ) + 2 * WSIZE + MAX_MATCH + 8
         + HASH_SIZE * 2 + WSIZE * 2 + LIT_BUFSIZE * 3 + OUT_BUFSIZE;
}

int DeflCreate( DeflEnc **out )
{
    DeflEnc *d;

    *out = 0;
    BuildTables();
    d = (DeflEnc *)calloc( 1, sizeof( DeflEnc ) );
    if ( !d ) return SZ_ERR_MEMORY;
    d->window = (Byte *)malloc( 2 * WSIZE + MAX_MATCH + 8 );
    d->head   = (UInt16 *)malloc( HASH_SIZE * sizeof( UInt16 ) );
    d->prev   = (UInt16 *)malloc( WSIZE * sizeof( UInt16 ) );
    d->lBuf   = (Byte *)malloc( LIT_BUFSIZE );
    d->dBuf   = (UInt16 *)malloc( LIT_BUFSIZE * sizeof( UInt16 ) );
    d->out    = (Byte *)malloc( OUT_BUFSIZE );
    if ( !d->window || !d->head || !d->prev || !d->lBuf || !d->dBuf || !d->out )
    {
        DeflFree( d );
        return SZ_ERR_MEMORY;
    }
    *out = d;
    return SZ_OK;
}

void DeflReset( DeflEnc *d, DeflWrite write, void *user )
{
    /* The slack past the window is compared against but never coded from;
     * zeroing it keeps those comparisons deterministic. */
    memset( d->window, 0, 2 * WSIZE + MAX_MATCH + 8 );
    memset( d->head, 0, HASH_SIZE * sizeof( UInt16 ) );
    memset( d->prev, 0, WSIZE * sizeof( UInt16 ) );
    d->strstart = 0;
    d->lookahead = 0;
    d->blockStart = 0;
    d->matchStart = 0;
    d->matchLength = MIN_MATCH - 1;
    d->prevLength = MIN_MATCH - 1;
    d->prevMatch = 0;
    d->matchAvailable = 0;
    d->bitBuf = 0;
    d->bitCount = 0;
    d->outPos = 0;
    d->outTotal = 0;
    d->write = write;
    d->user = user;
    d->failed = 0;
    InitBlock( d );
}

int DeflFeed( DeflEnc *d, const Byte *data, UInt32 len )
{
    while ( len > 0 && !d->failed )
    {
        unsigned room;
        unsigned n;

        /* Keep MAX_DIST of history behind the parse and the whole lookahead
         * ahead of it: slide once the parse is in the upper half. */
        if ( d->strstart >= WSIZE + MAX_DIST ) Slide( d );

        room = 2 * WSIZE - d->strstart - d->lookahead;
        if ( room == 0 )
        {
            /* Cannot happen once the parse has run, but be sure of it. */
            Parse( d, 0 );
            if ( d->strstart >= WSIZE + MAX_DIST ) Slide( d );
            room = 2 * WSIZE - d->strstart - d->lookahead;
            if ( room == 0 ) break;
        }
        n = ( len < room ) ? (unsigned)len : room;
        memcpy( d->window + d->strstart + d->lookahead, data, n );
        d->lookahead += n;
        data += n;
        len  -= n;
        Parse( d, 0 );
    }
    return d->failed ? SZ_ERR_WRITE : SZ_OK;
}

int DeflFinish( DeflEnc *d )
{
    Parse( d, 1 );
    if ( d->matchAvailable )
    {
        TallyLit( d, d->window[d->strstart - 1] );
        d->matchAvailable = 0;
    }
    FlushBlock( d, 1 );
    FlushOut( d );
    return d->failed ? SZ_ERR_WRITE : SZ_OK;
}

UInt32 DeflOutBytes( DeflEnc *d )
{
    return d->outTotal + d->outPos;
}

void DeflFree( DeflEnc *d )
{
    if ( !d ) return;
    free( d->window );
    free( d->head );
    free( d->prev );
    free( d->lBuf );
    free( d->dBuf );
    free( d->out );
    free( d );
}
