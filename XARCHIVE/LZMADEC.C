/*===========================================================================
 * LZMADEC.C  -  LZMA1 and LZMA2 decoders (buffered and streaming)
 * Target: MSVC 2.2  Win32s
 *
 * A clean implementation of the LZMA1 range-coder / sliding-dictionary
 * algorithm (the de-facto format defined by Igor Pavlov's public-domain LZMA
 * SDK).
 *
 * Everything runs through two small I/O layers, so one decode loop serves both
 * calling styles (see LZMADEC.H):
 *
 *   CIn  - compressed input: either a buffer the caller already holds, or a
 *          read callback pulled through a small staging buffer.
 *   CWin - the dictionary/output: either the caller's whole output buffer (no
 *          wrapping, nothing copied), or a ring buffer of the coder's
 *          dictionary size whose contents are handed to an emit callback each
 *          time it wraps.  Distances index backwards through it either way,
 *          which is what makes the streaming mode cost O(dictionary) instead
 *          of O(uncompressed size).
 *
 * The core decode loop is factored into a resettable state object (CLzma +
 * LzmaRun) so that LZMA2 - which is a chunked wrapper around LZMA1 sharing one
 * dictionary - can drive it chunk by chunk.
 *===========================================================================*/

#include <stdlib.h>
#include <string.h>
#include "lzmadec.h"

/* Staging buffer for callback-fed input.  Big enough that the read callback is
 * not called per byte, small enough to be irrelevant next to the dictionary. */
#define LZ_IN_STAGE   16384

/*---- Model constants ----------------------------------------------------- */
#define kNumPosBitsMax      4
#define kNumStates          12
#define kNumLenToPosStates  4
#define kNumAlignBits       4
#define kEndPosModelIndex   14
#define kNumFullDistances   (1 << (kEndPosModelIndex >> 1))   /* 128 */
#define kMatchMinLen        2
#define kNumPosSlotBits     6

#define kLenNumLowBits      3
#define kLenNumMidBits      3
#define kLenNumHighBits     8
#define kLenNumLowSymbols   (1 << kLenNumLowBits)   /* 8 */
#define kLenNumMidSymbols   (1 << kLenNumMidBits)   /* 8 */

/* Length-coder sub-offsets (relative to a length coder base) */
#define LenChoice   0
#define LenChoice2  1
#define LenLow      2
#define LenMid      (LenLow + ((1 << kNumPosBitsMax) * kLenNumLowSymbols)) /* 130 */
#define LenHigh     (LenMid + ((1 << kNumPosBitsMax) * kLenNumMidSymbols)) /* 258 */
#define kNumLenProbs (LenHigh + (1 << kLenNumHighBits))                    /* 514 */

/* Probability-array layout (offsets into the single probs[] block) */
#define oIsMatch     0
#define oIsRep       (oIsMatch + (kNumStates << kNumPosBitsMax))       /* 192 */
#define oIsRepG0     (oIsRep + kNumStates)
#define oIsRepG1     (oIsRepG0 + kNumStates)
#define oIsRepG2     (oIsRepG1 + kNumStates)
#define oIsRep0Long  (oIsRepG2 + kNumStates)
#define oPosSlot     (oIsRep0Long + (kNumStates << kNumPosBitsMax))
#define oSpecPos     (oPosSlot + (kNumLenToPosStates << kNumPosSlotBits))
#define oAlign       (oSpecPos + kNumFullDistances - kEndPosModelIndex)
#define oLenCoder    (oAlign + (1 << kNumAlignBits))
#define oRepLenCoder (oLenCoder + kNumLenProbs)
#define oLiteral     (oRepLenCoder + kNumLenProbs)

/* Range coder constants */
#define kNumBitModelTotalBits 11
#define kBitModelTotal       (1 << kNumBitModelTotalBits)   /* 2048 */
#define kNumMoveBits         5
#define kTopValue            (1 << 24)

/*---- Compressed input ---------------------------------------------------- *
 * Either a buffer the caller already holds (read == NULL) or a callback that
 * refills a staging buffer.  'remaining' is what is left of the declared
 * packed size, so a corrupt stream cannot read past its own data.
 *-------------------------------------------------------------------------- */
