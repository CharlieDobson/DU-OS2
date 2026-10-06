/*===========================================================================
 * LZXDEC.C  -  LZX decoder, in the form cabinets use it
 * Target: MSVC 2.2  Win32s, and the Open Watcom DOS/32A and OS/2 builds
 *
 * Written fresh from the format's public description - Microsoft's [MS-PATCH]
 * specification documents LZX, and a cabinet's LZX is that minus the reset
 * interval and the reference data - with no code taken from another decoder.
 * See LZXDEC.H for how a cabinet feeds it.
 *
 * The bit stream is a sequence of 16-bit little-endian words, each read from
 * its most significant bit down.  Inside it:
 *
 *   folder start   1 bit: is E8 translation on?  If so, 32 bits of "file
 *                  size" that the translation uses as its range.
 *   block header   3 bits type, 24 bits uncompressed length, then per type:
 *     verbatim     the main tree's lengths in two runs (the 256 literals,
 *                  then the match headers) and the length tree's, each run
 *                  sent through its own 20-symbol pretree as DELTAS against
 *                  the previous block's lengths;
 *     aligned      eight 3-bit lengths for the aligned-offset tree, then as
 *                  verbatim;
 *     stored       padding to the next word, the three repeated offsets as
 *                  raw 32-bit values, then the bytes themselves.
 *
 * A main-tree symbol below 256 is a literal.  Above it, the low three bits
 * are the match length (7 means "and more, from the length tree") and the
 * rest is a position slot: slots 0-2 reuse one of the three recent offsets,
 * the rest carry their own, with extra bits read raw or, in an aligned
 * block, split between raw bits and the aligned tree.
 *===========================================================================*/

#include <stdlib.h>
#include <string.h>

#include "lzxdec.h"

#define LZX_NUM_CHARS       256
#define LZX_MIN_MATCH       2
#define LZX_PRIMARY_LENS    7
#define LZX_PRETREE_SYMS    20
#define LZX_ALIGNED_SYMS    8
#define LZX_LENGTH_SYMS     249
#define LZX_MAX_SLOTS       50
#define LZX_MAIN_MAX        ( LZX_NUM_CHARS + LZX_MAX_SLOTS * 8 )
#define LZX_MAXBITS         16

/* A run of zero lengths may overshoot the end of the range being read - the
 * format does not forbid it, and the only harm is to lengths past the end,
 * which the next range overwrites or nothing reads.  The length arrays have
 * this much room beyond their symbols so an overshoot lands somewhere. */
#define LZX_LEN_SLACK       64

#define LZX_BLOCK_VERBATIM  1
#define LZX_BLOCK_ALIGNED   2
#define LZX_BLOCK_STORED    3

#define LZX_MAIN_FAST       10
#define LZX_LENGTH_FAST     10
#define LZX_ALIGNED_FAST    7
#define LZX_PRE_FAST        6

/* Code length above a 10-bit symbol; 0 means "longer than the table". */
#define LX_FAST_ENTRY( len, sym )  (UInt16)( ( (len) << 10 ) | (sym) )

typedef struct {
    UInt16  count[LZX_MAXBITS + 1];      /* how many codes of each length     */
    UInt32  first[LZX_MAXBITS + 1];      /* the lowest code of each length    */
    UInt16  start[LZX_MAXBITS + 2];      /* where that length's symbols begin */
    UInt16 *symbol;                      /* symbols in canonical order        */
    UInt16 *fast;                        /* 2^fastBits entries                */
    int     fastBits;
} LxHuff;

struct LzxDec {
    Byte   *window;
    UInt32  wsize, wmask;
    UInt32  wpos;                        /* next write position               */
    UInt32  total;                       /* bytes out since the folder began  */
    UInt32  framePos;                    /* where the current frame began     */
    UInt32  frames;                      /* frames finished                   */
    int     numSlots, mainSyms;

    UInt32  r0, r1, r2;                  /* the three repeated offsets        */
    int     headerDone;
    Int32   e8Size;                      /* 0 = no E8 translation             */

    int     blockType;
    UInt32  blockLen, blockLeft;
    int     padPending;                  /* a stored block's pad byte is owed */

    Byte    mainLen[LZX_MAIN_MAX + LZX_LEN_SLACK];
    Byte    lengthLen[LZX_LENGTH_SYMS + LZX_LEN_SLACK];
    Byte    alignedLen[LZX_ALIGNED_SYMS];

