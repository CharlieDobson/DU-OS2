/*===========================================================================
 * SZARC.C  -  7z archive parsing and extraction (LZMA / LZMA2 / Copy + BCJ)
 * Target: MSVC 2.2  Win32s
 *===========================================================================*/

#include <windows.h>     /* WideCharToMultiByte */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>      /* _mkdir */

#include "szarc.h"
#include "lzmadec.h"
#include "crc32.h"
#include "platform.h"   /* SetFileMTime */

/*---- 7z property IDs ----------------------------------------------------- */
#define k7zEnd                  0x00
#define k7zHeader               0x01
#define k7zArchiveProperties    0x02
#define k7zAdditionalStreams    0x03
#define k7zMainStreamsInfo      0x04
#define k7zFilesInfo            0x05
#define k7zPackInfo             0x06
#define k7zUnpackInfo           0x07
#define k7zSubStreamsInfo       0x08
#define k7zSize                 0x09
#define k7zCRC                  0x0A
#define k7zFolder               0x0B
#define k7zCodersUnpackSize     0x0C
#define k7zNumUnpackStream      0x0D
#define k7zEmptyStream          0x0E
#define k7zEmptyFile            0x0F
#define k7zAnti                 0x10
#define k7zName                 0x11
#define k7zCTime                0x12
#define k7zATime                0x13
#define k7zMTime                0x14
#define k7zWinAttributes        0x15
#define k7zEncodedHeader        0x17

/* "Main" coders (read the packed stream) */
#define SZ_M_COPY   0
#define SZ_M_LZMA   1
#define SZ_M_LZMA2  2

/* Optional filter coder applied to the main coder's output */
#define SZ_F_NONE     0
#define SZ_F_BCJ_X86  1

/* Coder kinds, as recognised from the coder id while parsing a folder. */
#define SZ_C_UNKNOWN  0         /* parsed structurally, cannot be decoded    */
#define SZ_C_COPY     1
#define SZ_C_LZMA     2
#define SZ_C_LZMA2    3
#define SZ_C_BCJ_X86  4
#define SZ_C_BCJ2     5

/*---- Internal structures ------------------------------------------------- *
 * A folder is a small graph of coders.  Every coder is parsed STRUCTURALLY,
 * whatever its id: the layout (coder count, per-coder in/out stream counts,
 * bind pairs, packed-stream indices) is self-describing and independent of
 * the codec, so an unknown coder can be stepped over without losing the
 * reader's place in the header.  Only after the whole folder is parsed do we
 * ask whether it can actually be DECODED; if not, 'unsupported' is set and
 * the archive still opens and lists, with just that folder's entries failing
 * at extract time.  This is what keeps one PPMd or encrypted folder from
 * making the entire archive unopenable.
 *
 * Three shapes are decodable:
 *   1 coder    - Copy / LZMA / LZMA2 straight from the packed stream.
 *   2 coders   - the above plus a BCJ x86 filter over its output, bound so
 *                the main coder's output feeds the filter's input.
 *   4 coders   - BCJ2: three coders producing the main, call and jump
 *                streams, plus the BCJ2 coder itself whose fourth input is
 *                the raw range-coder packed stream.
 *-------------------------------------------------------------------------- */
typedef struct {
    int    kind;            /* SZ_C_*                                        */
    Byte   props[8];
    UInt32 propsSize;
    UInt32 numIn, numOut;
    UInt32 firstIn;         /* folder-local index of this coder's first in   */
    UInt32 firstOut;        /*   and first out stream                        */
    UInt32 unpackSize;      /* size of its output stream                     */
} SzCoder;

typedef struct {
    int    method;          /* SZ_M_COPY / SZ_M_LZMA / SZ_M_LZMA2            */
    int    filter;          /* SZ_F_NONE / SZ_F_BCJ_X86                      */
    Byte   props[8];
    UInt32 propsSize;
    UInt32 numCoders;
    UInt32 mainOutLocal;    /* out-stream index (0-based, folder-local) of   */
    UInt32 finalOutLocal;   /*   the main coder and the final output         */
    UInt32 mainUnpackSize;  /* main coder output size (LZMA decode buffer)   */
    UInt32 unpackSize;      /* folder final output size                      */
    int    hasCrc;
    UInt32 crc;

    /* Generic structure, filled for every folder including unsupported ones */
    int      unsupported;   /* 1 = parsed but not decodable                  */
    SzCoder  coders[SZ_MAX_CODERS];
    UInt32   numOutStreams; /* total across coders (CodersUnpackSize count)  */
    UInt32   numInStreams;
    UInt32   numPackStreams;/* packed streams this folder consumes           */
    UInt32   firstPackStream;/* index of the first, into a->packSizes[]      */
    /* packed stream n (folder-relative) feeds folder in-stream packToIn[n] */
    UInt32   packToIn[SZ_MAX_FOLDER_STRMS];
    /* bind pair b joins out-stream bpOut[b] to in-stream bpIn[b] */
    UInt32   numBindPairs;
    UInt32   bpIn[SZ_MAX_FOLDER_STRMS];
    UInt32   bpOut[SZ_MAX_FOLDER_STRMS];
    int      hasFinalOut;   /* exactly one unbound out-stream was found      */

    /* BCJ2 wiring, valid when isBcj2.  Each is a coder index, except rcPack
     * which is a folder-relative packed stream index. */
    int      isBcj2;
    int      bcj2Main, bcj2Call, bcj2Jump;
    UInt32   bcj2RcPack;
} SzFolder;

struct SzArchive {
    FILE    *fp;
    UInt32   baseOffset;                    /* body start (past start hdr)  */

    UInt32   packPos;
    UInt32   numPackStreams;
    UInt32   packSizes[SZ_MAX_PACKSTREAMS];
    UInt32   packOffset[SZ_MAX_PACKSTREAMS];/* base-relative cumulative     */

    UInt32   numFolders;
    SzFolder folders[SZ_MAX_FOLDERS];
    UInt32   numUnpack[SZ_MAX_FOLDERS];     /* substreams per folder        */

    /* The entry table is sized to the archive's actual file count once the
     * header declares it (SZ_MAX_FILES is only the refusal limit).  A fixed
     * table here would cost ~4.8 MB for every archive opened, however small. */
    int      numEntries;
    SzEntry *entries;
};

/*---- Buffer reader over the (decoded) header ----------------------------- */
typedef struct {
    const Byte *p;
    const Byte *end;
    int         err;
} CRdr;

typedef struct {
    UInt32 folder;
    UInt32 size;
    UInt32 offsetInFolder;
    int    hasCrc;
    UInt32 crc;
} SzSub;

typedef struct {
    CRdr        rd;
    SzArchive  *a;
    UInt32      numSubs;

    /* Header-parsing scratch, grown on demand to the substream / file count the
     * header declares (PsEnsure).  These must NOT be function locals: the DOS
     * build's whole stack is 128K, and one frame holding two full-size ones
     * would overflow it before a byte is parsed.  They live in this (heap)
     * parse context rather than in file statics so that the two contexts SzOpen
     * keeps alive at once - the outer archive and the encoded header - cannot
     * tread on each other. */
    UInt32      cap;                /* elements allocated in each array below  */
    SzSub      *subs;               /* substream table from SubStreamsInfo     */
    Byte       *bitsA;              /* substream CRC-defined / emptyStream     */
    Byte       *bitsB;              /* emptyFile                               */
    Byte       *bitsC;              /* per-property "defined" bit vector        */
    UInt32     *crcTmp;             /* substream CRC digests                   */
} ParseState;

/* Release a parse context and its scratch. */
static void PsFree( ParseState *ps )
{
    if ( !ps ) return;
    if ( ps->subs )   free( ps->subs );
    if ( ps->bitsA )  free( ps->bitsA );
    if ( ps->bitsB )  free( ps->bitsB );
    if ( ps->bitsC )  free( ps->bitsC );
    if ( ps->crcTmp ) free( ps->crcTmp );
    free( ps );
}

/* Make room for at least 'n' substreams / files in the scratch arrays.  Grows
 * only (an earlier phase's contents stay valid and in place), and reallocs so
 * the substream table built by SubStreamsInfo survives the later growth that
 * FilesInfo may ask for. */
static int PsEnsure( ParseState *ps, UInt32 n )
{
    SzSub  *ns;
    Byte   *nA, *nB, *nC;
    UInt32 *nCrc;

    if ( n > SZ_MAX_FILES ) return SZ_ERR_TOOBIG;
    if ( n < 16 ) n = 16;               /* avoid a run of tiny reallocs, and
                                           keep the scratch non-NULL even for
                                           an archive with no files at all */
    if ( n <= ps->cap ) return SZ_OK;

    ns = (SzSub *)realloc( ps->subs, n * sizeof( SzSub ) );
    if ( !ns ) return SZ_ERR_MEMORY;
    ps->subs = ns;

    nA = (Byte *)realloc( ps->bitsA, n );
    if ( !nA ) return SZ_ERR_MEMORY;
    ps->bitsA = nA;

    nB = (Byte *)realloc( ps->bitsB, n );
    if ( !nB ) return SZ_ERR_MEMORY;
    ps->bitsB = nB;

    nC = (Byte *)realloc( ps->bitsC, n );
    if ( !nC ) return SZ_ERR_MEMORY;
    ps->bitsC = nC;

    nCrc = (UInt32 *)realloc( ps->crcTmp, n * sizeof( UInt32 ) );
    if ( !nCrc ) return SZ_ERR_MEMORY;
    ps->crcTmp = nCrc;

    ps->cap = n;
    return SZ_OK;
}

static Byte RdByte( CRdr *r )
{
    if ( r->p >= r->end ) { r->err = 1; return 0; }
    return *r->p++;
}

static UInt32 RdUInt32( CRdr *r )
{
    UInt32 v;
    v  = (UInt32)RdByte( r );
    v |= (UInt32)RdByte( r ) << 8;
    v |= (UInt32)RdByte( r ) << 16;
    v |= (UInt32)RdByte( r ) << 24;
    return v;
}

/* 7z variable-length number; clamped to 32 bits (overflow -> r->err). */
static UInt32 RdNum( CRdr *r )
{
    Byte   first = RdByte( r );
    Byte   mask  = 0x80;
    UInt32 value = 0;
    int    i;

    for ( i = 0; i < 8; i++ )
    {
        if ( ( first & mask ) == 0 )
        {
            UInt32 high = (UInt32)( first & ( mask - 1 ) );
            if ( i < 4 )
                value |= high << ( 8 * i );
            else if ( high )
                r->err = 1;
            return value;
        }
        {
            Byte b = RdByte( r );
            if ( i < 4 )
                value |= (UInt32)b << ( 8 * i );
            else if ( b )
                r->err = 1;
        }
        mask >>= 1;
    }
    return value;
}

/* Read numItems bits, MSB-first, into bits[] as 0/1 bytes. */
static void RdBits( CRdr *r, Byte *bits, UInt32 numItems )
{
    Byte   b = 0, mask = 0;
    UInt32 i;
    for ( i = 0; i < numItems; i++ )
    {
        if ( mask == 0 ) { b = RdByte( r ); mask = 0x80; }
        bits[i] = ( b & mask ) ? 1 : 0;
        mask >>= 1;
    }
}

/* "AllAreDefined" byte, then either all-ones or an explicit bit vector. */
static void RdBitsOptional( CRdr *r, Byte *bits, UInt32 numItems )
{
    Byte allDefined = RdByte( r );
    if ( allDefined != 0 )
    {
        UInt32 i;
        for ( i = 0; i < numItems; i++ )
            bits[i] = 1;
    }
    else
        RdBits( r, bits, numItems );
}

/*---- PackInfo ------------------------------------------------------------ */
static int ReadPackInfo( ParseState *ps )
{
    CRdr      *r = &ps->rd;
    SzArchive *a = ps->a;
    Byte       id;
    UInt32     i, off;

    a->packPos        = RdNum( r );
    a->numPackStreams = RdNum( r );
    if ( a->numPackStreams > SZ_MAX_PACKSTREAMS )
        return SZ_ERR_TOOBIG;

    id = RdByte( r );
    if ( id == k7zSize )
    {
        for ( i = 0; i < a->numPackStreams; i++ )
        {
            a->packSizes[i] = RdNum( r );
            if ( r->err ) return SZ_ERR_TOOBIG;
        }
        id = RdByte( r );
    }
    if ( id == k7zCRC )
    {
        Byte   defined[SZ_MAX_PACKSTREAMS];   /* one per PACK stream, and a
                                               * folder may own four */
        UInt32 c;
        RdBitsOptional( r, defined, a->numPackStreams );
        for ( i = 0; i < a->numPackStreams; i++ )
            if ( defined[i] ) c = RdUInt32( r );
        (void)c;
        id = RdByte( r );
    }
    if ( id != k7zEnd )
        return SZ_ERR_FORMAT;

    off = a->packPos;
    for ( i = 0; i < a->numPackStreams; i++ )
    {
        a->packOffset[i] = off;
        off += a->packSizes[i];
    }
    return SZ_OK;
}

