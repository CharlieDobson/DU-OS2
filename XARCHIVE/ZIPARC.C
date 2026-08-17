/*===========================================================================
 * ZIPARC.C  -  PKZIP (.zip) parsing and extraction
 * Target: MSVC 2.2  Win32s
 *
 * Ported from the DOS/Win16 UNZIP module (S:\CPP\DUSOURCE\MISC\UNZIP.CPP):
 * far pointers stripped, Win16 GlobalAllocPtr replaced by malloc, the DOS
 * file-time restore removed.  Restructured from a one-shot ExtractZip into the
 * open/list/extract-subset shape that SZARC uses, and returning the shared
 * SZ_ERR_* result codes.  Supports stored (0) and deflated (8) entries.
 *===========================================================================*/

#include <windows.h>     /* lstrcpyn */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>      /* _mkdir */

#include "ziparc.h"
#include "platform.h"   /* SetFileDosMTime */

/*---- ZIP structure constants --------------------------------------------- */
#define SIG_LOCAL   0x04034B50UL
#define SIG_CENT    0x02014B50UL
#define SIG_END     0x06054B50UL

#define METHOD_STORE   0
#define METHOD_IMPLODE 6
#define METHOD_DEFLATE 8

#define FLAG_DATADESC  0x0008
#define FLAG_ENCRYPTED 0x0001

#pragma pack(1)
typedef struct {
    unsigned long  sig;
    unsigned short verNeeded;
    unsigned short flags;
    unsigned short method;
    unsigned short modTime;
    unsigned short modDate;
    unsigned long  crc32;
    unsigned long  compSize;
    unsigned long  uncompSize;
    unsigned short fnLen;
    unsigned short extraLen;
} LocalHdr;

typedef struct {
    unsigned long  sig;
    unsigned short verMade;
    unsigned short verNeeded;
    unsigned short flags;
    unsigned short method;
    unsigned short modTime;
    unsigned short modDate;
    unsigned long  crc32;
    unsigned long  compSize;
    unsigned long  uncompSize;
    unsigned short fnLen;
    unsigned short extraLen;
    unsigned short commentLen;
    unsigned short diskStart;
    unsigned short intAttr;
    unsigned long  extAttr;
    long           localOffset;
} CentralHdr;

typedef struct {
    unsigned long  sig;
    unsigned short diskNum;
    unsigned short diskStart;
    unsigned short entriesThis;
    unsigned short entriesTotal;
    unsigned long  dirSize;
    unsigned long  dirOffset;
    unsigned short commentLen;
} EndRec;
#pragma pack()

/*---- CRC-32 (self-contained, reflected, poly 0xEDB88320) ----------------- */
static unsigned long crcTable[256];
static int           crcInited = 0;

static void BuildCrcTable( void )
{
    unsigned long c;
    int n, k;
    for ( n = 0; n < 256; n++ )
    {
        c = (unsigned long)n;
        for ( k = 0; k < 8; k++ )
            c = ( c & 1 ) ? ( 0xEDB88320UL ^ ( c >> 1 ) ) : ( c >> 1 );
        crcTable[n] = c;
    }
    crcInited = 1;
}

static unsigned long UpdateCrc( unsigned long crc,
                                const unsigned char *buf, unsigned long len )
{
    if ( !crcInited ) BuildCrcTable();
    crc ^= 0xFFFFFFFFUL;
    while ( len-- )
        crc = crcTable[(unsigned char)( crc ^ *buf++ )] ^ ( crc >> 8 );
    return crc ^ 0xFFFFFFFFUL;
}

/*===========================================================================
 * Inflate (RFC 1951) - sliding-window decoder streaming to a FILE
 *===========================================================================*/
#define WSIZE        32768U
#define WSIZE_MASK   (WSIZE - 1)

typedef struct {
    FILE         *fp;
    unsigned long bitsLeft;
    unsigned long bitBuf;
    unsigned long bytesLeft;
    int           eof;
} BitReader;

static void BrInit( BitReader *br, FILE *fp, unsigned long compSize )
{
    br->fp = fp; br->bitsLeft = 0; br->bitBuf = 0;
    br->bytesLeft = compSize; br->eof = 0;
}

static int BrFill( BitReader *br )
{
    int c;
    if ( br->bytesLeft == 0 ) { br->eof = 1; return -1; }
    c = fgetc( br->fp );
    if ( c == EOF ) { br->eof = 1; return -1; }
    br->bytesLeft--;
    br->bitBuf |= ( (unsigned long)(unsigned char)c ) << br->bitsLeft;
    br->bitsLeft += 8;
    return 0;
}

static unsigned long BrBits( BitReader *br, unsigned n )
{
    while ( br->bitsLeft < n )
        if ( BrFill( br ) ) return 0;
    return br->bitBuf & ( ( 1UL << n ) - 1UL );
}

