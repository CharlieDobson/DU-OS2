/*===========================================================================
 * RAR5ARC.C  -  RAR 5.x parsing and extraction
 * Target: MSVC 2.2  Win32s
 *
 * PHASE 1: container parsing, listing, and STORED (method 0) extraction.
 * Compressed entries return SZ_ERR_UNSUPPORTED until the RAR5 unpack engine is
 * ported.
 *
 * MULTI-VOLUME (.partN.rar) works as it does in RARARC.C - volumes walked one
 * at a time, split entries stitched from the SPLIT_BEFORE/SPLIT_AFTER flags,
 * which in RAR5 live in the COMMON block header rather than the file flags.
 *
 * ENCRYPTION: AES-256-CBC keyed by PBKDF2-HMAC-SHA256 (ARCCRYP.C), per entry
 * from the crypt record in its extra area, and for -hp archives per BLOCK from
 * the crypt header at the front - see Rar5ReadBlock.  Both carry a password
 * check value, so a wrong password is named before anything is decrypted.

 *
 * RAR5 block: [HeadCRC 4][HeaderSize vint][header body: HeaderSize bytes]
 *             [data area: DataSize bytes].  vint = 7 bits/byte, little-endian,
 *             high bit = continuation.
 *===========================================================================*/

#include <windows.h>     /* lstrcpyn, SYSTEMTIME -> FILETIME */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>      /* _mkdir */

#include "rar5arc.h"
#include "crc32.h"
#include "volio.h"      /* volume naming + the one-handle volume set */
#include "arccryp.h"    /* RAR5 key derivation + AES-256-CBC */
#include "platform.h"    /* SetFileMTime */

/* header types */
#define H5_MAIN     1
#define H5_FILE     2
#define H5_SERVICE  3
#define H5_ENCRYPT  4
#define H5_ENDARC   5

/* header flags */
#define H5F_EXTRA   0x0001   /* extra-area size present */
#define H5F_DATA    0x0002   /* data-area size present  */
#define H5F_SPLIT_BEFORE 0x0008  /* block continues the previous volume  */
#define H5F_SPLIT_AFTER  0x0010  /* block carries on into the next       */

/* main-header archive flags */
#define H5A_VOLUME  0x0001   /* one volume of a set                      */
#define H5A_VOLNUM  0x0002   /* the volume number follows (absent on #1) */

#define RAR5_MAX_VOLUMES 400
#define VOL5_PATH_MAX    260

/* file flags */
#define F5_DIR      0x0001
#define F5_MTIME    0x0002
#define F5_CRC      0x0004

/* Extra-area record types (a file header carries a list of them after the
 * name).  Only the crypt record matters here; the rest are stepped over. */
#define X5_CRYPT    1

/* Crypt-record flags */
#define X5C_PSWCHECK  0x0001   /* an 8-byte password check value is present */
#define X5C_TWEAKCRC  0x0002   /* the stored CRC is an HMAC, not a CRC      */

/* Crypt-HEADER flags (the -hp block at the front of the archive).  Same bit
 * for the check value as the per-entry record above, deliberately: it is the
 * same eight bytes computed the same way. */
#define H5C_PSWCHECK  0x0001


#define R5_SALT_LEN   16
#define R5_IV_LEN     16

static const unsigned char RAR5_SIG[8] =
    { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00 };

/*---- buffered reader over one header body -------------------------------- */
typedef struct {
    const Byte *p;
    const Byte *end;
    int         err;
} BRd;

static UInt32 BrVint( BRd *b )
{
    UInt32 val = 0;
    int    shift = 0;
    for ( ;; )
    {
        Byte c;
        if ( b->p >= b->end ) { b->err = 1; break; }
        c = *b->p++;
        if ( shift < 32 )
            val |= ( (UInt32)( c & 0x7F ) ) << shift;
        shift += 7;
        if ( !( c & 0x80 ) ) break;
    }
    return val;
}

static UInt32 BrU32( BRd *b )
{
    UInt32 v;
    if ( b->p + 4 > b->end ) { b->err = 1; return 0; }
    v = (UInt32)b->p[0] | ( (UInt32)b->p[1] << 8 ) |
        ( (UInt32)b->p[2] << 16 ) | ( (UInt32)b->p[3] << 24 );
    b->p += 4;
    return v;
}

/* Read a vint straight from the file (for the HeaderSize field). */
static int RdVintFile( VolFile *fp, UInt32 *val, int *nBytes )
{
    UInt32 v = 0;
    int    shift = 0, n = 0, c;
    for ( ;; )
    {
        c = VolGetc( fp );
        if ( c == EOF ) return -1;
        n++;
        if ( shift < 32 )
            v |= ( (UInt32)( c & 0x7F ) ) << shift;
        shift += 7;
        if ( !( c & 0x80 ) ) break;
    }
    *val = v; *nBytes = n;
    return 0;
}

/*---- Unix time -> FILETIME (UTC), no __int64 ----------------------------- */
static void UnixToFileTime( UInt32 t, UInt32 *lo, UInt32 *hi )
{
    SYSTEMTIME st;
    FILETIME   ft;
    long   days = (long)( t / 86400UL );
    UInt32 secs = t % 86400UL;
    long   z, era, doe, yoe, y, doy, mp, d, m;

    z   = days + 719468;
    era = ( z >= 0 ? z : z - 146096 ) / 146097;
    doe = z - era * 146097;
    yoe = ( doe - doe/1460 + doe/36524 - doe/146096 ) / 365;
    y   = yoe + era * 400;
    doy = doe - ( 365*yoe + yoe/4 - yoe/100 );
    mp  = ( 5*doy + 2 ) / 153;
    d   = doy - ( 153*mp + 2 )/5 + 1;
    m   = ( mp < 10 ) ? mp + 3 : mp - 9;
    y  += ( m <= 2 );

    st.wYear   = (WORD)y;   st.wMonth  = (WORD)m;   st.wDay    = (WORD)d;
    st.wHour   = (WORD)( secs / 3600 );
    st.wMinute = (WORD)( ( secs % 3600 ) / 60 );
    st.wSecond = (WORD)( secs % 60 );
    st.wMilliseconds = 0;   st.wDayOfWeek = 0;

    if ( SystemTimeToFileTime( &st, &ft ) )
    { *lo = ft.dwLowDateTime; *hi = ft.dwHighDateTime; }
    else
    { *lo = 0; *hi = 0; }
}

