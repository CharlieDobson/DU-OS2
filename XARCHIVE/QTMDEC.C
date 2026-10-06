/*===========================================================================
 * QTMDEC.C  -  Quantum decoder, in the form cabinets use it
 * Target: MSVC 2.2  Win32s, and the Open Watcom DOS/32A and OS/2 builds
 *
 * Written fresh from Matthew Russotto's description of the format (see
 * QTMDEC.H); no code was taken from another decoder.
 *
 * Every frame is a run of SELECTOR symbols, each from the same 7-symbol
 * model:
 *   0-3   a literal byte, from one of four 64-symbol models (one per quarter
 *         of the byte range: 00-3F, 40-7F, 80-BF, C0-FF)
 *   4     a 3-byte match: a position slot from its own model, then raw bits
 *   5     a 4-byte match, likewise
 *   6     a longer match: a length slot and raw bits (5..259 bytes), then a
 *         position slot and raw bits
 * There are no repeated-offset slots as there are in LZX.
 *
 * Each model is a list of (symbol, cumulative frequency) pairs, most frequent
 * first, ending in a zero sentinel.  Decoding a symbol adds 8 to its
 * frequency; when the total passes 3800 the frequencies are halved, and every
 * fiftieth halving re-sorts the list instead.  The decoder has to reproduce
 * the encoder's model EXACTLY, sort order included, or every symbol after the
 * first divergence comes out wrong - which is why the halving and the sort
 * are written out step for step below rather than with a library sort.
 *===========================================================================*/

#include <stdlib.h>
#include <string.h>

#include "qtmdec.h"

#define QT_FREQ_STEP     8
#define QT_RESCALE_AT    3800
#define QT_FIRST_SHIFTS  4       /* halvings before the first re-sort */
#define QT_LATER_SHIFTS  50      /* and between later ones            */

typedef struct {
    UInt16 sym;
    UInt16 cumfreq;
} QtSym;

typedef struct {
    int    shiftsLeft;
    int    entries;
    QtSym *syms;                 /* entries + 1; the last is the sentinel */
} QtModel;

struct QtmDec {
    Byte   *window;
    UInt32  wsize, wmask;
    UInt32  wpos;                /* next write position                   */
    UInt32  total;               /* bytes out since the folder began      */

    QtModel select;              /* what comes next: literal or match     */
    QtModel lit[4];              /* literals, a quarter of the bytes each */
    QtModel pos3, pos4;          /* position slots of 3- and 4-byte matches */
    QtModel posVar, lenVar;      /* position and length of longer ones    */

    QtSym   selectSyms[7 + 1];
    QtSym   litSyms[4][64 + 1];
    QtSym   pos3Syms[24 + 1], pos4Syms[36 + 1];
    QtSym   posVarSyms[42 + 1], lenVarSyms[27 + 1];
};

/* Position slots: each a base offset and a count of raw bits after it.  The
 * bits go 0,0,0,0,1,1,2,2 ... 19,19 and each base is the last plus its span. */
static const UInt32 s_posBase[42] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192,
    256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288,
    16384, 24576, 32768, 49152, 65536, 98304, 131072, 196608,
    262144, 393216, 524288, 786432, 1048576, 1572864
};
static const Byte s_posBits[42] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
    14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19
};