static void BrConsume( BitReader *br, unsigned n )
{
    br->bitBuf >>= n;
    br->bitsLeft -= n;
}

static unsigned long BrRead( BitReader *br, unsigned n )
{
    unsigned long v = BrBits( br, n );
    BrConsume( br, n );
    return v;
}

#define MAX_BITS   15
#define MAX_CODES  288

typedef struct {
    short          vals[MAX_CODES];
    unsigned short offsets[MAX_BITS + 2];
    unsigned short maxlen;
} HuffTree;

static int BuildHuff( HuffTree *ht, const unsigned char *lens, int n )
{
    int i, len;
    unsigned short cnt[MAX_BITS + 1];
    unsigned short nxt[MAX_BITS + 1];

    memset( cnt, 0, sizeof( cnt ) );
    for ( i = 0; i < n; i++ )
        if ( lens[i] ) cnt[(int)lens[i]]++;

    ht->maxlen = 0;
    for ( len = 1; len <= MAX_BITS; len++ )
        if ( cnt[len] && (unsigned)len > ht->maxlen )
            ht->maxlen = (unsigned short)len;
    if ( ht->maxlen == 0 ) return 0;

    ht->offsets[0] = 0;
    for ( len = 1; len <= MAX_BITS + 1; len++ )
        ht->offsets[len] = ht->offsets[len-1] + cnt[len-1];

    memcpy( nxt, ht->offsets, sizeof( nxt ) );
    for ( i = 0; i < n; i++ )
        if ( lens[i] )
            ht->vals[nxt[(int)lens[i]]++] = (short)i;

    return 0;
}

static int HuffDecode( HuffTree *ht, BitReader *br )
{
    unsigned long code, base;
    unsigned int  len, count;

    code = 0; base = 0;
    for ( len = 1; len <= (unsigned int)ht->maxlen; len++ )
    {
        code = ( code << 1 ) | (unsigned long)BrRead( br, 1 );
        if ( br->eof ) return -1;
        count = ht->offsets[len+1] - ht->offsets[len];
        if ( count && code >= base && code < base + count )
            return ht->vals[ht->offsets[len] + (unsigned int)( code - base )];
        base = ( base + count ) << 1;
    }
    return -1;
}

static void StaticLens( unsigned char *llit, unsigned char *ldist )
{
    int i;
    for ( i = 0;   i <= 143; i++ ) llit[i] = 8;
    for ( i = 144; i <= 255; i++ ) llit[i] = 9;
    for ( i = 256; i <= 279; i++ ) llit[i] = 7;
    for ( i = 280; i <= 287; i++ ) llit[i] = 8;
    for ( i = 0;   i <= 31;  i++ ) ldist[i] = 5;
}

static const unsigned short lenBase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const unsigned char lenExtra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const unsigned short distBase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,
    8193,12289,16385,24577
};
static const unsigned char distExtra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

#define OBUF_SIZE 8192U

typedef struct {
    FILE          *out;      /* file sink (NULL when testing or mem sink)   */
    unsigned char *mem;      /* memory sink (NULL when file / test)         */
    unsigned long  memPos;
    unsigned long  memCap;
    unsigned char *buf;
    unsigned int   len;
    unsigned long  crc;
    int            rc;       /* SZ_OK / SZ_ERR_WRITE */
} OutBuf;

static int OutFlush( OutBuf *o )
{
    if ( o->len )
    {
        o->crc = UpdateCrc( o->crc, o->buf, (unsigned long)o->len );
        if ( o->out && fwrite( o->buf, 1, o->len, o->out ) != o->len )
        {                                    /* out == NULL => test only */
            o->rc = SZ_ERR_WRITE;
            return -1;
        }
        if ( o->mem )                        /* memory sink (.imz unwrap)  */
        {
            if ( o->memPos + o->len > o->memCap )
            {
                o->rc = SZ_ERR_WRITE;
                return -1;
            }
            memcpy( o->mem + o->memPos, o->buf, o->len );
            o->memPos += o->len;
        }
        o->len = 0;
    }
    return 0;
}

static int OutByte( OutBuf *o, unsigned char b )
{
    o->buf[o->len++] = b;
    if ( o->len >= OBUF_SIZE )
        return OutFlush( o );
    return 0;
}