typedef struct {
    LzmaRead    read;          /* NULL: the whole stream is already in memory */
    void       *user;
    const Byte *p, *end;       /* bytes available right now                   */
    Byte       *stage;         /* owned when read != NULL                     */
    UInt32      remaining;     /* packed bytes not yet pulled from 'read'     */
} CIn;

/* Ensure at least one byte is available.  0 = end of the packed stream. */
static int InFill( CIn *s )
{
    UInt32 n;

    if ( s->p < s->end ) return 1;
    if ( !s->read || s->remaining == 0 ) return 0;

    n = ( s->remaining < LZ_IN_STAGE ) ? s->remaining : LZ_IN_STAGE;
    n = s->read( s->user, s->stage, n );
    if ( n == 0 ) return 0;

    s->remaining -= n;
    s->p   = s->stage;
    s->end = s->stage + n;
    return 1;
}

/* Skip 'n' bytes of the packed stream (used to step over a chunk's unread
 * tail).  0 if the stream ended early. */
static int InSkip( CIn *s, UInt32 n )
{
    while ( n )
    {
        UInt32 have;
        if ( !InFill( s ) ) return 0;
        have = (UInt32)( s->end - s->p );
        if ( have > n ) have = n;
        s->p += have;
        n    -= have;
    }
    return 1;
}

/*---- Range decoder ------------------------------------------------------- *
 * 'avail' is this range-coded stream's byte budget: the whole packed size for
 * LZMA1, one chunk's packed size for LZMA2.
 *-------------------------------------------------------------------------- */
typedef struct {
    CIn        *in;
    UInt32      avail;
    UInt32      range;
    UInt32      code;
    int         corrupted;
} CRangeDec;

static Byte RcByte( CRangeDec *rc )
{
    if ( rc->avail == 0 || !InFill( rc->in ) )
    {
        rc->corrupted = 1;
        return 0;
    }
    rc->avail--;
    return *rc->in->p++;
}

static int RcInit( CRangeDec *rc )
{
    int  i;
    Byte first;

    rc->corrupted = 0;
    if ( rc->avail < 5 )
        return 0;
    first = RcByte( rc );
    if ( first != 0 )
        rc->corrupted = 1;          /* first byte of an LZMA stream is zero */
    rc->code  = 0;
    rc->range = 0xFFFFFFFFUL;
    for ( i = 0; i < 4; i++ )
        rc->code = ( rc->code << 8 ) | RcByte( rc );
    return 1;                       /* corruption is reported by the caller */
}

/*---- Output window ------------------------------------------------------- *
 * emit == NULL: 'win' IS the caller's output buffer, sized to the whole
 * stream, and nothing is ever copied or wrapped.
 * emit != NULL: 'win' is a ring of the dictionary size; each time it fills,
 * the block is handed to the sink and writing continues at the start.
 * 'total' is the absolute output position - the value LZMA's position context
 * and the caller's limits are expressed in.
 *-------------------------------------------------------------------------- */
typedef struct {
    Byte    *win;
    UInt32   size;             /* bytes in win                               */
    UInt32   pos;              /* next write index within win                */
    UInt32   flushed;          /* index of the first byte not yet emitted    */
    UInt32   total;            /* absolute bytes produced                    */
    /* Absolute position of the most recent DICTIONARY RESET.  LZMA's position
     * context and its "how far back may a match reach" limit are both counted
     * from here, not from the start of the stream - which is what lets an
     * LZMA2 stream reset its dictionary part-way through.  Multi-threaded
     * 7-Zip does exactly that: it splits the input into independent blocks
     * and each one after the first begins with a reset. */
    UInt32   dictBase;
    LzmaEmit emit;             /* NULL in buffered mode                      */
    void    *user;
    int      err;              /* SZ_ERR_CANCEL once the sink says stop      */
} CWin;

/* Hand win[flushed..pos) to the sink. */
static int WinFlush( CWin *w )
{
    if ( w->emit && w->pos > w->flushed )
    {
        if ( !w->emit( w->user, w->win + w->flushed, w->pos - w->flushed ) )
        {
            w->err = SZ_ERR_CANCEL;
            return 0;
        }
    }
    w->flushed = w->pos;
    return 1;
}