    LxHuff  mainTree, lengthTree, alignedTree, preTree;
    UInt16  mainSym[LZX_MAIN_MAX],      mainFast[1 << LZX_MAIN_FAST];
    UInt16  lengthSym[LZX_LENGTH_SYMS], lengthFast[1 << LZX_LENGTH_FAST];
    UInt16  alignedSym[LZX_ALIGNED_SYMS], alignedFast[1 << LZX_ALIGNED_FAST];
    UInt16  preSym[LZX_PRETREE_SYMS],   preFast[1 << LZX_PRE_FAST];

    UInt32  slotBase[LZX_MAX_SLOTS];
    Byte    slotBits[LZX_MAX_SLOTS];
};

/*---- Bits ------------------------------------------------------------------ *
 * Running off the end of the frame's data supplies zero words rather than
 * failing at once, because every code is decoded with 16 bits of look-ahead.
 * Whether any made-up bit was CONSUMED is what LxOverran answers at the end.
 *--------------------------------------------------------------------------- */
typedef struct {
    const Byte *in;
    UInt32      inLen, inPos;
    UInt32      buf;                     /* the next bit is bit 31            */
    int         cnt;
    UInt32      fake;                    /* zero bytes supplied past the end  */
} LxBits;

static void LxNeed( LxBits *br, int n )             /* n <= 17 */
{
    while ( br->cnt < n )
    {
        UInt32 word;

        if ( br->inPos + 2 <= br->inLen )
        {
            word = br->in[br->inPos] | ( (UInt32)br->in[br->inPos + 1] << 8 );
            br->inPos += 2;
        }
        else if ( br->inPos < br->inLen )           /* an odd last byte */
        {
            word = br->in[br->inPos++];
            br->fake++;
        }
        else
        {
            word = 0;
            br->fake += 2;
        }
        br->buf |= word << ( 16 - br->cnt );
        br->cnt += 16;
    }
}

#define LX_PEEK( br, n )  ( (br)->buf >> ( 32 - (n) ) )

static void LxDrop( LxBits *br, int n )
{
    br->buf <<= n;
    br->cnt -= n;
}

static UInt32 LxGetBits( LxBits *br, int n )
{
    UInt32 val;

    if ( n == 0 ) return 0;
    LxNeed( br, n );
    val = LX_PEEK( br, n );
    LxDrop( br, n );
    return val;
}

static int LxOverran( const LxBits *br )
{
    return ( br->inPos + br->fake ) * 8 - (UInt32)br->cnt > br->inLen * 8;
}

/* A stored block starts on the next word boundary, and LZX always spends 1 to
 * 16 bits getting there - never 0, so a stream already on a boundary skips a
 * whole word.  From then on the block is read as bytes. */
static int LxAlignStored( LxBits *br )
{
    int    pad  = ( br->cnt & 15 ) ? ( br->cnt & 15 ) : 16;
    UInt32 bits = ( br->inPos + br->fake ) * 8 - (UInt32)br->cnt + (UInt32)pad;

    br->buf   = 0;
    br->cnt   = 0;
    br->fake  = 0;
    br->inPos = bits / 8;
    return ( br->inPos <= br->inLen ) ? SZ_OK : SZ_ERR_DATA;
}

static UInt32 LxLe32( const Byte *ptr )
{
    return ptr[0] | ( (UInt32)ptr[1] << 8 ) |
           ( (UInt32)ptr[2] << 16 ) | ( (UInt32)ptr[3] << 24 );
}

/*---- Huffman trees ---------------------------------------------------------
 * Canonical codes, sent most significant bit first.  Over-subscribed length
 * sets are refused; incomplete ones are allowed, because LZX's length tree is
 * legitimately EMPTY in a block with no long matches, and an unused bit
 * pattern is caught when something tries to decode it.
 *--------------------------------------------------------------------------- */
