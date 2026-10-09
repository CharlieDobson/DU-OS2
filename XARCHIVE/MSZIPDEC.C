/*===========================================================================
 * MSZIPDEC.C  -  MSZIP block decoder (deflate with a carried-over window)
 * Target: MSVC 2.2  Win32s, and the Open Watcom DOS/32A and OS/2 builds
 *
 * Written from RFC 1951; see MSZIPDEC.H for what MSZIP adds to deflate.
 *
 * Decoding is table-driven: a code of up to MZ_FAST_BITS bits is resolved by
 * one lookup, and the rare longer one walks the canonical code lengths.
 * ZIPARC.C's inflater used to resolve every code a bit at a time; it was
 * made table-driven the same way on 2026-10-07, after a zip measured about
 * thirty times slower to test than the same files in a cabinet.  The two
 * still share nothing but the RFC: a zip streams from a file through the
 * decryption layer, an MSZIP block arrives whole in memory.
 *===========================================================================*/

#include <stdlib.h>
#include <string.h>

#include "mszipdec.h"

#define MZ_WSIZE      32768U
#define MZ_WMASK      ( MZ_WSIZE - 1 )
#define MZ_MAXBITS    15
#define MZ_FAST_BITS  9
#define MZ_FAST_SIZE  ( 1U << MZ_FAST_BITS )
#define MZ_LITLEN     288
#define MZ_DIST       32

/* A fast-table entry holds the code length above a 9-bit symbol, so that 0
 * can mean "longer than MZ_FAST_BITS" - no real code has length 0. */
#define MZ_FAST_ENTRY( len, sym )  (UInt16)( ( (len) << 9 ) | (sym) )

typedef struct {
    UInt16 count[MZ_MAXBITS + 1];       /* how many codes of each length    */
    UInt16 symbol[MZ_LITLEN];           /* symbols in canonical order       */
    UInt16 fast[MZ_FAST_SIZE];
} MzHuff;

struct MszipDec {
    Byte   window[MZ_WSIZE];            /* the last 32 KB of output          */
    UInt32 wpos;                        /* next write position in it         */
    UInt32 history;                     /* bytes output so far, capped at 32K*/
    MzHuff lit, dist;                   /* the current block's trees         */
    MzHuff fixedLit, fixedDist;         /* the fixed trees, built once       */
};

/* Bits come out least significant first, as RFC 1951 orders them. */
typedef struct {
    const Byte *in;
    UInt32      inLen;
    UInt32      inPos;
    UInt32      bitBuf;                 /* the next bit is bit 0             */
    int         bitCnt;
    UInt32      fake;                   /* zero bytes supplied past the end  */
} MzBits;

static const UInt16 s_lenBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const Byte s_lenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const UInt16 s_distBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const Byte s_distExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const Byte s_clOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/*---- Bits ------------------------------------------------------------------ *
 * Running off the end of the block supplies zero bytes rather than failing on
 * the spot, because the decoder looks ahead MZ_MAXBITS bits for every code
 * and the last code of a block is usually shorter than that.  Whether any of
 * those made-up bits were actually CONSUMED is what MzOverran answers, and
 * that is checked at the end of every deflate block.
 *--------------------------------------------------------------------------- */
static void MzNeed( MzBits *br, int n )
{
    while ( br->bitCnt < n )
    {
        UInt32 byte = 0;

        if ( br->inPos < br->inLen ) byte = br->in[br->inPos++];
        else                         br->fake++;
        br->bitBuf |= byte << br->bitCnt;
        br->bitCnt += 8;
    }
}

static UInt32 MzGetBits( MzBits *br, int n )
{
    UInt32 val;

    if ( n == 0 ) return 0;
    MzNeed( br, n );
    val = br->bitBuf & ( ( 1UL << n ) - 1 );
    br->bitBuf >>= n;
    br->bitCnt -= n;
    return val;
}

static int MzOverran( const MzBits *br )
{
    return ( br->inPos + br->fake ) * 8 - (UInt32)br->bitCnt > br->inLen * 8;
}

/*---- Huffman trees ---------------------------------------------------------
 * Over-subscribed length sets are refused.  Incomplete ones are allowed, as
 * RFC 1951 itself allows a distance tree with a single code; a bit pattern
 * that is no code at all is caught when it is decoded.
 *--------------------------------------------------------------------------- */