/*---- archive object ------------------------------------------------------ */
/* Like RAR4, the entry count only emerges from the header walk, so the tables
 * start small and double instead of costing ~5 MB per archive up front. */
/*
 * One entry's encryption parameters, straight out of its crypt record.
 *
 * Held per ENTRY rather than per archive because RAR5 puts a fresh salt and IV
 * on every file, even though one password covers them all.  The derived key is
 * cached per archive (see keyValid below): the salt is what changes, and the
 * expensive PBKDF2 depends on it, so in practice the cache holds for exactly
 * as long as consecutive entries share a salt - which for a WinRAR archive is
 * never.  The cache is still worth having for the single-entry case and costs
 * one memcmp.
 */
typedef struct {
    int  encrypted;
    int  kdfCount;                    /* log2 of the PBKDF2 iteration count */
    int  tweakCrc;                    /* stored CRC is an HMAC, not a CRC   */
    int  hasPswCheck;
    Byte salt[ R5_SALT_LEN ];
    Byte iv[ R5_IV_LEN ];
    Byte pswCheck[8];
} Rar5Crypt;

/* One run of an entry's data inside one volume - see the same type in
 * RARARC.C.  RAR5 splits files across volumes exactly as RAR4 does; only the
 * flag encoding differs (SPLIT_BEFORE/AFTER live in the common block header
 * here rather than in the file-header flags). */
typedef struct {
    long   off;
    UInt32 len;
} Rar5Piece;

struct Rar5Archive {
    VolFile   *fp;
    int        numEntries;
    int        cap;                   /* entries the tables can hold */
    Rar5Entry *entries;
    long      *dataOffset;
    Rar5Crypt *crypt;                 /* parallel to entries               */

    /* Multi-volume; volumes is 1 for an ordinary archive. */
    int        volumes;
    int        volIncomplete;
    Rar5Piece *pieces;
    int        pieceN, pieceCap;
    int       *pieceFirst;
    int       *pieceCount;

    char       password[ AC_MAX_PW + 1 ];
    int        havePw;

    /* Derived-key cache, valid while cachedSalt/cachedCount match. */
    int        keyValid;
    Byte       cachedSalt[ R5_SALT_LEN ];
    int        cachedCount;
    Byte       key[32];
    Byte       hashKey[32];
    Byte       pswCheck[8];

    /* Encrypted HEADERS (-hp).  One key for the whole block chain, from the
     * salt in the crypt header - unlike the per-entry keys above, which change
     * with every file.  Each block still carries its own 16-byte IV. */
    int        hdrEncrypted;
    int        hdrKeyValid;
    int        hdrSawBlock;                /* an enciphered block was read   */
    Byte       hdrKey[32];
};


/* Make room for one more entry.  1 on success, 0 when out of memory, -1 when
 * the entry limit is what stopped us (see RarGrow in RARARC.C - the caller
 * has to report those two differently). */
static int Rar5Grow( Rar5Archive *z )
{
    int        n;
    Rar5Entry *ne;
    long      *nd;

    if ( z->numEntries < z->cap ) return 1;
    if ( (UInt32)z->numEntries >= SZ_MAX_FILES ) return -1;

    n = z->cap ? z->cap * 2 : 32;
    if ( (UInt32)n > SZ_MAX_FILES ) n = (int)SZ_MAX_FILES;
    if ( n <= z->numEntries ) return 0;

    ne = (Rar5Entry *)realloc( z->entries, (size_t)n * sizeof( Rar5Entry ) );
    if ( !ne ) return 0;
    z->entries = ne;

    nd = (long *)realloc( z->dataOffset, (size_t)n * sizeof( long ) );
    if ( !nd ) return 0;
    z->dataOffset = nd;

    {
        Rar5Crypt *nc = (Rar5Crypt *)realloc( z->crypt,
                                              (size_t)n * sizeof( Rar5Crypt ) );
        if ( !nc ) return 0;
        z->crypt = nc;
    }

    {
        int *pf = (int *)realloc( z->pieceFirst, (size_t)n * sizeof( int ) );
        int *pc;
        if ( !pf ) return 0;
        z->pieceFirst = pf;
        pc = (int *)realloc( z->pieceCount, (size_t)n * sizeof( int ) );
        if ( !pc ) return 0;
        z->pieceCount = pc;
    }

    z->cap = n;
    return 1;
}

/* Record one run of data for the entry being built - see RarAddPiece. */
static int Rar5AddPiece( Rar5Archive *z, int entry, long off, UInt32 len )
{
    if ( z->pieceN >= z->pieceCap )
    {
        int        nc = z->pieceCap ? z->pieceCap * 2 : 32;
        Rar5Piece *np = (Rar5Piece *)realloc( z->pieces,
                                              (size_t)nc * sizeof( Rar5Piece ) );
        if ( !np ) return 0;
        z->pieces   = np;
        z->pieceCap = nc;
    }
    z->pieces[z->pieceN].off = off;
    z->pieces[z->pieceN].len = len;
    if ( z->pieceCount[entry] == 0 )
        z->pieceFirst[entry] = z->pieceN;
    z->pieceCount[entry]++;
    z->pieceN++;
    return 1;
}