static int LxBuild( LxHuff *tree, const Byte *lens, int n )
{
    UInt32 next[LZX_MAXBITS + 1];
    UInt16 fill[LZX_MAXBITS + 1];
    UInt32 code;
    int    len, sym, left;
    UInt32 tableSize = 1UL << tree->fastBits;

    memset( tree->count, 0, sizeof( tree->count ) );
    for ( sym = 0; sym < n; sym++ )
        tree->count[lens[sym]]++;
    tree->count[0] = 0;

    left = 1;
    for ( len = 1; len <= LZX_MAXBITS; len++ )
    {
        left <<= 1;
        left -= tree->count[len];
        if ( left < 0 ) return -1;
    }

    code = 0;
    tree->start[1] = 0;
    for ( len = 1; len <= LZX_MAXBITS; len++ )
    {
        code = ( code + tree->count[len - 1] ) << 1;
        tree->first[len]     = code;
        next[len]         = code;
        tree->start[len + 1] = (UInt16)( tree->start[len] + tree->count[len] );
        fill[len]         = tree->start[len];
    }
    for ( sym = 0; sym < n; sym++ )
        if ( lens[sym] ) tree->symbol[fill[lens[sym]]++] = (UInt16)sym;

    memset( tree->fast, 0, tableSize * sizeof( UInt16 ) );
    for ( sym = 0; sym < n; sym++ )
    {
        UInt32 base, span, i;

        len = lens[sym];
        if ( !len || len > tree->fastBits ) continue;
        span = 1UL << ( tree->fastBits - len );
        base = next[len]++ << ( tree->fastBits - len );
        for ( i = 0; i < span; i++ )
            tree->fast[base + i] = LX_FAST_ENTRY( len, sym );
    }
    return 0;
}

static int LxDecode( LxBits *br, const LxHuff *tree )
{
    UInt16 entry;
    UInt32 code;
    int    len;

    LxNeed( br, LZX_MAXBITS );
    entry = tree->fast[LX_PEEK( br, tree->fastBits )];
    if ( entry )
    {
        len = entry >> 10;
        LxDrop( br, len );
        return entry & 0x3FF;
    }

    /* A code longer than the table.  Every code of fastBits or fewer would
     * have hit the table, so the search starts one past it; the unsigned
     * subtraction rejects a prefix below the length's first code. */
    for ( len = tree->fastBits + 1; len <= LZX_MAXBITS; len++ )
    {
        code = LX_PEEK( br, len );
        if ( code - tree->first[len] < tree->count[len] )
        {
            LxDrop( br, len );
            return tree->symbol[tree->start[len] + code - tree->first[len]];
        }
    }
    return -1;
}

/* One range of tree lengths through a fresh pretree.  Each pretree symbol is
 * a DELTA against the length the same symbol had in the previous block:
 *   0-16  one length, (old - delta) mod 17
 *   17    4..19 zeroes
 *   18    20..51 zeroes
 *   19    4..5 copies of one delta-coded length, worked out once from the
 *         first position of the run */
static int LxReadLens( LzxDec *dec, LxBits *br, Byte *lens, int cap,
                       int first, int last )
{
    Byte pre[LZX_PRETREE_SYMS];
    int  i, pos, sym, run, val;

    for ( i = 0; i < LZX_PRETREE_SYMS; i++ )
        pre[i] = (Byte)LxGetBits( br, 4 );
    if ( LxBuild( &dec->preTree, pre, LZX_PRETREE_SYMS ) ) return SZ_ERR_DATA;

    pos = first;
    while ( pos < last )
    {
        sym = LxDecode( br, &dec->preTree );
        if ( sym < 0 || br->fake > 8 ) return SZ_ERR_DATA;

        if ( sym == 17 || sym == 18 )
        {
            run = ( sym == 17 ) ? 4 + (int)LxGetBits( br, 4 )
                                : 20 + (int)LxGetBits( br, 5 );
            if ( pos + run > cap ) return SZ_ERR_DATA;
            while ( run-- ) lens[pos++] = 0;
        }
        else if ( sym == 19 )
        {
            run = 4 + (int)LxGetBits( br, 1 );
            sym = LxDecode( br, &dec->preTree );
            if ( sym < 0 || sym > 16 ) return SZ_ERR_DATA;
            val = lens[pos] - sym;
            if ( val < 0 ) val += 17;
            if ( pos + run > cap ) return SZ_ERR_DATA;
            while ( run-- ) lens[pos++] = (Byte)val;
        }
        else
        {
            val = lens[pos] - sym;
            if ( val < 0 ) val += 17;
            lens[pos++] = (Byte)val;
        }
    }
    return SZ_OK;
}