/* Length slots for selector 6, before its minimum of 5 is added. */
static const Byte s_lenBase[27] = {
    0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 18, 22, 26,
    30, 38, 46, 54, 62, 78, 94, 110, 126, 158, 190, 222, 254
};
static const Byte s_lenBits[27] = {
    0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

/*---- The input and the arithmetic coder ------------------------------------
 * Bits are taken most significant first, a byte at a time.  Past the end of
 * the frame's data zero bytes are supplied: the coder reads sixteen bits
 * ahead and the encoder need not have flushed all of them.
 *--------------------------------------------------------------------------- */
typedef struct {
    const Byte *in;
    UInt32      inLen, inPos;
    UInt32      buf;             /* the next bit is bit 31 */
    int         cnt;
    UInt32      fake;            /* zero bytes supplied past the end */
    UInt32      high, low, code; /* the coder's registers, 16 bits each */
} QtCoder;

static UInt32 QtGetBits( QtCoder *qc, int n )       /* n <= 19 */
{
    UInt32 val;

    if ( n == 0 ) return 0;
    while ( qc->cnt < n )
    {
        UInt32 byte = 0;

        if ( qc->inPos < qc->inLen ) byte = qc->in[qc->inPos++];
        else                         qc->fake++;
        qc->buf |= byte << ( 24 - qc->cnt );
        qc->cnt += 8;
    }
    val = qc->buf >> ( 32 - n );
    qc->buf <<= n;
    qc->cnt -= n;
    return val;
}

/*---- Models ----------------------------------------------------------------*/
static void QtInit( QtModel *model, QtSym *syms, int start, int count )
{
    int i;

    model->shiftsLeft = QT_FIRST_SHIFTS;
    model->entries    = count;
    model->syms       = syms;
    for ( i = 0; i <= count; i++ )
    {
        syms[i].sym     = (UInt16)( start + i );
        syms[i].cumfreq = (UInt16)( count - i );
    }
}

static void QtRescale( QtModel *model )
{
    QtSym *list = model->syms;
    int    i, j;

    if ( --model->shiftsLeft )
    {
        /* Halve, keeping every cumulative frequency strictly above the next
         * so no symbol is left with a frequency of zero. */
        for ( i = model->entries - 1; i >= 0; i-- )
        {
            list[i].cumfreq >>= 1;
            if ( list[i].cumfreq <= list[i + 1].cumfreq )
                list[i].cumfreq = (UInt16)( list[i + 1].cumfreq + 1 );
        }
        return;
    }

    model->shiftsLeft = QT_LATER_SHIFTS;

    /* To plain frequencies, halved rounding up.  Going upwards, list[i + 1] is
     * still cumulative when list[i] is converted, which is what is wanted. */
    for ( i = 0; i < model->entries; i++ )
    {
        list[i].cumfreq = (UInt16)( list[i].cumfreq - list[i + 1].cumfreq );
        list[i].cumfreq = (UInt16)( ( list[i].cumfreq + 1 ) >> 1 );
    }

    /* Most frequent first.  This must be exactly this exchange sort: equal
     * frequencies end up in the order the encoder's identical sort leaves
     * them, and a stable sort or a quicksort would not. */
    for ( i = 0; i < model->entries - 1; i++ )
        for ( j = i + 1; j < model->entries; j++ )
            if ( list[i].cumfreq < list[j].cumfreq )
            {
                QtSym swap = list[i];

                list[i] = list[j];
                list[j] = swap;
            }

    /* And back to cumulative, onto the zero sentinel. */
    for ( i = model->entries - 1; i >= 0; i-- )
        list[i].cumfreq = (UInt16)( list[i].cumfreq + list[i + 1].cumfreq );
}

static int QtSymbol( QtCoder *qc, QtModel *model )
{
    QtSym *list = model->syms;
    UInt32 range, total, target;
    int    i, sym;

    range  = ( ( qc->high - qc->low ) & 0xFFFF ) + 1;
    total  = list[0].cumfreq;
    target = ( ( ( ( qc->code - qc->low ) & 0xFFFF ) + 1 ) * total - 1 ) / range;

    for ( i = 1; i < model->entries; i++ )
        if ( list[i].cumfreq <= target ) break;
    sym = list[i - 1].sym;

    qc->high = ( qc->low + ( list[i - 1].cumfreq * range ) / total - 1 ) & 0xFFFF;
    qc->low  = ( qc->low + ( list[i].cumfreq * range ) / total ) & 0xFFFF;

    /* This symbol and everything listed before it carry its frequency. */
    do
        list[--i].cumfreq += QT_FREQ_STEP;
    while ( i > 0 );
    if ( list[0].cumfreq > QT_RESCALE_AT ) QtRescale( model );

    /* Renormalise: shift out every leading bit high and low agree on, and
     * handle the near-miss where they straddle the middle (01... against
     * 10...) by folding out the second bit instead. */
    for ( ;; )
    {
        if ( ( qc->low & 0x8000 ) != ( qc->high & 0x8000 ) )
        {
            if ( ( qc->low & 0x4000 ) && !( qc->high & 0x4000 ) )
            {
                qc->code ^= 0x4000;
                qc->low  &= 0x3FFF;
                qc->high |= 0x4000;
            }
            else
                break;
        }
        qc->low  = ( qc->low << 1 ) & 0xFFFF;
        qc->high = ( ( qc->high << 1 ) | 1 ) & 0xFFFF;
        qc->code = ( ( qc->code << 1 ) | QtGetBits( qc, 1 ) ) & 0xFFFF;
    }
    return sym;
}

/*---- Public ----------------------------------------------------------------*/
int QtmCreate( int windowBits, QtmDec **out )
{
    QtmDec *dec;
    int     slots = windowBits * 2;
    int     i;

    *out = NULL;
    if ( windowBits < QTM_MIN_WINDOW || windowBits > QTM_MAX_WINDOW )
        return SZ_ERR_UNSUPPORTED;

    dec = (QtmDec *)calloc( 1, sizeof( QtmDec ) );
    if ( !dec ) return SZ_ERR_MEMORY;
    dec->wsize  = 1UL << windowBits;
    dec->wmask  = dec->wsize - 1;
    dec->window = (Byte *)calloc( 1, dec->wsize );
    if ( !dec->window )
    {
        free( dec );
        return SZ_ERR_MEMORY;
    }

    /* The position models only hold the slots this window can reach. */
    QtInit( &dec->select, dec->selectSyms, 0, 7 );
    for ( i = 0; i < 4; i++ )
        QtInit( &dec->lit[i], dec->litSyms[i], i * 64, 64 );
    QtInit( &dec->pos3,   dec->pos3Syms,   0, ( slots > 24 ) ? 24 : slots );
    QtInit( &dec->pos4,   dec->pos4Syms,   0, ( slots > 36 ) ? 36 : slots );
    QtInit( &dec->posVar, dec->posVarSyms, 0, slots );
    QtInit( &dec->lenVar, dec->lenVarSyms, 0, 27 );

    *out = dec;
    return SZ_OK;
}

void QtmFree( QtmDec *dec )
{
    if ( !dec ) return;
    if ( dec->window ) free( dec->window );
    free( dec );
}

int QtmDecodeFrame( QtmDec *dec, const Byte *in, UInt32 inLen,
                    Byte *out, UInt32 outLen )
{
    QtCoder qc;
    UInt32  done = 0;

    if ( outLen > QTM_FRAME_SIZE ) return SZ_ERR_DATA;

    qc.in    = in;
    qc.inLen = inLen;
    qc.inPos = 0;
    qc.buf   = 0;
    qc.cnt   = 0;
    qc.fake  = 0;

    /* The coder starts afresh with every frame; the models do not. */
    qc.high = 0xFFFF;
    qc.low  = 0;
    qc.code = QtGetBits( &qc, 16 );

    while ( done < outLen )
    {
        int    sel, slot;
        UInt32 len, offset, src;

        /* A runaway guard: no frame legitimately needs this much past its
         * end, and garbage could otherwise decode zeroes to the frame end. */
        if ( qc.fake > 8 ) return SZ_ERR_DATA;

        sel = QtSymbol( &qc, &dec->select );
        if ( sel < 4 )
        {
            Byte val = (Byte)QtSymbol( &qc, &dec->lit[sel] );

            dec->window[dec->wpos] = val;
            dec->wpos = ( dec->wpos + 1 ) & dec->wmask;
            dec->total++;
            out[done++] = val;
            continue;
        }

        if ( sel == 4 )
        {
            slot = QtSymbol( &qc, &dec->pos3 );
            len  = 3;
        }
        else if ( sel == 5 )
        {
            slot = QtSymbol( &qc, &dec->pos4 );
            len  = 4;
        }
        else if ( sel == 6 )
        {
            int lslot = QtSymbol( &qc, &dec->lenVar );

            len  = s_lenBase[lslot] + QtGetBits( &qc, s_lenBits[lslot] ) + 5;
            slot = QtSymbol( &qc, &dec->posVar );
        }
        else
            return SZ_ERR_DATA;

        offset = s_posBase[slot] + QtGetBits( &qc, s_posBits[slot] ) + 1;

        /* A match never crosses the end of a frame, and never reaches back
         * past the start of the data. */
        if ( len > outLen - done ) return SZ_ERR_DATA;
        if ( offset > dec->total || offset > dec->wsize ) return SZ_ERR_DATA;

        src = ( dec->wpos - offset ) & dec->wmask;
        dec->total += len;
        while ( len-- )
        {
            Byte val = dec->window[src];

            src = ( src + 1 ) & dec->wmask;
            dec->window[dec->wpos] = val;
            dec->wpos = ( dec->wpos + 1 ) & dec->wmask;
            out[done++] = val;
        }
    }

    /* More than four bytes of made-up input consumed is a frame that was cut
     * short, not an encoder that flushed lightly. */
    if ( ( qc.inPos + qc.fake ) * 8 - (UInt32)qc.cnt > ( qc.inLen + 4 ) * 8 )
        return SZ_ERR_DATA;
    return SZ_OK;
}

UInt32 QtmMemNeeded( int windowBits )
{
    if ( windowBits < QTM_MIN_WINDOW ) windowBits = QTM_MIN_WINDOW;
    if ( windowBits > QTM_MAX_WINDOW ) windowBits = QTM_MAX_WINDOW;
    return (UInt32)sizeof( QtmDec ) + ( 1UL << windowBits );
}