/* How much output may accumulate before the sink is given some.
 *
 * The ring used to be handed over ONLY when it wrapped, which tied the size of
 * the first delivery to the size of the dictionary: with a 16 MB dictionary
 * nothing at all reached the sink - no filename for the progress box, no bytes
 * in any output file - until 16 MB had been decoded.  Measured in DOSBox on
 * one 1.8 MB archive, time before the first entry was even named:
 *
 *      1 MB dictionary   0 s        16 MB dictionary   45 s
 *      4 MB dictionary   1 s
 *
 * which is the bug a tester reported as "the extraction window is empty for
 * the first five seconds".  Flushing is unrelated to correctness: the bytes
 * stay in the ring for matches to reach back into, and 'flushed' only marks
 * how much of it the sink has seen.  So the delivery size can be chosen freely,
 * and 64K keeps the display honest without making the per-block overhead
 * (a sink call, a boundary walk, an fwrite) matter.
 *
 * A partial block was always possible - every stream ends with one - so no
 * sink needed changing for this. */
#define LZ_FLUSH_CHUNK 65536U

static int WinPutByte( CWin *w, Byte b )
{
    w->win[w->pos++] = b;
    w->total++;
    if ( w->pos == w->size )        /* ring full (or buffer complete) */
    {
        if ( !WinFlush( w ) ) return 0;
        w->pos = w->flushed = 0;
    }
    else if ( w->emit && w->pos - w->flushed >= LZ_FLUSH_CHUNK )
    {
        if ( !WinFlush( w ) ) return 0;
    }
    return 1;
}

/* The byte 'dist+1' positions back.  The caller has already checked that the
 * distance is within both the produced output and the window. */
static Byte WinBack( CWin *w, UInt32 dist )
{
    UInt32 d = dist + 1;
    return w->win[ ( w->pos >= d ) ? ( w->pos - d ) : ( w->pos + w->size - d ) ];
}

static void RcNormalize( CRangeDec *rc )
{
    if ( rc->range < kTopValue )
    {
        rc->range <<= 8;
        rc->code = ( rc->code << 8 ) | RcByte( rc );
    }
}

static unsigned RcBit( CRangeDec *rc, UInt16 *prob )
{
    unsigned v = *prob;
    UInt32   bound = ( rc->range >> kNumBitModelTotalBits ) * v;
    unsigned symbol;

    if ( rc->code < bound )
    {
        v += ( kBitModelTotal - v ) >> kNumMoveBits;
        rc->range = bound;
        symbol = 0;
    }
    else
    {
        v -= v >> kNumMoveBits;
        rc->code -= bound;
        rc->range -= bound;
        symbol = 1;
    }
    *prob = (UInt16)v;
    RcNormalize( rc );
    return symbol;
}

static UInt32 RcDirect( CRangeDec *rc, unsigned numBits )
{
    UInt32 res = 0;
    do
    {
        UInt32 t;
        rc->range >>= 1;
        rc->code -= rc->range;
        t = 0 - ( (UInt32)rc->code >> 31 );   /* 0xFFFFFFFF on borrow, else 0 */
        rc->code += rc->range & t;
        RcNormalize( rc );
        res <<= 1;
        res += t + 1;
    } while ( --numBits );
    return res;
}

static unsigned RcBitTree( CRangeDec *rc, UInt16 *probs, unsigned numBits )
{
    unsigned m = 1, i;
    for ( i = 0; i < numBits; i++ )
        m = ( m << 1 ) + RcBit( rc, probs + m );
    return m - ( (unsigned)1 << numBits );
}

static unsigned RcBitTreeRev( CRangeDec *rc, UInt16 *probs, unsigned numBits )
{
    unsigned m = 1, sym = 0, i;
    for ( i = 0; i < numBits; i++ )
    {
        unsigned bit = RcBit( rc, probs + m );
        m = ( m << 1 ) + bit;
        sym |= bit << i;
    }
    return sym;
}