/*---- Folder (one main coder, plus an optional BCJ x86 filter) ------------ *
 * We support a folder of one or two coders, each with exactly one input and
 * one output stream:
 *   - one "main" coder (Copy / LZMA / LZMA2) that consumes the packed stream;
 *   - optionally a BCJ x86 filter, bound so the main coder's output feeds the
 *     filter's input, with the filter's output being the folder's result.
 * Since every coder here is 1-in/1-out, the global out-stream index equals the
 * coder index, which lets us record which output is the main coder's (the LZMA
 * decode size) and which is the folder's final output.
 *-------------------------------------------------------------------------- */
/* Is folder in-stream 'inIdx' fed by a bind pair (rather than by a packed
 * stream)?  Returns the bind pair index, or -1. */
static int FolderBindForIn( const SzFolder *fo, UInt32 inIdx )
{
    UInt32 b;
    for ( b = 0; b < fo->numBindPairs; b++ )
        if ( fo->bpIn[b] == inIdx ) return (int)b;
    return -1;
}

/* Which coder produces folder out-stream 'outIdx'?  Returns the coder index,
 * or -1 when the folder has more coders than we stored. */
static int FolderCoderForOut( const SzFolder *fo, UInt32 outIdx )
{
    UInt32 c, n = fo->numCoders;
    if ( n > SZ_MAX_CODERS ) n = SZ_MAX_CODERS;
    for ( c = 0; c < n; c++ )
        if ( outIdx >= fo->coders[c].firstOut &&
             outIdx <  fo->coders[c].firstOut + fo->coders[c].numOut )
            return (int)c;
    return -1;
}

/* Folder-relative packed stream index feeding in-stream 'inIdx', or -1. */
static int FolderPackForIn( const SzFolder *fo, UInt32 inIdx )
{
    UInt32 p;
    for ( p = 0; p < fo->numPackStreams; p++ )
        if ( fo->packToIn[p] == inIdx ) return (int)p;
    return -1;
}

/* A main coder reads the packed stream directly and produces bytes. */
static int CoderIsMain( int kind )
{
    return kind == SZ_C_COPY || kind == SZ_C_LZMA || kind == SZ_C_LZMA2;
}

static int CoderMethod( int kind )
{
    if ( kind == SZ_C_COPY )  return SZ_M_COPY;
    if ( kind == SZ_C_LZMA )  return SZ_M_LZMA;
    return SZ_M_LZMA2;
}

/*---- Folder classification ----------------------------------------------- *
 * Decide, from the parsed structure alone, whether this folder is one of the
 * three decodable shapes, and fill in the fields the decoder uses.  Sets
 * fo->unsupported instead of failing: the caller carries on parsing either
 * way, so an unknown codec costs its own entries and nothing else.
 *-------------------------------------------------------------------------- */
static void ClassifyFolder( SzFolder *fo )
{
    UInt32 finalOut = 0;
    UInt32 o;
    int    bi;

    fo->unsupported = 1;
    fo->isBcj2      = 0;

    if ( fo->numCoders > SZ_MAX_CODERS ) return;
    if ( !fo->hasFinalOut ) return;       /* not a single-result folder */
    finalOut = fo->finalOutLocal;
    (void)o;

    /* --- one coder: straight from the packed stream --------------------- */
    if ( fo->numCoders == 1 )
    {
        if ( !CoderIsMain( fo->coders[0].kind ) ) return;
        if ( fo->numPackStreams != 1 ) return;
        fo->method       = CoderMethod( fo->coders[0].kind );
        fo->filter       = SZ_F_NONE;
        fo->propsSize    = fo->coders[0].propsSize;
        memcpy( fo->props, fo->coders[0].props, sizeof( fo->props ) );
        fo->mainOutLocal = fo->coders[0].firstOut;
        fo->unsupported  = 0;
        return;
    }

    /* --- two coders: main + BCJ x86 filter over its output --------------- */
    if ( fo->numCoders == 2 )
    {
        int mainIdx = -1, filtIdx = -1;
        UInt32 c;
        for ( c = 0; c < 2; c++ )
        {
            if ( CoderIsMain( fo->coders[c].kind ) )      mainIdx = (int)c;
            else if ( fo->coders[c].kind == SZ_C_BCJ_X86 ) filtIdx = (int)c;
        }
        if ( mainIdx < 0 || filtIdx < 0 ) return;
        if ( fo->numPackStreams != 1 || fo->numBindPairs != 1 ) return;
        /* the filter's input must be fed by the main coder's output */
        if ( fo->bpIn[0]  != fo->coders[filtIdx].firstIn )  return;
        if ( fo->bpOut[0] != fo->coders[mainIdx].firstOut ) return;
        if ( finalOut != fo->coders[filtIdx].firstOut )     return;

        fo->method       = CoderMethod( fo->coders[mainIdx].kind );
        fo->filter       = SZ_F_BCJ_X86;
        fo->propsSize    = fo->coders[mainIdx].propsSize;
        memcpy( fo->props, fo->coders[mainIdx].props, sizeof( fo->props ) );
        fo->mainOutLocal = fo->coders[mainIdx].firstOut;
        fo->unsupported  = 0;
        return;
    }

    /* --- four coders: BCJ2 ---------------------------------------------- *
     * The BCJ2 coder takes four inputs - main, call, jump, and the raw
     * range-coder stream.  The first three are bound to the outputs of three
     * other coders (each reading its own packed stream); the fourth is a
     * packed stream directly.  Nothing here assumes 7-Zip's coder ORDER: the
     * wiring is read out of the bind pairs.
     *-------------------------------------------------------------------- */
    if ( fo->numCoders != 4 ) return;

    bi = -1;
    for ( o = 0; o < 4; o++ )
        if ( fo->coders[o].kind == SZ_C_BCJ2 )
        {
            if ( bi >= 0 ) return;                /* two BCJ2 coders */
            bi = (int)o;
        }
    if ( bi < 0 ) return;
    if ( fo->coders[bi].numIn != 4 || fo->coders[bi].numOut != 1 ) return;
    if ( finalOut != fo->coders[bi].firstOut ) return;
    if ( fo->numPackStreams != 4 || fo->numBindPairs != 3 ) return;

    {
        int  sub[3];
        int  k;
        for ( k = 0; k < 3; k++ )
        {
            UInt32 in = fo->coders[bi].firstIn + (UInt32)k;
            int    b  = FolderBindForIn( fo, in );
            int    cd;
            if ( b < 0 ) return;                  /* must be a bound input */
            cd = FolderCoderForOut( fo, fo->bpOut[b] );
            if ( cd < 0 || cd == bi ) return;
            if ( !CoderIsMain( fo->coders[cd].kind ) ) return;
            /* each sub-coder must read a packed stream of its own */
            if ( FolderPackForIn( fo, fo->coders[cd].firstIn ) < 0 ) return;
            sub[k] = cd;
        }
        /* the fourth input is the range-coder stream, taken raw */
        {
            int p = FolderPackForIn( fo, fo->coders[bi].firstIn + 3 );
            if ( p < 0 ) return;
            fo->bcj2RcPack = (UInt32)p;
        }
        fo->bcj2Main = sub[0];
        fo->bcj2Call = sub[1];
        fo->bcj2Jump = sub[2];
    }

    fo->isBcj2       = 1;
    fo->filter       = SZ_F_NONE;
    fo->method       = CoderMethod( fo->coders[fo->bcj2Main].kind );
    fo->propsSize    = fo->coders[fo->bcj2Main].propsSize;
    memcpy( fo->props, fo->coders[fo->bcj2Main].props, sizeof( fo->props ) );
    fo->mainOutLocal = fo->coders[fo->bcj2Main].firstOut;
    fo->unsupported  = 0;
}

/*---- Folder: structural parse -------------------------------------------- *
 * Reads the folder record whatever it contains.  Returns an error ONLY for a
 * malformed header - an unrecognised codec is not an error here, it is a
 * classification result (see ClassifyFolder).
 *-------------------------------------------------------------------------- */
static int ReadFolder( ParseState *ps, SzFolder *fo )
{
    CRdr   *r = &ps->rd;
    UInt32  numCoders;
    UInt32  c, totalIn = 0, totalOut = 0, i;

    memset( fo, 0, sizeof( *fo ) );
    fo->method   = -1;
    fo->filter   = SZ_F_NONE;
    fo->bcj2Main = fo->bcj2Call = fo->bcj2Jump = -1;

    numCoders = RdNum( r );
    if ( numCoders < 1 || numCoders > SZ_MAX_FOLDER_STRMS )
        return SZ_ERR_FORMAT;
    fo->numCoders = numCoders;

    for ( c = 0; c < numCoders; c++ )
    {
        Byte     flags     = RdByte( r );
        unsigned idSize    = flags & 0x0F;
        int      isComplex = ( flags & 0x10 ) != 0;
        int      hasAttr   = ( flags & 0x20 ) != 0;
        Byte     id[8];
        unsigned k;
        UInt32   nin = 1, nout = 1;
        SzCoder  cd;

        if ( idSize > 8 ) return SZ_ERR_FORMAT;
        for ( k = 0; k < idSize; k++ ) id[k] = RdByte( r );

        if ( isComplex )
        {
            nin  = RdNum( r );
            nout = RdNum( r );
            if ( nin > SZ_MAX_FOLDER_STRMS || nout > SZ_MAX_FOLDER_STRMS )
                return SZ_ERR_FORMAT;
        }

        memset( &cd, 0, sizeof( cd ) );
        cd.kind     = SZ_C_UNKNOWN;
        cd.numIn    = nin;
        cd.numOut   = nout;
        cd.firstIn  = totalIn;
        cd.firstOut = totalOut;

        if ( idSize == 1 && id[0] == 0x00 )
            cd.kind = SZ_C_COPY;
        else if ( idSize == 3 && id[0] == 0x03 && id[1] == 0x01 && id[2] == 0x01 )
            cd.kind = SZ_C_LZMA;
        else if ( idSize == 1 && id[0] == 0x21 )
            cd.kind = SZ_C_LZMA2;
        else if ( idSize == 4 && id[0] == 0x03 && id[1] == 0x03 &&
                  id[2] == 0x01 && id[3] == 0x03 )
            cd.kind = SZ_C_BCJ_X86;
        else if ( idSize == 4 && id[0] == 0x03 && id[1] == 0x03 &&
                  id[2] == 0x01 && id[3] == 0x1B )
            cd.kind = SZ_C_BCJ2;

        if ( hasAttr )
        {
            UInt32 propSize = RdNum( r );
            UInt32 j;
            for ( j = 0; j < propSize; j++ )
            {
                Byte b = RdByte( r );
                if ( j < sizeof( cd.props ) ) cd.props[j] = b;
            }
            if ( propSize <= sizeof( cd.props ) )
                cd.propsSize = propSize;
            else
                cd.kind = SZ_C_UNKNOWN;   /* more props than we can hold */
        }

        totalIn  += nin;
        totalOut += nout;
        if ( totalIn > SZ_MAX_FOLDER_STRMS || totalOut > SZ_MAX_FOLDER_STRMS )
            return SZ_ERR_FORMAT;
        if ( c < SZ_MAX_CODERS ) fo->coders[c] = cd;
        if ( r->err ) return SZ_ERR_FORMAT;
    }

    fo->numInStreams  = totalIn;
    fo->numOutStreams = totalOut;
    if ( totalOut < 1 ) return SZ_ERR_FORMAT;

    /* Bind pairs: one fewer than the number of out-streams. */
    fo->numBindPairs = totalOut - 1;
    if ( fo->numBindPairs > SZ_MAX_FOLDER_STRMS ) return SZ_ERR_FORMAT;
    for ( i = 0; i < fo->numBindPairs; i++ )
    {
        fo->bpIn[i]  = RdNum( r );
        fo->bpOut[i] = RdNum( r );
        if ( fo->bpIn[i] >= totalIn || fo->bpOut[i] >= totalOut )
            return SZ_ERR_FORMAT;
    }

    /* Whatever is left unbound on the input side is fed by a packed stream. */
    if ( totalIn < fo->numBindPairs ) return SZ_ERR_FORMAT;
    fo->numPackStreams = totalIn - fo->numBindPairs;
    if ( fo->numPackStreams < 1 ||
         fo->numPackStreams > SZ_MAX_FOLDER_STRMS ) return SZ_ERR_FORMAT;

    if ( fo->numPackStreams == 1 )
    {
        /* Implicit: the single in-stream no bind pair claims.  Not stored. */
        int found = -1;
        for ( i = 0; i < totalIn; i++ )
            if ( FolderBindForIn( fo, i ) < 0 ) { found = (int)i; break; }
        if ( found < 0 ) return SZ_ERR_FORMAT;
        fo->packToIn[0] = (UInt32)found;
    }
    else
    {
        for ( i = 0; i < fo->numPackStreams; i++ )
        {
            fo->packToIn[i] = RdNum( r );
            if ( fo->packToIn[i] >= totalIn ) return SZ_ERR_FORMAT;
        }
    }
    if ( r->err ) return SZ_ERR_FORMAT;

    /* The folder's result is the one out-stream no bind pair consumes.  This
     * is structural, so it is worked out here rather than during
     * classification: an unsupported folder still needs its output size, or
     * its entries would list as zero bytes. */
    {
        UInt32 o, b;
        int    found = 0;
        for ( o = 0; o < fo->numOutStreams; o++ )
        {
            int bound = 0;
            for ( b = 0; b < fo->numBindPairs; b++ )
                if ( fo->bpOut[b] == o ) bound = 1;
            if ( !bound ) { fo->finalOutLocal = o; found++; }
        }
        fo->hasFinalOut = ( found == 1 );
    }

    ClassifyFolder( fo );
    fo->mainUnpackSize = 0;
    fo->unpackSize     = 0;
    fo->hasCrc         = 0;
    fo->crc            = 0;
    return SZ_OK;
}