/* Read an entry's data, stepping over the volume headers between its pieces. */
typedef struct {
    Rar5Archive *z;
    int          first, count, pi;
    UInt32       used;
    int          started;
} Rar5DataReader;

static void Rar5DataInit( Rar5DataReader *r, Rar5Archive *z, int idx )
{
    r->z       = z;
    r->first   = z->pieceFirst[idx];
    r->count   = z->pieceCount[idx];
    r->pi      = 0;
    r->used    = 0;
    r->started = 0;
}

static int Rar5DataRead( Rar5DataReader *r, void *buf, UInt32 len )
{
    Byte  *p    = (Byte *)buf;
    UInt32 done = 0;

    while ( done < len )
    {
        Rar5Piece *pc;
        UInt32     avail, take;

        if ( r->pi >= r->count ) return 0;
        pc = &r->z->pieces[ r->first + r->pi ];

        if ( !r->started )
        {
            if ( VolSeek( r->z->fp, pc->off + (long)r->used, SEEK_SET ) != 0 )
                return 0;
            r->started = 1;
        }

        avail = pc->len - r->used;
        if ( avail == 0 ) { r->pi++; r->used = 0; r->started = 0; continue; }

        take = ( avail < len - done ) ? avail : len - done;
        if ( VolRead( p + done, 1, take, r->z->fp ) != take ) return 0;
        done    += take;
        r->used += take;
        if ( r->used >= pc->len ) { r->pi++; r->used = 0; r->started = 0; }
    }
    return 1;
}

/* Hand back the slack the doubling left over once the scan is done. */
static void Rar5Trim( Rar5Archive *z )
{
    int        n = z->numEntries ? z->numEntries : 1;
    Rar5Entry *ne;
    long      *nd;

    if ( n >= z->cap ) return;

    ne = (Rar5Entry *)realloc( z->entries, (size_t)n * sizeof( Rar5Entry ) );
    if ( ne ) z->entries = ne;
    nd = (long *)realloc( z->dataOffset, (size_t)n * sizeof( long ) );
    if ( nd ) z->dataOffset = nd;
    {
        Rar5Crypt *nc = (Rar5Crypt *)realloc( z->crypt,
                                              (size_t)n * sizeof( Rar5Crypt ) );
        if ( nc ) z->crypt = nc;
        if ( ne && nd && nc ) z->cap = n;
    }
}

/*---- path helpers -------------------------------------------------------- */
static void MakeDirs( const char *path, int includeLast )
{
    char  buf[SZ_MAX_NAME * 2];
    char *p;
    lstrcpyn( buf, path, sizeof( buf ) );
    p = buf;
    if ( p[0] && p[1] == ':' ) p += 2;
    if ( *p == '\\' ) p++;
    for ( ; *p; p++ )
        if ( *p == '\\' ) { *p = '\0'; _mkdir( buf ); *p = '\\'; }
    if ( includeLast )
        _mkdir( buf );
}

/* destDir\name with the name made filesystem-safe (ArcFsName, ARCFILE.C). */
static void BuildOut( char *dst, int dstSize,
                      const char *destDir, const char *name, int isDir )
{
    char        fsname[SZ_MAX_NAME];
    const char *s = fsname;
    int         n = 0;

    ArcFsName( fsname, sizeof( fsname ), name, isDir );
    if ( destDir && destDir[0] )
    {
        while ( destDir[n] && n < dstSize - 2 ) { dst[n] = destDir[n]; n++; }
        /* '/' counts as a separator too - see BuildPath in SZARC.C. */
        if ( n > 0 && dst[n-1] != '\\' && dst[n-1] != '/' ) dst[n++] = '\\';
    }
    while ( *s && n < dstSize - 1 ) dst[n++] = *s++;
    dst[n] = '\0';
}

/*---- reading one block ---------------------------------------------------- *
 * RAR5's -hp puts a HEAD_CRYPT block in the clear at the front of the archive
 * and enciphers every block after it, so past that point not even a block's
 * length can be read without the password.
 *
 * An encrypted block is
 *      [16-byte IV][ HeadCRC(4) HeaderSize(vint) body ] padded up to 16
 * with a fresh CBC chain per block.  Note what is inside the ciphertext: the
 * CRC and the length field are encrypted too, so the first cipher block has to
 * be decrypted before the block's own size is known.  Unlike RAR4, the IV
 * differs per block while the key does not.
 *
 * *hdrOut receives a malloc'd buffer of *headSizeOut bytes holding the header
 * body alone - the same thing the plain path produces, so the walk cannot tell
 * the two apart.  *bodyOut is where the data area starts.  RAR5_BLK_END means
 * a clean end of file.
 */
#define RAR5_BLK_END  (-1)