static int MzBuild( MzHuff *tree, const Byte *lens, int n )
{
    UInt16 offs[MZ_MAXBITS + 2];
    UInt32 next[MZ_MAXBITS + 2];
    UInt32 code;
    int    len, sym, left;

    memset( tree->count, 0, sizeof( tree->count ) );
    for ( sym = 0; sym < n; sym++ )
        tree->count[lens[sym]]++;
    tree->count[0] = 0;

    left = 1;
    for ( len = 1; len <= MZ_MAXBITS; len++ )
    {
        left <<= 1;
        left -= tree->count[len];
        if ( left < 0 ) return -1;
    }

    offs[1] = 0;
    for ( len = 1; len < MZ_MAXBITS; len++ )
        offs[len + 1] = (UInt16)( offs[len] + tree->count[len] );
    for ( sym = 0; sym < n; sym++ )
        if ( lens[sym] ) tree->symbol[offs[lens[sym]]++] = (UInt16)sym;

    /* Codes short enough for the table, filed under their bits as they will
     * arrive - RFC 1951 sends a code's most significant bit first, into a
     * stream read least significant first, so the index is the code reversed. */
    memset( tree->fast, 0, sizeof( tree->fast ) );
    code = 0;
    for ( len = 1; len <= MZ_MAXBITS; len++ )
    {
        code = ( code + tree->count[len - 1] ) << 1;
        next[len] = code;
    }
    for ( sym = 0; sym < n; sym++ )
    {
        UInt32 rev = 0, val;
        int    i;

        len = lens[sym];
        if ( !len || len > MZ_FAST_BITS ) continue;
        val = next[len]++;
        for ( i = 0; i < len; i++ )
        {
            rev = ( rev << 1 ) | ( val & 1 );
            val >>= 1;
        }
        for ( ; rev < MZ_FAST_SIZE; rev += 1UL << len )
            tree->fast[rev] = MZ_FAST_ENTRY( len, sym );
    }
    return 0;
}

static int MzDecode( MzBits *br, const MzHuff *tree )
{
    UInt16 entry;
    int    len, code, first, index, count;

    MzNeed( br, MZ_MAXBITS );
    entry = tree->fast[br->bitBuf & ( MZ_FAST_SIZE - 1 )];
    if ( entry )
    {
        len = entry >> 9;
        br->bitBuf >>= len;
        br->bitCnt -= len;
        return entry & 0x1FF;
    }

    /* Longer than the table: walk the lengths, one code bit at a time.
     * 'code' never falls below 'first', so the subtraction cannot wrap. */
    code = first = index = 0;
    for ( len = 1; len <= MZ_MAXBITS; len++ )
    {
        code |= (int)( ( br->bitBuf >> ( len - 1 ) ) & 1 );
        count = tree->count[len];
        if ( code - first < count )
        {
            br->bitBuf >>= len;
            br->bitCnt -= len;
            return tree->symbol[index + code - first];
        }
        index += count;
        first  = ( first + count ) << 1;
        code <<= 1;
    }
    return -1;
}

/*---- Output ----------------------------------------------------------------*/
static void MzPut( MszipDec *dec, Byte *out, UInt32 *outPos, Byte val )
{
    out[(*outPos)++] = val;
    dec->window[dec->wpos] = val;
    dec->wpos = ( dec->wpos + 1 ) & MZ_WMASK;
    if ( dec->history < MZ_WSIZE ) dec->history++;
}

/*---- Block bodies ----------------------------------------------------------*/
static int MzStored( MszipDec *dec, MzBits *br,
                     Byte *out, UInt32 outCap, UInt32 *outPos )
{
    UInt32 len, nlen;

    MzGetBits( br, br->bitCnt & 7 );            /* to a byte boundary */
    len  = MzGetBits( br, 16 );
    nlen = MzGetBits( br, 16 );
    if ( len != ( ~nlen & 0xFFFFUL ) ) return SZ_ERR_DATA;
    if ( len > outCap - *outPos ) return SZ_ERR_DATA;
    while ( len-- )
        MzPut( dec, out, outPos, (Byte)MzGetBits( br, 8 ) );
    return SZ_OK;
}

static int MzCodes( MszipDec *dec, MzBits *br,
                    const MzHuff *lit, const MzHuff *dist,
                    Byte *out, UInt32 outCap, UInt32 *outPos )
{
    for ( ;; )
    {
        int sym = MzDecode( br, lit );

        /* A runaway guard as well as a check: four made-up bytes is more
         * look-ahead than any real code needs, so garbage cannot spin here
         * decoding zeros until the output fills. */
        if ( sym < 0 || br->fake > 4 ) return SZ_ERR_DATA;

        if ( sym < 256 )
        {
            if ( *outPos >= outCap ) return SZ_ERR_DATA;
            MzPut( dec, out, outPos, (Byte)sym );
        }
        else if ( sym == 256 )
            return SZ_OK;
        else
        {
            UInt32 len, distance, src;
            int    di;

            sym -= 257;
            if ( sym >= 29 ) return SZ_ERR_DATA;
            len = s_lenBase[sym] + MzGetBits( br, s_lenExtra[sym] );
            di  = MzDecode( br, dist );
            if ( di < 0 || di >= 30 ) return SZ_ERR_DATA;
            distance = s_distBase[di] + MzGetBits( br, s_distExtra[di] );

            /* Back past the start of the data is corruption, not a reference
             * to the zeroes the window happens to start with. */
            if ( distance > dec->history ) return SZ_ERR_DATA;
            if ( len > outCap - *outPos ) return SZ_ERR_DATA;

            src = ( dec->wpos - distance ) & MZ_WMASK;
            while ( len-- )
            {
                Byte val = dec->window[src];

                src = ( src + 1 ) & MZ_WMASK;
                MzPut( dec, out, outPos, val );
            }
        }
    }
}