static unsigned LenDecode( CRangeDec *rc, UInt16 *probs, unsigned posState )
{
    if ( RcBit( rc, probs + LenChoice ) == 0 )
        return RcBitTree( rc, probs + LenLow + ( posState << kLenNumLowBits ),
                          kLenNumLowBits );
    if ( RcBit( rc, probs + LenChoice2 ) == 0 )
        return kLenNumLowSymbols +
               RcBitTree( rc, probs + LenMid + ( posState << kLenNumMidBits ),
                          kLenNumMidBits );
    return kLenNumLowSymbols + kLenNumMidSymbols +
           RcBitTree( rc, probs + LenHigh, kLenNumHighBits );
}

/*===========================================================================
 * Resettable LZMA decode state
 *===========================================================================*/
typedef struct {
    UInt16  *probs;
    UInt32   numProbsAlloc;   /* probs[] capacity                            */
    UInt32   numProbs;        /* active prob count for current lc/lp         */
    unsigned lc, lp, pb;
    UInt32   pbMask, lpMask;
    unsigned state;
    UInt32   rep0, rep1, rep2, rep3;
} CLzma;

static UInt32 LzmaNumProbs( unsigned lc, unsigned lp )
{
    return oLiteral + ( (UInt32)0x300 << ( lc + lp ) );
}

/* Decode the lclppb byte; (re)allocate probs to fit.  SZ_ERR_* on failure. */
static int LzmaSetProps( CLzma *s, Byte d )
{
    unsigned lc, lp, pb;
    UInt32   need;

    if ( d >= 9 * 5 * 5 )
        return SZ_ERR_UNSUPPORTED;
    lc = d % 9;  d /= 9;
    lp = d % 5;  d /= 5;
    pb = d;

    need = LzmaNumProbs( lc, lp );
    if ( need > s->numProbsAlloc )
    {
        UInt16 *np = (UInt16 *)realloc( s->probs, need * sizeof( UInt16 ) );
        if ( !np )
            return SZ_ERR_MEMORY;
        s->probs        = np;
        s->numProbsAlloc = need;
    }
    s->numProbs = need;
    s->lc = lc;  s->lp = lp;  s->pb = pb;
    s->pbMask = ( (UInt32)1 << pb ) - 1;
    s->lpMask = ( (UInt32)1 << lp ) - 1;
    return SZ_OK;
}

/* Reset LZMA state, repeated distances and all probabilities to defaults. */
static void LzmaResetState( CLzma *s )
{
    UInt32 i;
    s->state = 0;
    s->rep0 = s->rep1 = s->rep2 = s->rep3 = 0;
    for ( i = 0; i < s->numProbs; i++ )
        s->probs[i] = kBitModelTotal >> 1;
}

/*
 * Decode from the current state into the window until the absolute output
 * position reaches limit (the end of the current LZMA1 stream or LZMA2 chunk).
 *
 * 'pos' is the absolute output position, which is what 'limit' and the
 * window are expressed in.  Everything the CODEC reasons about, though, is
 * counted from the last dictionary reset: the pos/literal context, and the
 * bound on how far back a match may reach.  That distinction is 'rel' below.
 * With no reset (LZMA1, or LZMA2 with one reset at the start) dictBase is 0
 * and rel == pos, exactly as before.
 */