static int Rar5ReadBlock( Rar5Archive *z, long pos, Byte **hdrOut,
                          UInt32 *headSizeOut, long *bodyOut )
{
    Byte  *hdr;
    UInt32 headSize;
    int    vLen;

    *hdrOut = NULL;
    if ( VolSeek( z->fp, pos, SEEK_SET ) != 0 ) return RAR5_BLK_END;

    if ( !z->hdrEncrypted )
    {
        Byte crc[4];

        if ( VolRead( crc, 1, 4, z->fp ) != 4 ) return RAR5_BLK_END;
        if ( RdVintFile( z->fp, &headSize, &vLen ) != 0 ) return RAR5_BLK_END;
        if ( headSize == 0 || headSize > 0x100000UL ) return SZ_ERR_FORMAT;

        hdr = (Byte *)malloc( headSize );
        if ( !hdr ) return SZ_ERR_MEMORY;
        if ( VolRead( hdr, 1, headSize, z->fp ) != headSize )
        { free( hdr ); return SZ_ERR_READ; }

        *hdrOut      = hdr;
        *headSizeOut = headSize;
        *bodyOut     = pos + 4 + vLen + (long)headSize;
        return SZ_OK;
    }
    else
    {
        AesCbcState cbc;
        Byte        iv[ R5_IV_LEN ];
        Byte       *blk;
        long        want, padded;
        UInt32      i, v, shift;

        if ( VolRead( iv, 1, R5_IV_LEN, z->fp ) != R5_IV_LEN )
            return RAR5_BLK_END;

        blk = (Byte *)malloc( 16 );
        if ( !blk ) return SZ_ERR_MEMORY;
        if ( VolRead( blk, 1, 16, z->fp ) != 16 )
        { free( blk ); return RAR5_BLK_END; }
        AesCbcInit( &cbc, z->hdrKey, 32, iv );
        AesCbcDecrypt( &cbc, blk, 16 );

        /* The length vint, read out of the twelve plaintext bytes that follow
         * the CRC.  A vint never needs more than five. */
        v = 0; shift = 0; vLen = 0;
        for ( i = 4; i < 16; i++ )
        {
            Byte c = blk[i];
            vLen++;
            if ( shift < 32 ) v |= (UInt32)( c & 0x7F ) << shift;
            shift += 7;
            if ( !( c & 0x80 ) ) break;
        }
        headSize = v;
        if ( headSize == 0 || headSize > 0x100000UL )
        { free( blk ); return SZ_ERR_BADPASS; }

        want   = 4 + vLen + (long)headSize;
        padded = ( want + 15 ) & ~15L;
        {
            Byte *nb = (Byte *)realloc( blk, (size_t)padded );
            if ( !nb ) { free( blk ); return SZ_ERR_MEMORY; }
            blk = nb;
        }
        if ( padded > 16 )
        {
            if ( VolRead( blk + 16, 1, (size_t)( padded - 16 ), z->fp )
                     != (size_t)( padded - 16 ) )
            { free( blk ); return SZ_ERR_BADPASS; }
            AesCbcDecrypt( &cbc, blk + 16, (UInt32)( padded - 16 ) );
        }

        /* THE PASSWORD CHECK for every block after the first.  The crypt
         * header's own check value has already settled whether the password is
         * right, so a mismatch here is a damaged archive rather than a typo -
         * but it is reported as BADPASS all the same, because the only way to
         * reach it with a valid check value is a CRC collision on the check,
         * and "wrong password" is the likelier truth. */
        if ( Crc32Calc( blk + 4, (UInt32)( vLen + (long)headSize ) ) !=
             ( (UInt32)blk[0] | ( (UInt32)blk[1] << 8 ) |
               ( (UInt32)blk[2] << 16 ) | ( (UInt32)blk[3] << 24 ) ) )
        { free( blk ); return SZ_ERR_BADPASS; }

        hdr = (Byte *)malloc( headSize );
        if ( !hdr ) { free( blk ); return SZ_ERR_MEMORY; }
        memcpy( hdr, blk + 4 + vLen, headSize );
        free( blk );
        z->hdrSawBlock = 1;

        *hdrOut      = hdr;
        *headSizeOut = headSize;
        *bodyOut     = pos + R5_IV_LEN + padded;
        return SZ_OK;
    }
}