static int InflateBlock( BitReader *br, OutBuf *o,
                         unsigned char *window, unsigned int *wpos,
                         HuffTree *hl, HuffTree *hd )
{
    int sym, li, di, len;
    unsigned int dist, back;
    unsigned char b;

    for ( ;; )
    {
        sym = HuffDecode( hl, br );
        if ( sym < 0 ) return SZ_ERR_DATA;
        if ( sym == 256 ) break;
        if ( sym < 256 )
        {
            b = (unsigned char)sym;
            window[*wpos] = b;
            *wpos = ( *wpos + 1 ) & WSIZE_MASK;
            if ( OutByte( o, b ) ) return o->rc;
        }
        else
        {
            li = sym - 257;
            if ( li < 0 || li >= 29 ) return SZ_ERR_DATA;
            len = lenBase[li] + (int)BrRead( br, lenExtra[li] );
            if ( br->eof ) return SZ_ERR_DATA;
            di = HuffDecode( hd, br );
            if ( br->eof || di < 0 || di >= 30 ) return SZ_ERR_DATA;
            dist = distBase[di] + (unsigned int)BrRead( br, distExtra[di] );
            if ( br->eof ) return SZ_ERR_DATA;
            while ( len-- > 0 )
            {
                back = ( *wpos + WSIZE - dist ) & WSIZE_MASK;
                b = window[back];
                window[*wpos] = b;
                *wpos = ( *wpos + 1 ) & WSIZE_MASK;
                if ( OutByte( o, b ) ) return o->rc;
            }
        }
    }
    return SZ_OK;
}

