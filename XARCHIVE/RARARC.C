/*===========================================================================
 * RARARC.C  -  RAR (2.x/3.x, "RAR4") parsing and extraction
 * Target: MSVC 2.2  Win32s
 *
 * Container parsing, listing and extraction: STORED (method 0x30) plus the
 * RAR2/RAR3 unpackers, solid chains included.  RAR5 has its own container
 * (RAR5ARC.C) and is detected here only to hand over.
 *
 * ENCRYPTION: AES-128-CBC over the packed stream, keyed by the RAR 2.9
 * derivation in ARCCRYP.C.  There is no password check value for entry data,
 * so a wrong password surfaces as a failed CRC and is reported as
 * SZ_ERR_BADPASS rather than as a corrupt archive.
 *
 * ENCRYPTED HEADERS (-hp) are decrypted too: past the main header the whole
 * block chain is enciphered, so the archive cannot be listed at all without
 * the password and RarOpenPw has to be given it up front.  Unlike entry data,
 * an encrypted header carries its own CRC, so there a wrong password IS
 * caught immediately - see RarReadBlock.
 *
 * MULTI-VOLUME (.partN.rar, or the older .rar/.r00/.r01) is handled here and
 * not by VOLIO: every RAR volume is a complete archive with its own marker and
 * main header, so the volumes are walked one at a time and a file that crosses
 * a join is stitched back from the SPLIT_BEFORE/SPLIT_AFTER flags.  An
 * incomplete set is refused with SZ_ERR_VOLUME rather than listed short.
 *===========================================================================*/

#include <windows.h>     /* lstrcpyn */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>      /* _mkdir */

#include "rararc.h"
#include "rar3dec.h"
#include "crc32.h"
#include "volio.h"      /* volume naming + the one-handle volume set */
#include "arccryp.h"    /* Rar3DeriveKeys, AesCbc* */
#include "platform.h"   /* SetFileDosMTime */

/*---- RAR4 block constants ------------------------------------------------ */
#define RAR_MARK      0x72   /* marker block type             */
#define RAR_MAIN      0x73   /* archive (main) header         */
#define RAR_FILE      0x74   /* file header                   */
#define RAR_COMMENT   0x75   /* old-style archive comment      */
#define RAR_NEWSUB    0x7A   /* new-style sub-block (CMT/ACL/STM/AV) */
#define RAR_ENDARC    0x7B   /* end-of-archive header         */

/* A sub-block says which KIND it is in its name field, not in its type: the
 * comment is the one called "CMT".  Three bytes, and NOT terminated. */
#define RAR_SUB_CMT      "CMT"
#define RAR_SUB_CMT_LEN  3

/* Bit 0 of a CMT sub-block's flags means the text is in RAR's own compressed
 * Unicode encoding rather than plain bytes.  It reuses the bit that means
 * SPLIT_BEFORE on a real file header - sub-blocks borrow the file-header
 * layout and give the flags different meanings. */
#define SUBHEAD_CMT_UNICODE 0x0001

/* Refuse to allocate for a comment past this.  RAR4 keeps comments well
 * under it; a header claiming more is either damaged or hostile, and either
 * way "no comment" beats a multi-megabyte allocation made on the say-so of
 * four bytes in a file. */
#define RAR_MAX_COMMENT  (256UL * 1024)

/* main-header flags */
#define MHD_SOLID       0x0008   /* solid archive               */
#define MHD_PASSWORD    0x0080   /* block headers are encrypted (-hp)   */
/* NB: RAR calls this bit MHD_PASSWORD.  MHD_ENCRYPTVER is a different bit
 * (0x0200) meaning something else; the two names are easy to swap. */

/* file-header flags */
#define LHD_SPLIT_BEFORE 0x0001  /* continues the previous volume        */
#define LHD_SPLIT_AFTER 0x0002
#define LHD_PASSWORD    0x0004   /* entry is encrypted          */
#define LHD_SOLID       0x0010   /* uses the previous file's window */
#define LHD_LARGE       0x0100   /* 64-bit size fields present  */
#define LHD_UNICODE     0x0200   /* name carries a unicode part */
#define LHD_SALT        0x0400   /* an 8-byte salt follows the name */
#define LHD_WINDOWMASK  0x00E0   /* dictionary size, as 64 KB << n       */
#define LHD_DIRECTORY   0x00E0   /* ...and all of them set means "directory",
                                  * which is why the two names share a value */
/* base-header flags */
#define LONG_BLOCK      0x8000   /* an ADD_SIZE/data area follows */

/* main-header flags, continued */
#define MHD_VOLUME      0x0001   /* this file is one volume of a set    */
#define MHD_FIRSTVOL    0x0100   /* ...and it is the first one          */

#define RAR_MAX_VOLUMES 400      /* .r00-.z99 is 2600, but a set that big
                                  * is past anything this runs on        */
#define VOL_PATH_MAX    260

#define RAR_METHOD_STORE 0x30
#define RAR_SALT_LEN     8

static const unsigned char RAR4_SIG[7] =
    { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00 };

/*---- little-endian field readers ----------------------------------------- */
static UInt32 Rd16( const Byte *p )
{
    return (UInt32)p[0] | ( (UInt32)p[1] << 8 );
}
static UInt32 Rd32( const Byte *p )
{
    return (UInt32)p[0] | ( (UInt32)p[1] << 8 ) |
           ( (UInt32)p[2] << 16 ) | ( (UInt32)p[3] << 24 );
}

/*---- archive object ------------------------------------------------------ */
/* RAR has no up-front file count - the entries are discovered while walking
 * the block headers - so the parallel tables start small and double.  Fixed
 * SZ_MAX_FILES tables here would cost ~5 MB for every archive opened. */
/* One run of an entry's data inside one volume.  An ordinary entry has a
 * single piece; an entry that spans volumes has one per volume it crosses,
 * because the pieces are NOT adjacent - each volume's marker and headers sit
 * between them.  That is the whole difference between RAR volumes and the
 * byte-split sets VOLIO joins on its own. */
typedef struct {
    long   off;                          /* offset in the joined stream      */
    UInt32 len;
} RarPiece;

struct RarArchive {
    VolFile  *fp;
    int       numEntries;
    int       cap;                       /* entries the tables can hold      */
    int       solid;                     /* archive uses solid compression   */
    RarEntry *entries;
    long     *dataOffset;                /* file offset of the data area     */
    UInt16   *headFlags;                 /* file-header flags                */

    /* Multi-volume.  pieces is a flat pool; pieceFirst/pieceCount index into
     * it per entry.  volumes is 1 for an ordinary archive, and everything
     * below behaves exactly as it did before when it is. */
    int       volumes;
    int       volIncomplete;             /* set ran out while a file was open */
    RarPiece *pieces;
    int       pieceN, pieceCap;
    int      *pieceFirst;
    int      *pieceCount;
    Byte     *salts;                     /* RAR_SALT_LEN bytes per entry     */
    char     *comment;                   /* archive comment, NULL if none    */

    /* Encryption.  One password covers the archive, but the salt is per
     * entry, so the derived key is cached against the salt it came from:
     * RAR3 key derivation is 262144 rounds of SHA-1, which is a visible pause
     * on era hardware and must not be paid twice for the same entry. */
    char      password[ AC_MAX_PW + 1 ];
    int       havePw;
    Byte      key[16], iv[16];
    Byte      cachedSalt[ RAR_SALT_LEN ];
    int       keyValid;

    /* Encrypted HEADERS (-hp).  Kept separate from the key above even though
     * WinRAR happens to derive both from the same salt: this one comes from
     * the salt written in FRONT of each block, which belongs to the archive,
     * and that one comes from the salt INSIDE a file header, which belongs to
     * the entry.  Nothing in the format makes them equal. */
    int       hdrEncrypted;              /* the block chain is enciphered    */
    int       hdrKeyValid;
    int       hdrPwOk;                   /* some header CRC has validated    */
    int       hdrTrunc;                  /* a block was cut short by EOF     */
    Byte      hdrKey[16], hdrIv[16];
    Byte      hdrSalt[ RAR_SALT_LEN ];
};


/*---- How big a window this archive actually needs ------------------------- *
 * RAR records the dictionary it compressed with in three flag bits of every
 * file header, as 64 KB << n.  The decoders were allocating RAR's maximum
 * (4 MB) every time regardless, which is why a RAR - any RAR - could not be
 * opened on a machine where a 7z with a 64 KB dictionary opened fine.  See
 * the banner over Rar2DecodeSized in RAR3DEC.H.
 *
 * n == 7 is not a dictionary at all: all three bits set is how RAR marks a
 * DIRECTORY entry, which has no data and therefore no window.  Answering 0
 * there means "nothing to say", and RarWinRound turns that into the maximum -
 * the right answer for a caller that has been told nothing.
 *-------------------------------------------------------------------------- */