/*---- open + walk the blocks ---------------------------------------------- */
int Rar5OpenPw( const char *path, const char *pw, Rar5Archive **out )
{
    Rar5Archive *z;
    Byte         sig[8];
    long         pos;
    int          vol;
    int          lastSplitAfter = 0;
    int          rc = SZ_OK;

    *out = NULL;

    z = (Rar5Archive *)calloc( 1, sizeof( Rar5Archive ) );
    if ( !z ) return SZ_ERR_MEMORY;
    /* Gather the volume set - see the same block in RarOpenPw.  RAR5 only ever
     * uses the .partN.rar scheme, but VolRarNextName covers both and there is
     * no reason to teach this file the difference. */
    {
        char  first[VOL5_PATH_MAX];
        char  cur[VOL5_PATH_MAX];
        char  next[VOL5_PATH_MAX];
        char *names[RAR5_MAX_VOLUMES];
        int   n = 0, i;

        if ( !VolRarFirstName( path, first, sizeof first ) )
        { Rar5Close( z ); return SZ_ERR_OPEN; }
        {
            FILE *probe = fopen( first, "rb" );
            if ( probe ) fclose( probe );
            else         lstrcpyn( first, path, sizeof first );
        }

        lstrcpyn( cur, first, sizeof cur );
        for ( ;; )
        {
            FILE *probe = fopen( cur, "rb" );
            if ( !probe ) break;
            fclose( probe );

            names[n] = (char *)malloc( strlen( cur ) + 1 );
            if ( !names[n] ) { for ( i = 0; i < n; i++ ) free( names[i] );
                               Rar5Close( z ); return SZ_ERR_MEMORY; }
            strcpy( names[n], cur );
            n++;
            if ( n >= RAR5_MAX_VOLUMES ) break;
            if ( !VolRarNextName( cur, next, sizeof next ) ) break;
            lstrcpyn( cur, next, sizeof cur );
        }

        if ( n == 0 ) { Rar5Close( z ); return SZ_ERR_OPEN; }

        rc = VolOpenList( (const char *const *)names, n, &z->fp );
        for ( i = 0; i < n; i++ ) free( names[i] );
        if ( rc != SZ_OK ) { Rar5Close( z ); return rc; }
        z->volumes = n;
        rc = SZ_OK;
    }

    /* Before the walk, not after it: a -hp archive has no readable directory
     * without the password. */
    if ( pw && pw[0] )
    {
        lstrcpyn( z->password, pw, sizeof( z->password ) );
        z->havePw = 1;
    }

    if ( VolRead( sig, 1, 8, z->fp ) != 8 || memcmp( sig, RAR5_SIG, 8 ) != 0 )
    { Rar5Close( z ); return SZ_ERR_SIG; }

    z->numEntries = 0;

    /* Once per volume: each is a whole archive with its own signature, main
     * header and end block.  See the same loop in RarOpenPw. */
    for ( vol = 0; vol < z->volumes && rc == SZ_OK; vol++ )
    {
        long volBase = VolVolumeStart( z->fp, vol );
        long volEnd  = volBase + VolVolumeLen( z->fp, vol );

        if ( vol > 0 )
        {
            Byte s[8];

            if ( VolSeek( z->fp, volBase, SEEK_SET ) != 0 ||
                 VolRead( s, 1, 8, z->fp ) != 8 ||
                 memcmp( s, RAR5_SIG, 8 ) != 0 )
            { z->volumes = vol; break; }
        }

        pos = volBase + 8;

    for ( ;; )
    {
        UInt32 headSize, dataSize = 0, extraSize = 0;
        Byte  *hdr;
        BRd    b;
        UInt32 type, flags;
        long   body;
        int    brc;

        if ( pos >= volEnd ) break;          /* this volume is done */

        /* One call for both the plain and the enciphered layout, so nothing
         * below has to know which archive it is walking. */
        brc = Rar5ReadBlock( z, pos, &hdr, &headSize, &body );
        if ( brc == RAR5_BLK_END ) break;                   /* clean EOF */
        if ( brc != SZ_OK ) { rc = brc; break; }

        b.p = hdr; b.end = hdr + headSize; b.err = 0;
        type  = BrVint( &b );
        flags = BrVint( &b );
        extraSize = 0;
        if ( flags & H5F_EXTRA ) extraSize = BrVint( &b );  /* extra-area size */
        if ( flags & H5F_DATA )  dataSize = BrVint( &b );

        if ( type == H5_ENDARC )   { free( hdr ); break; }

        /* An H5_ENCRYPT block means the HEADERS themselves are encrypted
         * (WinRAR -hp): it is the last thing written in the clear, and it
         * carries the salt and iteration count for everything after it.  With
         * no password the answer is "give me one" rather than "unsupported" -
         * the difference is whether the front end prompts or gives up. */
        if ( type == H5_ENCRYPT )
        {
            UInt32 encVer = BrVint( &b );
            UInt32 encFlg = BrVint( &b );

            if ( !z->havePw ) { free( hdr ); rc = SZ_ERR_PASSWORD; break; }

            /* Version 0 is the only one defined; a later one would have a
             * layout we cannot guess at, and guessing would mean decrypting
             * the whole archive wrongly rather than saying so. */
            if ( b.err || encVer != 0 ||
                 b.p + 1 + R5_SALT_LEN > b.end )
            { free( hdr ); rc = SZ_ERR_UNSUPPORTED; break; }

            {
                int         kdfCount = (int)*b.p++;
                const Byte *salt     = b.p;

                b.p += R5_SALT_LEN;

                /* The iteration count is 1 << kdfCount.  WinRAR writes 15;
                 * anything much past 24 is not a real archive but it would be
                 * minutes of PBKDF2 before finding that out, so it is refused
                 * rather than attempted. */
                if ( kdfCount < 0 || kdfCount > 24 )
                { free( hdr ); rc = SZ_ERR_UNSUPPORTED; break; }

                Rar5DeriveKeys( z->password, salt, kdfCount,
                                z->hdrKey, z->hashKey, z->pswCheck );

                /* THE PASSWORD CHECK, and this is the good case: it settles
                 * the password before a single header is decrypted, so a typo
                 * is named at once instead of surfacing as a broken archive.
                 * RAR3's -hp has to infer it from a header CRC instead. */
                if ( ( encFlg & H5C_PSWCHECK ) && b.p + 8 <= b.end &&
                     memcmp( b.p, z->pswCheck, 8 ) != 0 )
                { free( hdr ); rc = SZ_ERR_BADPASS; break; }

                z->hdrKeyValid  = 1;
                z->hdrEncrypted = 1;
            }

            /* The crypt header has no data area of its own; the first
             * enciphered block follows it immediately. */
            free( hdr );
            pos = body;
            continue;
        }

        /* Remember whether the newest file block expects to carry on into
         * another volume; if the set ends while that is true, it is short. */
        if ( type == H5_FILE )
            lastSplitAfter = ( flags & H5F_SPLIT_AFTER ) ? 1 : 0;

        /* The main header carries the archive flags, and the only one that
         * matters here is whether this is one volume of a set.  As in RAR4,
         * the names found a candidate and THIS is what confirms it, so an
         * ordinary archive cannot absorb an unrelated neighbour that happens
         * to fit the .partN.rar pattern. */
        if ( type == H5_MAIN )
        {
            UInt32 arcFlags = BrVint( &b );

            if ( vol == 0 && !b.err && !( arcFlags & H5A_VOLUME ) )
                z->volumes = 1;
        }

        if ( type == H5_FILE )
        {
            UInt32 fileFlags = BrVint( &b );
            UInt32 unpSize    = BrVint( &b );
            UInt32 attr       = BrVint( &b );
            UInt32 mtime = 0, dcrc = 0, compInfo, nameLen;
            int    hasMtime = 0, hasCrc = 0;

            if ( fileFlags & F5_MTIME ) { mtime = BrU32( &b ); hasMtime = 1; }
            if ( fileFlags & F5_CRC )   { dcrc  = BrU32( &b ); hasCrc = 1; }
            compInfo = BrVint( &b );
            (void)BrVint( &b );                              /* host OS */
            nameLen  = BrVint( &b );

            /* A CONTINUATION rather than a new file: this volume's bytes join
             * the entry already being built.  RAR5 puts SPLIT_BEFORE in the
             * COMMON block flags, not in the file flags - the one real
             * difference from RAR4 here.
             *
             * The CRC is taken from the latest part for the same reason as in
             * RAR4: intermediate parts carry the CRC of their own piece, the
             * final part carries the CRC of the whole reassembled file, so
             * letting each overwrite the last leaves the value the joined data
             * must match. */
            if ( !b.err && ( flags & H5F_SPLIT_BEFORE ) && z->numEntries > 0 )
            {
                int last5 = z->numEntries - 1;

                if ( !Rar5AddPiece( z, last5, body, dataSize ) )
                { rc = SZ_ERR_MEMORY; free( hdr ); break; }
                z->entries[last5].packed += dataSize;
                if ( fileFlags & F5_CRC )
                { z->entries[last5].crc = dcrc; z->entries[last5].hasCrc = 1; }
                free( hdr );
                pos = body + (long)dataSize;
                continue;
            }

            if ( !b.err )
            {
                Rar5Entry *e;
                UInt32 j; int k = 0, last;
                UInt32 avail = (UInt32)( b.end - b.p );
                int    g     = Rar5Grow( z );

                if ( g <= 0 )
                {
                    free( hdr );
                    rc = ( g < 0 )
                       ? ArcCheckEntryCount( (UInt32)z->numEntries + 1 )
                       : SZ_ERR_MEMORY;
                    break;
                }
                e = &z->entries[z->numEntries];
                if ( nameLen > avail ) nameLen = avail;

                for ( j = 0; j < nameLen && k < SZ_MAX_NAME - 1; j++ )
                {
                    char c = (char)b.p[j];
                    e->name[k++] = ( c == '/' ) ? '\\' : c;
                }
                e->name[k] = '\0';
                last = k - 1;

                e->size       = unpSize;
                e->packed     = dataSize;
                e->crc        = dcrc;
                e->hasCrc     = hasCrc;
                e->methodCode = (int)( ( compInfo >> 7 ) & 7 );
                e->attrib     = attr;
                e->isDir      = ( fileFlags & F5_DIR ) ? 1 : 0;
                if ( last >= 0 && e->name[last] == '\\' )
                { e->name[last] = '\0'; e->isDir = 1; }

                e->hasMtime = hasMtime;
                e->mtimeLo = e->mtimeHi = 0;
                if ( hasMtime )
                    UnixToFileTime( mtime, &e->mtimeLo, &e->mtimeHi );

                /*---------------------------------------------------------
                 * The extra area, which is where an encrypted entry keeps
                 * its salt and IV.  It sits at the END of the header body,
                 * after the name, and is a list of [size][type][data]
                 * records - so it is reached by measuring back from the end
                 * rather than by walking forward past fields whose presence
                 * depends on flags.
                 *
                 * Skipping this used to be harmless.  It is not any more:
                 * without the crypt record an encrypted entry looks like an
                 * ordinary one, extracts into garbage, and is reported as a
                 * CRC failure - telling the user their archive is corrupt
                 * when the truth is that it is locked.
                 *--------------------------------------------------------*/
                {
                    Rar5Crypt *cy = &z->crypt[z->numEntries];

                    memset( cy, 0, sizeof( *cy ) );

                    if ( extraSize > 0 && extraSize <= headSize )
                    {
                        BRd x;
                        x.p   = hdr + headSize - extraSize;
                        x.end = hdr + headSize;
                        x.err = 0;

                        while ( x.p < x.end && !x.err )
                        {
                            UInt32      recSize = BrVint( &x );
                            const Byte *recEnd;

                            if ( x.err || recSize == 0 ) break;
                            recEnd = x.p + recSize;
                            if ( recEnd > x.end ) break;

                            if ( BrVint( &x ) == X5_CRYPT && !x.err )
                            {
                                UInt32 ver  = BrVint( &x );
                                UInt32 cfl  = BrVint( &x );

                                /* Version 0 is the only one defined.  A later
                                 * one would have a layout we cannot guess, so
                                 * leave the entry marked unencrypted and let
                                 * it fail honestly further down. */
                                if ( ver == 0 &&
                                     x.p + 1 + R5_SALT_LEN + R5_IV_LEN <= recEnd )
                                {
                                    cy->kdfCount = (int)*x.p++;
                                    memcpy( cy->salt, x.p, R5_SALT_LEN );
                                    x.p += R5_SALT_LEN;
                                    memcpy( cy->iv, x.p, R5_IV_LEN );
                                    x.p += R5_IV_LEN;

                                    cy->tweakCrc = ( cfl & X5C_TWEAKCRC ) ? 1 : 0;
                                    if ( ( cfl & X5C_PSWCHECK ) &&
                                         x.p + 8 <= recEnd )
                                    {
                                        memcpy( cy->pswCheck, x.p, 8 );
                                        cy->hasPswCheck = 1;
                                    }
                                    cy->encrypted = 1;
                                }
                            }
                            x.p = recEnd;       /* next record, whatever this was */
                        }
                    }
                }

                /* Under -hp the data area does not start at the end of the
                 * header: the IV sits in front of the block and the block is
                 * padded to a cipher boundary.  Rar5ReadBlock worked that out
                 * and it is taken from there, so the arithmetic lives in one
                 * place rather than two. */
                z->dataOffset[z->numEntries] = body;

                z->pieceCount[z->numEntries] = 0;
                z->pieceFirst[z->numEntries] = 0;
                if ( !Rar5AddPiece( z, z->numEntries, body, dataSize ) )
                { rc = SZ_ERR_MEMORY; free( hdr ); break; }

                z->numEntries++;
            }
        }

        free( hdr );
        pos = body + (long)dataSize;
    }
    }   /* per-volume */

    /* Did the last block of the last volume expect a continuation? */
    if ( rc == SZ_OK && lastSplitAfter )
        z->volIncomplete = 1;

    /* AN INCOMPLETE SET - see the longer note in RarOpenPw.  The last block of
     * the last volume we have says whether RAR meant to continue into another
     * one; if it did and there is none, the listing is short of both the rest
     * of that file and everything after it. */
    if ( rc == SZ_OK && z->volIncomplete )
        rc = SZ_ERR_VOLUME;

    /* THE EMPTY-ARCHIVE TRAP, the same one RarOpenPw guards - see the longer
     * note there.  A -hp archive always has at least a main header behind the
     * crypt block, so if not one enciphered block could be read the file is
     * cut short.  Here that is the ONLY thing it can be, because the crypt
     * header's check value has already proved the password right; reporting it
     * as a bad password would be flatly untrue. */
    if ( rc == SZ_OK && z->hdrEncrypted && !z->hdrSawBlock )
        rc = SZ_ERR_READ;

    if ( rc != SZ_OK ) { Rar5Close( z ); return rc; }
    Rar5Trim( z );
    *out = z;
    return SZ_OK;
}