static int LzmaRun( CLzma *s, CRangeDec *rc, CWin *w, UInt32 limit )
{
    UInt16  *probs  = s->probs;
    UInt32   pbMask = s->pbMask;
    UInt32   lpMask = s->lpMask;
    unsigned lc     = s->lc;
    unsigned state  = s->state;
    UInt32   rep0 = s->rep0, rep1 = s->rep1, rep2 = s->rep2, rep3 = s->rep3;
    UInt32   pos  = w->total;
    UInt32   base = w->dictBase;
    int      ret  = SZ_OK;

    while ( pos < limit )
    {
        UInt32   rel      = pos - base;
        unsigned posState = (unsigned)( rel & pbMask );
        UInt16  *prob = probs + oIsMatch + ( state << kNumPosBitsMax ) + posState;

        if ( RcBit( rc, prob ) == 0 )
        {
            /*---- literal ---------------------------------------------- */
            UInt16  *probLit;
            unsigned symbol = 1;
            Byte     prevByte = ( rel == 0 ) ? 0 : WinBack( w, 0 );
            unsigned litState =
                (unsigned)( ( ( rel & lpMask ) << lc ) + ( prevByte >> ( 8 - lc ) ) );

            probLit = probs + oLiteral + (UInt32)0x300 * litState;

            if ( state >= 7 )
            {
                Byte matchByte;
                if ( rep0 + 1 > rel || rep0 + 1 > w->size )
                    { ret = SZ_ERR_DATA; break; }
                matchByte = WinBack( w, rep0 );
                do
                {
                    unsigned matchBit = ( matchByte >> 7 ) & 1;
                    unsigned bit;
                    matchByte <<= 1;
                    bit = RcBit( rc, probLit + ( ( 1 + matchBit ) << 8 ) + symbol );
                    symbol = ( symbol << 1 ) | bit;
                    if ( matchBit != bit )
                        break;
                } while ( symbol < 0x100 );
            }
            while ( symbol < 0x100 )
                symbol = ( symbol << 1 ) | RcBit( rc, probLit + symbol );

            if ( !WinPutByte( w, (Byte)symbol ) ) { ret = w->err; break; }
            pos++;
            state = ( state < 4 ) ? 0 : ( state < 10 ) ? state - 3 : state - 6;
            continue;
        }

        /*---- match (rep or new) --------------------------------------- */
        {
            unsigned len;

            prob = probs + oIsRep + state;
            if ( RcBit( rc, prob ) != 0 )
            {
                /* repeated-distance match */
                if ( rel == 0 ) { ret = SZ_ERR_DATA; break; }

                prob = probs + oIsRepG0 + state;
                if ( RcBit( rc, prob ) == 0 )
                {
                    prob = probs + oIsRep0Long +
                           ( state << kNumPosBitsMax ) + posState;
                    if ( RcBit( rc, prob ) == 0 )
                    {
                        /* short rep: one byte from rep0 */
                        Byte b;
                        if ( rep0 + 1 > rel || rep0 + 1 > w->size )
                            { ret = SZ_ERR_DATA; break; }
                        state = ( state < 7 ) ? 9 : 11;
                        b = WinBack( w, rep0 );
                        if ( !WinPutByte( w, b ) ) { ret = w->err; break; }
                        pos++;
                        continue;
                    }
                }
                else
                {
                    UInt32 dist;
                    prob = probs + oIsRepG1 + state;
                    if ( RcBit( rc, prob ) == 0 )
                    {
                        dist = rep1;
                    }
                    else
                    {
                        prob = probs + oIsRepG2 + state;
                        if ( RcBit( rc, prob ) == 0 )
                        {
                            dist = rep2;
                        }
                        else
                        {
                            dist = rep3;
                            rep3 = rep2;
                        }
                        rep2 = rep1;
                    }
                    rep1 = rep0;
                    rep0 = dist;
                }
                len = LenDecode( rc, probs + oRepLenCoder, posState );
                state = ( state < 7 ) ? 8 : 11;
            }
            else
            {
                /* new match: decode length, then distance */
                UInt32   dist;
                unsigned posSlot, lenToPosState;

                rep3 = rep2; rep2 = rep1; rep1 = rep0;
                len = LenDecode( rc, probs + oLenCoder, posState );
                state = ( state < 7 ) ? 7 : 10;

                lenToPosState = ( len < kNumLenToPosStates )
                                ? len : kNumLenToPosStates - 1;
                posSlot = RcBitTree( rc,
                            probs + oPosSlot + ( lenToPosState << kNumPosSlotBits ),
                            kNumPosSlotBits );

                if ( posSlot < 4 )
                {
                    dist = posSlot;
                }
                else
                {
                    unsigned numDirectBits = (unsigned)( ( posSlot >> 1 ) - 1 );
                    dist = ( 2 | ( posSlot & 1 ) ) << numDirectBits;
                    if ( posSlot < kEndPosModelIndex )
                    {
                        dist += RcBitTreeRev( rc,
                                  probs + oSpecPos + dist - posSlot - 1,
                                  numDirectBits );
                    }
                    else
                    {
                        dist += RcDirect( rc, numDirectBits - kNumAlignBits )
                                << kNumAlignBits;
                        dist += RcBitTreeRev( rc, probs + oAlign, kNumAlignBits );
                    }
                }

                if ( dist == 0xFFFFFFFFUL )
                    break;            /* end-of-stream marker */

                rep0 = dist;
            }

            len += kMatchMinLen;

            /* the distance must point inside what we have already produced
             * SINCE THE LAST DICTIONARY RESET, and inside the window we hold */
            if ( rep0 + 1 > rel || rep0 + 1 > w->size )
                { ret = SZ_ERR_DATA; break; }
            if ( pos + len > limit )
                len = limit - pos;    /* clamp (defensive) */

            {
                int cancelled = 0;
                do
                {
                    Byte b = WinBack( w, rep0 );
                    if ( !WinPutByte( w, b ) ) { cancelled = 1; break; }
                    pos++;
                } while ( --len );
                if ( cancelled ) { ret = w->err; break; }
            }
        }
    }

    s->state = state;
    s->rep0 = rep0; s->rep1 = rep1; s->rep2 = rep2; s->rep3 = rep3;
    return ret;
}