/*---- THE ARCHIVE COMMENT, new style --------------------------------------- *
 * The old RAR_COMMENT block (0x75) carried the text inside the header itself
 * and only ever stored it.  WinRAR stopped writing that a long time ago:
 * WinRAR 5 producing a RAR4 archive puts the comment in a NEWSUB block (0x7A)
 * whose name is "CMT", and PACKS it with the ordinary RAR unpacker.
 *
 * So two things were missing, not one - the block type, AND pointing the
 * unpacker at a header rather than at file data - and until both were here,
 * a comment that RAR itself prints did not reach any of the three front ends.
 * The archive simply reported having none, which after the comment dialogs
 * went in meant the menu item greyed out on archives that plainly had one.
 *
 * A NEWSUB header has exactly the FILE header layout, so the field offsets
 * below are the ones the RAR_FILE branch uses; only the meaning of the name
 * and of some flag bits differs.
 *
 * THE WINDOW IS FREE HERE, and provably so.  UnRAR fixes subdata at a 1 MB
 * window; we can do better without guessing, because an LZ match can never
 * reach further back than the data already produced - so a window of the
 * COMMENT's own unpacked size is always enough.  For a 300-byte comment that
 * is the 64 KB minimum instead of a megabyte, which matters on exactly the
 * machines this program exists for.
 *-------------------------------------------------------------------------- */
static void RarReadSubComment( RarArchive *z, const Byte *hdr, UInt16 headSize,
                               UInt16 flags, long body )
{
    UInt32 packSize, unpSize, crc;
    UInt16 nameSize;
    int    nameOff, unpVer, method, rc;
    Byte  *packBuf, *outBuf;

    if ( z->comment ) return;                       /* first one wins */
    if ( headSize < 32 ) return;

    packSize = Rd32( hdr + 7 );
    unpSize  = Rd32( hdr + 11 );
    crc      = Rd32( hdr + 16 );
    unpVer   = hdr[24];
    method   = hdr[25];
    nameSize = (UInt16)Rd16( hdr + 26 );
    nameOff  = ( flags & LHD_LARGE ) ? 40 : 32;

    if ( nameSize != RAR_SUB_CMT_LEN ) return;
    if ( nameOff + RAR_SUB_CMT_LEN > (int)headSize ) return;
    if ( memcmp( hdr + nameOff, RAR_SUB_CMT, RAR_SUB_CMT_LEN ) != 0 )
        return;                                     /* ACL, STM, AV - not ours */

    if ( unpSize == 0 || unpSize > RAR_MAX_COMMENT ) return;
    if ( packSize == 0 || packSize > RAR_MAX_COMMENT ) return;

    /* AN ENCRYPTED ARCHIVE NEEDS NOTHING EXTRA HERE, which is not what I
     * expected and is worth writing down.  Under -hp the whole block chain is
     * enciphered, so the header above arrived through RarReadBlock already
     * decrypted - but the sub-block's DATA is not encrypted at all.  The
     * header says so itself (a CMT sub-block carries neither LHD_PASSWORD nor
     * LHD_SALT even in an -hp archive), and it was then confirmed against
     * WinRAR on four fixtures: -hp with two different passwords, -hp solid,
     * and -p data-only encryption.  All four come out byte-identical to what
     * "rar cw" writes.
     *
     * So the only bail-out left is a sub-block that declares its own
     * encryption, which there is no key to hand for.  And if some other
     * writer enciphers one anyway, the CRC below catches it and the archive
     * reports no comment - the safe direction. */
    if ( flags & LHD_PASSWORD ) return;

    /* RAR's compressed-Unicode comment encoding is a different job from
     * unpacking, and this build has no use for it: the front ends draw in one
     * byte per character.  Showing the raw bytes would be mojibake presented
     * as text, so the archive reports no comment instead. */
    if ( flags & SUBHEAD_CMT_UNICODE ) return;

    packBuf = (Byte *)malloc( (size_t)packSize );
    if ( !packBuf ) return;
    if ( VolSeek( z->fp, body, SEEK_SET ) != 0 ||
         VolRead( packBuf, 1, (size_t)packSize, z->fp ) != (size_t)packSize )
    { free( packBuf ); return; }

    outBuf = (Byte *)malloc( (size_t)unpSize + 1 );
    if ( !outBuf ) { free( packBuf ); return; }

    if ( method == RAR_METHOD_STORE )
    {
        UInt32 n = ( packSize < unpSize ) ? packSize : unpSize;
        memcpy( outBuf, packBuf, (size_t)n );
        rc = ( n == unpSize ) ? SZ_OK : SZ_ERR_DATA;
    }
    else if ( unpVer >= 29 )
        rc = Rar3DecodeSized( packBuf, packSize, outBuf, unpSize,
                              RarWindowFor( unpSize ) );
    else if ( unpVer >= 20 )
        rc = Rar2DecodeSized( packBuf, packSize, outBuf, unpSize,
                              RarWindowFor( unpSize ) );
    else
        rc = SZ_ERR_UNSUPPORTED;                    /* RAR 1.x */

    free( packBuf );

    /* THE CRC IS NOT OPTIONAL HERE.  Everywhere else in this file a bad CRC
     * is reported to the user; a comment has nobody to report to, so the only
     * way it can fail safely is to fail silently.  Without the check, a
     * comment decoded with the wrong tables would be DISPLAYED - and as the
     * solid-chain bug showed, wrongly decoded output is perfectly readable
     * bytes and announces nothing. */
    if ( rc == SZ_OK && Crc32Calc( outBuf, unpSize ) != crc ) rc = SZ_ERR_CRC;

    if ( rc != SZ_OK ) { free( outBuf ); return; }

    outBuf[unpSize] = '\0';
    z->comment = (char *)outBuf;
}

static UInt32 RarWinSize( UInt16 flags )

{
    unsigned n = ( flags & LHD_WINDOWMASK ) >> 5;

    if ( n == 7 ) return 0;                        /* directory, not a size */
    return 0x10000UL << n;
}

/* The window one SOLID chain needs: the largest any member asks for.  A chain
 * shares one window from end to end, so it cannot be resized part way and the
 * smallest safe figure is the biggest of them.  In practice every member of a
 * chain carries the same bits - the dictionary is an archive-wide setting -
 * but nothing in the format promises it, and a window that is too small for
 * one member in the middle would fail the whole extraction. */
static UInt32 RarMaxWinSize( RarArchive *z )
{
    UInt32 best = 0;
    int    i;

    for ( i = 0; i < z->numEntries; i++ )
    {
        UInt32 w = RarWinSize( z->headFlags[i] );
        if ( w > best ) best = w;
    }
    return best;
}

/*---- The largest single extraction this archive will ask for -------------- *
 * Two quite different shapes, and RAR picks between them by whether the
 * archive is solid:
 *
 *   SOLID    one window for the whole chain, and one member's PACKED bytes in
 *            a buffer at a time.  Nothing grows with the archive.
 *   NON-SOLID a window for the entry, plus its packed bytes AND its unpacked
 *            bytes, all three live at once - RarExtractOne decodes to a
 *            buffer rather than streaming.  So here a single large member is
 *            what decides it, and a 40 MB file inside a RAR costs more than
 *            any dictionary ever could.
 *
 * Members are extracted one at a time either way, so the answer is the worst
 * single member and not a sum.
 *
 * Measured against the tracking allocator after the window was sized from the
 * header: a 64 KB-dictionary solid RAR now peaks at 220 KB with a 147 KB
 * largest block, where before the change every RAR of every size peaked at
 * 4.2 MB.  Callers add the slack.
 *-------------------------------------------------------------------------- */
/* Saturating add.  The three figures below are each a UInt32 read straight
 * out of a header, so a damaged or hostile archive can make them sum past
 * 4 GB - and a silent wrap here would produce a SMALL number, which is the
 * one direction this estimate must never fail in: it would wave the archive
 * past the check and back into the bare malloc failure the check exists to
 * replace.  Saturating reports "more than this machine will ever have",
 * which is the truthful answer for a header claiming that much. */
static UInt32 RarMemAdd( UInt32 a, UInt32 b )
{
    if ( a > 0xFFFFFFFFUL - b ) return 0xFFFFFFFFUL;
    return a + b;
}