int Rar5Open( const char *path, Rar5Archive **out )
{
    return Rar5OpenPw( path, NULL, out );
}

/* RAR5 is STORED-ONLY here (compressed RAR5 returns SZ_ERR_UNSUPPORTED before
 * any buffer is reached), so extraction is a copy through a small fixed
 * buffer and costs nothing worth reporting.  This exists so that the format
 * dispatch in ArcMemNeeded has an answer from every backend rather than a
 * silent zero that could equally mean "free" or "nobody asked". */
UInt32 Rar5MemNeeded( Rar5Archive *r )
{
    (void)r;
    return 512;
}

int Rar5NumEntries( Rar5Archive *r )
{
    return r ? r->numEntries : 0;
}

int Rar5VolumeCount( Rar5Archive *r )
{
    return ( r && r->volumes > 0 ) ? r->volumes : 1;
}

const Rar5Entry *Rar5GetEntry( Rar5Archive *r, int index )
{
    if ( !r || index < 0 || index >= r->numEntries ) return NULL;
    return &r->entries[index];
}

/*---- extract one stored entry -------------------------------------------- */
/*
 * Derive the keys for this entry, reusing the last set when the salt and
 * iteration count match.
 *
 * The cache matters more than it looks.  WinRAR salts every file separately,
 * so consecutive entries normally miss - but PBKDF2 at the default setting is
 * 32768 HMAC-SHA256 rounds, which on era hardware is a visible pause, and
 * doing it twice for the same entry (once to test the password, once to
 * decrypt) would double it for no reason.
 *
 * Returns SZ_ERR_PASSWORD when none has been set, SZ_ERR_BADPASS when the
 * entry carries a check value and it does not match.
 */