static int LxBlockHeader( LzxDec *dec, LxBits *br )
{
    UInt32 hi, lo;
    int    i, rc;

    dec->blockType = (int)LxGetBits( br, 3 );
    hi = LxGetBits( br, 16 );
    lo = LxGetBits( br, 8 );
    dec->blockLen  = ( hi << 8 ) | lo;
    dec->blockLeft = dec->blockLen;

    switch ( dec->blockType )
    {
    case LZX_BLOCK_ALIGNED:
        for ( i = 0; i < LZX_ALIGNED_SYMS; i++ )
            dec->alignedLen[i] = (Byte)LxGetBits( br, 3 );
        if ( LxBuild( &dec->alignedTree, dec->alignedLen, LZX_ALIGNED_SYMS ) )
            return SZ_ERR_DATA;
        /* and the rest is a verbatim block's header */

    case LZX_BLOCK_VERBATIM:
        rc = LxReadLens( dec, br, dec->mainLen, (int)sizeof( dec->mainLen ),
                         0, LZX_NUM_CHARS );
        if ( rc == SZ_OK )
            rc = LxReadLens( dec, br, dec->mainLen, (int)sizeof( dec->mainLen ),
                             LZX_NUM_CHARS, dec->mainSyms );
        if ( rc == SZ_OK &&
             LxBuild( &dec->mainTree, dec->mainLen, dec->mainSyms ) )
            rc = SZ_ERR_DATA;
        if ( rc == SZ_OK )
            rc = LxReadLens( dec, br, dec->lengthLen,
                             (int)sizeof( dec->lengthLen ),
                             0, LZX_LENGTH_SYMS );
        if ( rc == SZ_OK &&
             LxBuild( &dec->lengthTree, dec->lengthLen, LZX_LENGTH_SYMS ) )
            rc = SZ_ERR_DATA;
        return rc;

    case LZX_BLOCK_STORED:
        if ( LxAlignStored( br ) != SZ_OK ) return SZ_ERR_DATA;
        if ( br->inLen - br->inPos < 12 ) return SZ_ERR_DATA;
        dec->r0 = LxLe32( br->in + br->inPos );
        dec->r1 = LxLe32( br->in + br->inPos + 4 );
        dec->r2 = LxLe32( br->in + br->inPos + 8 );
        br->inPos += 12;
        return SZ_OK;

    default:
        return SZ_ERR_DATA;
    }
}

/*---- Block bodies ----------------------------------------------------------
 * Both decode exactly 'run' bytes.  A match that would run past that crosses
 * the end of the block or of the frame, which no LZX stream does.
 *--------------------------------------------------------------------------- */
static int LxStored( LzxDec *dec, LxBits *br, UInt32 run )
{
    if ( br->inPos > br->inLen || br->inLen - br->inPos < run )
        return SZ_ERR_DATA;
    while ( run-- )
    {
        dec->window[dec->wpos] = br->in[br->inPos++];
        dec->wpos = ( dec->wpos + 1 ) & dec->wmask;
        dec->total++;
    }
    return SZ_OK;
}

static int LxCompressed( LzxDec *dec, LxBits *br, UInt32 run )
{
    Byte  *win  = dec->window;
    UInt32 mask = dec->wmask;

    while ( run > 0 )
    {
        int    sym = LxDecode( br, &dec->mainTree );
        UInt32 len, offset, slot, src, left;

        if ( sym < 0 || br->fake > 8 ) return SZ_ERR_DATA;

        if ( sym < LZX_NUM_CHARS )
        {
            win[dec->wpos] = (Byte)sym;
            dec->wpos = ( dec->wpos + 1 ) & mask;
            dec->total++;
            run--;
            continue;
        }

        sym -= LZX_NUM_CHARS;
        len = (UInt32)( sym & LZX_PRIMARY_LENS );
        if ( len == LZX_PRIMARY_LENS )
        {
            int more = LxDecode( br, &dec->lengthTree );

            if ( more < 0 ) return SZ_ERR_DATA;
            len += (UInt32)more;
        }
        len += LZX_MIN_MATCH;

        slot = (UInt32)sym >> 3;
        if ( slot > 2 )
        {
            UInt32 extra = dec->slotBits[slot];

            offset = dec->slotBase[slot] - 2;
            if ( dec->blockType == LZX_BLOCK_ALIGNED && extra >= 3 )
            {
                int aligned;

                offset += LxGetBits( br, (int)extra - 3 ) << 3;
                aligned = LxDecode( br, &dec->alignedTree );
                if ( aligned < 0 ) return SZ_ERR_DATA;
                offset += (UInt32)aligned;
            }
            else
                offset += LxGetBits( br, (int)extra );

            dec->r2 = dec->r1;
            dec->r1 = dec->r0;
            dec->r0 = offset;
        }
        else if ( slot == 0 )
            offset = dec->r0;
        else if ( slot == 1 )
        {
            offset  = dec->r1;
            dec->r1 = dec->r0;
            dec->r0 = offset;
        }
        else
        {
            offset  = dec->r2;
            dec->r2 = dec->r0;
            dec->r0 = offset;
        }

        if ( len > run ) return SZ_ERR_DATA;
        if ( offset == 0 || offset > dec->total || offset > dec->wsize )
            return SZ_ERR_DATA;

        src  = ( dec->wpos - offset ) & mask;
        left = len;
        while ( left-- )
        {
            win[dec->wpos] = win[src];
            src       = ( src + 1 ) & mask;
            dec->wpos = ( dec->wpos + 1 ) & mask;
        }
        dec->total += len;
        run        -= len;
    }
    return SZ_OK;
}