UInt32 RarMemNeeded( RarArchive *z )
{
    UInt32 best = 0;
    int    i;

    if ( !z ) return 0;

    if ( z->solid )
    {
        UInt32 win = RarWindowFor( RarMaxWinSize( z ) );

        for ( i = 0; i < z->numEntries; i++ )
        {
            UInt32 need;
            if ( z->entries[i].isDir ) continue;
            need = RarMemAdd( win, z->entries[i].packed );
            if ( need > best ) best = need;
        }
        if ( best < win ) best = win;        /* a chain of directories */
        return best;
    }

    for ( i = 0; i < z->numEntries; i++ )
    {
        RarEntry *e = &z->entries[i];
        UInt32    need;

        if ( e->isDir ) continue;
        if ( e->methodCode == RAR_METHOD_STORE )
            need = 512;                      /* streams through a small buffer */
        else
            need = RarMemAdd( RarMemAdd( RarWindowFor( RarWinSize( z->headFlags[i] ) ),
                                         e->packed ), e->size );
        if ( need > best ) best = need;
    }
    return best;
}

const char *RarComment( RarArchive *z )

{
    return ( z && z->comment && z->comment[0] ) ? z->comment : NULL;
}

/* Record one run of data for the entry being built.  Returns 0 out of memory.
 * Entries are finished one at a time, so a piece always belongs to the last
 * entry and the pool only ever grows at the end. */