static int Rar5EnsureKey( Rar5Archive *z, const Rar5Crypt *cy )
{
    if ( !z->havePw )
        return SZ_ERR_PASSWORD;

    if ( !z->keyValid ||
         z->cachedCount != cy->kdfCount ||
         memcmp( z->cachedSalt, cy->salt, R5_SALT_LEN ) != 0 )
    {
        Rar5DeriveKeys( z->password, cy->salt, cy->kdfCount,
                        z->key, z->hashKey, z->pswCheck );
        memcpy( z->cachedSalt, cy->salt, R5_SALT_LEN );
        z->cachedCount = cy->kdfCount;
        z->keyValid    = 1;
    }

    /* RAR5 is the one format here that can say "wrong password" for certain
     * and before decoding anything - 7z and RAR3 have no such value and have
     * to infer it from a failed CRC. */
    if ( cy->hasPswCheck &&
         memcmp( z->pswCheck, cy->pswCheck, 8 ) != 0 )
        return SZ_ERR_BADPASS;

    return SZ_OK;
}

static int Rar5ExtractIndex( Rar5Archive *z, int idx, const char *destDir )
{
    Rar5Entry    *e  = &z->entries[idx];
    Rar5Crypt    *cy = &z->crypt[idx];
    AesCbcState   cbc;
    char          outPath[SZ_MAX_NAME * 4];
    FILE         *out;
    UInt32        crc, remain;
    unsigned int  toRead;
    unsigned char buf[512];
    Rar5DataReader dr;
    int           rc;

    /* destDir == NULL means "test only": read + CRC-check but write nothing. */
    if ( destDir )
    {
        BuildOut( outPath, sizeof( outPath ), destDir, e->name, e->isDir );
        /* Nothing to create, or skipped/cancelled at the 8.3 prompt - see
         * ArcNameVerdict in ARCDEFS.H. */
        if ( ArcNameVerdict() == ARC_NAME_ABORT ) return SZ_ERR_CANCEL;
        if ( ArcNameVerdict() == ARC_NAME_SKIP )  return SZ_OK;
    }

    if ( e->isDir )
    {
        if ( destDir && !ArcFlattenPaths() ) MakeDirs( outPath, 1 );
        return SZ_OK;
    }
    if ( destDir && !ArcWantWrite( outPath ) )
        return SZ_OK;                  /* exists and the user chose to keep it */
    if ( e->methodCode != 0 )
        return SZ_ERR_UNSUPPORTED;                 /* compressed: decoder TBD */

    /* Encrypted?  Derive (or reuse) the key and check the password before a
     * byte is read, so a wrong one costs nothing and is reported as a wrong
     * password rather than as a corrupt file. */
    if ( cy->encrypted )
    {
        int krc = Rar5EnsureKey( z, cy );
        if ( krc != SZ_OK ) return krc;
        if ( AesCbcInit( &cbc, z->key, 32, cy->iv ) != SZ_OK )
            return SZ_ERR_UNSUPPORTED;
    }

    Rar5DataInit( &dr, z, idx );

    if ( destDir )
    {
        MakeDirs( outPath, 0 );
        out = fopen( outPath, "wb" );
        if ( !out ) return SZ_ERR_WRITE;
    }
    else
        out = NULL;

    rc     = SZ_OK;
    crc    = Crc32Init();

    /* For a stored entry the packed size IS the size - except when encrypted,
     * where CBC has padded the stream up to a 16-byte boundary, so the data on
     * disk is longer than the file.  Read the padded length and write only the
     * real one. */
    remain = e->packed;
    {
        UInt32 written = 0;

        while ( remain > 0 )
        {
            toRead = ( remain > 512UL ) ? 512U : (unsigned int)remain;

            /* Whole blocks only while decrypting: a 512-byte buffer is already
             * a multiple of 16, so only the final chunk can be ragged, and a
             * ragged final chunk means the stream was truncated. */
            if ( cy->encrypted && ( toRead & 15 ) )
            { rc = SZ_ERR_DATA; break; }

            if ( !Rar5DataRead( &dr, buf, toRead ) )
            { rc = SZ_ERR_READ; break; }

            if ( cy->encrypted )
                AesCbcDecrypt( &cbc, buf, toRead );

            /* Trim the CBC padding off the tail: it is not part of the file
             * and must reach neither the CRC nor the disk. */
            {
                unsigned int use = toRead;

                if ( written + use > e->size )
                    use = (unsigned int)( e->size - written );

                if ( use )
                {
                    crc = Crc32Update( crc, buf, use );
                    if ( out && fwrite( buf, 1, use, out ) != use )
                    { rc = SZ_ERR_WRITE; break; }
                    written += use;
                }
            }
            remain -= toRead;
        }
    }
    if ( out ) fclose( out );

    if ( rc == SZ_OK && e->hasCrc )
    {
        UInt32 got = Crc32Done( crc );

        /* An encrypted entry may store an HMAC of the CRC rather than the CRC
         * itself, so that the checksum cannot be used to test passwords
         * offline.  Transform ours the same way before comparing. */
        if ( cy->encrypted && cy->tweakCrc )
            got = Rar5TweakCrc( got, z->hashKey );

        if ( got != e->crc )
        {
            /* With a password check value a wrong password was already caught
             * above, so a mismatch here really is damage.  Without one, the
             * likelier explanation is the password - and sending the user to
             * retype it costs a moment, where crying corruption costs them the
             * archive. */
            rc = ( cy->encrypted && !cy->hasPswCheck )
               ? SZ_ERR_BADPASS : SZ_ERR_CRC;
        }
    }
    if ( rc == SZ_OK )
    {
        if ( destDir && e->hasMtime )
        {
            FILETIME ft;
            ft.dwLowDateTime = e->mtimeLo; ft.dwHighDateTime = e->mtimeHi;
            SetFileMTime( outPath, &ft );
        }
    }
    else if ( destDir )
        remove( outPath );
    return rc;
}