/*---- E8 translation --------------------------------------------------------
 * The compressor rewrote the 32-bit target of every E8 byte (an x86 CALL, or
 * anything that looks like one) from relative to absolute, so that calls to
 * the same function compress as the same bytes.  This undoes it on the
 * frame's OUTPUT copy only: the window must keep what was really decoded,
 * because later matches refer to that.  The last ten bytes of a frame are
 * never translated, and nothing is after the first 32768 frames (1 GB).
 *--------------------------------------------------------------------------- */
static void LxUndoE8( Byte *data, UInt32 len, UInt32 framePos, Int32 fileSize )
{
    Byte *ptr = data;
    Byte *end = data + len - 10;
    Int32 cur = (Int32)framePos;

    while ( ptr < end )
    {
        Int32 absolute, rel;

        if ( *ptr++ != 0xE8 )
        {
            cur++;
            continue;
        }
        absolute = (Int32)LxLe32( ptr );
        if ( absolute >= -cur && absolute < fileSize )
        {
            rel = ( absolute >= 0 ) ? absolute - cur : absolute + fileSize;
            ptr[0] = (Byte)rel;
            ptr[1] = (Byte)( rel >> 8 );
            ptr[2] = (Byte)( rel >> 16 );
            ptr[3] = (Byte)( rel >> 24 );
        }
        ptr += 4;
        cur += 5;
    }
}

/*---- Public ----------------------------------------------------------------*/
int LzxCreate( int windowBits, LzxDec **out )
{
    /* Position slots per window size, 2^15 .. 2^21. */
    static const int slotsFor[7] = { 30, 32, 34, 36, 38, 42, 50 };
    LzxDec *dec;
    int     slot;

    *out = NULL;
    if ( windowBits < LZX_MIN_WINDOW || windowBits > LZX_MAX_WINDOW )
        return SZ_ERR_UNSUPPORTED;

    dec = (LzxDec *)calloc( 1, sizeof( LzxDec ) );
    if ( !dec ) return SZ_ERR_MEMORY;
    dec->wsize  = 1UL << windowBits;
    dec->wmask  = dec->wsize - 1;
    dec->window = (Byte *)calloc( 1, dec->wsize );
    if ( !dec->window )
    {
        free( dec );
        return SZ_ERR_MEMORY;
    }

    dec->numSlots = slotsFor[windowBits - LZX_MIN_WINDOW];
    dec->mainSyms = LZX_NUM_CHARS + dec->numSlots * 8;
    dec->r0 = dec->r1 = dec->r2 = 1;

    /* Extra bits per slot: 0,0,0,0,1,1,2,2,3,3 ... rising every second slot
     * and stopping at 17; each slot's base is the last one's plus its span. */
    for ( slot = 0; slot < LZX_MAX_SLOTS; slot++ )
    {
        int bits = ( slot < 4 ) ? 0 : ( slot - 2 ) >> 1;

        dec->slotBits[slot] = (Byte)( ( bits > 17 ) ? 17 : bits );
        dec->slotBase[slot] = ( slot == 0 ) ? 0
                         : dec->slotBase[slot - 1] + ( 1UL << dec->slotBits[slot - 1] );
    }

    dec->mainTree.symbol    = dec->mainSym;
    dec->mainTree.fast      = dec->mainFast;
    dec->mainTree.fastBits  = LZX_MAIN_FAST;
    dec->lengthTree.symbol  = dec->lengthSym;
    dec->lengthTree.fast    = dec->lengthFast;
    dec->lengthTree.fastBits = LZX_LENGTH_FAST;
    dec->alignedTree.symbol = dec->alignedSym;
    dec->alignedTree.fast   = dec->alignedFast;
    dec->alignedTree.fastBits = LZX_ALIGNED_FAST;
    dec->preTree.symbol     = dec->preSym;
    dec->preTree.fast       = dec->preFast;
    dec->preTree.fastBits   = LZX_PRE_FAST;

    *out = dec;
    return SZ_OK;
}