static const int clOrder[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

static int Inflate( FILE *in, FILE *out,
                    unsigned char *memSink, unsigned long memCap,
                    unsigned long compSize, unsigned long uncompSize,
                    unsigned long *crcOut )
{
    unsigned char *window;
    unsigned char  llit[288], ldist[32];
    unsigned char  clLens[19];
    unsigned char  allLens[288 + 32];
    HuffTree      *hl, *hd, *hcl;
    BitReader      br;
    OutBuf         o;
    unsigned int   wpos;
    int            rc, bfinal, btype;
    int            hlit, hdist, hclen, ci, total, idx, s, rep;
    unsigned int   blen, bnlen;
    unsigned char  stbyte;
    unsigned int   excess;

    (void)uncompSize;
    window = NULL; hl = NULL; hd = NULL; hcl = NULL;
    wpos = 0; rc = SZ_OK;
    o.buf = NULL;

    window = (unsigned char *)malloc( WSIZE );
    hl     = (HuffTree *)malloc( sizeof( HuffTree ) );
    hd     = (HuffTree *)malloc( sizeof( HuffTree ) );
    o.buf  = (unsigned char *)malloc( OBUF_SIZE );
    if ( !window || !hl || !hd || !o.buf ) { rc = SZ_ERR_MEMORY; goto done; }

    o.out = out; o.mem = memSink; o.memPos = 0; o.memCap = memCap;
    o.len = 0; o.crc = 0; o.rc = SZ_OK;
    BrInit( &br, in, compSize );

    for ( ;; )
    {
        bfinal = (int)BrRead( &br, 1 );
        btype  = (int)BrRead( &br, 2 );

        if ( btype == 0 )
        {
            excess = (unsigned int)( br.bitsLeft & 7U );
            if ( excess ) BrConsume( &br, excess );
            blen  = (unsigned int)BrRead( &br, 16 );
            bnlen = (unsigned int)BrRead( &br, 16 );
            (void)bnlen;
            while ( blen-- > 0 )
            {
                stbyte = (unsigned char)BrRead( &br, 8 );
                if ( br.eof ) { rc = SZ_ERR_DATA; goto done; }
                window[wpos] = stbyte;
                wpos = ( wpos + 1 ) & WSIZE_MASK;
                if ( OutByte( &o, stbyte ) ) { rc = o.rc; goto done; }
            }
        }
        else if ( btype == 1 )
        {
            StaticLens( llit, ldist );
            BuildHuff( hl, llit, 288 );
            BuildHuff( hd, ldist, 32 );
            rc = InflateBlock( &br, &o, window, &wpos, hl, hd );
            if ( rc ) goto done;
        }
        else if ( btype == 2 )
        {
            hlit  = (int)BrRead( &br, 5 ) + 257;
            hdist = (int)BrRead( &br, 5 ) + 1;
            hclen = (int)BrRead( &br, 4 ) + 4;

            memset( clLens, 0, sizeof( clLens ) );
            for ( ci = 0; ci < hclen; ci++ )
                clLens[clOrder[ci]] = (unsigned char)BrRead( &br, 3 );

            hcl = (HuffTree *)malloc( sizeof( HuffTree ) );
            if ( !hcl ) { rc = SZ_ERR_MEMORY; goto done; }
            BuildHuff( hcl, clLens, 19 );

            total = hlit + hdist;
            idx   = 0;
            while ( idx < total )
            {
                s = HuffDecode( hcl, &br );
                if ( s < 0 ) { rc = SZ_ERR_DATA; goto done; }
                if ( s < 16 )
                    allLens[idx++] = (unsigned char)s;
                else if ( s == 16 )
                {
                    rep = (int)BrRead( &br, 2 ) + 3;
                    stbyte = idx ? allLens[idx-1] : 0;
                    while ( rep-- && idx < total ) allLens[idx++] = stbyte;
                }
                else if ( s == 17 )
                {
                    rep = (int)BrRead( &br, 3 ) + 3;
                    while ( rep-- && idx < total ) allLens[idx++] = 0;
                }
                else
                {
                    rep = (int)BrRead( &br, 7 ) + 11;
                    while ( rep-- && idx < total ) allLens[idx++] = 0;
                }
            }
            BuildHuff( hl, allLens,        hlit );
            BuildHuff( hd, allLens + hlit, hdist );
            free( hcl ); hcl = NULL;
            rc = InflateBlock( &br, &o, window, &wpos, hl, hd );
            if ( rc ) goto done;
        }
        else
        {
            rc = SZ_ERR_DATA; goto done;
        }

        if ( bfinal ) break;
    }

    if ( OutFlush( &o ) ) { rc = o.rc; goto done; }
    *crcOut = o.crc;

done:
    if ( window ) free( window );
    if ( hl )     free( hl );
    if ( hd )     free( hd );
    if ( hcl )    free( hcl );
    if ( o.buf )  free( o.buf );
    return rc;
}

/*===========================================================================
 * Explode (ZIP method 6, Imploded) - the old PKZIP 1.x algorithm: 2 or 3
 * Shannon-Fano trees feeding a 4K/8K sliding dictionary.  Bits are read
 * LSB-first (same BitReader as inflate); the SF trees are byte-aligned at the
 * start of the data.  General-purpose flag bit 1 = 8K dict, bit 2 = a literal
 * tree is present (3 trees); with 2 trees literals are raw 8-bit bytes.
 *===========================================================================*/
typedef struct {
    unsigned short codeval[256];      /* code (clen bits, MSB-first) per symbol */
    unsigned char  clen[256];         /* bit length per symbol                  */
    int            n;
    int            maxlen;
} SfTree;

/* Read a run-length-encoded Shannon-Fano tree and derive each symbol's code.
   The code assignment is PKWARE's (APPNOTE): symbols ordered by (length, value),
   codes handed out longest-first with a 1<<(16-len) increment. */
static int SfLoad( SfTree *t, BitReader *br, int n )
{
    int           numbytes, i, sym, run, blen, j, k, lastlen;
    unsigned char order[256];
    unsigned long code, incr;

    t->n = n; t->maxlen = 0;
    numbytes = (int)BrRead( br, 8 ) + 1;
    sym = 0;
    for ( i = 0; i < numbytes; i++ )
    {
        int by = (int)BrRead( br, 8 );
        blen = ( by & 0x0F ) + 1;
        run  = ( ( by >> 4 ) & 0x0F ) + 1;
        while ( run-- > 0 )
        {
            if ( sym >= n ) return -1;
            t->clen[sym++] = (unsigned char)blen;
        }
    }
    if ( sym != n ) return -1;

    k = 0;
    for ( blen = 1; blen <= 16; blen++ )
        for ( j = 0; j < n; j++ )
            if ( t->clen[j] == blen )
            {
                order[k++] = (unsigned char)j;
                if ( blen > t->maxlen ) t->maxlen = blen;
            }

    code = 0; incr = 0; lastlen = 0;
    for ( i = n - 1; i >= 0; i-- )
    {
        int s = order[i];
        code += incr;
        if ( t->clen[s] != lastlen )
        { lastlen = t->clen[s]; incr = 1UL << ( 16 - lastlen ); }
        t->codeval[s] = (unsigned short)( code >> ( 16 - t->clen[s] ) );
    }
    return 0;
}

static int SfDecode( SfTree *t, BitReader *br )
{
    unsigned long acc = 0;
    int nbits = 0, i;
    while ( nbits < t->maxlen )
    {
        unsigned bit = (unsigned)BrRead( br, 1 );
        if ( br->eof ) return -1;
        acc = ( acc << 1 ) | bit;            /* codes assembled MSB-first */
        nbits++;
        for ( i = 0; i < t->n; i++ )
            if ( t->clen[i] == nbits && t->codeval[i] == acc )
                return i;
    }
    return -1;
}

static int Explode( FILE *in, FILE *out,
                    unsigned char *memSink, unsigned long memCap,
                    unsigned long compSize, unsigned long uncompSize,
                    unsigned int gpflag, unsigned long *crcOut )
{
    unsigned char *window;
    SfTree        *litT, *lenT, *distT;
    BitReader      br;
    OutBuf         o;
    unsigned int   wpos;
    unsigned long  produced;
    int            rc, bigDict, threeTrees, distLow, minMatch;

    window = NULL; litT = NULL; lenT = NULL; distT = NULL;
    wpos = 0; produced = 0; rc = SZ_OK;
    o.buf = NULL;

    bigDict    = ( gpflag & 2 ) ? 1 : 0;     /* bit 1: 8K (else 4K) dictionary */
    threeTrees = ( gpflag & 4 ) ? 1 : 0;     /* bit 2: literal tree present    */
    distLow    = bigDict ? 7 : 6;
    minMatch   = threeTrees ? 3 : 2;

    window = (unsigned char *)malloc( WSIZE );
    lenT   = (SfTree *)malloc( sizeof( SfTree ) );
    distT  = (SfTree *)malloc( sizeof( SfTree ) );
    o.buf  = (unsigned char *)malloc( OBUF_SIZE );
    if ( threeTrees ) litT = (SfTree *)malloc( sizeof( SfTree ) );
    if ( !window || !lenT || !distT || !o.buf || ( threeTrees && !litT ) )
    { rc = SZ_ERR_MEMORY; goto edone; }
    memset( window, 0, WSIZE );

    o.out = out; o.mem = memSink; o.memPos = 0; o.memCap = memCap;
    o.len = 0; o.crc = 0; o.rc = SZ_OK;
    BrInit( &br, in, compSize );

    if ( threeTrees && SfLoad( litT, &br, 256 ) ) { rc = SZ_ERR_DATA; goto edone; }
    if ( SfLoad( lenT,  &br, 64 ) )               { rc = SZ_ERR_DATA; goto edone; }
    if ( SfLoad( distT, &br, 64 ) )               { rc = SZ_ERR_DATA; goto edone; }

    while ( produced < uncompSize )
    {
        int bit = (int)BrRead( &br, 1 );
        if ( br.eof ) { rc = SZ_ERR_DATA; goto edone; }
        if ( bit )                               /* literal */
        {
            int lit;
            if ( threeTrees )
            {
                lit = SfDecode( litT, &br );
                if ( lit < 0 ) { rc = SZ_ERR_DATA; goto edone; }
            }
            else
                lit = (int)BrRead( &br, 8 );
            window[wpos] = (unsigned char)lit;
            wpos = ( wpos + 1 ) & WSIZE_MASK;
            if ( OutByte( &o, (unsigned char)lit ) ) { rc = o.rc; goto edone; }
            produced++;
        }
        else                                     /* (length, distance) match */
        {
            unsigned long dlow = BrRead( &br, (unsigned)distLow );
            int           dhigh = SfDecode( distT, &br );
            int           len   = SfDecode( lenT, &br );
            unsigned long dist;
            int           n;

            if ( dhigh < 0 || len < 0 ) { rc = SZ_ERR_DATA; goto edone; }
            dist = ( ( (unsigned long)dhigh ) << distLow ) | dlow;
            dist += 1;
            n = len;
            if ( n == 63 ) n += (int)BrRead( &br, 8 );
            n += minMatch;
            while ( n-- > 0 && produced < uncompSize )
            {
                unsigned int  back = ( wpos + WSIZE - (unsigned int)dist ) & WSIZE_MASK;
                unsigned char b    = window[back];
                window[wpos] = b;
                wpos = ( wpos + 1 ) & WSIZE_MASK;
                if ( OutByte( &o, b ) ) { rc = o.rc; goto edone; }
                produced++;
            }
        }
    }

    if ( OutFlush( &o ) ) { rc = o.rc; goto edone; }
    *crcOut = o.crc;

edone:
    if ( window ) free( window );
    if ( litT )   free( litT );
    if ( lenT )   free( lenT );
    if ( distT )  free( distT );
    if ( o.buf )  free( o.buf );
    return rc;
}

/*===========================================================================
 * ZIP container
 *===========================================================================*/
/* The per-entry tables are sized to the central directory's own entry count
 * (SZ_MAX_FILES is only the refusal limit).  Fixed tables here would cost
 * ~5 MB for every zip opened, however few files it holds. */
struct ZipArchive {
    FILE    *fp;
    long     bias;                          /* SFX stub offset bias         */
    int      numEntries;
    ZipEntry      *entries;                 /* public: name/size/crc/isDir  */
    unsigned long *compSize;                /* extraction metadata          */
    unsigned int  *method;
    unsigned int  *flags;
    long          *localOffset;
};

/* Allocate the parallel entry tables for 'n' entries.  0 on failure. */
static int ZipAllocTables( ZipArchive *z, unsigned n )
{
    if ( n == 0 ) n = 1;
    z->entries     = (ZipEntry *)calloc( n, sizeof( ZipEntry ) );
    z->compSize    = (unsigned long *)calloc( n, sizeof( unsigned long ) );
    z->method      = (unsigned int *)calloc( n, sizeof( unsigned int ) );
    z->flags       = (unsigned int *)calloc( n, sizeof( unsigned int ) );
    z->localOffset = (long *)calloc( n, sizeof( long ) );
    return ( z->entries && z->compSize && z->method &&
             z->flags && z->localOffset );
}

/*---- Path helpers -------------------------------------------------------- */

/* Create every directory component of 'path' (back-slash separated).  If
 * includeLast, also create the final component as a directory. */
static void MakeDirs( const char *path, int includeLast )
{
    char  buf[SZ_MAX_NAME * 2];
    char *p;

    lstrcpyn( buf, path, sizeof( buf ) );
    p = buf;
    if ( p[0] && p[1] == ':' ) p += 2;     /* skip drive   */
    if ( *p == '\\' ) p++;                 /* skip root    */

    for ( ; *p; p++ )
        if ( *p == '\\' )
        {
            *p = '\0';
            _mkdir( buf );
            *p = '\\';
        }
    if ( includeLast )
        _mkdir( buf );
}

/* Build destDir\name, with the name made filesystem-safe (invalid characters,
 * device names, 8.3 truncation on DOS/Win32s - see ArcFsName in ARCFILE.C). */
static void BuildOut( char *dst, int dstSize,
                      const char *destDir, const char *name )
{
    char        fsname[SZ_MAX_NAME];
    const char *s = fsname;
    int         n = 0;

    ArcFsName( fsname, sizeof( fsname ), name );
    if ( destDir && destDir[0] )
    {
        while ( destDir[n] && n < dstSize - 2 ) { dst[n] = destDir[n]; n++; }
        if ( n > 0 && dst[n-1] != '\\' ) dst[n++] = '\\';
    }
    while ( *s && n < dstSize - 1 ) dst[n++] = *s++;
    dst[n] = '\0';
}

/*---- End-of-central-directory scan --------------------------------------- */
static int FindEndRec( FILE *fp, EndRec *er, long *eocdPos )
{
    long          fileLen, pos;
    unsigned char buf[4];

    fseek( fp, 0L, SEEK_END );
    fileLen = ftell( fp );
    if ( fileLen < 22L ) return SZ_ERR_SIG;

    pos = fileLen - 22L;
    for ( ; pos >= 0L && ( fileLen - pos ) < 65557L; pos-- )
    {
        fseek( fp, pos, SEEK_SET );
        if ( fread( buf, 1, 4, fp ) != 4 ) continue;
        if ( buf[0]=='P' && buf[1]=='K' && buf[2]==5 && buf[3]==6 )
        {
            fseek( fp, pos, SEEK_SET );
            if ( fread( er, sizeof( EndRec ), 1, fp ) == 1 &&
                 er->sig == SIG_END &&
                 pos + (long)sizeof( EndRec ) + er->commentLen == fileLen )
            {
                *eocdPos = pos;
                return SZ_OK;
            }
        }
    }
    return SZ_ERR_SIG;
}

/*---- Open + parse central directory -------------------------------------- */
int ZipOpen( const char *path, ZipArchive **out )
{
    ZipArchive *z;
    EndRec      er;
    CentralHdr  ch;
    char        fname[SZ_MAX_NAME];
    long        eocdPos, resumePos;
    unsigned    i;
    int         rc;

    *out = NULL;

    z = (ZipArchive *)calloc( 1, sizeof( ZipArchive ) );
    if ( !z ) return SZ_ERR_MEMORY;

    z->fp = fopen( path, "rb" );
    if ( !z->fp ) { ZipClose( z ); return SZ_ERR_OPEN; }

    rc = FindEndRec( z->fp, &er, &eocdPos );
    if ( rc ) { ZipClose( z ); return rc; }

    if ( er.diskNum != 0 || er.diskStart != 0 )
    { ZipClose( z ); return SZ_ERR_UNSUPPORTED; }

    if ( er.entriesTotal > SZ_MAX_FILES )
    { ZipClose( z ); return SZ_ERR_TOOBIG; }

    if ( !ZipAllocTables( z, er.entriesTotal ) )
    { ZipClose( z ); return SZ_ERR_MEMORY; }

    /* A self-extracting EXE prefixes the archive with a stub, so stored
     * offsets are short by its size; the central directory ends where the
     * EOCD begins, giving the bias to add to every offset. */
    z->bias = ( eocdPos - (long)er.dirSize ) - (long)er.dirOffset;

    if ( fseek( z->fp, (long)er.dirOffset + z->bias, SEEK_SET ) != 0 )
    { ZipClose( z ); return SZ_ERR_FORMAT; }

    z->numEntries = 0;
    for ( i = 0; i < er.entriesTotal; i++ )
    {
        ZipEntry *e;
        unsigned int fnLen;
        int          j, last;

        if ( fread( &ch, sizeof( CentralHdr ), 1, z->fp ) != 1 )
        { ZipClose( z ); return SZ_ERR_READ; }
        if ( ch.sig != SIG_CENT )
        { ZipClose( z ); return SZ_ERR_FORMAT; }

        fnLen = ( ch.fnLen < SZ_MAX_NAME - 1 ) ? ch.fnLen : SZ_MAX_NAME - 1;
        if ( fread( fname, 1, fnLen, z->fp ) != fnLen )
        { ZipClose( z ); return SZ_ERR_READ; }
        fname[fnLen] = '\0';
        /* skip any of the field we clamped, plus extra + comment */
        resumePos = ftell( z->fp ) + (long)( ch.fnLen - fnLen ) +
                    (long)ch.extraLen + (long)ch.commentLen;

        e = &z->entries[z->numEntries];
        for ( j = 0; fname[j]; j++ )
            e->name[j] = ( fname[j] == '/' ) ? '\\' : fname[j];
        e->name[j] = '\0';

        last = j - 1;
        e->isDir = ( last >= 0 && e->name[last] == '\\' );
        if ( e->isDir ) e->name[last] = '\0';     /* drop trailing slash */

        e->size       = ch.uncompSize;
        e->packed     = ch.compSize;
        e->crc        = ch.crc32;
        e->methodCode = ch.method;
        e->modDate    = ch.modDate;
        e->modTime    = ch.modTime;
        e->attrib     = ch.extAttr;
        z->compSize[z->numEntries]    = ch.compSize;
        z->method[z->numEntries]      = ch.method;
        z->flags[z->numEntries]       = ch.flags;
        z->localOffset[z->numEntries] = ch.localOffset;
        z->numEntries++;

        fseek( z->fp, resumePos, SEEK_SET );
    }

    *out = z;
    return SZ_OK;
}

int ZipNumEntries( ZipArchive *z )
{
    return z ? z->numEntries : 0;
}

const ZipEntry *ZipGetEntry( ZipArchive *z, int index )
{
    if ( !z || index < 0 || index >= z->numEntries ) return NULL;
    return &z->entries[index];
}

/*---- Extract one entry by index ------------------------------------------ */
static int ZipExtractIndex( ZipArchive *z, int idx, const char *destDir )
{
    ZipEntry     *e = &z->entries[idx];
    LocalHdr      lh;
    char          outPath[SZ_MAX_NAME * 4];
    FILE         *out;
    unsigned long crc, remain, wrote;
    unsigned int  toRead;
    unsigned char buf[512];
    int           rc;

    /* destDir == NULL means "test only": decode + CRC-check but write nothing. */
    if ( destDir ) BuildOut( outPath, sizeof( outPath ), destDir, e->name );

    if ( e->isDir )
    {
        if ( destDir && !ArcFlattenPaths() ) MakeDirs( outPath, 1 );
        return SZ_OK;
    }
    if ( destDir && !ArcWantWrite( outPath ) )
        return SZ_OK;                  /* exists and the user chose to keep it */

    if ( z->flags[idx] & FLAG_ENCRYPTED )                 return SZ_ERR_UNSUPPORTED;
    if ( z->method[idx] != METHOD_STORE &&
         z->method[idx] != METHOD_DEFLATE &&
         z->method[idx] != METHOD_IMPLODE )               return SZ_ERR_UNSUPPORTED;

    /* The local header repeats the name/extra fields; read it to find where
     * the compressed data actually starts (extra fields can differ from the
     * central directory copy). */
    if ( fseek( z->fp, z->localOffset[idx] + z->bias, SEEK_SET ) != 0 )
        return SZ_ERR_READ;
    if ( fread( &lh, sizeof( LocalHdr ), 1, z->fp ) != 1 )
        return SZ_ERR_READ;
    if ( lh.sig != SIG_LOCAL )
        return SZ_ERR_FORMAT;
    if ( fseek( z->fp, (long)lh.fnLen + (long)lh.extraLen, SEEK_CUR ) != 0 )
        return SZ_ERR_READ;

    if ( destDir )
    {
        MakeDirs( outPath, 0 );
        out = fopen( outPath, "wb" );
        if ( !out ) return SZ_ERR_WRITE;
    }
    else
        out = NULL;

    rc  = SZ_OK;
    crc = 0;
    if ( z->method[idx] == METHOD_STORE )
    {
        remain = e->size;
        wrote  = 0;
        while ( remain > 0 )
        {
            toRead = ( remain > 512UL ) ? 512U : (unsigned int)remain;
            if ( fread( buf, 1, toRead, z->fp ) != toRead ) { rc = SZ_ERR_READ;  break; }
            crc = UpdateCrc( crc, buf, toRead );
            if ( out && fwrite( buf, 1, toRead, out ) != toRead ) { rc = SZ_ERR_WRITE; break; }
            remain -= toRead;
            wrote  += toRead;
        }
    }
    else if ( z->method[idx] == METHOD_IMPLODE )
    {
        rc = Explode( z->fp, out, NULL, 0, z->compSize[idx], e->size,
                      z->flags[idx], &crc );
    }
    else
    {
        rc = Inflate( z->fp, out, NULL, 0, z->compSize[idx], e->size, &crc );
    }

    if ( out ) fclose( out );

    if ( rc == SZ_OK && crc != e->crc )
        rc = SZ_ERR_CRC;
    if ( rc == SZ_OK )
    {
        if ( destDir ) SetFileDosMTime( outPath, e->modDate, e->modTime );
    }
    else if ( destDir )
        remove( outPath );          /* don't leave a corrupt/partial file */
    return rc;
}

/* Decompress one entry wholly into memory (for unwrapping a .imz disk image). */
int ZipExtractToMemory( ZipArchive *z, int index,
                        unsigned char **outBuf, UInt32 *outLen )
{
    ZipEntry     *e;
    LocalHdr      lh;
    unsigned char *buf;
    unsigned long crc = 0;
    int           rc;

    *outBuf = NULL;
    *outLen = 0;
    if ( !z || index < 0 || index >= z->numEntries ) return SZ_ERR_FORMAT;

    e = &z->entries[index];
    if ( e->isDir )                                       return SZ_ERR_FORMAT;
    if ( z->flags[index] & FLAG_ENCRYPTED )               return SZ_ERR_UNSUPPORTED;
    if ( z->method[index] != METHOD_STORE &&
         z->method[index] != METHOD_DEFLATE &&
         z->method[index] != METHOD_IMPLODE )             return SZ_ERR_UNSUPPORTED;

    if ( fseek( z->fp, z->localOffset[index] + z->bias, SEEK_SET ) != 0 )
        return SZ_ERR_READ;
    if ( fread( &lh, sizeof( LocalHdr ), 1, z->fp ) != 1 )
        return SZ_ERR_READ;
    if ( lh.sig != SIG_LOCAL )
        return SZ_ERR_FORMAT;
    if ( fseek( z->fp, (long)lh.fnLen + (long)lh.extraLen, SEEK_CUR ) != 0 )
        return SZ_ERR_READ;

    buf = (unsigned char *)malloc( e->size ? e->size : 1 );
    if ( !buf ) return SZ_ERR_MEMORY;

    if ( z->method[index] == METHOD_STORE )
    {
        if ( e->size && fread( buf, 1, e->size, z->fp ) != e->size )
        { free( buf ); return SZ_ERR_READ; }
        crc = UpdateCrc( 0, buf, e->size );
        rc  = SZ_OK;
    }
    else if ( z->method[index] == METHOD_IMPLODE )
    {
        rc = Explode( z->fp, NULL, buf, e->size,
                      z->compSize[index], e->size, z->flags[index], &crc );
        if ( rc != SZ_OK ) { free( buf ); return rc; }
    }
    else
    {
        rc = Inflate( z->fp, NULL, buf, e->size,
                      z->compSize[index], e->size, &crc );
        if ( rc != SZ_OK ) { free( buf ); return rc; }
    }

    if ( crc != e->crc ) { free( buf ); return SZ_ERR_CRC; }

    *outBuf = buf;
    *outLen = e->size;
    return SZ_OK;
}

int ZipExtractAll( ZipArchive *z, const char *destDir,
                   SzProgress prog, void *user )
{
    int i, rc;
    if ( !z ) return SZ_ERR_FORMAT;
    for ( i = 0; i < z->numEntries; i++ )
    {
        if ( !z->entries[i].name[0] ) continue;
        if ( prog && !prog( user, i, z->numEntries, z->entries[i].name ) )
            return SZ_ERR_CANCEL;
        rc = ZipExtractIndex( z, i, destDir );
        if ( rc ) return rc;
    }
    return SZ_OK;
}

int ZipExtractItems( ZipArchive *z, const int *indices, int count,
                     const char *destDir, SzProgress prog, void *user )
{
    int k, idx, rc;
    if ( !z ) return SZ_ERR_FORMAT;
    for ( k = 0; k < count; k++ )
    {
        idx = indices[k];
        if ( idx < 0 || idx >= z->numEntries ) continue;
        if ( !z->entries[idx].name[0] ) continue;
        if ( prog && !prog( user, idx, z->numEntries, z->entries[idx].name ) )
            return SZ_ERR_CANCEL;
        rc = ZipExtractIndex( z, idx, destDir );
        if ( rc ) return rc;
    }
    return SZ_OK;
}

void ZipClose( ZipArchive *z )
{
    if ( z )
    {
        if ( z->fp )          fclose( z->fp );
        if ( z->entries )     free( z->entries );
        if ( z->compSize )    free( z->compSize );
        if ( z->method )      free( z->method );
        if ( z->flags )       free( z->flags );
        if ( z->localOffset ) free( z->localOffset );
        free( z );
    }
}