/* The header of a dynamic block: the code-length code, then the two trees.
 * The code-length tree borrows dec->dist, which is rebuilt straight after. */
static int MzDynamic( MszipDec *dec, MzBits *br )
{
    Byte lens[MZ_LITLEN + MZ_DIST];
    Byte clLens[19];
    int  nlit, ndist, ncl, i, idx;

    nlit  = (int)MzGetBits( br, 5 ) + 257;
    ndist = (int)MzGetBits( br, 5 ) + 1;
    ncl   = (int)MzGetBits( br, 4 ) + 4;

    memset( clLens, 0, sizeof( clLens ) );
    for ( i = 0; i < ncl; i++ )
        clLens[s_clOrder[i]] = (Byte)MzGetBits( br, 3 );
    if ( MzBuild( &dec->dist, clLens, 19 ) ) return SZ_ERR_DATA;

    idx = 0;
    while ( idx < nlit + ndist )
    {
        int  sym = MzDecode( br, &dec->dist );
        int  rep;
        Byte val;

        if ( sym < 0 || br->fake > 4 ) return SZ_ERR_DATA;
        if ( sym < 16 )
        {
            lens[idx++] = (Byte)sym;
            continue;
        }
        if ( sym == 16 )
        {
            if ( idx == 0 ) return SZ_ERR_DATA;      /* nothing to repeat */
            val = lens[idx - 1];
            rep = 3 + (int)MzGetBits( br, 2 );
        }
        else if ( sym == 17 )
        {
            val = 0;
            rep = 3 + (int)MzGetBits( br, 3 );
        }
        else
        {
            val = 0;
            rep = 11 + (int)MzGetBits( br, 7 );
        }
        if ( idx + rep > nlit + ndist ) return SZ_ERR_DATA;
        while ( rep-- ) lens[idx++] = val;
    }

    if ( lens[256] == 0 ) return SZ_ERR_DATA;       /* no end-of-block code */
    if ( MzBuild( &dec->lit, lens, nlit ) ) return SZ_ERR_DATA;
    if ( MzBuild( &dec->dist, lens + nlit, ndist ) ) return SZ_ERR_DATA;
    return SZ_OK;
}

/*---- Public ----------------------------------------------------------------*/
int MszipCreate( MszipDec **out )
{
    MszipDec *dec;
    Byte      lens[MZ_LITLEN];
    int       i;

    *out = NULL;
    dec = (MszipDec *)calloc( 1, sizeof( MszipDec ) );
    if ( !dec ) return SZ_ERR_MEMORY;

    for ( i = 0;   i < 144; i++ ) lens[i] = 8;
    for ( i = 144; i < 256; i++ ) lens[i] = 9;
    for ( i = 256; i < 280; i++ ) lens[i] = 7;
    for ( i = 280; i < 288; i++ ) lens[i] = 8;
    MzBuild( &dec->fixedLit, lens, MZ_LITLEN );
    for ( i = 0; i < MZ_DIST; i++ ) lens[i] = 5;
    MzBuild( &dec->fixedDist, lens, MZ_DIST );

    *out = dec;
    return SZ_OK;
}

void MszipFree( MszipDec *dec )
{
    if ( dec ) free( dec );
}

int MszipDecodeBlock( MszipDec *dec, const Byte *in, UInt32 inLen,
                      Byte *out, UInt32 outCap, UInt32 *outLen,
                      UInt32 *inUsed )
{
    MzBits br;
    UInt32 pos = 0;
    int    final, type, rc;

    *outLen = 0;
    if ( inUsed ) *inUsed = 0;
    if ( inLen < 2 || in[0] != 'C' || in[1] != 'K' ) return SZ_ERR_DATA;

    br.in     = in + 2;
    br.inLen  = inLen - 2;
    br.inPos  = 0;
    br.bitBuf = 0;
    br.bitCnt = 0;
    br.fake   = 0;

    do
    {
        final = (int)MzGetBits( &br, 1 );
        type  = (int)MzGetBits( &br, 2 );

        if ( type == 0 )
            rc = MzStored( dec, &br, out, outCap, &pos );
        else if ( type == 1 )
            rc = MzCodes( dec, &br, &dec->fixedLit, &dec->fixedDist,
                          out, outCap, &pos );
        else if ( type == 2 )
        {
            rc = MzDynamic( dec, &br );
            if ( rc == SZ_OK )
                rc = MzCodes( dec, &br, &dec->lit, &dec->dist,
                              out, outCap, &pos );
        }
        else
            rc = SZ_ERR_DATA;

        if ( rc != SZ_OK ) return rc;
        if ( MzOverran( &br ) ) return SZ_ERR_DATA;
    }
    while ( !final );

    *outLen = pos;
    if ( inUsed )
    {
        UInt32 bits = ( br.inPos + br.fake ) * 8 - (UInt32)br.bitCnt;

        *inUsed = 2 + ( bits + 7 ) / 8;
    }
    return SZ_OK;
}

UInt32 MszipMemNeeded( void )
{
    return (UInt32)sizeof( MszipDec );
}