/*---- UnpackInfo ---------------------------------------------------------- */
static int ReadUnpackInfo( ParseState *ps )
{
    CRdr      *r = &ps->rd;
    SzArchive *a = ps->a;
    Byte       id, external;
    UInt32     i;
    int        rc;

    id = RdByte( r );
    if ( id != k7zFolder )
        return SZ_ERR_FORMAT;

    a->numFolders = RdNum( r );
    if ( a->numFolders > SZ_MAX_FOLDERS )
        return SZ_ERR_TOOBIG;

    external = RdByte( r );
    if ( external != 0 )
        return SZ_ERR_UNSUPPORTED;     /* folder defs in another stream */

    {   /* Folders consume packed streams in order; a folder is not one
         * stream (BCJ2 takes four), so walk a running total. */
        UInt32 packBase = 0;
        for ( i = 0; i < a->numFolders; i++ )
        {
            SzFolder *fo = &a->folders[i];
            rc = ReadFolder( ps, fo );
            if ( rc ) return rc;
            fo->firstPackStream = packBase;
            packBase += fo->numPackStreams;
            if ( packBase > SZ_MAX_PACKSTREAMS ) return SZ_ERR_TOOBIG;
        }
    }

    id = RdByte( r );
    if ( id != k7zCodersUnpackSize )
        return SZ_ERR_FORMAT;
    /* One unpack size per coder OUTPUT STREAM, in folder/coder order - not
     * one per coder.  Pick out the main coder's output (the LZMA decode
     * size) and the folder's final output (after any filter or BCJ2). */
    for ( i = 0; i < a->numFolders; i++ )
    {
        SzFolder *fo = &a->folders[i];
        UInt32    sizes[SZ_MAX_FOLDER_STRMS];
        UInt32    o, c, n;

        for ( o = 0; o < fo->numOutStreams; o++ )
        {
            sizes[o] = RdNum( r );
            if ( r->err )
                return SZ_ERR_TOOBIG;
            if ( sizes[o] > SZ_MAX_UNPACK_SIZE )
                return SZ_ERR_TOOBIG;
        }

        n = ( fo->numCoders > SZ_MAX_CODERS ) ? SZ_MAX_CODERS : fo->numCoders;
        for ( c = 0; c < n; c++ )
            if ( fo->coders[c].firstOut < fo->numOutStreams )
                fo->coders[c].unpackSize = sizes[fo->coders[c].firstOut];

        /* The folder's output size is structural and is needed even for an
         * unsupported folder, so its entries still list their real sizes. */
        if ( fo->hasFinalOut && fo->finalOutLocal < fo->numOutStreams )
            fo->unpackSize = sizes[fo->finalOutLocal];

        if ( fo->unsupported )
            continue;                  /* nothing further to wire up */

        fo->mainUnpackSize = sizes[fo->mainOutLocal];
    }

    id = RdByte( r );
    if ( id == k7zCRC )
    {
        Byte   defined[SZ_MAX_FOLDERS];
        UInt32 crcs[SZ_MAX_FOLDERS];
        RdBitsOptional( r, defined, a->numFolders );
        for ( i = 0; i < a->numFolders; i++ )
            if ( defined[i] ) crcs[i] = RdUInt32( r );
        for ( i = 0; i < a->numFolders; i++ )
        {
            a->folders[i].hasCrc = defined[i];
            a->folders[i].crc    = defined[i] ? crcs[i] : 0;
        }
        id = RdByte( r );
    }
    if ( id != k7zEnd )
        return SZ_ERR_FORMAT;
    return SZ_OK;
}

/*---- SubStreamsInfo ------------------------------------------------------ */
static int ReadSubStreamsInfo( ParseState *ps )
{
    CRdr      *r = &ps->rd;
    SzArchive *a = ps->a;
    Byte       id;
    UInt32     f, s, totalSubs;
    int        rc;

    for ( f = 0; f < a->numFolders; f++ )
        a->numUnpack[f] = 1;

    id = RdByte( r );
    if ( id == k7zNumUnpackStream )
    {
        for ( f = 0; f < a->numFolders; f++ )
            a->numUnpack[f] = RdNum( r );
        id = RdByte( r );
    }

    totalSubs = 0;
    for ( f = 0; f < a->numFolders; f++ )
        totalSubs += a->numUnpack[f];
    if ( totalSubs > SZ_MAX_FILES )
        return SZ_ERR_TOOBIG;

    rc = PsEnsure( ps, totalSubs );
    if ( rc ) return rc;

    ps->numSubs = 0;

    if ( id == k7zSize )
    {
        for ( f = 0; f < a->numFolders; f++ )
        {
            UInt32 n   = a->numUnpack[f];
            UInt32 sum = 0, off = 0;
            if ( n == 0 ) continue;
            for ( s = 0; s + 1 < n; s++ )
            {
                UInt32 sz = RdNum( r );
                if ( r->err ) return SZ_ERR_TOOBIG;
                ps->subs[ps->numSubs].folder         = f;
                ps->subs[ps->numSubs].offsetInFolder = off;
                ps->subs[ps->numSubs].size           = sz;
                ps->subs[ps->numSubs].hasCrc         = 0;
                ps->numSubs++;
                off += sz; sum += sz;
            }
            ps->subs[ps->numSubs].folder         = f;
            ps->subs[ps->numSubs].offsetInFolder = off;
            ps->subs[ps->numSubs].size           = a->folders[f].unpackSize - sum;
            ps->subs[ps->numSubs].hasCrc         = 0;
            ps->numSubs++;
        }
        id = RdByte( r );
    }
    else
    {
        for ( f = 0; f < a->numFolders; f++ )
        {
            if ( a->numUnpack[f] != 1 )
                return SZ_ERR_FORMAT;
            ps->subs[ps->numSubs].folder         = f;
            ps->subs[ps->numSubs].offsetInFolder = 0;
            ps->subs[ps->numSubs].size           = a->folders[f].unpackSize;
            ps->subs[ps->numSubs].hasCrc         = 0;
            ps->numSubs++;
        }
    }

    /* CRC digests: defined only for substreams without an inherited folder CRC */
    {
        UInt32 numDigests = 0;
        for ( f = 0; f < a->numFolders; f++ )
        {
            UInt32 n = a->numUnpack[f];
            if ( n != 1 || !a->folders[f].hasCrc )
                numDigests += n;
        }

        if ( id == k7zCRC )
        {
            Byte   *defined = ps->bitsA;      /* scratch: see ParseState */
            UInt32 *crcs    = ps->crcTmp;
            UInt32 di = 0, si = 0, i;

            RdBitsOptional( r, defined, numDigests );
            for ( i = 0; i < numDigests; i++ )
                if ( defined[i] ) crcs[i] = RdUInt32( r );

            for ( f = 0; f < a->numFolders; f++ )
            {
                UInt32 n = a->numUnpack[f];
                if ( n == 1 && a->folders[f].hasCrc )
                {
                    ps->subs[si].hasCrc = 1;
                    ps->subs[si].crc    = a->folders[f].crc;
                    si++;
                }
                else
                {
                    for ( s = 0; s < n; s++ )
                    {
                        ps->subs[si].hasCrc = defined[di];
                        ps->subs[si].crc    = defined[di] ? crcs[di] : 0;
                        di++; si++;
                    }
                }
            }
            id = RdByte( r );
        }
        else
        {
            UInt32 si = 0;
            for ( f = 0; f < a->numFolders; f++ )
            {
                UInt32 n = a->numUnpack[f];
                if ( n == 1 && a->folders[f].hasCrc )
                {
                    ps->subs[si].hasCrc = 1;
                    ps->subs[si].crc    = a->folders[f].crc;
                }
                si += n;
            }
        }
    }

    if ( id != k7zEnd )
        return SZ_ERR_FORMAT;
    return SZ_OK;
}

/* Default substreams when no SubStreamsInfo is present (1 per folder). */
static int DefaultSubStreams( ParseState *ps )
{
    SzArchive *a = ps->a;
    UInt32     f;
    int        rc = PsEnsure( ps, a->numFolders );

    if ( rc ) return rc;
    ps->numSubs = 0;
    for ( f = 0; f < a->numFolders; f++ )
    {
        a->numUnpack[f] = 1;
        ps->subs[ps->numSubs].folder         = f;
        ps->subs[ps->numSubs].offsetInFolder = 0;
        ps->subs[ps->numSubs].size           = a->folders[f].unpackSize;
        ps->subs[ps->numSubs].hasCrc         = a->folders[f].hasCrc;
        ps->subs[ps->numSubs].crc            = a->folders[f].crc;
        ps->numSubs++;
    }
    return SZ_OK;
}

/*---- StreamsInfo (PackInfo + UnpackInfo + SubStreamsInfo) ----------------- */
static int ReadStreamsInfo( ParseState *ps )
{
    CRdr *r = &ps->rd;
    Byte  id;
    int   rc;

    id = RdByte( r );
    if ( id == k7zPackInfo )
    {
        rc = ReadPackInfo( ps ); if ( rc ) return rc;
        id = RdByte( r );
    }
    if ( id == k7zUnpackInfo )
    {
        rc = ReadUnpackInfo( ps ); if ( rc ) return rc;
        id = RdByte( r );
    }
    if ( id == k7zSubStreamsInfo )
    {
        rc = ReadSubStreamsInfo( ps ); if ( rc ) return rc;
        id = RdByte( r );
    }
    else
    {
        rc = DefaultSubStreams( ps ); if ( rc ) return rc;
    }
    if ( id != k7zEnd )
        return SZ_ERR_FORMAT;

    /* Folders claim packed streams in order and a folder may claim more than
     * one (BCJ2 claims four), so the total has to add up rather than match
     * the folder count. */
    if ( ps->a->numFolders != 0 )
    {
        SzFolder *last = &ps->a->folders[ps->a->numFolders - 1];
        if ( last->firstPackStream + last->numPackStreams !=
             ps->a->numPackStreams )
            return SZ_ERR_FORMAT;
    }

    return SZ_OK;
}

/*---- ArchiveProperties (skipped) ----------------------------------------- */
static int SkipArchiveProps( ParseState *ps )
{
    CRdr *r = &ps->rd;
    for ( ;; )
    {
        Byte   type = RdByte( r );
        UInt32 size;
        if ( type == k7zEnd ) break;
        size = RdNum( r );
        r->p += size;
        if ( r->p > r->end ) return SZ_ERR_FORMAT;
    }
    return SZ_OK;
}