void LzxFree( LzxDec *dec )
{
    if ( !dec ) return;
    if ( dec->window ) free( dec->window );
    free( dec );
}

int LzxDecodeFrame( LzxDec *dec, const Byte *in, UInt32 inLen,
                    Byte *out, UInt32 outLen )
{
    LxBits br;
    UInt32 frameStart = dec->wpos;
    UInt32 done       = 0;
    int    rc;

    if ( outLen > LZX_FRAME_SIZE ) return SZ_ERR_DATA;

    br.in    = in;
    br.inLen = inLen;
    br.inPos = 0;
    br.buf   = 0;
    br.cnt   = 0;
    br.fake  = 0;

    /* The pad byte an odd-length stored block ended the previous frame with,
     * when that frame's data ran out before it. */
    if ( dec->padPending )
    {
        if ( inLen == 0 ) return SZ_ERR_DATA;
        br.inPos = 1;
        dec->padPending = 0;
    }

    if ( !dec->headerDone )
    {
        if ( LxGetBits( &br, 1 ) )
        {
            UInt32 hi = LxGetBits( &br, 16 );
            UInt32 lo = LxGetBits( &br, 16 );

            dec->e8Size = (Int32)( ( hi << 16 ) | lo );
        }
        dec->headerDone = 1;
    }

    while ( done < outLen )
    {
        UInt32 run;

        if ( dec->blockLeft == 0 )
        {
            rc = LxBlockHeader( dec, &br );
            if ( rc != SZ_OK ) return rc;
            if ( br.fake > 8 ) return SZ_ERR_DATA;
            continue;                     /* an empty block is legal */
        }

        run = dec->blockLeft;
        if ( run > outLen - done ) run = outLen - done;

        rc = ( dec->blockType == LZX_BLOCK_STORED )
           ? LxStored( dec, &br, run )
           : LxCompressed( dec, &br, run );
        if ( rc != SZ_OK ) return rc;

        dec->blockLeft -= run;
        done           += run;

        /* An odd-length stored block is followed by a pad byte that keeps the
         * stream on a word boundary.  It is the next byte of this frame's data
         * when there is one, and the first byte of the next frame's when not. */
        if ( dec->blockType == LZX_BLOCK_STORED && dec->blockLeft == 0 &&
             ( dec->blockLen & 1 ) )
        {
            if ( br.inPos < br.inLen ) br.inPos++;
            else                       dec->padPending = 1;
        }
    }

    if ( LxOverran( &br ) ) return SZ_ERR_DATA;

    /* Out of the window, which the frame may have wrapped around. */
    {
        UInt32 first = dec->wsize - frameStart;

        if ( first >= outLen )
            memcpy( out, dec->window + frameStart, outLen );
        else
        {
            memcpy( out, dec->window + frameStart, first );
            memcpy( out + first, dec->window, outLen - first );
        }
    }

    if ( dec->e8Size && outLen > 10 && dec->frames < 32768UL )
        LxUndoE8( out, outLen, dec->framePos, dec->e8Size );

    dec->framePos += outLen;
    dec->frames++;
    return SZ_OK;
}

UInt32 LzxMemNeeded( int windowBits )
{
    if ( windowBits < LZX_MIN_WINDOW ) windowBits = LZX_MIN_WINDOW;
    if ( windowBits > LZX_MAX_WINDOW ) windowBits = LZX_MAX_WINDOW;
    return (UInt32)sizeof( LzxDec ) + ( 1UL << windowBits );
}