static int RarAddPiece( RarArchive *z, int entry, long off, UInt32 len )
{
    if ( z->pieceN >= z->pieceCap )
    {
        int       nc = z->pieceCap ? z->pieceCap * 2 : 32;
        RarPiece *np = (RarPiece *)realloc( z->pieces,
                                            (size_t)nc * sizeof( RarPiece ) );
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

/*---- reading an entry's data, which may be in pieces --------------------- *
 * An entry that does not cross a volume join has exactly one piece and this
 * behaves as the plain seek-and-read it replaces.  One that does has the
 * volume's marker and headers sitting between its pieces, so the reader has
 * to step over them - which is why extraction cannot simply read `packed`
 * bytes from the start of the data any more.
 *--------------------------------------------------------------------------*/
typedef struct {
    RarArchive *z;
    int         first, count;      /* the entry's slice of the piece pool */
    int         pi;                /* piece being read, relative to first */
    UInt32      used;              /* bytes taken from that piece         */
    int         started;
} RarDataReader;

static void RarDataInit( RarDataReader *r, RarArchive *z, int idx )
{
    r->z       = z;
    r->first   = z->pieceFirst[idx];
    r->count   = z->pieceCount[idx];
    r->pi      = 0;
    r->used    = 0;
    r->started = 0;
}

/* Fill buf with exactly len bytes, crossing pieces as needed.  1 on success. */
static int RarDataRead( RarDataReader *r, void *buf, UInt32 len )
{
    Byte  *p    = (Byte *)buf;
    UInt32 done = 0;

    while ( done < len )
    {
        RarPiece *pc;
        UInt32    avail, take;

        if ( r->pi >= r->count ) return 0;      /* ran out of data */
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

/* Make room for one more entry.  Returns 1 on success, 0 when out of memory,
 * and -1 when the entry limit itself is in the way - the caller has to tell
 * those two apart, because one is a broken machine and the other is an
 * archive this machine cannot hold the listing for. */
static int RarGrow( RarArchive *z )
{
    int       n;
    RarEntry *ne;
    long     *nd;
    UInt16   *nf;
    Byte     *ns;

    if ( z->numEntries < z->cap ) return 1;
    if ( (UInt32)z->numEntries >= SZ_MAX_FILES ) return -1;

    n = z->cap ? z->cap * 2 : 32;
    if ( (UInt32)n > SZ_MAX_FILES ) n = (int)SZ_MAX_FILES;
    if ( n <= z->numEntries ) return 0;

    ne = (RarEntry *)realloc( z->entries, (size_t)n * sizeof( RarEntry ) );
    if ( !ne ) return 0;
    z->entries = ne;

    nd = (long *)realloc( z->dataOffset, (size_t)n * sizeof( long ) );
    if ( !nd ) return 0;
    z->dataOffset = nd;

    nf = (UInt16 *)realloc( z->headFlags, (size_t)n * sizeof( UInt16 ) );
    if ( !nf ) return 0;
    z->headFlags = nf;

    ns = (Byte *)realloc( z->salts, (size_t)n * RAR_SALT_LEN );
    if ( !ns ) return 0;
    z->salts = ns;

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

/* Hand back the slack the doubling left over once the scan is done.  A shrink
 * that fails is harmless - the tables just stay as they are. */
static void RarTrim( RarArchive *z )
{
    int       n = z->numEntries ? z->numEntries : 1;
    RarEntry *ne;
    long     *nd;
    UInt16   *nf;
    Byte     *ns;

    if ( n >= z->cap ) return;

    ne = (RarEntry *)realloc( z->entries, (size_t)n * sizeof( RarEntry ) );
    if ( ne ) z->entries = ne;
    nd = (long *)realloc( z->dataOffset, (size_t)n * sizeof( long ) );
    if ( nd ) z->dataOffset = nd;
    nf = (UInt16 *)realloc( z->headFlags, (size_t)n * sizeof( UInt16 ) );
    if ( nf ) z->headFlags = nf;
    ns = (Byte *)realloc( z->salts, (size_t)n * RAR_SALT_LEN );
    if ( ns ) z->salts = ns;
    if ( ne && nd && nf && ns ) z->cap = n;
}

/*---- path helpers (shared style with ZIPARC) ----------------------------- */
static void MakeDirs( const char *path, int includeLast )
{
    char  buf[SZ_MAX_NAME * 2];
    char *p;

    lstrcpyn( buf, path, sizeof( buf ) );
    p = buf;
    if ( p[0] && p[1] == ':' ) p += 2;
    if ( *p == '\\' ) p++;

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

/*---- reading one block header -------------------------------------------- *
 * With -hp every block after the main header is enciphered, so nothing - not
 * even a block's type or its length - can be read without the password.  That
 * is the whole difference between -p and -hp: -p leaves a readable directory
 * and hides only the data.
 *
 * An encrypted block is laid out
 *      [8-byte salt][headSize bytes, AES-128-CBC, padded up to a multiple of 16]
 * with a fresh CBC chain per block.  Two consequences are easy to get wrong:
 * the salt sits OUTSIDE the header, and the header is padded, so the data area
 * does NOT begin at pos + headSize.  WinRAR writes the same salt in front of
 * every block, which is why the key is cached rather than re-derived 262144
 * rounds at a time for each one.
 *
 * On success *hdrOut is a malloc'd buffer holding headSize bytes in the clear
 * and *bodyOut is the offset of the data area.  RAR_BLK_END means a clean end
 * of file, which is how a well-formed archive normally finishes and is not an
 * error.
 */
#define RAR_BLK_END  (-1)

static int RarReadBlock( RarArchive *z, long pos, Byte **hdrOut,
                         UInt16 *headSizeOut, long *bodyOut )
{
    Byte        *hdr;
    UInt16       headSize;
    AesCbcState  cbc;

    *hdrOut = NULL;
    if ( VolSeek( z->fp, pos, SEEK_SET ) != 0 ) return RAR_BLK_END;

    if ( !z->hdrEncrypted )
    {
        Byte base[7];

        if ( VolRead( base, 1, 7, z->fp ) != 7 ) return RAR_BLK_END;
        headSize = (UInt16)Rd16( base + 5 );
        if ( headSize < 7 ) return SZ_ERR_FORMAT;

        hdr = (Byte *)malloc( headSize );
        if ( !hdr ) return SZ_ERR_MEMORY;
        memcpy( hdr, base, 7 );
        if ( headSize > 7 &&
             VolRead( hdr + 7, 1, (size_t)( headSize - 7 ), z->fp )
                 != (size_t)( headSize - 7 ) )
        { free( hdr ); return SZ_ERR_READ; }

        *hdrOut      = hdr;
        *headSizeOut = headSize;
        *bodyOut     = pos + headSize;
        return SZ_OK;
    }
    else
    {
        Byte salt[ RAR_SALT_LEN ];
        Byte first[16];
        long padded;

        /* Too little left for a salt, or for the block that must follow it,
         * means the file is CUT SHORT - and that is true whatever the key is,
         * so it must not later be blamed on the password.  See the verdict at
         * the end of RarOpenPw. */
        if ( VolRead( salt, 1, RAR_SALT_LEN, z->fp ) != RAR_SALT_LEN )
        { z->hdrTrunc = 1; return RAR_BLK_END; }
        if ( !z->hdrKeyValid || memcmp( z->hdrSalt, salt, RAR_SALT_LEN ) != 0 )
        {
            Rar3DeriveKeys( z->password, salt, z->hdrKey, z->hdrIv );
            memcpy( z->hdrSalt, salt, RAR_SALT_LEN );
            z->hdrKeyValid = 1;
        }

        /* headSize is at offset 5, inside the first cipher block, so a block
         * has to be decrypted before its own length is known. */
        if ( VolRead( first, 1, 16, z->fp ) != 16 )
        { z->hdrTrunc = 1; return RAR_BLK_END; }
        AesCbcInit( &cbc, z->hdrKey, 16, z->hdrIv );
        AesCbcDecrypt( &cbc, first, 16 );
        headSize = (UInt16)Rd16( first + 5 );
        if ( headSize < 7 ) return SZ_ERR_BADPASS;

        padded = ( (long)headSize + 15 ) & ~15L;
        hdr = (Byte *)malloc( (size_t)padded );
        if ( !hdr ) return SZ_ERR_MEMORY;
        memcpy( hdr, first, 16 );
        if ( padded > 16 )
        {
            if ( VolRead( hdr + 16, 1, (size_t)( padded - 16 ), z->fp )
                     != (size_t)( padded - 16 ) )
            { free( hdr ); return SZ_ERR_BADPASS; }
            AesCbcDecrypt( &cbc, hdr + 16, (UInt32)( padded - 16 ) );
        }

        /* THE PASSWORD CHECK, and the only one RAR3 has.  An encrypted ENTRY
         * carries no verifier, so a typo there cannot be named until a CRC
         * fails much later; an encrypted HEADER carries its own CRC, so here
         * a wrong password is caught on the first block. */
        if ( (UInt16)( Crc32Calc( hdr + 2, (UInt32)headSize - 2 ) & 0xFFFF )
                 != (UInt16)Rd16( hdr ) )
        { free( hdr ); return SZ_ERR_BADPASS; }

        z->hdrPwOk   = 1;
        *hdrOut      = hdr;
        *headSizeOut = headSize;
        *bodyOut     = pos + RAR_SALT_LEN + padded;
        return SZ_OK;
    }
}

/*---- open + walk the block headers --------------------------------------- */
int RarOpenPw( const char *path, const char *pw, RarArchive **out )
{
    RarArchive *z;
    Byte        marker[7];
    long        pos;
    int         vol;
    int         rc = SZ_OK;

    *out = NULL;

    z = (RarArchive *)calloc( 1, sizeof( RarArchive ) );
    if ( !z ) return SZ_ERR_MEMORY;

    /* Gather the volume set.  Start from the FIRST volume whatever the user
     * named - opening part3 of six means the archive, not the fragment - and
     * take names while the files are there.  A single-volume archive comes out
     * of this as a set of one and behaves exactly as it always did.
     *
     * Note this is name-led, and it has to be: the only way to learn there is
     * a next volume is the current one saying so, and the name is how you find
     * it.  The block walk below cross-checks by asking whether the last file
     * header expected a continuation. */
    {
        char  first[VOL_PATH_MAX];
        char  cur[VOL_PATH_MAX];
        char  next[VOL_PATH_MAX];
        char *names[RAR_MAX_VOLUMES];
        int   n = 0, i;

        if ( !VolRarFirstName( path, first, sizeof first ) )
        { RarClose( z ); return SZ_ERR_OPEN; }

        /* If the first volume is not there, fall back to what we were given:
         * a lone part3.rar is still worth opening and reporting on. */
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
                               RarClose( z ); return SZ_ERR_MEMORY; }
            strcpy( names[n], cur );
            n++;
            if ( n >= RAR_MAX_VOLUMES ) break;
            if ( !VolRarNextName( cur, next, sizeof next ) ) break;
            lstrcpyn( cur, next, sizeof cur );
        }

        if ( n == 0 ) { RarClose( z ); return SZ_ERR_OPEN; }

        rc = VolOpenList( (const char *const *)names, n, &z->fp );
        for ( i = 0; i < n; i++ ) free( names[i] );
        if ( rc != SZ_OK ) { RarClose( z ); return rc; }
        z->volumes = n;
        rc = SZ_OK;
    }

    /* The password is taken BEFORE the walk, not set on the open archive
     * afterwards, because with -hp the block chain itself is enciphered and
     * there is no directory to read without it.  Same reason SzOpenPw takes
     * one. */
    if ( pw && pw[0] )
    {
        lstrcpyn( z->password, pw, sizeof( z->password ) );
        z->havePw = 1;
    }

    if ( VolRead( marker, 1, 7, z->fp ) != 7 )
    { RarClose( z ); return SZ_ERR_SIG; }

    /* "Rar!\x1A\x07" then 0x00 = RAR4, 0x01 = RAR5 (not yet supported). */
    if ( memcmp( marker, RAR4_SIG, 6 ) != 0 )
    { RarClose( z ); return SZ_ERR_SIG; }
    if ( marker[6] != 0x00 )
    { RarClose( z ); return SZ_ERR_UNSUPPORTED; }  /* RAR5 */

    z->numEntries = 0;

    /* EVERY RAR VOLUME IS A WHOLE LITTLE ARCHIVE - its own marker, its own
     * main header, its own end-of-archive block - so the walk runs once per
     * volume rather than straight through.  A file that crosses a join is
     * written as a file header in EACH volume it touches, carrying the same
     * name and the same unpacked size but only the packed bytes that fit;
     * SPLIT_BEFORE on the later ones is what says "this continues the last
     * one" rather than "here is another file of the same name".  Merging
     * those back into one entry is the whole job here. */
    for ( vol = 0; vol < z->volumes && rc == SZ_OK; vol++ )
    {
        long volBase = VolVolumeStart( z->fp, vol );
        long volEnd  = volBase + VolVolumeLen( z->fp, vol );

        if ( vol > 0 )
        {
            Byte m[7];

            /* Later volumes repeat the marker.  If one does not, the name
             * matched but the file is not part of this set - stop rather
             * than parse a stranger. */
            if ( VolSeek( z->fp, volBase, SEEK_SET ) != 0 ||
                 VolRead( m, 1, 7, z->fp ) != 7 ||
                 memcmp( m, RAR4_SIG, 6 ) != 0 || m[6] != 0x00 )
            { z->volumes = vol; break; }
        }

        pos = volBase + 7;

    for ( ;; )
    {
        Byte  *hdr;
        Byte   type;
        UInt16 flags, headSize;
        long   body;
        int    brc;

        if ( pos >= volEnd ) break;          /* this volume is done */

        /* One call covers both the plain and the enciphered layouts, so the
         * walk below does not repeat itself once per format. */
        brc = RarReadBlock( z, pos, &hdr, &headSize, &body );
        if ( brc == RAR_BLK_END ) break;                  /* clean EOF */
        if ( brc != SZ_OK ) { rc = brc; break; }

        type  = hdr[2];
        flags = (UInt16)Rd16( hdr + 3 );

        if ( type == RAR_ENDARC ) { free( hdr ); break; }

        /* THE ARCHIVE COMMENT, when it is stored rather than packed.  The
         * block is UnpSize(2) UnpVer(1) Method(1) CommCRC(2) and then the text,
         * all inside headSize.  Method 0x30 is "stored", which is what RAR
         * uses for a comment small enough not to be worth packing; anything
         * else is squeezed with the RAR method and would need the whole
         * unpacker pointed at a header, so it is left alone and the archive
         * simply reports no comment.  Better that than half a comment. */
        if ( type == RAR_COMMENT && !z->comment && headSize > 13 &&
             hdr[10] == 0x30 )                                /* stored */
        {
            int n = (int)headSize - 13;
            z->comment = (char *)malloc( (size_t)n + 1 );
            if ( z->comment )
            {
                memcpy( z->comment, hdr + 13, (size_t)n );
                z->comment[n] = '\0';
            }
        }

        /* THE ARCHIVE COMMENT, new style - a "CMT" sub-block, usually packed.
         * This is what WinRAR 5 writes; the 0x75 branch above is what WinRAR
         * used to write.  Both are read, and whichever turns up first wins. */
        if ( type == RAR_NEWSUB && !z->comment )
            RarReadSubComment( z, hdr, headSize, flags, body );

        if ( type == RAR_MAIN )
        {
            z->solid = ( flags & MHD_SOLID ) ? 1 : 0;

            /* The names found a candidate set; THIS is what confirms it.  An
             * ordinary archive sitting next to an unrelated file that happens
             * to fit the continuation pattern - a.rar beside someone else's
             * a.r00 - must not swallow it, so a first volume that does not
             * claim to be one of a set is taken alone. */
            if ( vol == 0 && !( flags & MHD_VOLUME ) )
                z->volumes = 1;

            /* -hp: everything past this header is enciphered.  Without a
             * password there is nothing to report but "give me one" - the
             * archive cannot even be listed, which is why this is
             * SZ_ERR_PASSWORD and not SZ_ERR_UNSUPPORTED as it once was.
             * RarReadBlock picks the enciphered layout up from here on. */
            if ( ( flags & MHD_PASSWORD ) && !z->hdrEncrypted )
            {
                if ( !z->havePw ) { free( hdr ); rc = SZ_ERR_PASSWORD; break; }
                z->hdrEncrypted = 1;
            }
        }

        if ( type == RAR_FILE )
        {
            UInt32 packSize, unpSize, ftime, attr;
            UInt16 nameSize;
            int    nameOff;

            packSize = Rd32( hdr + 7 );
            unpSize  = Rd32( hdr + 11 );
            ftime    = Rd32( hdr + 20 );
            attr     = Rd32( hdr + 28 );
            nameSize = (UInt16)Rd16( hdr + 26 );
            nameOff  = 32;
            if ( flags & LHD_LARGE )           /* skip 8 bytes of high sizes */
                nameOff = 40;
            if ( nameOff + (int)nameSize > (int)headSize )
                nameSize = ( headSize > nameOff ) ? (UInt16)( headSize - nameOff ) : 0;

            /* A CONTINUATION rather than a new file: add this volume's bytes
             * to the entry already being built and take its CRC, then move on.
             *
             * Taking the CRC from the LATEST part is not arbitrary.  RAR puts
             * the CRC of just that piece in an intermediate part, but in the
             * FINAL part it puts the CRC of the whole reassembled file - so
             * letting each part overwrite the last leaves exactly the value
             * that the joined data has to match.  (Checked against fixtures:
             * for a 250000-byte file split across six volumes, the last
             * part's CRC is the CRC of all 250000 bytes, not of the 38482 in
             * that volume.) */
            if ( ( flags & LHD_SPLIT_BEFORE ) && z->numEntries > 0 )
            {
                int last = z->numEntries - 1;

                if ( !RarAddPiece( z, last, body, packSize ) )
                { rc = SZ_ERR_MEMORY; free( hdr ); break; }
                z->entries[last].packed += packSize;
                z->entries[last].crc     = Rd32( hdr + 16 );
                z->headFlags[last]       = flags;
                pos = body + (long)packSize;
                free( hdr );
                continue;
            }

            {
                int g = RarGrow( z );
                if ( g <= 0 )
                {
                    rc = ( g < 0 )
                       ? ArcCheckEntryCount( (UInt32)z->numEntries + 1 )
                       : SZ_ERR_MEMORY;
                    break;
                }
            }

            {
                RarEntry *e = &z->entries[z->numEntries];
                const Byte *np = hdr + nameOff;
                UInt16 j; int k = 0, last;

                for ( j = 0; j < nameSize && k < SZ_MAX_NAME - 1; j++ )
                {
                    char c = (char)np[j];
                    if ( c == '\0' ) break;    /* unicode: ascii part ends here */
                    e->name[k++] = ( c == '/' ) ? '\\' : c;
                }
                e->name[k] = '\0';

                e->size       = unpSize;
                e->packed     = packSize;
                e->crc        = Rd32( hdr + 16 );
                e->unpVer     = hdr[24];
                e->methodCode = hdr[25];
                e->modDate    = (UInt16)( ftime >> 16 );
                e->modTime    = (UInt16)( ftime & 0xFFFF );
                e->attrib     = attr;
                e->isDir      = ( ( flags & LHD_DIRECTORY ) == LHD_DIRECTORY );

                last = k - 1;
                if ( last >= 0 && e->name[last] == '\\' )
                { e->name[last] = '\0'; e->isDir = 1; }

                /* The salt sits immediately after the name.  It is only
                 * present when the flag says so: a very old RAR 2.0 entry can
                 * be encrypted WITHOUT one, and that variant is not supported
                 * here - better to leave the salt zeroed and let the entry
                 * report itself unsupported than to read whatever follows. */
                memset( z->salts + (size_t)z->numEntries * RAR_SALT_LEN,
                        0, RAR_SALT_LEN );
                if ( ( flags & LHD_SALT ) &&
                     nameOff + (int)nameSize + RAR_SALT_LEN <= (int)headSize )
                    memcpy( z->salts + (size_t)z->numEntries * RAR_SALT_LEN,
                            hdr + nameOff + nameSize, RAR_SALT_LEN );

                /* Where the DATA starts, which under -hp is not pos+headSize:
                 * the salt sits in front of the header and the header itself
                 * is padded out to a cipher block.  RarReadBlock worked it
                 * out; taking it from there is what keeps that arithmetic in
                 * one place. */
                z->dataOffset[z->numEntries] = body;
                z->headFlags[z->numEntries]  = flags;

                z->pieceCount[z->numEntries] = 0;
                z->pieceFirst[z->numEntries] = 0;
                if ( !RarAddPiece( z, z->numEntries, body, packSize ) )
                { rc = SZ_ERR_MEMORY; free( hdr ); break; }

                z->numEntries++;
            }

            pos = body + (long)packSize;
        }
        else
        {
            /* ADD_SIZE follows the 7-byte base header and is counted inside
             * headSize, so it is already in hand - it must be read from the
             * decrypted buffer, never re-read from the file, or an -hp
             * archive would take four bytes of ciphertext for a length. */
            UInt32 addSize = 0;
            if ( ( flags & LONG_BLOCK ) && headSize >= 11 )
                addSize = Rd32( hdr + 7 );
            pos = body + (long)addSize;
        }

        free( hdr );
    }
    }   /* per-volume */

    /* AN INCOMPLETE SET.  The last file header of the last volume we have
     * says whether RAR meant to carry on into another one.  If it did and
     * there is no next volume, the listing we just built is missing both the
     * rest of that file and every file after it.
     *
     * This is the case that used to pass silently, and it was the worst kind
     * of silence: the entry carried the FULL unpacked size from its header, so
     * the listing looked complete, the partial data matched the partial CRC
     * that volume carried, and test and extract both reported success while
     * writing a third of a file.  Nothing anywhere said the archive was cut
     * short.  A missing volume has to be its own answer. */
    if ( rc == SZ_OK && z->numEntries > 0 &&
         ( z->headFlags[z->numEntries - 1] & LHD_SPLIT_AFTER ) )
        z->volIncomplete = 1;

    /* THE EMPTY-ARCHIVE TRAP.  Nothing above fails loudly when no block can be
     * read at all: the walk simply ends, which is also how a well-formed
     * archive finishes, and the result would be "opened fine, no files in it"
     * - the one answer that is never useful.  So with -hp at least one header
     * CRC must actually have validated.
     *
     * WHICH failure it was matters, and the two are distinguishable.  If the
     * file ran out before a whole block could even be READ, that is true
     * whatever the password is, so calling it a bad password would send the
     * user off retyping a password that was right - the same misreport the
     * PPMd case cost us last round, pointed at a different cause.  A block
     * that was read in full and then failed its CRC is the password. */
    if ( rc == SZ_OK && z->hdrEncrypted && !z->hdrPwOk )
        rc = z->hdrTrunc ? SZ_ERR_READ : SZ_ERR_BADPASS;

    /* Refuse an incomplete set rather than list it.  Showing what we can see
     * would be friendlier if there were any way to say "and there is more
     * missing" alongside it, but there is not: a listing is just a listing, and
     * one that silently stops short of the archive's real contents is the
     * misreport this whole change exists to remove. */
    if ( rc == SZ_OK && z->volIncomplete )
        rc = SZ_ERR_VOLUME;

    if ( rc != SZ_OK ) { RarClose( z ); return rc; }

    /* The per-entry key cache starts warm: the header key was derived from
     * the same password, and WinRAR writes the archive salt in front of the
     * headers AND inside them, so the first entry extracted would otherwise
     * pay 262144 rounds of SHA-1 over again for a key already in hand.  If an
     * entry ever carries a different salt, RarEnsureKey notices and re-derives
     * - the cache is keyed by salt, so seeding it cannot make it wrong. */
    if ( z->hdrEncrypted && z->hdrKeyValid && !z->keyValid )
    {
        memcpy( z->key, z->hdrKey, 16 );
        memcpy( z->iv,  z->hdrIv,  16 );
        memcpy( z->cachedSalt, z->hdrSalt, RAR_SALT_LEN );
        z->keyValid = 1;
    }

    RarTrim( z );
    *out = z;
    return SZ_OK;
}

int RarOpen( const char *path, RarArchive **out )
{
    return RarOpenPw( path, NULL, out );
}

int RarNumEntries( RarArchive *r )
{
    return r ? r->numEntries : 0;
}

int RarVolumeCount( RarArchive *r )
{
    return ( r && r->volumes > 0 ) ? r->volumes : 1;
}

const RarEntry *RarGetEntry( RarArchive *r, int index )
{
    if ( !r || index < 0 || index >= r->numEntries ) return NULL;
    return &r->entries[index];
}


/*---- encryption ---------------------------------------------------------- *
 * RAR3 is AES-128-CBC over the PACKED bytes, so decryption sits between the
 * file and the unpacker and neither of them needs to know about it.
 *
 * There is no password check value - RAR5 has one, RAR3 does not - so a wrong
 * password cannot be reported until the CRC of the extracted data fails.  That
 * is the same bind 7z is in, and it is resolved the same way: see the
 * SZ_ERR_PASSWORD / SZ_ERR_BADPASS note in ARCDEFS.H for why the inference
 * only runs in that one direction.
 *--------------------------------------------------------------------------*/

/* Derive (or reuse) the key for this entry.  Returns SZ_ERR_PASSWORD when no
 * password has been set and SZ_OK once a key is in hand - never BADPASS,
 * because at this point nothing can tell. */
static int RarEnsureKey( RarArchive *z, int idx )
{
    const Byte *salt = z->salts + (size_t)idx * RAR_SALT_LEN;

    if ( !z->havePw ) return SZ_ERR_PASSWORD;

    if ( !z->keyValid || memcmp( z->cachedSalt, salt, RAR_SALT_LEN ) != 0 )
    {
        Rar3DeriveKeys( z->password, salt, z->key, z->iv );
        memcpy( z->cachedSalt, salt, RAR_SALT_LEN );
        z->keyValid = 1;
    }
    return SZ_OK;
}

/* Could a wrong password have caused this failure?
 *
 * With the wrong key the packed stream is noise, so the decoder either gives
 * up (SZ_ERR_DATA) or produces bytes that fail the CRC - those two, and only
 * those two, are worth re-reporting as a bad password.
 *
 * The list matters in BOTH directions, and the second one is easy to miss.
 * Blaming the archive for a typo is the obvious mistake; blaming a typo for
 * something else is just as bad.  An entry using a compression feature the
 * decoder does not implement fails with SZ_ERR_UNSUPPORTED whether or not it
 * is encrypted, and telling the user "wrong password" about it would send them
 * off retyping a password that was right all along.  Ditto out of memory, a
 * read error, or a cancel.
 */
static int RarCouldBeBadPassword( int rc )
{
    return rc == SZ_ERR_DATA || rc == SZ_ERR_CRC;
}

/* Is this entry encrypted in a way we can actually undo?  An encrypted entry
 * with no salt is RAR 2.0 era and uses a different scheme entirely. */
static int RarEntrySupportedCrypt( RarArchive *z, int idx )
{
    UInt16 f = z->headFlags[idx];
    return ( f & LHD_PASSWORD ) && ( f & LHD_SALT );
}

void RarSetPassword( RarArchive *r, const char *pw )
{
    if ( !r ) return;

    /* Any change invalidates the cached key, clearing included - a key derived
     * from a password the user has retracted must not outlive it. */
    r->keyValid = 0;

    if ( !pw || !pw[0] )
    {
        r->password[0] = '\0';
        r->havePw      = 0;
        return;
    }
    lstrcpyn( r->password, pw, sizeof( r->password ) );
    r->havePw = 1;
}

int RarEntryEncrypted( RarArchive *r, int index )
{
    if ( !r || index < 0 || index >= r->numEntries ) return 0;
    return ( r->headFlags[index] & LHD_PASSWORD ) ? 1 : 0;
}

int RarNeedsPassword( RarArchive *r )
{
    int i;
    if ( !r ) return 0;
    for ( i = 0; i < r->numEntries; i++ )
        if ( r->headFlags[i] & LHD_PASSWORD ) return 1;
    return 0;
}

/*---- extract one entry by index ------------------------------------------ */
static int RarExtractIndex( RarArchive *z, int idx, const char *destDir )
{
    RarEntry   *e = &z->entries[idx];
    char        outPath[SZ_MAX_NAME * 4];
    FILE       *out;
    AesCbcState cbc;
    RarDataReader dr;
    int         encrypted = 0;
    int         rc = SZ_OK;

    /* destDir == NULL means "test only": decode + CRC-check but write nothing. */
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
    if ( z->headFlags[idx] & LHD_PASSWORD )
    {
        if ( !RarEntrySupportedCrypt( z, idx ) )
            return SZ_ERR_UNSUPPORTED;             /* RAR 2.0 saltless crypt */
        rc = RarEnsureKey( z, idx );
        if ( rc != SZ_OK ) return rc;              /* SZ_ERR_PASSWORD        */
        AesCbcInit( &cbc, z->key, 16, z->iv );
        encrypted = 1;
        rc = SZ_OK;
    }

    RarDataInit( &dr, z, idx );
    if ( destDir ) MakeDirs( outPath, 0 );

    if ( e->methodCode == RAR_METHOD_STORE )
    {
        UInt32        crc = Crc32Init(), remain = e->packed;
        UInt32        left = e->size;      /* real bytes still owed          */
        unsigned int  toRead, use;
        unsigned char buf[512];

        out = destDir ? fopen( outPath, "wb" ) : NULL;
        if ( destDir && !out ) return SZ_ERR_WRITE;
        while ( remain > 0 )
        {
            toRead = ( remain > 512UL ) ? 512U : (unsigned int)remain;
            if ( !RarDataRead( &dr, buf, toRead ) ) { rc = SZ_ERR_READ;  break; }

            /* CBC pads to 16, so an encrypted STORED entry is LONGER on disk
             * than the file it holds.  Decrypt the whole block but only keep
             * e->size bytes - the tail is padding and belongs to neither the
             * output nor the CRC.  512 is a multiple of 16, so a chunk never
             * splits a cipher block. */
            if ( encrypted ) AesCbcDecrypt( &cbc, buf, toRead );
            use = ( left < (UInt32)toRead ) ? (unsigned int)left : toRead;

            crc = Crc32Update( crc, buf, use );
            if ( out && use && fwrite( buf, 1, use, out ) != use ) { rc = SZ_ERR_WRITE; break; }
            left   -= use;
            remain -= toRead;
        }
        if ( out ) fclose( out );
        if ( rc == SZ_OK && Crc32Done( crc ) != e->crc )
            rc = encrypted ? SZ_ERR_BADPASS : SZ_ERR_CRC;
    }
    else                                           /* compressed */
    {
        Byte *packBuf, *outBuf;

        if ( z->headFlags[idx] & LHD_SOLID )
            return SZ_ERR_UNSUPPORTED;             /* needs cross-file window */
        if ( e->unpVer < 20 )
            return SZ_ERR_UNSUPPORTED;             /* RAR 1.x (v1)            */
        /* Non-solid RAR decodes to a buffer, so this entry has to fit in RAM
         * whole (unlike the 7z folder path, which streams).  NORAM rather
         * than TOOBIG: the cap is a measured memory budget, so this is a
         * shortage of RAM and not a configured limit (see SzDecodeFolder). */
        if ( e->packed > SZ_MAX_BUFFER_SIZE || e->size > SZ_MAX_BUFFER_SIZE )
            return SZ_ERR_NORAM;

        packBuf = (Byte *)malloc( e->packed ? e->packed : 1 );
        if ( !packBuf ) return SZ_ERR_MEMORY;
        if ( !RarDataRead( &dr, packBuf, e->packed ) )
        { free( packBuf ); return SZ_ERR_READ; }

        /* Decryption sits underneath the unpacker: what was encrypted is the
         * PACKED stream, so by the time the decoder sees it the entry looks
         * exactly like an ordinary one. */
        if ( encrypted ) AesCbcDecrypt( &cbc, packBuf, e->packed & ~15UL );

        outBuf = (Byte *)malloc( e->size ? e->size : 1 );
        if ( !outBuf ) { free( packBuf ); return SZ_ERR_MEMORY; }

        rc = ( e->unpVer >= 29 )
             ? Rar3DecodeSized( packBuf, e->packed, outBuf, e->size,
                                RarWinSize( z->headFlags[idx] ) )  /* RAR3 v3 */
             : Rar2DecodeSized( packBuf, e->packed, outBuf, e->size,
                                RarWinSize( z->headFlags[idx] ) ); /* RAR2 v2 */
        free( packBuf );

        /* A wrong password makes the packed stream noise, so the decoder is
         * as likely to bail out as to produce wrong bytes - either way the
         * honest report is BADPASS and not a corrupt archive.  Only for the
         * failures a wrong key can actually cause, though: see
         * RarCouldBeBadPassword. */
        if ( encrypted && RarCouldBeBadPassword( rc ) )
            rc = SZ_ERR_BADPASS;
        if ( rc == SZ_OK && Crc32Calc( outBuf, e->size ) != e->crc )
            rc = encrypted ? SZ_ERR_BADPASS : SZ_ERR_CRC;
        if ( rc == SZ_OK && destDir )
        {
            out = fopen( outPath, "wb" );
            if ( !out ) { free( outBuf ); return SZ_ERR_WRITE; }
            if ( e->size && fwrite( outBuf, 1, e->size, out ) != e->size )
                rc = SZ_ERR_WRITE;
            fclose( out );
        }
        free( outBuf );
    }

    if ( rc == SZ_OK )
    {
        if ( destDir ) SetFileDosMTime( outPath, e->modDate, e->modTime );
    }
    else if ( destDir )
        remove( outPath );
    return rc;
}

/*---- solid extraction (streaming) ---------------------------------------- *
 * In a solid archive every file shares one continuous LZ window, so a file
 * can't be decoded without the ones before it.  We decode the data-bearing
 * files in archive order through one persistent RAR2 context whose bounded
 * ring window supplies match look-back, streaming each output byte through a
 * sink that writes to the current file and rolls over at file boundaries.
 * Memory stays flat (window + small buffers) regardless of archive size.
 *-------------------------------------------------------------------------- */
typedef struct {
    RarArchive *z;
    const char *destDir;
    const char *req;             /* 1 per entry: requested for output         */
    SzProgress  prog;
    void       *user;
    int         scanNext;        /* next entry index to scan for a chain file */
    int         curIdx;          /* current file's entry index (-1 = none)    */
    int         curReq;          /* current file is requested                 */
    UInt32      fileRemaining;
    UInt32      fileCrc;
    FILE       *out;
    char        outPath[SZ_MAX_NAME * 4];
    Byte        buf[8192];
    UInt32      bufLen;
    int         rc;
} SolidSink;

static void SolidFlush( SolidSink *s )
{
    if ( s->bufLen )
    {
        if ( s->curReq )
            s->fileCrc = Crc32Update( s->fileCrc, s->buf, s->bufLen );
        if ( s->out && fwrite( s->buf, 1, s->bufLen, s->out ) != s->bufLen )
            s->rc = SZ_ERR_WRITE;
        s->bufLen = 0;
    }
}

static void SolidFinishFile( SolidSink *s )
{
    if ( s->curIdx < 0 ) return;
    SolidFlush( s );
    if ( s->out ) { fclose( s->out ); s->out = NULL; }
    if ( s->rc == SZ_OK && s->curReq )
    {
        RarEntry *e = &s->z->entries[s->curIdx];
        if ( Crc32Done( s->fileCrc ) != e->crc )
            /* In a solid chain the password covers every member, so a CRC
             * failure on an encrypted one is a bad password rather than a
             * corrupt archive - same inference as the non-solid path. */
            s->rc = ( s->z->headFlags[ s->curIdx ] & LHD_PASSWORD )
                  ? SZ_ERR_BADPASS : SZ_ERR_CRC;
        else if ( s->destDir )
            SetFileDosMTime( s->outPath, e->modDate, e->modTime );
    }
    s->curIdx = -1;
}

static void SolidOpenNext( SolidSink *s )
{
    RarArchive *z = s->z;
    int i;
    for ( i = s->scanNext; i < z->numEntries; i++ )
        if ( !z->entries[i].isDir && z->entries[i].size > 0 )
        {
            RarEntry *e = &z->entries[i];
            s->scanNext      = i + 1;
            s->curIdx        = i;
            s->curReq        = s->req[i];
            s->fileRemaining = e->size;
            s->fileCrc       = Crc32Init();
            s->bufLen        = 0;
            s->out           = NULL;
            if ( s->curReq && s->destDir )    /* NULL destDir = test only */
            {
                BuildOut( s->outPath, sizeof( s->outPath ),
                          s->destDir, e->name, e->isDir );
                /* Declined overwrite: demote to "not requested" - the chain
                 * still decodes through it, but nothing is written, CRC-
                 * checked, or timestamped, and the kept file is never
                 * removed.  A skipped or unnameable entry is demoted the same
                 * way; a cancel has to stop the chain outright. */
                if ( ArcNameVerdict() == ARC_NAME_ABORT )
                {
                    s->rc     = SZ_ERR_CANCEL;   /* the callers all check rc */
                    s->curReq = 0;
                }
                else if ( ArcNameVerdict() == ARC_NAME_SKIP ||
                          !ArcWantWrite( s->outPath ) )
                    s->curReq = 0;
            }
            if ( s->curReq )
            {
                if ( s->destDir )
                {
                    MakeDirs( s->outPath, 0 );
                    s->out = fopen( s->outPath, "wb" );
                    if ( !s->out ) { s->rc = SZ_ERR_WRITE; return; }
                }
                if ( s->prog && !s->prog( s->user, i, z->numEntries, e->name ) )
                    s->rc = SZ_ERR_CANCEL;
            }
            return;
        }
    s->rc = SZ_ERR_DATA;             /* data left but no chain file to hold it */
}

static int SolidEmit( void *user, Byte b )
{
    SolidSink *s = (SolidSink *)user;
    if ( s->rc != SZ_OK ) return s->rc;
    if ( s->fileRemaining == 0 )
    {
        SolidFinishFile( s );
        if ( s->rc != SZ_OK ) return s->rc;
        SolidOpenNext( s );
        if ( s->rc != SZ_OK ) return s->rc;
    }
    s->buf[s->bufLen++] = b;
    s->fileRemaining--;
    if ( s->bufLen == sizeof( s->buf ) )
        SolidFlush( s );
    return s->rc;
}

static int SolidExtract( RarArchive *z, const int *indices, int count,
                         const char *destDir, SzProgress prog, void *user )
{
    char       *req;                 /* per-entry "wanted" flags (heap: the  */
    SolidSink   sink;                /* stack cannot hold one per entry)     */
    Rar2Ctx    *ctx2 = NULL;
    Rar3Ctx    *ctx3 = NULL;
    Byte       *packBuf = NULL;
    UInt32      packCap = 0, cum = 0;
    int         i, k, lastReqChain = -1, useV3 = 0, rc = SZ_OK;
    int         needTables = 1;      /* v2 only - v3 asks Rar3NeedTables */

    req = (char *)calloc( z->numEntries ? z->numEntries : 1, 1 );
    if ( !req ) return SZ_ERR_MEMORY;
    if ( indices )
    {
        for ( k = 0; k < count; k++ )
            if ( indices[k] >= 0 && indices[k] < z->numEntries )
                req[indices[k]] = 1;
    }
    else
        for ( i = 0; i < z->numEntries; i++ ) req[i] = 1;

    if ( destDir && !ArcFlattenPaths() )      /* NULL destDir = test only */
        for ( i = 0; i < z->numEntries; i++ ) /* requested directories */
            if ( req[i] && z->entries[i].isDir && z->entries[i].name[0] )
            {
                char outPath[SZ_MAX_NAME * 4];
                BuildOut( outPath, sizeof( outPath ), destDir, z->entries[i].name,
                          z->entries[i].isDir );
                if ( ArcNameVerdict() != ARC_NAME_OK ) continue;
                MakeDirs( outPath, 1 );
            }

    for ( i = 0; i < z->numEntries; i++ )
        if ( req[i] && !z->entries[i].isDir && z->entries[i].size > 0 )
            lastReqChain = i;

    if ( lastReqChain >= 0 )
    {
        /* One solid chain is produced by a single RAR version, so pick the
         * decoder from the first compressed data-bearing entry.  A chain with
         * only STORED entries needs no real codec (Feed handles those); the
         * v2 context then just supplies the shared ring window. */
        for ( i = 0; i <= lastReqChain; i++ )
        {
            RarEntry *e = &z->entries[i];
            if ( e->isDir || e->size == 0 ) continue;
            if ( e->methodCode == RAR_METHOD_STORE ) continue;
            useV3 = ( e->unpVer >= 29 ) ? 1 : 0;
            break;
        }

        /* One window for the whole chain, sized from what the archive says it
         * used rather than from what RAR could possibly have used. */
        {
            UInt32 win = RarMaxWinSize( z );

            if ( useV3 ) { ctx3 = Rar3CreateSized( win ); if ( !ctx3 ) { free( req ); return SZ_ERR_MEMORY; } }
            else         { ctx2 = Rar2CreateSized( win ); if ( !ctx2 ) { free( req ); return SZ_ERR_MEMORY; } }
        }

        sink.z = z; sink.destDir = destDir; sink.req = req;
        sink.prog = prog; sink.user = user;
        sink.scanNext = 0; sink.curIdx = -1; sink.curReq = 0;
        sink.fileRemaining = 0; sink.fileCrc = 0;
        sink.out = NULL; sink.bufLen = 0; sink.rc = SZ_OK;

        for ( i = 0; i <= lastReqChain && rc == SZ_OK; i++ )
        {
            RarEntry *e = &z->entries[i];

            if ( e->isDir || e->size == 0 ) continue;
            if ( z->headFlags[i] & LHD_PASSWORD )
            {
                if ( !RarEntrySupportedCrypt( z, i ) )
                { rc = SZ_ERR_UNSUPPORTED; break; }   /* RAR 2.0 saltless   */
                rc = RarEnsureKey( z, i );
                if ( rc != SZ_OK ) break;             /* SZ_ERR_PASSWORD    */
            }
            if ( e->unpVer < 20 ) { rc = SZ_ERR_UNSUPPORTED; break; }  /* v1 */
            if ( e->methodCode != RAR_METHOD_STORE &&
                 ( ( e->unpVer >= 29 ) ? 1 : 0 ) != useV3 )
            { rc = SZ_ERR_UNSUPPORTED; break; }        /* mixed-version solid */

            if ( e->packed > packCap )
            {
                Byte *nb = (Byte *)realloc( packBuf, e->packed );
                if ( !nb ) { rc = SZ_ERR_MEMORY; break; }
                packBuf = nb; packCap = e->packed;
            }
            {
                RarDataReader sdr;
                RarDataInit( &sdr, z, i );
                if ( !RarDataRead( &sdr, packBuf, e->packed ) )
                { rc = SZ_ERR_READ; break; }
            }

            /* Each member of a solid chain is encrypted on its own, with its
             * own IV, even though the LZ window runs across all of them - so
             * the CBC state starts fresh here and does NOT carry over. */
            if ( z->headFlags[i] & LHD_PASSWORD )
            {
                AesCbcState scbc;
                AesCbcInit( &scbc, z->key, 16, z->iv );
                AesCbcDecrypt( &scbc, packBuf, e->packed & ~15UL );
            }

            cum += e->size;
            if ( e->methodCode == RAR_METHOD_STORE )
                rc = useV3
                     ? Rar3Feed( ctx3, SolidEmit, &sink, packBuf, e->packed )
                     : Rar2Feed( ctx2, SolidEmit, &sink, packBuf, e->packed );
            else if ( useV3 )
            {
                Rar3SetInput( ctx3, packBuf, e->packed );
                if ( Rar3NeedTables( ctx3 ) )
                { rc = Rar3ReadTables( ctx3 ); if ( rc ) break; }
                rc = Rar3Decode2( ctx3, SolidEmit, &sink, cum );
                /* Read the trailer that closes this member BEFORE its
                 * buffer goes away: it carries the only statement in the
                 * archive about whether the next member brings its own
                 * tables, and Rar3Decode2 stops one symbol short of it.
                 * Skipping it decodes the next member with stale tables,
                 * which fails as a CRC mismatch or a bogus symbol rather
                 * than as anything that names the real cause. */
                if ( rc == SZ_OK ) Rar3EndOfFile( ctx3 );
            }
            else
            {
                /* v2 has no end-of-block trailer: it announces new tables
                 * inline with symbol 269, which Rar2Decode2 already acts
                 * on, so the chain reads tables exactly once at the top. */
                Rar2SetInput( ctx2, packBuf, e->packed );
                if ( needTables )
                { rc = Rar2ReadTables( ctx2 ); needTables = 0; if ( rc ) break; }
                rc = Rar2Decode2( ctx2, SolidEmit, &sink, cum );
            }

            /* A wrong password leaves the decoder reading noise, so it gives
             * up long before any CRC is reached and the raw verdict is
             * "corrupt compressed data".  For an encrypted member that is the
             * wrong thing to tell the user: it blames the archive for what is
             * almost always a typo.  Same inference as the non-solid path. */
            if ( RarCouldBeBadPassword( rc ) && ( z->headFlags[i] & LHD_PASSWORD ) )
                rc = SZ_ERR_BADPASS;
        }

        if ( rc == SZ_OK ) { SolidFinishFile( &sink ); rc = sink.rc; }
        if ( sink.out ) fclose( sink.out );
        if ( packBuf ) free( packBuf );
        if ( ctx3 ) Rar3Free( ctx3 );
        if ( ctx2 ) Rar2Free( ctx2 );
        if ( rc != SZ_OK ) { free( req ); return rc; }
    }

    if ( destDir )                            /* NULL destDir = test only */
        for ( i = 0; i < z->numEntries; i++ ) /* requested empty files */
            if ( req[i] && !z->entries[i].isDir && z->entries[i].size == 0
                 && z->entries[i].name[0] )
            {
                char  outPath[SZ_MAX_NAME * 4];
                FILE *out;
                BuildOut( outPath, sizeof( outPath ), destDir, z->entries[i].name,
                          z->entries[i].isDir );
                if ( ArcNameVerdict() != ARC_NAME_OK ) continue;
                if ( !ArcWantWrite( outPath ) ) continue;
                MakeDirs( outPath, 0 );
                out = fopen( outPath, "wb" );
                if ( out ) fclose( out );
            }

    free( req );
    return SZ_OK;
}

int RarExtractAll( RarArchive *r, const char *destDir,
                   SzProgress prog, void *user )
{
    int i, rc;
    if ( !r ) return SZ_ERR_FORMAT;
    if ( r->solid )
        return SolidExtract( r, NULL, 0, destDir, prog, user );
    for ( i = 0; i < r->numEntries; i++ )
    {
        if ( !r->entries[i].name[0] ) continue;
        if ( prog && !prog( user, i, r->numEntries, r->entries[i].name ) )
            return SZ_ERR_CANCEL;
        rc = RarExtractIndex( r, i, destDir );
        if ( rc ) return rc;
    }
    return SZ_OK;
}

int RarExtractItems( RarArchive *r, const int *indices, int count,
                     const char *destDir, SzProgress prog, void *user )
{
    int k, idx, rc;
    if ( !r ) return SZ_ERR_FORMAT;
    if ( r->solid )
        return SolidExtract( r, indices, count, destDir, prog, user );
    for ( k = 0; k < count; k++ )
    {
        idx = indices[k];
        if ( idx < 0 || idx >= r->numEntries ) continue;
        if ( !r->entries[idx].name[0] ) continue;
        if ( prog && !prog( user, idx, r->numEntries, r->entries[idx].name ) )
            return SZ_ERR_CANCEL;
        rc = RarExtractIndex( r, idx, destDir );
        if ( rc ) return rc;
    }
    return SZ_OK;
}

void RarClose( RarArchive *r )
{
    if ( r )
    {
        if ( r->fp )         VolClose( r->fp );
        if ( r->entries )    free( r->entries );
        if ( r->dataOffset ) free( r->dataOffset );
        if ( r->pieces )     free( r->pieces );
        if ( r->pieceFirst ) free( r->pieceFirst );
        if ( r->pieceCount ) free( r->pieceCount );
        if ( r->headFlags )  free( r->headFlags );
        if ( r->salts )      free( r->salts );
        if ( r->comment )    free( r->comment );
        free( r );
    }
}