/*---- FilesInfo ----------------------------------------------------------- */
static int ReadFilesInfo( ParseState *ps )
{
    CRdr      *r = &ps->rd;
    SzArchive *a = ps->a;
    UInt32     numFiles, i;
    Byte      *emptyStream, *emptyFile;
    UInt32     numEmptyStreams = 0;
    int        rc;

    numFiles = RdNum( r );
    if ( numFiles > SZ_MAX_FILES )
        return SZ_ERR_TOOBIG;

    /* Size the scratch and the entry table to what this archive actually holds
     * (the scratch may already be larger from SubStreamsInfo - PsEnsure only
     * grows, so the substream table it built stays intact). */
    rc = PsEnsure( ps, numFiles );
    if ( rc ) return rc;
    emptyStream = ps->bitsA;                 /* scratch: see ParseState */
    emptyFile   = ps->bitsB;

    if ( a->entries ) { free( a->entries ); a->entries = NULL; }
    a->entries = (SzEntry *)calloc( numFiles ? numFiles : 1, sizeof( SzEntry ) );
    if ( !a->entries ) return SZ_ERR_MEMORY;
    a->numEntries = (int)numFiles;

    memset( emptyStream, 0, numFiles );
    memset( emptyFile,   0, numFiles );
    for ( i = 0; i < numFiles; i++ )         /* calloc zeroed the rest */
        a->entries[i].folderIndex = -1;

    for ( ;; )
    {
        Byte        propType = RdByte( r );
        UInt32      size;
        const Byte *next;

        if ( propType == k7zEnd ) break;
        size = RdNum( r );
        next = r->p + size;
        if ( next > r->end ) return SZ_ERR_FORMAT;

        switch ( propType )
        {
        case k7zEmptyStream:
            RdBits( r, emptyStream, numFiles );
            for ( i = 0; i < numFiles; i++ )
                if ( emptyStream[i] ) numEmptyStreams++;
            break;

        case k7zEmptyFile:
            RdBits( r, emptyFile, numEmptyStreams );
            break;

        case k7zName:
        {
            Byte external = RdByte( r );
            if ( external != 0 )
                return SZ_ERR_UNSUPPORTED;
            for ( i = 0; i < numFiles; i++ )
            {
                WCHAR wbuf[SZ_MAX_NAME];
                int   wn = 0;
                for ( ;; )
                {
                    UInt32 ch = RdByte( r );
                    ch |= (UInt32)RdByte( r ) << 8;
                    if ( ch == 0 ) break;
                    if ( wn < SZ_MAX_NAME - 1 )
                        wbuf[wn++] = (WCHAR)ch;
                }
                wbuf[wn] = 0;
                WideCharToMultiByte( CP_ACP, 0, wbuf, -1,
                                     a->entries[i].name, SZ_MAX_NAME,
                                     NULL, NULL );
            }
            break;
        }

        case k7zMTime:
        {
            /* [AllAreDefined bits][external][per-defined: 8-byte FILETIME] */
            Byte  *defined = ps->bitsC;      /* scratch: see ParseState */
            Byte   external;
            RdBitsOptional( r, defined, numFiles );
            external = RdByte( r );
            if ( external == 0 )
                for ( i = 0; i < numFiles; i++ )
                    if ( defined[i] )
                    {
                        a->entries[i].mtimeLo  = RdUInt32( r );
                        a->entries[i].mtimeHi  = RdUInt32( r );
                        a->entries[i].hasMtime = 1;
                    }
            break;
        }

        case k7zWinAttributes:
        {
            /* [AllAreDefined bits][external][per-defined: 4-byte attributes] */
            Byte  *defined = ps->bitsC;      /* scratch: see ParseState */
            Byte   external;
            RdBitsOptional( r, defined, numFiles );
            external = RdByte( r );
            if ( external == 0 )
                for ( i = 0; i < numFiles; i++ )
                    if ( defined[i] )
                    {
                        a->entries[i].attrib    = RdUInt32( r );
                        a->entries[i].hasAttrib = 1;
                    }
            break;
        }

        default:
            break;   /* other properties skipped via next */
        }

        r->p = next;       /* resync to the declared property end */
    }

    /* Map files to substreams / mark directories and empty files. */
    {
        UInt32 subIdx = 0, emptyIdx = 0;
        for ( i = 0; i < numFiles; i++ )
        {
            if ( !emptyStream[i] )
            {
                if ( subIdx >= ps->numSubs )
                    return SZ_ERR_FORMAT;
                a->entries[i].folderIndex    = (int)ps->subs[subIdx].folder;
                a->entries[i].offsetInFolder = ps->subs[subIdx].offsetInFolder;
                a->entries[i].size           = ps->subs[subIdx].size;
                a->entries[i].hasCrc         = ps->subs[subIdx].hasCrc;
                a->entries[i].crc            = ps->subs[subIdx].crc;
                a->entries[i].isDir          = 0;
                subIdx++;
            }
            else
            {
                int isEmptyFile = emptyFile[emptyIdx++];
                a->entries[i].folderIndex = -1;
                a->entries[i].size        = 0;
                a->entries[i].isDir       = isEmptyFile ? 0 : 1;
            }
        }
    }
    return SZ_OK;
}

/*---- Header dispatcher --------------------------------------------------- */
static int ReadHeader( ParseState *ps )
{
    CRdr *r = &ps->rd;
    Byte  id;
    int   rc;

    id = RdByte( r );
    if ( id == k7zArchiveProperties )
    {
        rc = SkipArchiveProps( ps ); if ( rc ) return rc;
        id = RdByte( r );
    }
    if ( id == k7zAdditionalStreams )
        return SZ_ERR_UNSUPPORTED;
    if ( id == k7zMainStreamsInfo )
    {
        rc = ReadStreamsInfo( ps ); if ( rc ) return rc;
        id = RdByte( r );
    }
    if ( id == k7zFilesInfo )
    {
        rc = ReadFilesInfo( ps ); if ( rc ) return rc;
        id = RdByte( r );
    }
    if ( id != k7zEnd )
        return SZ_ERR_FORMAT;
    return SZ_OK;
}

/*---- BCJ x86 branch filter (decode) -------------------------------------- *
 * Inverse of the x86 BCJ filter that 7-Zip applies to executables ahead of
 * LZMA: it rewrites the 32-bit operand of E8/E9 (CALL/JMP rel32) instructions
 * from the filter's absolute form back to the original PE-relative form.  This
 * is the standard whole-buffer single pass (start IP = 0); the transform is
 * size-preserving, so it runs in place.
 *-------------------------------------------------------------------------- */
#define Test86MSByte( b )  ( ( ( (b) + 1 ) & 0xFE ) == 0 )   /* 0x00 or 0xFF */

/* Filter state.  'base' is the absolute stream offset of data[0] on the next
 * call and 'mask' the E8/E9 history, so the same pass can be run over a whole
 * buffer in one go or over a stream in pieces. */
typedef struct {
    UInt32 base;
    UInt32 mask;
} SzBcj;

static void BcjInit( SzBcj *b )
{
    b->base = 0;
    b->mask = 0;
}

/*
 * Convert 'size' bytes in place and return how many are FINISHED.  The last
 * bytes of the buffer are left unconverted whenever an E8/E9 there could still
 * own a rel32 operand that continues past the end - the caller keeps that tail
 * and passes it again at the head of the next call.  Called once with the
 * whole buffer, this is exactly the original single-pass filter.
 */