int Rar5ExtractAll( Rar5Archive *r, const char *destDir,
                    SzProgress prog, void *user )
{
    int i, rc;
    if ( !r ) return SZ_ERR_FORMAT;
    for ( i = 0; i < r->numEntries; i++ )
    {
        if ( !r->entries[i].name[0] ) continue;
        if ( prog && !prog( user, i, r->numEntries, r->entries[i].name ) )
            return SZ_ERR_CANCEL;
        rc = Rar5ExtractIndex( r, i, destDir );
        if ( rc ) return rc;
    }
    return SZ_OK;
}

int Rar5ExtractItems( Rar5Archive *r, const int *indices, int count,
                      const char *destDir, SzProgress prog, void *user )
{
    int k, idx, rc;
    if ( !r ) return SZ_ERR_FORMAT;
    for ( k = 0; k < count; k++ )
    {
        idx = indices[k];
        if ( idx < 0 || idx >= r->numEntries ) continue;
        if ( !r->entries[idx].name[0] ) continue;
        if ( prog && !prog( user, idx, r->numEntries, r->entries[idx].name ) )
            return SZ_ERR_CANCEL;
        rc = Rar5ExtractIndex( r, idx, destDir );
        if ( rc ) return rc;
    }
    return SZ_OK;
}

void Rar5Close( Rar5Archive *r )
{
    if ( r )
    {
        if ( r->fp )         VolClose( r->fp );
        if ( r->entries )    free( r->entries );
        if ( r->dataOffset ) free( r->dataOffset );
        if ( r->pieces )     free( r->pieces );
        if ( r->pieceFirst ) free( r->pieceFirst );
        if ( r->pieceCount ) free( r->pieceCount );
        if ( r->crypt )      free( r->crypt );
        free( r );
    }
}

/*---- Password ------------------------------------------------------------ */

void Rar5SetPassword( Rar5Archive *r, const char *pw )
{
    if ( !r ) return;

    /* Any change invalidates the derived key, including clearing it - keeping
     * a key derived from a password that is no longer set would decrypt with
     * something the user has retracted. */
    r->keyValid = 0;

    if ( !pw || !pw[0] )
    {
        r->password[0] = '\0';
        r->havePw      = 0;
        return;
    }
    strncpy( r->password, pw, AC_MAX_PW );
    r->password[ AC_MAX_PW ] = '\0';
    r->havePw = 1;
}

int Rar5EntryEncrypted( Rar5Archive *r, int index )
{
    if ( !r || index < 0 || index >= r->numEntries ) return 0;
    return r->crypt[index].encrypted;
}

int Rar5NeedsPassword( Rar5Archive *r )
{
    int i;

    if ( !r ) return 0;
    for ( i = 0; i < r->numEntries; i++ )
        if ( r->crypt[i].encrypted )
            return 1;
    return 0;
}