/*===========================================================================
 * Shared set-up for the buffered and streaming entry points
 *===========================================================================*/

/* Wire an input source: a caller-held buffer, or a read callback with its
 * staging buffer.  Returns SZ_OK or SZ_ERR_MEMORY. */
static int InOpen( CIn *in, const Byte *src, UInt32 srcLen,
                   LzmaRead read, void *user, UInt32 packSize )
{
    in->read      = read;
    in->user      = user;
    in->stage     = NULL;
    in->remaining = 0;
    in->p = in->end = NULL;

    if ( read )
    {
        in->stage = (Byte *)malloc( LZ_IN_STAGE );
        if ( !in->stage ) return SZ_ERR_MEMORY;
        in->remaining = packSize;
    }
    else
    {
        in->p   = src;
        in->end = src + srcLen;
    }
    return SZ_OK;
}

static void InClose( CIn *in )
{
    if ( in->stage ) free( in->stage );
    in->stage = NULL;
}

/* Wire the output window: the caller's buffer, or a ring of the dictionary
 * size (never bigger than the output itself).  SZ_OK or SZ_ERR_MEMORY. */
static int WinOpen( CWin *w, Byte *dst, UInt32 dstLen,
                    UInt32 dictSize, UInt32 unpackSize,
                    LzmaEmit emit, void *user )
{
    w->pos = w->flushed = w->total = w->dictBase = 0;
    w->emit = emit;
    w->user = user;
    w->err  = SZ_OK;

    if ( !emit )                       /* buffered: dst is the dictionary */
    {
        w->win  = dst;
        w->size = dstLen ? dstLen : 1;
        return SZ_OK;
    }

    if ( dictSize < 4096 ) dictSize = 4096;
    if ( unpackSize && unpackSize < dictSize )
        dictSize = unpackSize;         /* a match can never reach further back */

    w->win = (Byte *)malloc( dictSize );
    if ( !w->win ) return SZ_ERR_MEMORY;
    w->size = dictSize;
    return SZ_OK;
}

static void WinClose( CWin *w )
{
    if ( w->emit && w->win ) free( w->win );
    w->win = NULL;
}

/*===========================================================================
 * LZMA1
 *===========================================================================*/
static int LzmaRunStream( const Byte *props, UInt32 propsSize,
                          CIn *in, UInt32 packSize, CWin *w, UInt32 unpackSize )
{
    CLzma     s;
    CRangeDec rc;
    int       rcCode;

    if ( propsSize < 5 )
        return SZ_ERR_UNSUPPORTED;

    s.probs         = NULL;
    s.numProbsAlloc = 0;
    s.numProbs      = 0;
    rcCode = LzmaSetProps( &s, props[0] );
    if ( rcCode != SZ_OK )
        return rcCode;
    LzmaResetState( &s );

    rc.in    = in;
    rc.avail = packSize;
    if ( !RcInit( &rc ) )
    {
        free( s.probs );
        return SZ_ERR_DATA;
    }

    rcCode = LzmaRun( &s, &rc, w, unpackSize );
    free( s.probs );
    if ( rcCode != SZ_OK )
        return rcCode;
    return rc.corrupted ? SZ_ERR_DATA : SZ_OK;
}