static UInt32 BcjX86Chunk( SzBcj *b, Byte *data, UInt32 size )
{
    UInt32 pos  = 0;
    UInt32 mask = b->mask;
    UInt32 lim;
    const UInt32 ip = 5;          /* start IP (0) + 5, per the SDK */

    if ( size < 5 )
        return 0;                 /* not enough to decide anything yet */
    lim = size - 4;

    for ( ;; )
    {
        UInt32 p;

        for ( p = pos; p < lim; p++ )
            if ( ( data[p] & 0xFE ) == 0xE8 )
                break;

        {
            UInt32 d = p - pos;
            pos = p;
            if ( p >= lim )
                break;
            if ( d > 2 )
                mask = 0;
            else
            {
                mask >>= (unsigned)d;
                if ( mask != 0 &&
                     ( mask > 4 || mask == 3 ||
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
            UInt32 v   = ( (UInt32)data[pos + 4] << 24 ) |
                         ( (UInt32)data[pos + 3] << 16 ) |
                         ( (UInt32)data[pos + 2] << 8  ) |
                         ( (UInt32)data[pos + 1] );
            UInt32 cur = ip + b->base + pos;

            v -= cur;
            if ( mask != 0 )
            {
                unsigned sh = ( mask & 6 ) << 2;
                if ( Test86MSByte( (Byte)( v >> sh ) ) )
                {
                    v ^= ( ( (UInt32)0x100 << sh ) - 1 );
                    v -= cur;
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

    b->mask  = mask;
    b->base += pos;
    return pos;
}

/* Whole-buffer form, for the (small, already in memory) encoded header. */
static void BcjX86Decode( Byte *data, UInt32 size )
{
    SzBcj b;
    BcjInit( &b );
    BcjX86Chunk( &b, data, size );      /* the <5-byte tail never converts */
}

/*---- Decode one folder to a freshly malloc'd buffer ---------------------- */
static int DecodeFolder( SzArchive *a, UInt32 fIdx, Byte **outBufOut )
{
    SzFolder *fo       = &a->folders[fIdx];
    UInt32    packOff  = a->baseOffset + a->packOffset[fIdx];
    UInt32    packSize = a->packSizes[fIdx];
    Byte     *packBuf  = NULL;
    Byte     *outBuf   = NULL;
    int       rc       = SZ_OK;

    /* Buffered: both the packed and the unpacked stream are held whole, so
     * this path is bounded by the in-RAM cap, not the streaming one.  It is
     * used for the encoded archive header, which is small. */
    if ( packSize > SZ_MAX_BUFFER_SIZE )        return SZ_ERR_TOOBIG;
    if ( fo->unpackSize > SZ_MAX_BUFFER_SIZE )  return SZ_ERR_TOOBIG;

    packBuf = (Byte *)malloc( packSize ? packSize : 1 );
    if ( !packBuf ) return SZ_ERR_MEMORY;

    if ( fseek( a->fp, (long)packOff, SEEK_SET ) != 0 ||
         fread( packBuf, 1, packSize, a->fp ) != packSize )
    {
        free( packBuf );
        return SZ_ERR_READ;
    }

    /* Decode the main coder into a buffer sized by its own output stream; a
     * filter, if present, then transforms that buffer in place (BCJ is
     * size-preserving, so the main output and the final output are equal). */
    {
        UInt32 mainSize = fo->mainUnpackSize;

        outBuf = (Byte *)malloc( mainSize ? mainSize : 1 );
        if ( !outBuf ) { free( packBuf ); return SZ_ERR_MEMORY; }

        if ( fo->method == SZ_M_COPY )
        {
            if ( packSize != mainSize ) rc = SZ_ERR_DATA;
            else memcpy( outBuf, packBuf, packSize );
        }
        else if ( fo->method == SZ_M_LZMA )
        {
            if ( fo->propsSize < 5 )
                rc = SZ_ERR_UNSUPPORTED;
            else
            {
                UInt32 dictSize = fo->props[1] | ( (UInt32)fo->props[2] << 8 ) |
                                  ( (UInt32)fo->props[3] << 16 ) |
                                  ( (UInt32)fo->props[4] << 24 );
                if ( dictSize > SZ_MAX_DICT_SIZE )
                    rc = SZ_ERR_NORAM;
                else
                    rc = LzmaDecode( fo->props, fo->propsSize,
                                     packBuf, packSize, outBuf, mainSize );
            }
        }
        else /* SZ_M_LZMA2 */
        {
            if ( fo->propsSize < 1 )
                rc = SZ_ERR_UNSUPPORTED;
            else
            {
                Byte pbyte = fo->props[0];
                if ( pbyte > 40 )
                    rc = SZ_ERR_UNSUPPORTED;
                else
                {
                    UInt32 dictSize = ( pbyte == 40 )
                        ? 0xFFFFFFFFUL
                        : ( (UInt32)( 2 | ( pbyte & 1 ) ) << ( pbyte / 2 + 11 ) );
                    if ( dictSize > SZ_MAX_DICT_SIZE )
                        rc = SZ_ERR_NORAM;
                    else
                        rc = Lzma2Decode( pbyte, packBuf, packSize,
                                          outBuf, mainSize );
                }
            }
        }

        if ( rc == SZ_OK && fo->filter == SZ_F_BCJ_X86 )
        {
            if ( fo->unpackSize != mainSize )
                rc = SZ_ERR_DATA;            /* BCJ must preserve size */
            else
                BcjX86Decode( outBuf, mainSize );
        }
    }

    free( packBuf );
    if ( rc ) { free( outBuf ); return rc; }

    if ( fo->hasCrc )
    {
        if ( Crc32Calc( outBuf, fo->unpackSize ) != fo->crc )
        {
            free( outBuf );
            return SZ_ERR_CRC;
        }
    }

    *outBufOut = outBuf;
    return SZ_OK;
}

/*===========================================================================
 * Public API
 *===========================================================================*/

static const Byte g_sig[6] = { 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C };

/* Read and validate the 32-byte start header at file offset 'off'.  Returns 1
 * (and fills outHdr) only when both the 6-byte signature and the start-header
 * CRC check out, so a stray signature in a stub's code is rejected. */
static int SzCheckStartHeader( FILE *fp, long off, Byte outHdr[32] )
{
    Byte   hdr[32];
    UInt32 startCrc;

    if ( fseek( fp, off, SEEK_SET ) != 0 )  return 0;
    if ( fread( hdr, 1, 32, fp ) != 32 )    return 0;
    if ( memcmp( hdr, g_sig, 6 ) != 0 )     return 0;
    startCrc = hdr[8] | ( (UInt32)hdr[9] << 8 ) |
               ( (UInt32)hdr[10] << 16 ) | ( (UInt32)hdr[11] << 24 );
    if ( Crc32Calc( hdr + 12, 20 ) != startCrc ) return 0;
    memcpy( outHdr, hdr, 32 );
    return 1;
}

/* Locate the 7z start header: offset 0 for a plain .7z, or scanned for after
 * the stub of a self-extracting .exe.  Fills outHdr / *outOff.  SZ_OK on
 * success, SZ_ERR_SIG if no valid start header exists. */
#define SZ_SFX_CHUNK 65536

static int SzFindStartHeader( FILE *fp, Byte outHdr[32], UInt32 *outOff )
{
    Byte *buf;
    long  fileLen, base;
    int   found = 0;

    if ( SzCheckStartHeader( fp, 0, outHdr ) ) { *outOff = 0; return SZ_OK; }

    if ( fseek( fp, 0, SEEK_END ) != 0 ) return SZ_ERR_READ;
    fileLen = ftell( fp );
    if ( fileLen < 32 ) return SZ_ERR_SIG;

    buf = (Byte *)malloc( SZ_SFX_CHUNK + 5 );   /* +5 to span chunk boundaries */
    if ( !buf ) return SZ_ERR_MEMORY;

    for ( base = 0; base < fileLen && !found; base += SZ_SFX_CHUNK )
    {
        long toRead = fileLen - base;
        int  n, i;
        if ( toRead > SZ_SFX_CHUNK + 5 ) toRead = SZ_SFX_CHUNK + 5;
        if ( fseek( fp, base, SEEK_SET ) != 0 ) break;
        n = (int)fread( buf, 1, (size_t)toRead, fp );
        for ( i = 0; i + 6 <= n; i++ )
        {
            if ( buf[i] == 0x37 && memcmp( buf + i, g_sig, 6 ) == 0 &&
                 SzCheckStartHeader( fp, base + i, outHdr ) )
            {
                *outOff = (UInt32)( base + i );
                found = 1;
                break;
            }
        }
    }
    free( buf );
    return found ? SZ_OK : SZ_ERR_SIG;
}

int SzOpen( const char *path, SzArchive **out )
{
    SzArchive *a;
    FILE      *fp;
    Byte       sigHdr[32];
    UInt32     hdrOff = 0;
    UInt32     nhOffLo, nhOffHi, nhSizeLo, nhSizeHi, nhCrc;
    Byte      *headerBuf = NULL;
    UInt32     headerSize;
    ParseState *ps = NULL;
    int        rc = SZ_OK;
    Byte       id;

    *out = NULL;

    fp = fopen( path, "rb" );
    if ( !fp ) return SZ_ERR_OPEN;

    /* Offset 0 for a plain .7z, or after the stub of a self-extracting .exe. */
    rc = SzFindStartHeader( fp, sigHdr, &hdrOff );
    if ( rc ) { fclose( fp ); return rc; }

    nhOffLo  = sigHdr[12] | ( (UInt32)sigHdr[13] << 8 ) |
               ( (UInt32)sigHdr[14] << 16 ) | ( (UInt32)sigHdr[15] << 24 );
    nhOffHi  = sigHdr[16] | ( (UInt32)sigHdr[17] << 8 ) |
               ( (UInt32)sigHdr[18] << 16 ) | ( (UInt32)sigHdr[19] << 24 );
    nhSizeLo = sigHdr[20] | ( (UInt32)sigHdr[21] << 8 ) |
               ( (UInt32)sigHdr[22] << 16 ) | ( (UInt32)sigHdr[23] << 24 );
    nhSizeHi = sigHdr[24] | ( (UInt32)sigHdr[25] << 8 ) |
               ( (UInt32)sigHdr[26] << 16 ) | ( (UInt32)sigHdr[27] << 24 );
    nhCrc    = sigHdr[28] | ( (UInt32)sigHdr[29] << 8 ) |
               ( (UInt32)sigHdr[30] << 16 ) | ( (UInt32)sigHdr[31] << 24 );

    if ( nhOffHi != 0 || nhSizeHi != 0 ) { fclose( fp ); return SZ_ERR_TOOBIG; }

    a = (SzArchive *)calloc( 1, sizeof( SzArchive ) );
    if ( !a ) { fclose( fp ); return SZ_ERR_MEMORY; }
    a->fp         = fp;
    a->baseOffset = hdrOff + 32;    /* archive body follows the 32-byte header */
    a->numEntries = 0;

    if ( nhSizeLo == 0 )            /* empty archive: no files */
    {
        *out = a;
        return SZ_OK;
    }

    headerSize = nhSizeLo;
    headerBuf  = (Byte *)malloc( headerSize );
    if ( !headerBuf ) { rc = SZ_ERR_MEMORY; goto fail; }

    if ( fseek( fp, (long)( a->baseOffset + nhOffLo ), SEEK_SET ) != 0 ||
         fread( headerBuf, 1, headerSize, fp ) != headerSize )
    {
        rc = SZ_ERR_READ; goto fail;
    }
    if ( Crc32Calc( headerBuf, headerSize ) != nhCrc )
    {
        rc = SZ_ERR_CRC; goto fail;
    }

    ps = (ParseState *)calloc( 1, sizeof( ParseState ) );
    if ( !ps ) { rc = SZ_ERR_MEMORY; goto fail; }
    ps->a       = a;
    ps->numSubs = 0;
    ps->rd.p    = headerBuf;
    ps->rd.end  = headerBuf + headerSize;
    ps->rd.err  = 0;

    id = RdByte( &ps->rd );
    if ( id == k7zHeader )
    {
        rc = ReadHeader( ps );
    }
    else if ( id == k7zEncodedHeader )
    {
        /* The real header is itself an LZMA-compressed folder. Parse the
         * StreamsInfo that describes it into a temporary context, decode it,
         * then parse the resulting bytes as a plain header. */
        SzArchive  *tmp  = (SzArchive *)calloc( 1, sizeof( SzArchive ) );
        ParseState *tps  = (ParseState *)calloc( 1, sizeof( ParseState ) );
        Byte       *hdr2 = NULL;

        if ( !tmp || !tps )
        {
            if ( tmp ) free( tmp );
            if ( tps ) PsFree( tps );
            rc = SZ_ERR_MEMORY; goto fail;
        }
        tmp->fp         = fp;
        tmp->baseOffset = a->baseOffset;
        tps->a          = tmp;
        tps->numSubs    = 0;
        tps->rd         = ps->rd;        /* continue after the kEncodedHeader byte */

        rc = ReadStreamsInfo( tps );
        if ( rc == SZ_OK )
        {
            if ( tmp->numFolders != 1 )
                rc = SZ_ERR_UNSUPPORTED;
            else
                rc = DecodeFolder( tmp, 0, &hdr2 );
        }

        if ( rc == SZ_OK )
        {
            ps->rd.p   = hdr2;
            ps->rd.end = hdr2 + tmp->folders[0].unpackSize;
            ps->rd.err = 0;
            id = RdByte( &ps->rd );
            if ( id != k7zHeader )
                rc = SZ_ERR_FORMAT;
            else
                rc = ReadHeader( ps );
        }

        if ( hdr2 ) free( hdr2 );
        PsFree( tps );
        if ( tmp->entries ) free( tmp->entries );   /* tmp->fp is 'a's, keep it */
        free( tmp );
    }
    else
    {
        rc = SZ_ERR_FORMAT;
    }

    if ( rc == SZ_OK && ps->rd.err )
        rc = SZ_ERR_FORMAT;

    PsFree( ps );  ps = NULL;
    free( headerBuf ); headerBuf = NULL;

    if ( rc != SZ_OK ) goto fail;

    *out = a;
    return SZ_OK;

fail:
    if ( ps )        PsFree( ps );
    if ( headerBuf ) free( headerBuf );
    if ( a )         SzClose( a );      /* closes the file, frees the tables */
    return rc;
}

int SzGetNumEntries( SzArchive *a )
{
    return a ? a->numEntries : 0;
}

const SzEntry *SzGetEntry( SzArchive *a, int index )
{
    if ( !a || index < 0 || index >= a->numEntries )
        return NULL;
    return &a->entries[index];
}

const char *SzEntryMethod( SzArchive *a, int index )
{
    static char buf[32];
    SzEntry  *e;
    SzFolder *fo;
    const char *m;

    buf[0] = '\0';
    if ( !a || index < 0 || index >= a->numEntries )
        return buf;
    e = &a->entries[index];
    if ( e->folderIndex < 0 )                 /* directory / empty file */
        return buf;

    fo = &a->folders[e->folderIndex];
    if ( fo->unsupported )                    /* parsed, but not decodable */
    {
        lstrcpy( buf, "unsupported" );
        return buf;
    }
    m  = ( fo->method == SZ_M_COPY )  ? "Copy"
       : ( fo->method == SZ_M_LZMA )  ? "LZMA"
       : ( fo->method == SZ_M_LZMA2 ) ? "LZMA2"
       : "?";
    lstrcpy( buf, m );
    if ( fo->filter == SZ_F_BCJ_X86 )
        lstrcat( buf, "+BCJ" );
    if ( fo->isBcj2 )
        lstrcat( buf, "+BCJ2" );
    return buf;
}

UInt32 SzEntryPacked( SzArchive *a, int index )
{
    SzEntry *e;
    int      i;

    if ( !a || index < 0 || index >= a->numEntries )
        return 0xFFFFFFFFUL;
    e = &a->entries[index];
    if ( e->folderIndex < 0 )
        return 0xFFFFFFFFUL;
    /* report the folder's pack size only on its first member */
    for ( i = 0; i < index; i++ )
        if ( a->entries[i].folderIndex == e->folderIndex )
            return 0xFFFFFFFFUL;
    return a->packSizes[e->folderIndex];
}

void SzClose( SzArchive *a )
{
    if ( a )
    {
        if ( a->fp )      fclose( a->fp );
        if ( a->entries ) free( a->entries );
        free( a );
    }
}

/*---- Path helpers -------------------------------------------------------- */

/* Create directory 'path' and every parent component (back-slash separated). */
static void MakeTree( const char *path )
{
    char  buf[SZ_MAX_NAME * 2];
    char *p;

    lstrcpyn( buf, path, sizeof( buf ) );

    p = buf;
    if ( p[0] && p[1] == ':' ) p += 2;     /* skip drive spec   */
    if ( *p == '\\' ) p++;                 /* skip leading root */

    for ( ; *p; p++ )
    {
        if ( *p == '\\' )
        {
            *p = '\0';
            _mkdir( buf );
            *p = '\\';
        }
    }
    _mkdir( buf );
}

/* Create the parent directory tree of a file path. */
static void MakeParentTree( const char *filePath )
{
    char  buf[SZ_MAX_NAME * 2];
    char *bs;

    lstrcpyn( buf, filePath, sizeof( buf ) );
    bs = strrchr( buf, '\\' );
    if ( bs )
    {
        *bs = '\0';
        if ( buf[0] )
            MakeTree( buf );
    }
}

/* Build destDir\name with the name made filesystem-safe - ArcFsName also
 * normalises '/' to '\' (invalid characters, device names, 8.3 truncation
 * on DOS/Win32s; see ARCFILE.C). */
static void BuildPath( char *dst, int dstSize,
                       const char *destDir, const char *name )
{
    char rel[SZ_MAX_NAME];

    ArcFsName( rel, sizeof( rel ), name );
    wsprintf( dst, "%s\\%s", (LPSTR)destDir, (LPSTR)rel );
    dst[dstSize - 1] = '\0';
}

/*---- Extraction ---------------------------------------------------------- */

/* Stamp an extracted file with the entry's stored modification time (7z keeps
 * a UTC FILETIME, so it goes to SetFileMTime directly). */
static void ApplySzTime( const char *outPath, const SzEntry *e )
{
    FILETIME ft;
    if ( !e->hasMtime ) return;
    ft.dwLowDateTime  = e->mtimeLo;
    ft.dwHighDateTime = e->mtimeHi;
    SetFileMTime( outPath, &ft );
}

/* Create an empty file entry (no compressed data). */
static int WriteEmptyEntry( const SzEntry *e, const char *destDir )
{
    char  outPath[SZ_MAX_NAME * 4];
    FILE *fout;
    if ( !destDir ) return SZ_OK;              /* test only */
    BuildPath( outPath, sizeof( outPath ), destDir, e->name );
    if ( !ArcWantWrite( outPath ) )
        return SZ_OK;                  /* exists and the user chose to keep it */
    MakeParentTree( outPath );
    fout = fopen( outPath, "wb" );
    if ( !fout ) return SZ_ERR_WRITE;
    fclose( fout );
    ApplySzTime( outPath, e );
    return SZ_OK;
}

/* Create a directory entry (and its parents). */
static void WriteDirEntry( const SzEntry *e, const char *destDir )
{
    char outPath[SZ_MAX_NAME * 4];
    if ( !destDir ) return;                    /* test only */
    BuildPath( outPath, sizeof( outPath ), destDir, e->name );
    MakeTree( outPath );
}

/*===========================================================================
 * Streaming folder extraction
 *
 * A folder's decoded bytes are pushed through a short pipeline rather than
 * collected in one buffer:
 *
 *     packed file bytes -> LZMA/LZMA2 (ring dictionary) -> [BCJ] -> sink
 *
 * The sink knows where each entry starts and ends inside the folder's output
 * stream, so it opens each file, writes and CRCs it as the bytes arrive, and
 * closes it at its boundary.  Nothing but the dictionary and small work
 * buffers is ever held, so a 500 MB solid folder costs the same as a 5 MB one.
 *===========================================================================*/

/*---- Packed input: read the folder's compressed bytes from the archive ---- */
typedef struct {
    FILE  *fp;
    UInt32 remaining;
} SzPackSrc;

static UInt32 SzReadPacked( void *user, Byte *buf, UInt32 len )
{
    SzPackSrc *s = (SzPackSrc *)user;
    size_t     n;

    if ( len > s->remaining ) len = s->remaining;
    if ( len == 0 ) return 0;
    n = fread( buf, 1, len, s->fp );
    s->remaining -= (UInt32)n;
    return (UInt32)n;
}

/*---- BCJ x86 stage: filters the decoder's output on its way to the sink --- */
#define SZ_BCJ_BUF   32768

typedef struct {
    SzBcj    st;
    Byte    *buf;            /* carry-over bytes + the run being converted */
    UInt32   nCarry;
    LzmaEmit next;           /* downstream sink */
    void    *nextUser;
} SzBcjStage;

static int SzBcjEmit( void *user, const Byte *data, UInt32 len )
{
    SzBcjStage *s = (SzBcjStage *)user;

    while ( len )
    {
        UInt32 take = SZ_BCJ_BUF - s->nCarry;
        UInt32 have, done;

        if ( take > len ) take = len;
        memcpy( s->buf + s->nCarry, data, take );
        have = s->nCarry + take;
        data += take;
        len  -= take;

        done = BcjX86Chunk( &s->st, s->buf, have );
        if ( done && !s->next( s->nextUser, s->buf, done ) )
            return 0;

        s->nCarry = have - done;
        if ( s->nCarry )
            memmove( s->buf, s->buf + done, s->nCarry );
    }
    return 1;
}

/* Push the last few bytes (too short to hold a rel32 operand) through as-is. */
static int SzBcjFinish( SzBcjStage *s )
{
    if ( s->nCarry )
    {
        if ( !s->next( s->nextUser, s->buf, s->nCarry ) ) return 0;
        s->nCarry = 0;
    }
    return 1;
}

/*---- BCJ2 stage ---------------------------------------------------------- *
 * BCJ2 splits x86 branch TARGETS out of the code into two side streams, so
 * that the main stream compresses without the noise of absolute addresses.
 * Rebuilding the code needs four inputs at once:
 *
 *   main  - the code with the target operands removed   (streamed)
 *   call  - the 4-byte big-endian targets of E8 CALLs   (buffered)
 *   jump  - the same for E9 JMPs and 0F 8x Jcc          (buffered)
 *   rc    - a range-coded bit per branch OPCODE saying whether that opcode
 *           really had its target extracted                (buffered)
 *
 * Only 'main' is streamed; the other three are held whole, which is what
 * keeps this affordable.  They are small - a few bytes per branch - and
 * their sizes are known from the header before anything is allocated, so an
 * oversized one is refused rather than attempted.  The converter itself is
 * a state machine over the main bytes: nothing about it needs main in RAM.
 *
 * The transform is position-dependent: a stored target is absolute and the
 * emitted operand is relative, so the output position at the operand has to
 * be tracked exactly - which is why outPos counts EMITTED bytes, including
 * the four this stage inserts itself.
 *-------------------------------------------------------------------------- */

/* Branch opcodes BCJ2 considers: E8, E9, and the 0F 8x two-byte Jcc. */
#define SZ_IS_J( b0, b1 ) ( ( ( (b1) & 0xFE ) == 0xE8 ) || \
                            ( (b0) == 0x0F && ( (b1) & 0xF0 ) == 0x80 ) )

#define SZ_RC_TOP_VALUE   ( 1UL << 24 )
#define SZ_RC_MODEL_TOTAL ( 1 << 11 )
#define SZ_RC_MOVE_BITS   5

typedef struct {
    /* range-coded control stream, held whole */
    const Byte *rc;
    UInt32      rcLen, rcPos;
    UInt32      range, code;
    UInt16      probs[256 + 2];

    /* branch target streams, held whole */
    const Byte *call;  UInt32 callLen, callPos;
    const Byte *jump;  UInt32 jumpLen, jumpPos;

    /* output position and the byte before the current one */
    UInt32      outPos, outSize;
    Byte        prevByte;

    Byte       *obuf;             /* staging, SZ_BCJ_BUF bytes */
    UInt32      oN;

    LzmaEmit    next;             /* downstream sink */
    void       *nextUser;
    int         err;              /* SZ_OK or SZ_ERR_DATA on a short stream */
} SzBcj2Stage;

static void Bcj2Init( SzBcj2Stage *s )
{
    int i;
    for ( i = 0; i < 256 + 2; i++ )
        s->probs[i] = SZ_RC_MODEL_TOTAL >> 1;
    s->rcPos    = 0;
    s->callPos  = 0;
    s->jumpPos  = 0;
    s->outPos   = 0;
    s->prevByte = 0;
    s->oN       = 0;
    s->err      = SZ_OK;
    s->range    = 0xFFFFFFFFUL;
    s->code     = 0;
}

/* Prime the range decoder: five bytes, the first of which is discarded. */
static int Bcj2RcStart( SzBcj2Stage *s )
{
    int i;
    if ( s->rcLen < 5 ) { s->err = SZ_ERR_DATA; return 0; }
    s->rcPos = 1;                              /* byte 0 is always zero */
    s->code  = 0;
    for ( i = 0; i < 4; i++ )
        s->code = ( s->code << 8 ) | (UInt32)s->rc[s->rcPos++];
    s->range = 0xFFFFFFFFUL;
    return 1;
}

/* One adaptive bit.  Returns 0/1, or -1 when the rc stream runs out. */
static int Bcj2Bit( SzBcj2Stage *s, unsigned idx )
{
    UInt16 *p = &s->probs[idx];
    UInt32  bound = ( s->range >> 11 ) * (UInt32)*p;
    int     sym;

    if ( s->code < bound )
    {
        s->range = bound;
        *p = (UInt16)( *p + ( ( SZ_RC_MODEL_TOTAL - *p ) >> SZ_RC_MOVE_BITS ) );
        sym = 0;
    }
    else
    {
        s->range -= bound;
        s->code  -= bound;
        *p = (UInt16)( *p - ( *p >> SZ_RC_MOVE_BITS ) );
        sym = 1;
    }
    while ( s->range < SZ_RC_TOP_VALUE )
    {
        if ( s->rcPos >= s->rcLen ) { s->err = SZ_ERR_DATA; return -1; }
        s->code   = ( s->code << 8 ) | (UInt32)s->rc[s->rcPos++];
        s->range <<= 8;
    }
    return sym;
}

static int Bcj2Put( SzBcj2Stage *s, Byte b )
{
    if ( s->outPos >= s->outSize ) return 1;   /* folder complete; drop */
    s->obuf[s->oN++] = b;
    s->outPos++;
    if ( s->oN == SZ_BCJ_BUF )
    {
        if ( !s->next( s->nextUser, s->obuf, s->oN ) ) return 0;
        s->oN = 0;
    }
    return 1;
}

/* Emit callback: receives the main stream and rebuilds the original code. */
static int SzBcj2Emit( void *user, const Byte *data, UInt32 len )
{
    SzBcj2Stage *s = (SzBcj2Stage *)user;
    UInt32       i;

    for ( i = 0; i < len; i++ )
    {
        Byte b = data[i];

        if ( !Bcj2Put( s, b ) ) return 0;

        if ( SZ_IS_J( s->prevByte, b ) )
        {
            unsigned idx = ( b == 0xE8 ) ? 2 + (unsigned)s->prevByte
                         : ( b == 0xE9 ) ? 1 : 0;
            int bit = Bcj2Bit( s, idx );
            if ( bit < 0 ) return 0;

            if ( bit )
            {
                const Byte *v;
                UInt32      dest;

                if ( b == 0xE8 )
                {
                    if ( s->callPos + 4 > s->callLen )
                        { s->err = SZ_ERR_DATA; return 0; }
                    v = s->call + s->callPos;
                    s->callPos += 4;
                }
                else
                {
                    if ( s->jumpPos + 4 > s->jumpLen )
                        { s->err = SZ_ERR_DATA; return 0; }
                    v = s->jump + s->jumpPos;
                    s->jumpPos += 4;
                }

                /* stored big-endian and absolute; emitted little-endian and
                 * relative to the address just past the operand */
                dest = ( (UInt32)v[0] << 24 ) | ( (UInt32)v[1] << 16 ) |
                       ( (UInt32)v[2] <<  8 ) |   (UInt32)v[3];
                dest -= ( s->outPos + 4 );

                if ( !Bcj2Put( s, (Byte)( dest       ) ) ) return 0;
                if ( !Bcj2Put( s, (Byte)( dest >>  8 ) ) ) return 0;
                if ( !Bcj2Put( s, (Byte)( dest >> 16 ) ) ) return 0;
                s->prevByte = (Byte)( dest >> 24 );
                if ( !Bcj2Put( s, s->prevByte ) ) return 0;
            }
            else
                s->prevByte = b;
        }
        else
            s->prevByte = b;
    }
    return 1;
}

static int SzBcj2Finish( SzBcj2Stage *s )
{
    if ( s->oN )
    {
        if ( !s->next( s->nextUser, s->obuf, s->oN ) ) return 0;
        s->oN = 0;
    }
    return 1;
}

/* Absolute packed-stream index feeding coder 'coderIdx' of folder 'fo', or
 * -1 if that coder is not fed by a packed stream. */
static int SzCoderPackIndex( SzArchive *a, SzFolder *fo, int coderIdx )
{
    int rel = FolderPackForIn( fo, fo->coders[coderIdx].firstIn );
    UInt32 abs;
    if ( rel < 0 ) return -1;
    abs = fo->firstPackStream + (UInt32)rel;
    if ( abs >= a->numPackStreams ) return -1;
    return (int)abs;
}

/* Decode one sub-coder of a BCJ2 folder whole into a fresh buffer.  Sizes are
 * known from the header, so an oversized stream is refused before any
 * allocation is attempted.  The caller frees *outBuf. */
static int SzDecodeSubCoder( SzArchive *a, SzFolder *fo, int coderIdx,
                             Byte **outBuf, UInt32 *outLen )
{
    SzCoder *cd = &fo->coders[coderIdx];
    int      pk = SzCoderPackIndex( a, fo, coderIdx );
    UInt32   packOff, packSize, unpackSize;
    Byte    *packBuf = NULL, *dst = NULL;
    int      rc = SZ_OK;

    *outBuf = NULL;
    *outLen = 0;
    if ( pk < 0 ) return SZ_ERR_FORMAT;

    packOff    = a->baseOffset + a->packOffset[pk];
    packSize   = a->packSizes[pk];
    unpackSize = cd->unpackSize;

    if ( packSize > SZ_MAX_BUFFER_SIZE || unpackSize > SZ_MAX_BUFFER_SIZE )
        return SZ_ERR_NORAM;

    packBuf = (Byte *)malloc( packSize ? packSize : 1 );
    if ( !packBuf ) return SZ_ERR_MEMORY;
    if ( fseek( a->fp, (long)packOff, SEEK_SET ) != 0 ||
         fread( packBuf, 1, packSize, a->fp ) != packSize )
    {
        free( packBuf );
        return SZ_ERR_READ;
    }

    dst = (Byte *)malloc( unpackSize ? unpackSize : 1 );
    if ( !dst ) { free( packBuf ); return SZ_ERR_MEMORY; }

    /* Buffered decode: dst doubles as the match dictionary, so this costs
     * the output size and no separate dictionary. */
    if ( cd->kind == SZ_C_COPY )
    {
        if ( packSize != unpackSize ) rc = SZ_ERR_DATA;
        else memcpy( dst, packBuf, unpackSize );
    }
    else if ( cd->kind == SZ_C_LZMA )
    {
        if ( cd->propsSize < 5 ) rc = SZ_ERR_UNSUPPORTED;
        else
        {
            UInt32 dictSize = cd->props[1] | ( (UInt32)cd->props[2] << 8 ) |
                              ( (UInt32)cd->props[3] << 16 ) |
                              ( (UInt32)cd->props[4] << 24 );
            if ( dictSize > SZ_MAX_DICT_SIZE ) rc = SZ_ERR_NORAM;
            else rc = LzmaDecode( cd->props, cd->propsSize,
                                  packBuf, packSize, dst, unpackSize );
        }
    }
    else if ( cd->kind == SZ_C_LZMA2 )
    {
        if ( cd->propsSize < 1 ) rc = SZ_ERR_UNSUPPORTED;
        else
        {
            Byte pb = cd->props[0];
            if ( pb > 40 ) rc = SZ_ERR_UNSUPPORTED;
            else
            {
                UInt32 dictSize = ( pb == 40 )
                    ? 0xFFFFFFFFUL
                    : ( (UInt32)( 2 | ( pb & 1 ) ) << ( pb / 2 + 11 ) );
                if ( dictSize > SZ_MAX_DICT_SIZE ) rc = SZ_ERR_NORAM;
                else rc = Lzma2Decode( pb, packBuf, packSize, dst, unpackSize );
            }
        }
    }
    else
        rc = SZ_ERR_UNSUPPORTED;

    free( packBuf );
    if ( rc != SZ_OK ) { free( dst ); return rc; }
    *outBuf = dst;
    *outLen = unpackSize;
    return SZ_OK;
}

/* Read a folder's raw (uncoded) packed stream whole - the BCJ2 rc stream. */
static int SzReadRawPack( SzArchive *a, int packIdx, Byte **outBuf,
                          UInt32 *outLen )
{
    UInt32 off, len;
    Byte  *buf;

    *outBuf = NULL;
    *outLen = 0;
    if ( packIdx < 0 || (UInt32)packIdx >= a->numPackStreams )
        return SZ_ERR_FORMAT;
    off = a->baseOffset + a->packOffset[packIdx];
    len = a->packSizes[packIdx];
    if ( len > SZ_MAX_BUFFER_SIZE ) return SZ_ERR_NORAM;

    buf = (Byte *)malloc( len ? len : 1 );
    if ( !buf ) return SZ_ERR_MEMORY;
    if ( fseek( a->fp, (long)off, SEEK_SET ) != 0 ||
         fread( buf, 1, len, a->fp ) != len )
    {
        free( buf );
        return SZ_ERR_READ;
    }
    *outBuf = buf;
    *outLen = len;
    return SZ_OK;
}

/*---- The sink: folder bytes -> the right file, CRC-checked ---------------- */
typedef struct {
    SzArchive  *a;
    const char *destDir;      /* NULL = test only: CRC but write nothing */
    const int  *order;        /* this folder's entries, ascending offset  */
    int         count;
    int         next;         /* next entry in 'order' to open            */
    const Byte *want;         /* per-entry selection flags, NULL = all    */
    UInt32      pos;          /* absolute position in the folder stream   */

    int         cur;          /* entry currently open, -1 = none          */
    int         curSkip;      /* declined overwrite: decode past, keep    */
    UInt32      curEnd;       /* folder offset where it ends              */
    FILE       *out;          /* NULL when testing or when not wanted     */
    UInt32      crc;          /* running CRC of the current entry         */
    char        curPath[SZ_MAX_NAME * 4];

    UInt32      folderCrc;    /* running CRC of the whole folder output   */
    SzProgress  prog;
    void       *user;
    int         rc;           /* first error, reported by the caller      */
} SzSink;

static int SzSinkWanted( SzSink *s, int idx )
{
    return s->want ? ( s->want[idx] != 0 ) : 1;
}

/* Start the next entry at the current position. */
static int SzSinkOpen( SzSink *s )
{
    int            idx = s->order[s->next];
    const SzEntry *e   = &s->a->entries[idx];

    s->cur     = idx;
    s->curEnd  = e->offsetInFolder + e->size;
    s->crc     = Crc32Init();
    s->out     = NULL;
    s->curSkip = 0;
    s->next++;

    if ( !SzSinkWanted( s, idx ) )
        return 1;                       /* decode past it, write nothing */

    if ( s->prog &&
         !s->prog( s->user, idx, s->a->numEntries, e->name ) )
    {
        s->rc = SZ_ERR_CANCEL;
        return 0;
    }

    if ( s->destDir )
    {
        BuildPath( s->curPath, sizeof( s->curPath ), s->destDir, e->name );
        /* Declined overwrite: decode past like an unwanted entry, and never
         * CRC-fail/remove/timestamp the file the user chose to keep. */
        if ( !ArcWantWrite( s->curPath ) )
        {
            s->curSkip = 1;
            return 1;
        }
        MakeParentTree( s->curPath );
        s->out = fopen( s->curPath, "wb" );
        if ( !s->out ) { s->rc = SZ_ERR_WRITE; return 0; }
    }
    return 1;
}

/* Finish the entry at its boundary: verify the CRC and stamp the file. */
static int SzSinkClose( SzSink *s )
{
    const SzEntry *e     = &s->a->entries[s->cur];
    int            check = SzSinkWanted( s, s->cur ) && !s->curSkip;

    if ( s->out )
    {
        fclose( s->out );
        s->out = NULL;
    }

    if ( check && e->hasCrc && Crc32Done( s->crc ) != e->crc )
    {
        /* The bytes are wrong: drop what we wrote rather than leave a corrupt
         * file (the buffered decoder used to CRC before writing at all). */
        if ( s->destDir ) remove( s->curPath );
        s->rc  = SZ_ERR_CRC;
        s->cur = -1;
        return 0;
    }

    if ( check && s->destDir )
        ApplySzTime( s->curPath, e );

    s->cur = -1;
    return 1;
}

static int SzSinkEmit( void *user, const Byte *data, UInt32 len )
{
    SzSink *s = (SzSink *)user;

    s->folderCrc = Crc32Update( s->folderCrc, data, len );

    while ( len )
    {
        UInt32 take;

        if ( s->cur < 0 )
        {
            UInt32 off;

            if ( s->next >= s->count )      /* nothing left to write */
            {
                s->pos += len;
                return 1;
            }
            off = s->a->entries[s->order[s->next]].offsetInFolder;
            if ( s->pos < off )             /* gap between entries */
            {
                UInt32 skip = off - s->pos;
                if ( skip > len ) skip = len;
                s->pos += skip; data += skip; len -= skip;
                continue;
            }
            if ( s->pos > off ) { s->rc = SZ_ERR_DATA; return 0; }
            if ( !SzSinkOpen( s ) ) return 0;
            if ( s->pos == s->curEnd )      /* zero-length entry */
            {
                if ( !SzSinkClose( s ) ) return 0;
                continue;
            }
        }

        take = s->curEnd - s->pos;
        if ( take > len ) take = len;

        s->crc = Crc32Update( s->crc, data, take );
        if ( s->out && fwrite( data, 1, take, s->out ) != take )
        {
            s->rc = SZ_ERR_WRITE;
            return 0;
        }

        s->pos += take; data += take; len -= take;
        if ( s->pos == s->curEnd && !SzSinkClose( s ) ) return 0;
    }
    return 1;
}

/*
 * Decode one folder straight to disk.  'want' is a per-entry flag array (NULL
 * = every entry in the folder); entries that are not wanted are decoded past
 * but not written, which is what a solid folder requires.
 */
static int SzStreamFolder( SzArchive *a, UInt32 fIdx, const char *destDir,
                           const Byte *want, SzProgress prog, void *user )
{
    SzFolder   *fo       = &a->folders[fIdx];
    UInt32      packOff, packSize;
    SzPackSrc   src;
    SzSink      sink;
    SzBcjStage  bcj;
    SzBcj2Stage bcj2;
    Byte       *callBuf = NULL, *jumpBuf = NULL, *rcBuf = NULL;
    UInt32      callLen = 0,     jumpLen = 0,     rcLen = 0;
    LzmaEmit    emit     = SzSinkEmit;
    void       *emitUser = &sink;
    int        *order    = NULL;
    int         mainPack;
    int         i, n = 0;
    int         rc = SZ_OK;

    /* Parsed but not decodable (unknown codec, encryption, an odd coder
     * graph).  Only this folder's entries are lost; the rest extract. */
    if ( fo->unsupported )                     return SZ_ERR_UNSUPPORTED;

    /* Which packed stream feeds the main coder.  For a plain folder that is
     * the folder's only one; for BCJ2 it is whichever of the four the bind
     * pairs say, which need not be the first. */
    mainPack = fo->isBcj2
             ? SzCoderPackIndex( a, fo, fo->bcj2Main )
             : (int)fo->firstPackStream;
    if ( mainPack < 0 || (UInt32)mainPack >= a->numPackStreams )
        return SZ_ERR_FORMAT;
    packOff  = a->baseOffset + a->packOffset[mainPack];
    packSize = a->packSizes[mainPack];

    if ( packSize > SZ_MAX_PACK_SIZE )         return SZ_ERR_TOOBIG;
    if ( fo->unpackSize > SZ_MAX_UNPACK_SIZE ) return SZ_ERR_TOOBIG;

    /* This folder's entries, in stream order. */
    for ( i = 0; i < a->numEntries; i++ )
        if ( a->entries[i].folderIndex == (int)fIdx ) n++;
    if ( n == 0 ) return SZ_OK;

    order = (int *)malloc( (size_t)n * sizeof( int ) );
    if ( !order ) return SZ_ERR_MEMORY;
    n = 0;
    for ( i = 0; i < a->numEntries; i++ )
        if ( a->entries[i].folderIndex == (int)fIdx ) order[n++] = i;
    {   /* sort by offset (they are produced in order, but do not rely on it) */
        int j, k;
        for ( j = 1; j < n; j++ )
        {
            int v = order[j];
            for ( k = j - 1; k >= 0 &&
                  a->entries[order[k]].offsetInFolder >
                  a->entries[v].offsetInFolder; k-- )
                order[k + 1] = order[k];
            order[k + 1] = v;
        }
    }

    sink.a         = a;
    sink.destDir   = destDir;
    sink.order     = order;
    sink.count     = n;
    sink.next      = 0;
    sink.want      = want;
    sink.pos       = 0;
    sink.cur       = -1;
    sink.curSkip   = 0;
    sink.curEnd    = 0;
    sink.out       = NULL;
    sink.crc       = 0;
    sink.folderCrc = Crc32Init();
    sink.prog      = prog;
    sink.user      = user;
    sink.rc        = SZ_OK;

    bcj2.obuf = NULL;
    if ( fo->isBcj2 )
    {
        /* Bring up the three side streams before touching main.  Their
         * sub-decodes are sequential and buffered, so their scratch is gone
         * again before the main dictionary is allocated. */
        rc = SzDecodeSubCoder( a, fo, fo->bcj2Call, &callBuf, &callLen );
        if ( rc == SZ_OK )
            rc = SzDecodeSubCoder( a, fo, fo->bcj2Jump, &jumpBuf, &jumpLen );
        if ( rc == SZ_OK )
            rc = SzReadRawPack( a,
                     (int)( fo->firstPackStream + fo->bcj2RcPack ),
                     &rcBuf, &rcLen );
        if ( rc == SZ_OK )
        {
            bcj2.obuf = (Byte *)malloc( SZ_BCJ_BUF );
            if ( !bcj2.obuf ) rc = SZ_ERR_MEMORY;
        }
        if ( rc != SZ_OK )
        {
            if ( callBuf ) free( callBuf );
            if ( jumpBuf ) free( jumpBuf );
            if ( rcBuf )   free( rcBuf );
            if ( bcj2.obuf ) free( bcj2.obuf );
            free( order );
            return rc;
        }

        bcj2.rc       = rcBuf;   bcj2.rcLen   = rcLen;
        bcj2.call     = callBuf; bcj2.callLen = callLen;
        bcj2.jump     = jumpBuf; bcj2.jumpLen = jumpLen;
        bcj2.outSize  = fo->unpackSize;
        bcj2.next     = SzSinkEmit;
        bcj2.nextUser = &sink;
        Bcj2Init( &bcj2 );
        if ( !Bcj2RcStart( &bcj2 ) )
        {
            free( callBuf ); free( jumpBuf ); free( rcBuf );
            free( bcj2.obuf ); free( order );
            return SZ_ERR_DATA;
        }
        emit     = SzBcj2Emit;
        emitUser = &bcj2;
    }

    bcj.buf = NULL;
    if ( fo->filter == SZ_F_BCJ_X86 )
    {
        if ( fo->unpackSize != fo->mainUnpackSize )
        {
            free( order );
            return SZ_ERR_DATA;              /* BCJ must preserve size */
        }
        bcj.buf = (Byte *)malloc( SZ_BCJ_BUF );
        if ( !bcj.buf ) { free( order ); return SZ_ERR_MEMORY; }
        BcjInit( &bcj.st );
        bcj.nCarry   = 0;
        bcj.next     = SzSinkEmit;
        bcj.nextUser = &sink;
        emit         = SzBcjEmit;
        emitUser     = &bcj;
    }

    src.fp        = a->fp;
    src.remaining = packSize;
    if ( fseek( a->fp, (long)packOff, SEEK_SET ) != 0 )
        rc = SZ_ERR_READ;

    if ( rc == SZ_OK )
    {
        if ( fo->method == SZ_M_COPY )
        {
            /* Stored: hand the packed bytes straight to the pipeline. */
            Byte *buf = (Byte *)malloc( SZ_BCJ_BUF );
            if ( !buf ) rc = SZ_ERR_MEMORY;
            else
            {
                UInt32 left = packSize;
                if ( packSize != fo->mainUnpackSize ) rc = SZ_ERR_DATA;
                while ( rc == SZ_OK && left )
                {
                    UInt32 got = SzReadPacked( &src, buf,
                                     ( left < SZ_BCJ_BUF ) ? left : SZ_BCJ_BUF );
                    if ( got == 0 ) { rc = SZ_ERR_READ; break; }
                    if ( !emit( emitUser, buf, got ) )
                        rc = sink.rc ? sink.rc : SZ_ERR_CANCEL;
                    left -= got;
                }
                free( buf );
            }
        }
        else if ( fo->method == SZ_M_LZMA )
        {
            if ( fo->propsSize < 5 )
                rc = SZ_ERR_UNSUPPORTED;
            else
            {
                UInt32 dictSize = fo->props[1] | ( (UInt32)fo->props[2] << 8 ) |
                                  ( (UInt32)fo->props[3] << 16 ) |
                                  ( (UInt32)fo->props[4] << 24 );
                if ( dictSize > SZ_MAX_DICT_SIZE )
                    rc = SZ_ERR_NORAM;
                else
                    rc = LzmaDecodeStream( fo->props, fo->propsSize,
                                           SzReadPacked, &src, packSize,
                                           fo->mainUnpackSize, dictSize,
                                           emit, emitUser );
            }
        }
        else /* SZ_M_LZMA2 */
        {
            if ( fo->propsSize < 1 )
                rc = SZ_ERR_UNSUPPORTED;
            else
            {
                Byte pbyte = fo->props[0];
                if ( pbyte > 40 )
                    rc = SZ_ERR_UNSUPPORTED;
                else
                {
                    UInt32 dictSize = ( pbyte == 40 )
                        ? 0xFFFFFFFFUL
                        : ( (UInt32)( 2 | ( pbyte & 1 ) ) << ( pbyte / 2 + 11 ) );
                    if ( dictSize > SZ_MAX_DICT_SIZE )
                        rc = SZ_ERR_NORAM;
                    else
                        rc = Lzma2DecodeStream( pbyte,
                                                SzReadPacked, &src, packSize,
                                                fo->mainUnpackSize, dictSize,
                                                emit, emitUser );
                }
            }
        }
    }

    if ( rc == SZ_OK && bcj.buf && !SzBcjFinish( &bcj ) )
        rc = sink.rc ? sink.rc : SZ_ERR_CANCEL;

    if ( fo->isBcj2 )
    {
        if ( rc == SZ_OK && !SzBcj2Finish( &bcj2 ) )
            rc = sink.rc ? sink.rc : SZ_ERR_CANCEL;
        /* A short call/jump/rc stream shows up as a stage error, not as a
         * decoder error - the main stream was fine. */
        if ( rc == SZ_OK && bcj2.err != SZ_OK )
            rc = bcj2.err;
    }

    /* A sink error (write failure, CRC mismatch, cancel) surfaces as a cancel
     * from the decoder; report what actually went wrong. */
    if ( sink.rc != SZ_OK && ( rc == SZ_OK || rc == SZ_ERR_CANCEL ) )
        rc = sink.rc;

    if ( rc == SZ_OK && sink.pos != fo->unpackSize )
        rc = SZ_ERR_DATA;
    if ( rc == SZ_OK && fo->hasCrc && Crc32Done( sink.folderCrc ) != fo->crc )
        rc = SZ_ERR_CRC;

    /* A file still open here was cut short (cancel, bad data, disk full):
     * remove the fragment rather than leave a truncated file behind. */
    if ( sink.out )
    {
        fclose( sink.out );
        remove( sink.curPath );
    }
    if ( bcj.buf )   free( bcj.buf );
    if ( bcj2.obuf ) free( bcj2.obuf );
    if ( callBuf )   free( callBuf );
    if ( jumpBuf )   free( jumpBuf );
    if ( rcBuf )     free( rcBuf );
    free( order );
    return rc;
}

int SzExtractAll( SzArchive *a, const char *destDir,
                  SzProgress prog, void *user )
{
    int    rc = SZ_OK;
    UInt32 f;
    int    i;

    if ( !a ) return SZ_ERR_FORMAT;

    /* 1. Directory entries first, so files can drop straight in. */
    for ( i = 0; i < a->numEntries; i++ )
    {
        const SzEntry *e = &a->entries[i];
        if ( e->isDir && e->name[0] )
            WriteDirEntry( e, destDir );
    }

    /* 2. Folder by folder, straight from the decoder to the files.  A folder
     * we cannot decode costs its own entries and nothing more: remember that
     * it happened, carry on, and report it once at the end.  Aborting here
     * would throw away every supported entry after the first bad folder. */
    for ( f = 0; f < a->numFolders; f++ )
    {
        int frc = SzStreamFolder( a, f, destDir, NULL, prog, user );
        if ( frc == SZ_ERR_UNSUPPORTED || frc == SZ_ERR_NORAM )
        {
            if ( rc == SZ_OK ) rc = frc;      /* first reason wins */
            continue;
        }
        if ( frc ) return frc;
    }

    /* 3. Empty files (no folder, not a directory). */
    for ( i = 0; i < a->numEntries; i++ )
    {
        const SzEntry *e = &a->entries[i];
        if ( e->folderIndex == -1 && !e->isDir && e->name[0] )
        {
            int erc;
            if ( prog && !prog( user, i, a->numEntries, e->name ) )
                return SZ_ERR_CANCEL;
            erc = WriteEmptyEntry( e, destDir );
            if ( erc ) return erc;
        }
    }

    /* SZ_OK, or the first skipped folder's reason. */
    return rc;
}

/*
 * Extract a chosen subset of entries (by entry index).  A folder holding any
 * selected entry is decoded once; unselected entries in it are decoded past
 * (a solid folder cannot be entered in the middle) but not written.
 */
int SzExtractItems( SzArchive *a, const int *indices, int count,
                    const char *destDir, SzProgress prog, void *user )
{
    int    rc = SZ_OK;
    int    k;
    UInt32 f;
    Byte  *want;

    if ( !a ) return SZ_ERR_FORMAT;
    if ( count <= 0 ) return SZ_OK;

    want = (Byte *)calloc( a->numEntries ? a->numEntries : 1, 1 );
    if ( !want ) return SZ_ERR_MEMORY;
    for ( k = 0; k < count; k++ )
        if ( indices[k] >= 0 && indices[k] < a->numEntries )
            want[indices[k]] = 1;

    /* 1. Selected directory entries. */
    for ( k = 0; k < count; k++ )
    {
        int idx = indices[k];
        if ( idx < 0 || idx >= a->numEntries ) continue;
        if ( a->entries[idx].isDir && a->entries[idx].name[0] )
            WriteDirEntry( &a->entries[idx], destDir );
    }

    /* 2. Folders holding at least one selected entry.  As in SzExtractAll, a
     * folder we cannot decode is noted and stepped over rather than ending
     * the run - the rest of the selection still extracts. */
    for ( f = 0; f < a->numFolders; f++ )
    {
        int used = 0, frc;
        for ( k = 0; k < a->numEntries; k++ )
            if ( want[k] && a->entries[k].folderIndex == (int)f ) { used = 1; break; }
        if ( !used ) continue;

        frc = SzStreamFolder( a, f, destDir, want, prog, user );
        if ( frc == SZ_ERR_UNSUPPORTED || frc == SZ_ERR_NORAM )
        {
            if ( rc == SZ_OK ) rc = frc;
            continue;
        }
        if ( frc ) { rc = frc; break; }
    }

    /* 3. Selected empty files. */
    for ( k = 0; k < count && rc == SZ_OK; k++ )
    {
        int            idx = indices[k];
        const SzEntry *e;
        if ( idx < 0 || idx >= a->numEntries ) continue;
        e = &a->entries[idx];
        if ( e->folderIndex == -1 && !e->isDir && e->name[0] )
        {
            if ( prog && !prog( user, idx, a->numEntries, e->name ) )
                rc = SZ_ERR_CANCEL;
            else
                rc = WriteEmptyEntry( e, destDir );
        }
    }

    free( want );
    return rc;
}

const char *SzErrorText( int code )
{
    switch ( code )
    {
    case SZ_OK:             return "OK";
    case SZ_ERR_OPEN:       return "Cannot open the archive file.";
    case SZ_ERR_READ:       return "Read error or truncated archive.";
    case SZ_ERR_SIG:        return "Not a 7z archive.";
    case SZ_ERR_CRC:        return "CRC mismatch (corrupt archive).";
    case SZ_ERR_FORMAT:     return "Malformed or unexpected archive header.";
    case SZ_ERR_UNSUPPORTED:return "Unsupported archive feature, compression "
                                   "method, or encryption.";
    case SZ_ERR_MEMORY:     return "Out of memory.";
    case SZ_ERR_TOOBIG:     return "Archive exceeds a configured size limit.";
    case SZ_ERR_WRITE:      return "Cannot create or write an output file.";
    case SZ_ERR_DATA:       return "Corrupt compressed data.";
    case SZ_ERR_CANCEL:     return "Cancelled.";
    case SZ_ERR_NORAM:      return "Not enough memory: this archive needs a "
                                   "larger compression dictionary than this "
                                   "build can hold.";
    default:                return "Unknown error.";
    }
}