int LzmaDecode( const Byte *props, UInt32 propsSize,
                const Byte *src, UInt32 srcLen,
                Byte *dst, UInt32 dstLen )
{
    CIn  in;
    CWin w;
    int  rc;

    rc = InOpen( &in, src, srcLen, NULL, NULL, 0 );
    if ( rc != SZ_OK ) return rc;
    rc = WinOpen( &w, dst, dstLen, 0, 0, NULL, NULL );
    if ( rc != SZ_OK ) { InClose( &in ); return rc; }

    rc = LzmaRunStream( props, propsSize, &in, srcLen, &w, dstLen );

    WinClose( &w );
    InClose( &in );
    return rc;
}

int LzmaDecodeStream( const Byte *props, UInt32 propsSize,
                      LzmaRead read, void *readUser, UInt32 packSize,
                      UInt32 unpackSize, UInt32 dictSize,
                      LzmaEmit emit, void *emitUser )
{
    CIn  in;
    CWin w;
    int  rc;

    if ( !read || !emit ) return SZ_ERR_UNSUPPORTED;

    rc = InOpen( &in, NULL, 0, read, readUser, packSize );
    if ( rc != SZ_OK ) return rc;
    rc = WinOpen( &w, NULL, 0, dictSize, unpackSize, emit, emitUser );
    if ( rc != SZ_OK ) { InClose( &in ); return rc; }

    rc = LzmaRunStream( props, propsSize, &in, packSize, &w, unpackSize );
    if ( rc == SZ_OK && !WinFlush( &w ) ) rc = w.err;   /* tail of the ring */
    if ( rc == SZ_OK && w.total != unpackSize ) rc = SZ_ERR_DATA;

    WinClose( &w );
    InClose( &in );
    return rc;
}

/*===========================================================================
 * LZMA2 - public entry
 *
 * LZMA2 is a chunked container around LZMA1.  Each chunk carries a control
 * byte:
 *   0x00                      end of stream
 *   0x01                      uncompressed chunk, reset dictionary
 *   0x02                      uncompressed chunk, keep dictionary
 *   0x80..0xFF                LZMA chunk; bits 6-5 select the reset mode
 *                             (0 none, 1 state, 2 state+props, 3 state+props+
 *                             dict) and bits 4-0 are the high bits of the
 *                             uncompressed size.
 * Every LZMA chunk is an independent range-coded stream (its own 5-byte init),
 * but the dictionary (the dst buffer) and - unless reset - the LZMA state and
 * probabilities persist across chunks.  A dictionary reset is honoured
 * ANYWHERE in the stream, not just at position 0: multi-threaded 7-Zip
 * compresses in independent blocks and every block after the first opens
 * with one.  CWin::dictBase records where the current dictionary starts, and
 * LzmaRun counts its position context and match reach from there.
 *===========================================================================*/
static int Lzma2RunStream( CIn *in, CWin *w, UInt32 unpackTotal )
{
    CLzma s;
    int   haveProps = 0;
    int   rcCode    = SZ_OK;

    s.probs         = NULL;
    s.numProbsAlloc = 0;
    s.numProbs      = 0;

    for ( ;; )
    {
        Byte control, hdr[5];
        int  i;

        if ( !InFill( in ) ) { rcCode = SZ_ERR_DATA; break; }
        control = *in->p++;

        if ( control == 0 )
            break;                              /* end of LZMA2 stream */

        if ( control < 0x80 )
        {
            /*---- uncompressed chunk (control 1 = reset dict, 2 = keep) -- */
            UInt32 size;

            if ( control > 2 ) { rcCode = SZ_ERR_DATA; break; }
            for ( i = 0; i < 2; i++ )
            {
                if ( !InFill( in ) ) { rcCode = SZ_ERR_DATA; break; }
                hdr[i] = *in->p++;
            }
            if ( rcCode != SZ_OK ) break;

            size = ( ( (UInt32)hdr[0] << 8 ) | hdr[1] ) + 1;
            if ( control == 1 )
                w->dictBase = w->total;      /* reset dictionary here */
            if ( w->total + size > unpackTotal ) { rcCode = SZ_ERR_DATA; break; }

            while ( size )
            {
                if ( !InFill( in ) ) { rcCode = SZ_ERR_DATA; break; }
                if ( !WinPutByte( w, *in->p++ ) ) { rcCode = w->err; break; }
                size--;
            }
            if ( rcCode != SZ_OK ) break;
            /* A following LZMA chunk must reset state (spec); rely on its
             * control byte to request it. */
        }
        else
        {
            /*---- LZMA chunk ------------------------------------------- */
            unsigned  reset      = (unsigned)( ( control >> 5 ) & 3 );
            UInt32    unpackSize = (UInt32)( control & 0x1F ) << 16;
            UInt32    packSize;
            CRangeDec rc;
            UInt32    limit;

            for ( i = 0; i < 4; i++ )
            {
                if ( !InFill( in ) ) { rcCode = SZ_ERR_DATA; break; }
                hdr[i] = *in->p++;
            }
            if ( rcCode != SZ_OK ) break;

            unpackSize += ( ( (UInt32)hdr[0] << 8 ) | hdr[1] ) + 1;
            packSize    = ( ( (UInt32)hdr[2] << 8 ) | hdr[3] ) + 1;

            if ( reset >= 2 )
            {
                if ( !InFill( in ) ) { rcCode = SZ_ERR_DATA; break; }
                rcCode = LzmaSetProps( &s, *in->p++ );
                if ( rcCode != SZ_OK ) break;
                haveProps = 1;
            }
            if ( !haveProps ) { rcCode = SZ_ERR_DATA; break; }
            if ( reset == 3 )
                w->dictBase = w->total;         /* reset dictionary here */
            if ( reset >= 1 )
                LzmaResetState( &s );           /* state + reps + probs */

            limit = w->total + unpackSize;
            if ( limit > unpackTotal ) { rcCode = SZ_ERR_DATA; break; }

            rc.in    = in;
            rc.avail = packSize;
            if ( !RcInit( &rc ) ) { rcCode = SZ_ERR_DATA; break; }

            rcCode = LzmaRun( &s, &rc, w, limit );
            if ( rcCode != SZ_OK ) break;
            if ( rc.corrupted )       { rcCode = SZ_ERR_DATA; break; }
            if ( w->total != limit )  { rcCode = SZ_ERR_DATA; break; }

            /* step over any of the chunk's packed bytes the coder did not
             * consume, so the next control byte is where we expect it */
            if ( rc.avail && !InSkip( in, rc.avail ) )
                { rcCode = SZ_ERR_DATA; break; }
        }
    }

    free( s.probs );
    if ( rcCode == SZ_OK && w->total != unpackTotal )
        rcCode = SZ_ERR_DATA;
    return rcCode;
}

int Lzma2Decode( Byte dictProp,
                 const Byte *src, UInt32 srcLen,
                 Byte *dst, UInt32 dstLen )
{
    CIn  in;
    CWin w;
    int  rc;

    (void)dictProp;     /* size already validated against the dictionary cap */

    rc = InOpen( &in, src, srcLen, NULL, NULL, 0 );
    if ( rc != SZ_OK ) return rc;
    rc = WinOpen( &w, dst, dstLen, 0, 0, NULL, NULL );
    if ( rc != SZ_OK ) { InClose( &in ); return rc; }

    rc = Lzma2RunStream( &in, &w, dstLen );

    WinClose( &w );
    InClose( &in );
    return rc;
}

int Lzma2DecodeStream( Byte dictProp,
                       LzmaRead read, void *readUser, UInt32 packSize,
                       UInt32 unpackSize, UInt32 dictSize,
                       LzmaEmit emit, void *emitUser )
{
    CIn  in;
    CWin w;
    int  rc;

    (void)dictProp;     /* the caller derives dictSize from it */

    if ( !read || !emit ) return SZ_ERR_UNSUPPORTED;

    rc = InOpen( &in, NULL, 0, read, readUser, packSize );
    if ( rc != SZ_OK ) return rc;
    rc = WinOpen( &w, NULL, 0, dictSize, unpackSize, emit, emitUser );
    if ( rc != SZ_OK ) { InClose( &in ); return rc; }

    rc = Lzma2RunStream( &in, &w, unpackSize );
    if ( rc == SZ_OK && !WinFlush( &w ) ) rc = w.err;   /* tail of the ring */

    WinClose( &w );
    InClose( &in );
    return rc;
}
