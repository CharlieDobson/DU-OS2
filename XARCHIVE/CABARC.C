/*===========================================================================
 * CABARC.C  -  Microsoft cabinet (.cab) parsing and extraction
 * Target: MSVC 2.2  Win32s, and the Open Watcom DOS/32A and OS/2 builds
 *
 * Layout, from Microsoft's published [MS-CAB] specification:
 *
 *   CFHEADER  "MSCF", sizes, version 1.3, folder and file counts, flags; an
 *             optional set of reserve sizes and reserved bytes; optional
 *             names of the previous and next cabinets of a set
 *   CFFOLDER  per folder: where its first data block is, how many blocks it
 *             has, and the compression type (window size included)
 *   CFFILE    per file: size, offset in its folder's output, folder index,
 *             DOS date, time and attributes, name
 *   CFDATA    per block: checksum, compressed and uncompressed sizes, then
 *             the compressed bytes - at most 32 KB of output each
 *
 * A file's folder index has three special values for a SET: continued from
 * the previous cabinet, continued to the next, or both.  A file that crosses
 * a join is listed in every cabinet it touches; it is kept from the first and
 * the later mentions are dropped.  Its folder crosses the join too: the last
 * folder of one cabinet and the first of the next are one compressed stream,
 * and the block that straddles the join is split in two, the first piece
 * declaring an uncompressed size of 0.
 *
 * Extraction is a folder at a time, in offset order, decoding straight to the
 * output files - a folder is a solid stream, and must be decoded from its
 * start to reach anything in it.  The decoders each take one block's bytes
 * and hand back its output; see MSZIPDEC.H, LZXDEC.H and QTMDEC.H.
 *===========================================================================*/

#include <windows.h>     /* lstrcpyn, WideCharToMultiByte */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>      /* _mkdir */

#include "cabarc.h"
#include "mszipdec.h"
#include "lzxdec.h"
#include "qtmdec.h"
#include "deflenc.h"    /* deleting: a part-kept folder is packed again */
#include "platform.h"   /* SetFileDosMTime */
#include "volio.h"      /* every cabinet of a set through one file handle */

#define CAB_HDR_SIZE        36
#define CAB_FOLDER_SIZE     8
#define CAB_FILE_SIZE       16
#define CAB_DATA_SIZE       8

#define CAB_FLAG_PREV       0x0001
#define CAB_FLAG_NEXT       0x0002
#define CAB_FLAG_RESERVE    0x0004

#define CAB_FROM_PREV       0xFFFD
#define CAB_TO_NEXT         0xFFFE
#define CAB_PREV_AND_NEXT   0xFFFF

#define CAB_ATTR_SHOWN      0x27      /* read-only, hidden, system, archive */
#define CAB_ATTR_UTF8       0x80      /* the name is UTF-8                  */

#define CAB_COMP_MASK       0x000F
#define CAB_COMP_NONE       0
#define CAB_COMP_MSZIP      1
#define CAB_COMP_QUANTUM    2
#define CAB_COMP_LZX        3
#define CAB_WINDOW( type )  ( ( (type) >> 8 ) & 0x1F )

/* What one block can decode to, and the most compressed bytes it can bring
 * with it.  The format caps a block at 32 KB + 6 KB of compressed data; the
 * input buffer is sized for a whole 64 KB record plus that, so that a block
 * split across two cabinets and joined back together still fits. */
#define CAB_BLOCK_OUT       32768U
#define CAB_BLOCK_IN        ( 65536UL + 8192UL )

#define CAB_MAX_SET         256       /* cabinets in one set               */
#define CAB_NAME_MAX        256       /* the longest name field kept        */
#define CAB_PATH_MAX        ( SZ_MAX_NAME * 2 )
#define CAB_SCAN_CHUNK      65536L

/* One cabinet of the set. */
typedef struct {
    long   base;                /* where it starts in the joined stream     */
    long   embed;               /* how far into its own file it starts     */
    UInt32 size;                /* cbCabinet                               */
    UInt16 setId, index;        /* which set, and its place in it          */
    UInt16 flags;
    UInt16 numFolders, numFiles;
    UInt32 filesAt;             /* coffFiles: the CFFILE table             */
    UInt32 foldersAt;           /* just past the header: the CFFOLDER table */
    int    folderReserve;       /* bytes reserved after each CFFOLDER      */
    int    dataReserve;         /* and after each CFDATA header            */
} CabVol;

/* One cabinet's share of a folder. */
typedef struct {
    int    vol;
    UInt32 dataAt;              /* coffCabStart: its first CFDATA          */
    UInt16 numBlocks;
    UInt32 packed;              /* compressed bytes, or 0xFFFFFFFF          */
} CabPart;

/* A folder: one compressed stream, perhaps spread over several cabinets.
 * Its parts are always consecutive in the part table, because the folder
 * that crosses a join is the LAST of one cabinet and the FIRST of the next. */
typedef struct {
    UInt16 type;                /* typeCompress                            */
    int    firstPart, numParts;
    int    firstEntry;          /* the entry that reports its packed size  */
} CabFolder;

struct CabArchive {
    VolFile   *fp;
    int        numVols;
    CabVol    *vols;
    CabFolder *folders;
    int        numFolders, capFolders;
    CabPart   *parts;
    int        numParts, capParts;
    CabEntry  *entries;
    int        numEntries, capEntries;
    UInt32     sumBlocks;       /* blocks the last run checked by checksum */
    UInt32     bareBlocks;      /* and blocks that carried none            */
};

/*---- Little-endian fields --------------------------------------------------*/
static UInt32 CabLe16( const Byte *ptr )
{
    return ptr[0] | ( (UInt32)ptr[1] << 8 );
}

static UInt32 CabLe32( const Byte *ptr )
{
    return ptr[0] | ( (UInt32)ptr[1] << 8 ) |
           ( (UInt32)ptr[2] << 16 ) | ( (UInt32)ptr[3] << 24 );
}

/*---- The block checksum ----------------------------------------------------
 * XOR of the data taken as little-endian 32-bit words.  A tail of one to
 * three bytes is folded in BIG-endian - the opposite way round from the rest,
 * and the one detail worth getting right.  A block's checksum covers its
 * data first, then the four size bytes of its header; not the reserved area.
 *--------------------------------------------------------------------------- */
static UInt32 CabChecksum( const Byte *data, UInt32 len, UInt32 sum )
{
    UInt32 i, tail = 0;

    for ( i = 0; i + 4 <= len; i += 4 )
        sum ^= CabLe32( data + i );
    switch ( len & 3 )
    {
    case 3: tail |= (UInt32)data[i++] << 16;   /* and on into the next */
    case 2: tail |= (UInt32)data[i++] << 8;
    case 1: tail |= data[i];
    }
    return sum ^ tail;
}

/*---- Names -----------------------------------------------------------------*/

/* A NUL-ended string field.  Anything past 'keep' bytes is read and dropped,
 * so the file position still ends up after the terminator. */
static int CabReadString( VolFile *fp, char *dst, int keep )
{
    int len = 0;

    for ( ;; )
    {
        int ch = VolGetc( fp );

        if ( ch == EOF ) return SZ_ERR_READ;
        if ( ch == 0 ) break;
        if ( len < keep - 1 ) dst[len++] = (char)ch;
    }
    dst[len] = '\0';
    return SZ_OK;
}

/* A UTF-8 name (attribute 0x80) into the local code page, the way SZARC
 * turns 7z's UTF-16 names into one.  Outside the 16-bit range, and for
 * anything malformed, '_'. */
static void CabUtf8Name( const char *src, char *dst, int dstSize )
{
    WCHAR       wide[CAB_NAME_MAX + 1];
    const Byte *in = (const Byte *)src;
    int         len = 0;

    while ( *in && len < CAB_NAME_MAX )
    {
        UInt32 ch = *in++;

        if ( ch >= 0xC0 && ch < 0xE0 && ( in[0] & 0xC0 ) == 0x80 )
        {
            ch = ( ( ch & 0x1F ) << 6 ) | ( in[0] & 0x3F );
            in += 1;
        }
        else if ( ch >= 0xE0 && ch < 0xF0 && ( in[0] & 0xC0 ) == 0x80 &&
                  ( in[1] & 0xC0 ) == 0x80 )
        {
            ch = ( ( ch & 0x0F ) << 12 ) | ( (UInt32)( in[0] & 0x3F ) << 6 ) |
                 ( in[1] & 0x3F );
            in += 2;
        }
        else if ( ch >= 0x80 )
        {
            ch = '_';
            while ( ( *in & 0xC0 ) == 0x80 ) in++;
        }
        wide[len++] = (WCHAR)ch;
    }
    wide[len] = 0;
    WideCharToMultiByte( CP_ACP, 0, wide, -1, dst, dstSize, NULL, NULL );
}

/*---- The header ------------------------------------------------------------
 * Fills 'vol' from the header at 'at', with the previous and next cabinets'
 * file names when the set has them ("" otherwise).  SZ_ERR_SIG when there is
 * no cabinet there; 'strict' adds the reserved-field checks a scan through
 * an .exe needs, where "MSCF" can turn up by accident.
 *--------------------------------------------------------------------------- */
static int CabReadHeader( VolFile *fp, long at, long avail, int strict,
                          CabVol *vol, char *prevName, char *nextName )
{
    Byte   hdr[CAB_HDR_SIZE];
    char   disk[CAB_NAME_MAX];
    UInt32 pos = CAB_HDR_SIZE;
    int    rc;

    if ( prevName ) prevName[0] = '\0';
    if ( nextName ) nextName[0] = '\0';

    if ( VolSeek( fp, at, SEEK_SET ) != 0 ||
         VolRead( hdr, 1, CAB_HDR_SIZE, fp ) != CAB_HDR_SIZE )
        return SZ_ERR_SIG;
    if ( memcmp( hdr, "MSCF", 4 ) != 0 || CabLe32( hdr + 4 ) != 0 )
        return SZ_ERR_SIG;

    memset( vol, 0, sizeof( *vol ) );
    vol->size       = CabLe32( hdr + 8 );
    vol->filesAt    = CabLe32( hdr + 16 );
    vol->numFolders = (UInt16)CabLe16( hdr + 26 );
    vol->numFiles   = (UInt16)CabLe16( hdr + 28 );
    vol->flags      = (UInt16)CabLe16( hdr + 30 );
    vol->setId      = (UInt16)CabLe16( hdr + 32 );
    vol->index      = (UInt16)CabLe16( hdr + 34 );

    if ( strict &&
         ( CabLe32( hdr + 12 ) != 0 || CabLe32( hdr + 20 ) != 0 ||
           vol->size < CAB_HDR_SIZE || (long)vol->size > avail ||
           vol->filesAt < CAB_HDR_SIZE || vol->filesAt >= vol->size ) )
        return SZ_ERR_SIG;

    /* Version 1.3 is the only one ever written.  A different minor number
     * is read anyway; a different major one is a different format. */
    if ( hdr[25] != 1 ) return strict ? SZ_ERR_SIG : SZ_ERR_UNSUPPORTED;
    if ( vol->numFolders == 0 || vol->numFiles == 0 )
        return strict ? SZ_ERR_SIG : SZ_ERR_FORMAT;

    if ( vol->flags & CAB_FLAG_RESERVE )
    {
        Byte res[4];

        if ( VolRead( res, 1, 4, fp ) != 4 ) return SZ_ERR_READ;
        vol->folderReserve = res[2];
        vol->dataReserve   = res[3];
        pos += 4 + CabLe16( res );
        if ( VolSeek( fp, at + (long)pos, SEEK_SET ) != 0 ) return SZ_ERR_READ;
    }

    if ( vol->flags & CAB_FLAG_PREV )
    {
        rc = CabReadString( fp, prevName ? prevName : disk, CAB_NAME_MAX );
        if ( rc == SZ_OK ) rc = CabReadString( fp, disk, CAB_NAME_MAX );
        if ( rc != SZ_OK ) return rc;
    }
    if ( vol->flags & CAB_FLAG_NEXT )
    {
        rc = CabReadString( fp, nextName ? nextName : disk, CAB_NAME_MAX );
        if ( rc == SZ_OK ) rc = CabReadString( fp, disk, CAB_NAME_MAX );
        if ( rc != SZ_OK ) return rc;
    }

    vol->foldersAt = (UInt32)( VolTell( fp ) - at );
    return SZ_OK;
}

/* Read one cabinet's header straight from its file. */
static int CabPeek( const char *path, long at, int strict,
                    CabVol *vol, char *prevName, char *nextName )
{
    VolFile *fp;
    long     len;
    int      rc;

    if ( VolOpen( path, &fp ) != SZ_OK ) return SZ_ERR_OPEN;
    len = VolSize( fp );
    rc  = CabReadHeader( fp, at, len - at, strict, vol, prevName, nextName );
    VolClose( fp );
    vol->embed = at;
    return rc;
}

/* 'dir' + 'name', where dir is everything in 'path' up to its file name. */
static void CabSibling( char *dst, const char *path, const char *name )
{
    int len = 0, keep = 0;

    while ( path[len] && len < CAB_PATH_MAX - 1 )
    {
        if ( path[len] == '\\' || path[len] == '/' || path[len] == ':' )
            keep = len + 1;
        len++;
    }
    memcpy( dst, path, keep );
    lstrcpyn( dst + keep, name, CAB_PATH_MAX - keep );
}

/*---- The set ---------------------------------------------------------------
 * The cabinet named is not necessarily the first, so walk back to the first,
 * then forward along the "next" names to the last.  The two directions are
 * NOT mirror images.  A "next" name is always the very next cabinet, but
 * makecab writes as "previous" the cabinet where the file being continued
 * BEGAN - in a seven-cabinet set holding one big file, cabinets 2, 3 and 4
 * all name cabinet 1.  So the walk back only asks for a lower index each
 * step, and the walk forward fills in everything between.
 *
 * Every cabinet has to claim the same set ID, and on the way forward the
 * next index; one that is not there, or is some other set's, is a missing
 * volume.  The names only PROPOSE the set and the headers confirm it, as
 * with RAR.  The named cabinet is used by its own path when the walk reaches
 * its index, so a renamed member, or one inside a self-extractor, still
 * counts as present.
 *--------------------------------------------------------------------------- */
static int CabFindSet( const char *path, long embed,
                       char *paths, CabVol *vols, int *count )
{
    CabVol named, cur;
    char   namedNext[CAB_NAME_MAX];
    char   prev[CAB_NAME_MAX], next[CAB_NAME_MAX];
    char   cand[CAB_PATH_MAX], curPath[CAB_PATH_MAX];
    int    steps = 0, n, rc;

    rc = CabPeek( path, embed, embed != 0, &named, prev, namedNext );
    if ( rc != SZ_OK ) return rc;

    /* Back to the cabinet that has no previous one. */
    cur = named;
    lstrcpyn( curPath, path, CAB_PATH_MAX );
    lstrcpyn( next, namedNext, CAB_NAME_MAX );
    while ( cur.flags & CAB_FLAG_PREV )
    {
        CabVol vol;

        if ( !prev[0] || ++steps >= CAB_MAX_SET ) return SZ_ERR_VOLUME;
        CabSibling( cand, curPath, prev );
        rc = CabPeek( cand, 0, 0, &vol, prev, next );
        if ( rc != SZ_OK || vol.setId != cur.setId || vol.index >= cur.index )
            return SZ_ERR_VOLUME;
        cur = vol;
        lstrcpyn( curPath, cand, CAB_PATH_MAX );
    }

    /* And forward from there, one cabinet at a time. */
    vols[0] = cur;
    lstrcpyn( paths, curPath, CAB_PATH_MAX );
    n = 1;
    while ( vols[n - 1].flags & CAB_FLAG_NEXT )
    {
        CabVol vol;
        char   dummy[CAB_NAME_MAX];
        UInt16 want = (UInt16)( vols[n - 1].index + 1 );

        if ( n >= CAB_MAX_SET ) return SZ_ERR_VOLUME;
        if ( want == named.index && named.setId == vols[0].setId )
        {
            vol = named;
            lstrcpyn( cand, path, CAB_PATH_MAX );
            lstrcpyn( next, namedNext, CAB_NAME_MAX );
        }
        else
        {
            if ( !next[0] ) return SZ_ERR_VOLUME;
            CabSibling( cand, paths + ( n - 1 ) * CAB_PATH_MAX, next );
            rc = CabPeek( cand, 0, 0, &vol, dummy, next );
            if ( rc != SZ_OK || vol.setId != vols[0].setId ||
                 vol.index != want )
                return SZ_ERR_VOLUME;
        }
        vols[n] = vol;
        lstrcpyn( paths + n * CAB_PATH_MAX, cand, CAB_PATH_MAX );
        n++;
    }

    /* The named cabinet has to be in what was found, or the names have led
     * somewhere else altogether. */
    if ( named.index < vols[0].index || named.index > vols[n - 1].index )
        return SZ_ERR_VOLUME;
    *count = n;
    return SZ_OK;
}

/*---- Growing tables --------------------------------------------------------*/
static int CabGrow( void **table, int *cap, int need, size_t size )
{
    void *bigger;
    int   newCap;

    if ( need <= *cap ) return 1;
    newCap = *cap ? *cap * 2 : 16;
    while ( newCap < need ) newCap *= 2;
    bigger = realloc( *table, (size_t)newCap * size );
    if ( !bigger ) return 0;
    *table = bigger;
    *cap   = newCap;
    return 1;
}

/*---- One cabinet's folders and files ---------------------------------------
 * *joinFolder comes in as the previous cabinet's last folder when that one
 * runs on into this cabinet, or -1; it goes out as the same for this
 * cabinet's last folder.  The join needs both sides to agree: the previous
 * cabinet must have a file continued TO the next, and this one a file
 * continued FROM the previous.
 *--------------------------------------------------------------------------- */
static int CabReadVolume( CabArchive *cab, int volIdx, int *joinFolder )
{
    CabVol  *vol      = &cab->vols[volIdx];
    int      startEnt = cab->numEntries;
    int     *map      = NULL;
    UInt32  *starts   = NULL;
    int      hasFromPrev = 0, hasToNext = 0;
    int      merge, j, k, out, rc = SZ_OK;
    UInt32   want;

    map    = (int *)malloc( vol->numFolders * sizeof( int ) );
    starts = (UInt32 *)malloc( ( vol->numFolders + 1 ) * sizeof( UInt32 ) );
    if ( !map || !starts ) { rc = SZ_ERR_MEMORY; goto done; }

    /* The files first: whether this cabinet's first folder continues the
     * last one is something only the files can say. */
    want = (UInt32)cab->numEntries + vol->numFiles;
    rc = ArcCheckEntryCount( want );
    if ( rc != SZ_OK ) goto done;
    if ( !CabGrow( (void **)&cab->entries, &cab->capEntries, (int)want,
                   sizeof( CabEntry ) ) )
    { rc = SZ_ERR_MEMORY; goto done; }

    if ( VolSeek( cab->fp, vol->base + (long)vol->filesAt, SEEK_SET ) != 0 )
    { rc = SZ_ERR_READ; goto done; }
    for ( k = 0; k < vol->numFiles; k++ )
    {
        CabEntry *ent = &cab->entries[cab->numEntries];
        Byte      rec[CAB_FILE_SIZE];
        char      raw[CAB_NAME_MAX];
        UInt32    iFolder, attrib;
        int       i;

        if ( VolRead( rec, 1, CAB_FILE_SIZE, cab->fp ) != CAB_FILE_SIZE )
        { rc = SZ_ERR_READ; goto done; }
        rc = CabReadString( cab->fp, raw, CAB_NAME_MAX );
        if ( rc != SZ_OK ) goto done;

        memset( ent, 0, sizeof( *ent ) );
        ent->size    = CabLe32( rec );
        ent->offset  = CabLe32( rec + 4 );
        iFolder      = CabLe16( rec + 8 );
        ent->modDate = (UInt16)CabLe16( rec + 10 );
        ent->modTime = (UInt16)CabLe16( rec + 12 );
        attrib       = CabLe16( rec + 14 );
        ent->attrib  = attrib & CAB_ATTR_SHOWN;
        ent->folder  = (int)iFolder;        /* resolved below */

        if ( attrib & CAB_ATTR_UTF8 ) CabUtf8Name( raw, ent->name, SZ_MAX_NAME );
        else lstrcpyn( ent->name, raw, SZ_MAX_NAME );
        for ( i = 0; ent->name[i]; i++ )
            if ( ent->name[i] == '/' ) ent->name[i] = '\\';

        if ( iFolder == CAB_FROM_PREV || iFolder == CAB_PREV_AND_NEXT )
            hasFromPrev = 1;
        if ( iFolder == CAB_TO_NEXT || iFolder == CAB_PREV_AND_NEXT )
            hasToNext = 1;
        cab->numEntries++;
    }

    /* The folders, joined onto the previous cabinet's last where it runs on. */
    merge = ( *joinFolder >= 0 && hasFromPrev );
    if ( !CabGrow( (void **)&cab->folders, &cab->capFolders,
                   cab->numFolders + vol->numFolders, sizeof( CabFolder ) ) ||
         !CabGrow( (void **)&cab->parts, &cab->capParts,
                   cab->numParts + vol->numFolders, sizeof( CabPart ) ) )
    { rc = SZ_ERR_MEMORY; goto done; }

    if ( VolSeek( cab->fp, vol->base + (long)vol->foldersAt, SEEK_SET ) != 0 )
    { rc = SZ_ERR_READ; goto done; }
    for ( j = 0; j < vol->numFolders; j++ )
    {
        Byte     rec[CAB_FOLDER_SIZE];
        CabPart *part = &cab->parts[cab->numParts];
        UInt16   type;

        if ( VolRead( rec, 1, CAB_FOLDER_SIZE, cab->fp ) != CAB_FOLDER_SIZE )
        { rc = SZ_ERR_READ; goto done; }
        if ( vol->folderReserve &&
             VolSeek( cab->fp, (long)vol->folderReserve, SEEK_CUR ) != 0 )
        { rc = SZ_ERR_READ; goto done; }

        part->vol       = volIdx;
        part->dataAt    = CabLe32( rec );
        part->numBlocks = (UInt16)CabLe16( rec + 4 );
        type            = (UInt16)CabLe16( rec + 6 );
        starts[j]       = part->dataAt;

        if ( j == 0 && merge )
        {
            CabFolder *fold = &cab->folders[*joinFolder];

            /* One stream, so one method: anything else is not a join. */
            if ( fold->type != type ) { rc = SZ_ERR_FORMAT; goto done; }
            fold->numParts++;
            map[j] = *joinFolder;
        }
        else
        {
            CabFolder *fold = &cab->folders[cab->numFolders];

            fold->type       = type;
            fold->firstPart  = cab->numParts;
            fold->numParts   = 1;
            fold->firstEntry = -1;
            map[j] = cab->numFolders++;
        }
        cab->numParts++;
    }

    /* Each part's compressed size, from where the next folder's data starts
     * (or the cabinet ends) less the block headers.  Folders are written one
     * after another, so this is exact; when it does not add up, unknown. */
    starts[vol->numFolders] = vol->size;
    for ( j = 0; j < vol->numFolders; j++ )
    {
        CabPart *part  = &cab->parts[cab->numParts - vol->numFolders + j];
        UInt32   end   = starts[j + 1];
        UInt32   heads = (UInt32)part->numBlocks *
                         ( CAB_DATA_SIZE + vol->dataReserve );

        if ( end > part->dataAt && end - part->dataAt >= heads )
            part->packed = end - part->dataAt - heads;
        else
            part->packed = 0xFFFFFFFFUL;
    }

    /* Resolve each file's folder; drop the files the previous cabinet has
     * already listed. */
    out = startEnt;
    for ( k = startEnt; k < cab->numEntries; k++ )
    {
        CabEntry *ent = &cab->entries[k];
        UInt32    raw = (UInt32)ent->folder;

        if ( raw == CAB_FROM_PREV || raw == CAB_PREV_AND_NEXT )
        {
            if ( merge ) continue;              /* listed before: drop */
            ent->folder = -1;                   /* its start is missing */
        }
        else if ( raw == CAB_TO_NEXT )
            ent->folder = map[vol->numFolders - 1];
        else if ( raw < vol->numFolders )
            ent->folder = map[raw];
        else
            ent->folder = -1;

        if ( out != k ) cab->entries[out] = *ent;
        out++;
    }
    cab->numEntries = out;

    *joinFolder = hasToNext ? map[vol->numFolders - 1] : -1;

done:
    if ( map )    free( map );
    if ( starts ) free( starts );
    return rc;
}

/*---- Opening ---------------------------------------------------------------*/
static int CabOpenAt( const char *path, long embed, CabArchive **out )
{
    CabArchive  *cab;
    char        *paths = NULL;
    CabVol      *vols  = NULL;
    char       **list  = NULL;
    int          count = 0, i, joinFolder = -1, rc;

    *out = NULL;
    cab = (CabArchive *)calloc( 1, sizeof( CabArchive ) );
    paths = (char *)malloc( (size_t)CAB_MAX_SET * CAB_PATH_MAX );
    vols  = (CabVol *)calloc( CAB_MAX_SET, sizeof( CabVol ) );
    list  = (char **)malloc( CAB_MAX_SET * sizeof( char * ) );
    if ( !cab || !paths || !vols || !list ) { rc = SZ_ERR_MEMORY; goto fail; }

    rc = CabFindSet( path, embed, paths, vols, &count );
    if ( rc != SZ_OK ) goto fail;

    for ( i = 0; i < count; i++ ) list[i] = paths + i * CAB_PATH_MAX;
    rc = VolOpenList( (const char *const *)list, count, &cab->fp );
    if ( rc != SZ_OK ) { rc = SZ_ERR_VOLUME; goto fail; }

    cab->vols = (CabVol *)malloc( count * sizeof( CabVol ) );
    if ( !cab->vols ) { rc = SZ_ERR_MEMORY; goto fail; }
    memcpy( cab->vols, vols, count * sizeof( CabVol ) );
    cab->numVols = count;
    for ( i = 0; i < count; i++ )
        cab->vols[i].base = VolVolumeStart( cab->fp, i ) + cab->vols[i].embed;

    for ( i = 0; i < count && rc == SZ_OK; i++ )
        rc = CabReadVolume( cab, i, &joinFolder );
    if ( rc != SZ_OK ) goto fail;

    /* Which entry reports each folder's packed size: the first one in it. */
    for ( i = 0; i < cab->numEntries; i++ )
    {
        int fold = cab->entries[i].folder;

        if ( fold >= 0 && cab->folders[fold].firstEntry < 0 )
            cab->folders[fold].firstEntry = i;
    }

    free( paths );
    free( vols );
    free( list );
    *out = cab;
    return SZ_OK;

fail:
    if ( paths ) free( paths );
    if ( vols )  free( vols );
    if ( list )  free( list );
    CabClose( cab );
    return rc;
}

int CabProbe( const unsigned char *sig, int len )
{
    return len >= 8 && memcmp( sig, "MSCF", 4 ) == 0 &&
           sig[4] == 0 && sig[5] == 0 && sig[6] == 0 && sig[7] == 0;
}

int CabOpen( const char *path, CabArchive **out )
{
    return CabOpenAt( path, 0L, out );
}

/* A cabinet somewhere inside the file: IExpress packages and Microsoft's
 * self-extracting updates keep theirs in the .exe's resources.  Each "MSCF"
 * found has to pass the strict header checks and then open properly; the
 * scan carries on past any that do not. */
int CabOpenScan( const char *path, CabArchive **out )
{
    VolFile *fp;
    Byte    *buf;
    long     len, base;
    int      rc = SZ_ERR_SIG;

    *out = NULL;
    if ( VolOpen( path, &fp ) != SZ_OK ) return SZ_ERR_OPEN;
    len = VolSize( fp );
    buf = (Byte *)malloc( CAB_SCAN_CHUNK + CAB_HDR_SIZE );
    if ( !buf ) { VolClose( fp ); return SZ_ERR_MEMORY; }

    for ( base = 0; base < len && rc != SZ_OK; base += CAB_SCAN_CHUNK )
    {
        long want = len - base;
        long got, i;

        if ( want > CAB_SCAN_CHUNK + CAB_HDR_SIZE )
            want = CAB_SCAN_CHUNK + CAB_HDR_SIZE;
        if ( VolSeek( fp, base, SEEK_SET ) != 0 ) break;
        got = (long)VolRead( buf, 1, (UInt32)want, fp );

        for ( i = 0; i + CAB_HDR_SIZE <= got && i < CAB_SCAN_CHUNK; i++ )
        {
            CabVol vol;

            if ( buf[i] != 'M' || memcmp( buf + i, "MSCF", 4 ) != 0 )
                continue;
            if ( CabReadHeader( fp, base + i, len - ( base + i ), 1,
                                &vol, NULL, NULL ) != SZ_OK )
                continue;
            if ( CabOpenAt( path, base + i, out ) == SZ_OK )
            {
                rc = SZ_OK;
                break;
            }
        }
    }

    free( buf );
    VolClose( fp );
    return rc;
}

int CabNumEntries( CabArchive *cab )
{
    return cab ? cab->numEntries : 0;
}

const CabEntry *CabGetEntry( CabArchive *cab, int index )
{
    if ( !cab || index < 0 || index >= cab->numEntries ) return NULL;
    return &cab->entries[index];
}

UInt32 CabEntryPacked( CabArchive *cab, int index )
{
    const CabFolder *fold;
    UInt32           sum = 0;
    int              p;

    if ( !cab || index < 0 || index >= cab->numEntries ) return 0xFFFFFFFFUL;
    if ( cab->entries[index].folder < 0 ) return 0xFFFFFFFFUL;
    fold = &cab->folders[cab->entries[index].folder];
    if ( fold->firstEntry != index ) return 0xFFFFFFFFUL;

    for ( p = 0; p < fold->numParts; p++ )
    {
        UInt32 part = cab->parts[fold->firstPart + p].packed;

        if ( part == 0xFFFFFFFFUL ) return 0xFFFFFFFFUL;
        sum += part;
    }
    return sum;
}

const char *CabEntryMethod( CabArchive *cab, int index )
{
    int fold;

    if ( !cab || index < 0 || index >= cab->numEntries ) return "";
    fold = cab->entries[index].folder;
    if ( fold < 0 ) return "";
    switch ( cab->folders[fold].type & CAB_COMP_MASK )
    {
    case CAB_COMP_NONE:    return "Store";
    case CAB_COMP_MSZIP:   return "MSZIP";
    case CAB_COMP_QUANTUM: return "Quantum";
    case CAB_COMP_LZX:     return "LZX";
    default:               return "?";
    }
}

int CabVolumeCount( CabArchive *cab )
{
    return cab ? cab->numVols : 1;
}

void CabChecksumCount( CabArchive *cab, UInt32 *checked, UInt32 *bare )
{
    if ( checked ) *checked = cab ? cab->sumBlocks : 0;
    if ( bare )    *bare    = cab ? cab->bareBlocks : 0;
}

/*---- Reading a folder ------------------------------------------------------
 * A forward-only stream of the folder's output, a block at a time.  Going
 * backwards means starting again from the folder's first block - which only
 * an archive whose files overlap, or are listed out of order, ever asks for.
 *--------------------------------------------------------------------------- */
typedef struct {
    CabArchive *cab;
    int         folder;
    int         method;
    int         part;           /* which of the folder's parts             */
    UInt32      block;          /* blocks of it already read               */
    long        nextAt;         /* the next CFDATA, in the joined stream   */
    MszipDec   *mszip;
    LzxDec     *lzx;
    QtmDec     *qtm;
    Byte       *inBuf;
    Byte       *frame;
    UInt32      frameLen, frameUsed;
    UInt32      pos;            /* folder offset of frame[frameUsed]       */
} CabReader;

static void CrClose( CabReader *rd )
{
    MszipFree( rd->mszip );
    LzxFree( rd->lzx );
    QtmFree( rd->qtm );
    if ( rd->inBuf ) free( rd->inBuf );
    if ( rd->frame ) free( rd->frame );
    memset( rd, 0, sizeof( *rd ) );
    rd->folder = -1;
}

static int CrOpen( CabArchive *cab, int folder, CabReader *rd )
{
    CabFolder *fold = &cab->folders[folder];
    CabPart   *part = &cab->parts[fold->firstPart];
    int        rc   = SZ_OK;

    memset( rd, 0, sizeof( *rd ) );
    rd->cab    = cab;
    rd->folder = folder;
    rd->method = fold->type & CAB_COMP_MASK;
    rd->nextAt = cab->vols[part->vol].base + (long)part->dataAt;

    switch ( rd->method )
    {
    case CAB_COMP_NONE:    break;
    case CAB_COMP_MSZIP:   rc = MszipCreate( &rd->mszip ); break;
    case CAB_COMP_QUANTUM: rc = QtmCreate( CAB_WINDOW( fold->type ), &rd->qtm ); break;
    case CAB_COMP_LZX:     rc = LzxCreate( CAB_WINDOW( fold->type ), &rd->lzx ); break;
    default:               rc = SZ_ERR_UNSUPPORTED; break;
    }
    if ( rc == SZ_OK )
    {
        rd->inBuf = (Byte *)malloc( CAB_BLOCK_IN );
        rd->frame = (Byte *)malloc( CAB_BLOCK_OUT );
        if ( !rd->inBuf || !rd->frame ) rc = SZ_ERR_MEMORY;
    }
    if ( rc != SZ_OK ) CrClose( rd );
    return rc;
}

/* The next block's output into rd->frame.  A block split across a join comes
 * as two records, the first declaring no output; their data is joined before
 * it is decoded. */
static int CrNextBlock( CabReader *rd )
{
    CabFolder *fold = &rd->cab->folders[rd->folder];
    UInt32     have = 0, outLen = 0;
    int        rc;

    for ( ;; )
    {
        CabPart *part;
        CabVol  *vol;
        Byte     hdr[CAB_DATA_SIZE];
        UInt32   csum, cbData;

        if ( rd->part >= fold->numParts ) return SZ_ERR_DATA;   /* ran out */
        part = &rd->cab->parts[fold->firstPart + rd->part];
        if ( rd->block >= part->numBlocks )
        {
            if ( ++rd->part >= fold->numParts ) return SZ_ERR_DATA;
            part = &rd->cab->parts[fold->firstPart + rd->part];
            rd->block  = 0;
            rd->nextAt = rd->cab->vols[part->vol].base + (long)part->dataAt;
            continue;
        }
        vol = &rd->cab->vols[part->vol];

        if ( VolSeek( rd->cab->fp, rd->nextAt, SEEK_SET ) != 0 ||
             VolRead( hdr, 1, CAB_DATA_SIZE, rd->cab->fp ) != CAB_DATA_SIZE )
            return SZ_ERR_READ;
        csum   = CabLe32( hdr );
        cbData = CabLe16( hdr + 4 );
        outLen = CabLe16( hdr + 6 );
        if ( have + cbData > CAB_BLOCK_IN || outLen > CAB_BLOCK_OUT )
            return SZ_ERR_DATA;

        if ( VolSeek( rd->cab->fp,
                      rd->nextAt + CAB_DATA_SIZE + vol->dataReserve,
                      SEEK_SET ) != 0 ||
             VolRead( rd->inBuf + have, 1, cbData, rd->cab->fp ) != cbData )
            return SZ_ERR_READ;

        if ( csum == 0 )
            rd->cab->bareBlocks++;          /* the writer chose not to */
        else if ( CabChecksum( hdr + 4, 4,
                               CabChecksum( rd->inBuf + have, cbData, 0 ) ) != csum )
            return SZ_ERR_CRC;
        else
            rd->cab->sumBlocks++;

        have       += cbData;
        rd->nextAt += CAB_DATA_SIZE + vol->dataReserve + (long)cbData;
        rd->block++;
        if ( outLen != 0 ) break;      /* 0: the rest is in the next cabinet */
    }

    switch ( rd->method )
    {
    case CAB_COMP_NONE:
        if ( have != outLen ) return SZ_ERR_DATA;
        memcpy( rd->frame, rd->inBuf, outLen );
        rc = SZ_OK;
        break;
    case CAB_COMP_MSZIP:
    {
        UInt32 got;

        rc = MszipDecodeBlock( rd->mszip, rd->inBuf, have, rd->frame,
                               CAB_BLOCK_OUT, &got, NULL );
        if ( rc == SZ_OK && got != outLen ) rc = SZ_ERR_DATA;
        break;
    }
    case CAB_COMP_QUANTUM:
        rc = QtmDecodeFrame( rd->qtm, rd->inBuf, have, rd->frame, outLen );
        break;
    default:
        rc = LzxDecodeFrame( rd->lzx, rd->inBuf, have, rd->frame, outLen );
        break;
    }
    if ( rc != SZ_OK ) return rc;

    rd->frameLen  = outLen;
    rd->frameUsed = 0;
    return SZ_OK;
}

/* The next n bytes of the folder: to 'out', or nowhere when it is NULL. */
static int CrCopy( CabReader *rd, UInt32 n, FILE *out )
{
    while ( n > 0 )
    {
        UInt32 take;

        if ( rd->frameUsed == rd->frameLen )
        {
            int rc = CrNextBlock( rd );

            if ( rc != SZ_OK ) return rc;
            continue;
        }
        take = rd->frameLen - rd->frameUsed;
        if ( take > n ) take = n;
        if ( out && fwrite( rd->frame + rd->frameUsed, 1, take, out ) != take )
            return SZ_ERR_WRITE;
        rd->frameUsed += take;
        rd->pos       += take;
        n             -= take;
    }
    return SZ_OK;
}

/*---- Paths -----------------------------------------------------------------*/

/* Create the folders leading to 'path' (not 'path' itself). */
static void MakeDirs( const char *path )
{
    char  buf[SZ_MAX_NAME * 4];
    char *scan;

    lstrcpyn( buf, path, sizeof( buf ) );
    scan = buf;
    if ( scan[0] && scan[1] == ':' ) scan += 2;
    if ( *scan == '\\' ) scan++;
    for ( ; *scan; scan++ )
        if ( *scan == '\\' )
        {
            *scan = '\0';
            _mkdir( buf );
            *scan = '\\';
        }
}

/* destDir\name, the name made safe for the target (see ArcFsName). */
static void BuildOut( char *dst, int dstSize,
                      const char *destDir, const char *name )
{
    char        fsname[SZ_MAX_NAME];
    const char *src = fsname;
    int         len = 0;

    ArcFsName( fsname, sizeof( fsname ), name, 0 );
    if ( destDir && destDir[0] )
    {
        while ( destDir[len] && len < dstSize - 2 )
        {
            dst[len] = destDir[len];
            len++;
        }
        /* "C:\" and "C:/" already end in one; doubling it makes a path DOS
         * reads as a network name (see BuildPath in SZARC.C). */
        if ( len > 0 && dst[len - 1] != '\\' && dst[len - 1] != '/' )
            dst[len++] = '\\';
    }
    while ( *src && len < dstSize - 1 ) dst[len++] = *src++;
    dst[len] = '\0';
}

/*---- Extraction ------------------------------------------------------------*/

/* One file, from wherever the reader is.  Everything before the file's start
 * is decoded first, so a fault in the part of the folder in front of it
 * leaves no empty file behind. */
static int CabExtractEntry( CabReader *rd, const CabEntry *ent,
                            const char *destDir )
{
    char  outPath[SZ_MAX_NAME * 4];
    FILE *out = NULL;
    int   rc;

    if ( destDir )
    {
        BuildOut( outPath, sizeof( outPath ), destDir, ent->name );
        if ( ArcNameVerdict() == ARC_NAME_ABORT ) return SZ_ERR_CANCEL;
        if ( ArcNameVerdict() == ARC_NAME_SKIP )  return SZ_OK;
        if ( !ArcWantWrite( outPath ) )           return SZ_OK;
    }

    if ( ent->size > 0 )
    {
        if ( ent->offset < rd->pos )
        {
            CabArchive *cab    = rd->cab;
            int         folder = rd->folder;

            CrClose( rd );                 /* clears rd, cab included */
            rc = CrOpen( cab, folder, rd );
            if ( rc != SZ_OK ) return rc;
        }
        rc = CrCopy( rd, ent->offset - rd->pos, NULL );
        if ( rc != SZ_OK ) return rc;
    }

    if ( destDir )
    {
        MakeDirs( outPath );
        out = fopen( outPath, "wb" );
        if ( !out ) return SZ_ERR_WRITE;
    }

    rc = ( ent->size > 0 ) ? CrCopy( rd, ent->size, out ) : SZ_OK;
    if ( out ) fclose( out );

    if ( rc == SZ_OK )
    {
        if ( destDir )
        {
            SetFileDosMTime( outPath, ent->modDate, ent->modTime );
            SetFileDosAttr( outPath, ent->attrib, 0 );
        }
    }
    else if ( destDir )
        remove( outPath );             /* don't leave a partial file */
    return rc;
}

typedef struct {
    int    folder;
    UInt32 offset;
    int    index;
} CabOrder;

static int CabOrderCmp( const void *left, const void *right )
{
    const CabOrder *one = (const CabOrder *)left;
    const CabOrder *two = (const CabOrder *)right;

    if ( one->folder != two->folder ) return ( one->folder < two->folder ) ? -1 : 1;
    if ( one->offset != two->offset ) return ( one->offset < two->offset ) ? -1 : 1;
    return ( one->index < two->index ) ? -1 : ( one->index > two->index );
}

/* Extract (or with destDir NULL, test) the entries flagged in 'want', NULL
 * meaning all.  A folder with a method this cannot decode, or too big a
 * window for the memory there is, costs its own files and nothing more: the
 * rest still extract and the reason comes back at the end, as with 7z. */
static int CabExtractSet( CabArchive *cab, const Byte *want,
                          const char *destDir, SzProgress prog, void *user )
{
    CabOrder *order;
    CabReader rd;
    int       count = 0, i, rc = SZ_OK, skipped = SZ_OK, badFolder = -1;

    cab->sumBlocks  = 0;
    cab->bareBlocks = 0;
    order = (CabOrder *)malloc( ( cab->numEntries ? cab->numEntries : 1 ) *
                                sizeof( CabOrder ) );
    if ( !order ) return SZ_ERR_MEMORY;
    for ( i = 0; i < cab->numEntries; i++ )
    {
        if ( want && !want[i] ) continue;
        if ( !cab->entries[i].name[0] ) continue;
        order[count].folder = cab->entries[i].folder;
        order[count].offset = cab->entries[i].offset;
        order[count].index  = i;
        count++;
    }
    qsort( order, count, sizeof( CabOrder ), CabOrderCmp );

    memset( &rd, 0, sizeof( rd ) );
    rd.folder = -1;

    for ( i = 0; i < count && rc == SZ_OK; i++ )
    {
        const CabEntry *ent = &cab->entries[order[i].index];

        if ( ent->size > 0 )
        {
            /* No folder it could be in: a bad index, or "continued from the
             * previous cabinet" in a cabinet that has no previous one.  The
             * set itself was checked complete at open, so this is damage. */
            if ( ent->folder < 0 ) { rc = SZ_ERR_FORMAT; break; }
            if ( ent->folder == badFolder ) continue;
            if ( ent->folder != rd.folder )
            {
                int orc;

                CrClose( &rd );
                orc = CrOpen( cab, ent->folder, &rd );
                if ( orc == SZ_ERR_UNSUPPORTED || orc == SZ_ERR_NORAM ||
                     orc == SZ_ERR_MEMORY )
                {
                    if ( skipped == SZ_OK ) skipped = orc;
                    badFolder = ent->folder;
                    continue;
                }
                if ( orc != SZ_OK ) { rc = orc; break; }
            }
        }

        if ( prog && !prog( user, order[i].index, cab->numEntries, ent->name ) )
        {
            rc = SZ_ERR_CANCEL;
            break;
        }
        rc = CabExtractEntry( &rd, ent, destDir );
    }

    CrClose( &rd );
    free( order );
    return ( rc != SZ_OK ) ? rc : skipped;
}

int CabExtractAll( CabArchive *cab, const char *destDir,
                   SzProgress prog, void *user )
{
    if ( !cab ) return SZ_ERR_FORMAT;
    return CabExtractSet( cab, NULL, destDir, prog, user );
}

int CabExtractItems( CabArchive *cab, const int *indices, int count,
                     const char *destDir, SzProgress prog, void *user )
{
    Byte *want;
    int   k, rc;

    if ( !cab ) return SZ_ERR_FORMAT;
    if ( count <= 0 ) return SZ_OK;
    want = (Byte *)calloc( cab->numEntries ? cab->numEntries : 1, 1 );
    if ( !want ) return SZ_ERR_MEMORY;
    for ( k = 0; k < count; k++ )
        if ( indices[k] >= 0 && indices[k] < cab->numEntries )
            want[indices[k]] = 1;
    rc = CabExtractSet( cab, want, destDir, prog, user );
    free( want );
    return rc;
}

/*---- Memory ----------------------------------------------------------------
 * One folder is open at a time, so the cost is the most expensive folder's
 * decoder - its window, which for LZX and Quantum is whatever the cabinet
 * declared, up to 2 MB - plus a block in and a block out, plus the order
 * table and selection flags CabExtractSet keeps per entry.  The last is
 * small but not nothing: a tracking allocator caught this figure 2 KB short
 * on a 159-file cabinet before it was counted.
 *--------------------------------------------------------------------------- */
UInt32 CabMemNeeded( CabArchive *cab )
{
    UInt32 most = 0, perEntry = 0;
    int    i;

    if ( cab )
        perEntry = (UInt32)cab->numEntries * ( (UInt32)sizeof( CabOrder ) + 1 );

    for ( i = 0; cab && i < cab->numFolders; i++ )
    {
        UInt16 type = cab->folders[i].type;
        UInt32 cost = 0;

        switch ( type & CAB_COMP_MASK )
        {
        case CAB_COMP_MSZIP:   cost = MszipMemNeeded(); break;
        case CAB_COMP_QUANTUM: cost = QtmMemNeeded( CAB_WINDOW( type ) ); break;
        case CAB_COMP_LZX:     cost = LzxMemNeeded( CAB_WINDOW( type ) ); break;
        }
        if ( cost > most ) most = cost;
    }
    return most + CAB_BLOCK_IN + CAB_BLOCK_OUT + (UInt32)sizeof( CabReader ) +
           perEntry;
}

void CabClose( CabArchive *cab )
{
    if ( !cab ) return;
    if ( cab->fp )      VolClose( cab->fp );
    if ( cab->vols )    free( cab->vols );
    if ( cab->folders ) free( cab->folders );
    if ( cab->parts )   free( cab->parts );
    if ( cab->entries ) free( cab->entries );
    free( cab );
}

/*---- Deleting entries ------------------------------------------------------
 * A whole new cabinet is written (ARCFILE.C owns the temporary file and the
 * swap).  Each folder goes one of three ways:
 *
 *   every file kept   its CFDATA records are copied byte for byte, checksums
 *                     and reserved bytes included, and its files keep their
 *                     offsets
 *   no file kept      dropped, data and all
 *   some kept         decoded from its first block, and the kept files'
 *                     bytes - end to end, in the order they had - compressed
 *                     again as MSZIP in blocks of 32 KB.  A stored folder
 *                     stays stored.
 *
 * MSZIP because it is the one cabinet method this program can write, and
 * every reader has it.  Each new block is a deflate stream on its own, with
 * nothing referring back into the block before.  The format lets a block
 * lean on the previous one's output without requiring it, and DEFLENC starts
 * every stream with an empty window.  The cost: a folder packed again comes
 * out about 2% larger than makecab's MSZIP of the same files.
 *
 * A folder that keeps only empty files has no bytes left, and is written
 * with no CFDATA at all - which is what makecab does with one.
 *
 * The header's reserved area is dropped.  What goes there in practice is
 * Authenticode's pointer to a digital signature, and once a file has gone
 * the signature matches nothing: kept, it would make the new cabinet look
 * tampered with instead of simply unsigned.  The per-folder and per-block
 * reserve SIZES stay, because every CFFOLDER and CFDATA record is laid out
 * with them; copied records bring their reserved bytes along, and new ones
 * get zeros.  So the reserve flag stays only while one of those two is not
 * 0, with the header's own reserve written as 0 bytes.
 *
 * Refused: a cabinet inside a program (an IExpress package or an update -
 * the cabinet is a resource of the .exe, and rewriting the .exe is another
 * job), a cabinet that is one of a set, and a selection that would leave no
 * file at all.  Microsoft's FCI never writes a cabinet with no folder or no
 * file, and this program's own reader refuses one (see CabReadHeader).
 *--------------------------------------------------------------------------- */

#define CAB_PACK_CAP    ( CAB_BLOCK_OUT + 16 )      /* "CK" + deflate, stored at worst */
#define CAB_REC_CAP     ( CAB_FILE_SIZE + 1024 )    /* one CFFILE: the spec allows a
                                                       256-byte name; this is generous */
#define CAB_SIZE_MAX    0x7FFFFFFFUL                /* cbCabinet, as far as a long seeks */
#define CAB_MAX_BLOCKS  65535UL                     /* cCFData is 16 bits              */

#define CD_DROP         0
#define CD_COPY         1
#define CD_RECODE       2

static const char cdWhySet[] =
    "This cabinet is one of a set that runs across several files.  Files "
    "cannot be deleted from a cabinet set.";
static const char cdWhyExe[] =
    "This cabinet is stored inside a program file, such as a setup package.  "
    "Files cannot be deleted from a cabinet inside a program.";
static const char cdWhyDamaged[] =
    "A file in this cabinet's list belongs to a folder the cabinet does not "
    "have.  The cabinet may be damaged.";
static const char cdWhyAll[] =
    "A cabinet must hold at least one file.  To remove every file, delete "
    "the cabinet itself.";
static const char cdWhyMethod[] =
    "A folder in this cabinet holds files being kept and files being "
    "deleted, and that folder uses a compression method this program "
    "cannot read.";
static const char cdWhyBig[] =
    "The new cabinet would be larger than 2 GB, the most a cabinet can hold.";
static const char cdWhyBlocks[] =
    "The files kept from one folder would need more data blocks than a "
    "cabinet folder can hold.";

/* What happens to each of the old cabinet's folders. */
typedef struct {
    int    how;                 /* CD_DROP, CD_COPY or CD_RECODE           */
    int    newIndex;            /* its number in the new cabinet           */
    UInt32 kept, gone;          /* its files kept and deleted              */
    UInt32 extent;              /* bytes to decode: the furthest kept end  */
    UInt32 dataAt;              /* the new coffCabStart                    */
    UInt32 blocks;              /* the new cCFData                         */
    UInt16 type;                /* the new typeCompress                    */
} CdFold;

/* The new cabinet's data area as it is written, and the progress report. */
typedef struct {
    FILE           *out;
    UInt32          at;             /* bytes written so far                */
    int             dataReserve;
    int             stored;         /* this folder's blocks are not packed  */
    UInt32          blocks;         /* CFDATA records in this folder        */
    UInt32          folderPos;      /* its uncompressed bytes so far        */
    DeflEnc        *defl;
    Byte           *raw;            /* the block being gathered            */
    UInt32          rawLen;
    Byte           *pack;           /* and that block packed               */
    UInt32          packLen;
    Byte           *copy;           /* one CFDATA record, copied whole      */
    const char     *why;
    ArcCompProgress prog;
    void           *user;
    int             fileIndex, fileCount;
    const char     *name;
    UInt32          doneKB, doneFrac, totalKB;
} CabWriter;

static void CabPut16( Byte *ptr, UInt32 val )
{
    ptr[0] = (Byte)val;
    ptr[1] = (Byte)( val >> 8 );
}

static void CabPut32( Byte *ptr, UInt32 val )
{
    CabPut16( ptr, val );
    CabPut16( ptr + 2, val >> 16 );
}

/* Kilobytes, rounded up, without overflowing near 4 GB. */
static UInt32 CdKB( UInt32 bytes )
{
    return ( bytes >> 10 ) + ( ( bytes & 1023 ) ? 1 : 0 );
}

/*---- Progress --------------------------------------------------------------*/
static int CwTell( CabWriter *cw )
{
    int index = cw->fileIndex;

    if ( !cw->prog ) return SZ_OK;
    if ( index >= cw->fileCount ) index = cw->fileCount - 1;
    if ( !cw->prog( cw->user, index, cw->fileCount, cw->name,
                    cw->doneKB, cw->totalKB ) )
        return SZ_ERR_CANCEL;
    return SZ_OK;
}

/* 'bytes' more of the work done.  Files that overlap make the decoder go
 * over part of a folder twice, so the count is held at the total. */
static int CwWork( CabWriter *cw, UInt32 bytes )
{
    cw->doneFrac += bytes & 1023;
    cw->doneKB   += ( bytes >> 10 ) + ( cw->doneFrac >> 10 );
    cw->doneFrac &= 1023;
    if ( cw->doneKB > cw->totalKB ) cw->doneKB = cw->totalKB;
    return CwTell( cw );
}

/*---- Writing the data area -------------------------------------------------*/
static int CwWrite( CabWriter *cw, const void *data, UInt32 len )
{
    if ( len > CAB_SIZE_MAX - cw->at )
    {
        cw->why = cdWhyBig;
        return SZ_ERR_UNSUPPORTED;
    }
    if ( len && fwrite( data, 1, (size_t)len, cw->out ) != (size_t)len )
        return SZ_ERR_WRITE;
    cw->at += len;
    return SZ_OK;
}

/* DEFLENC's output, into the block buffer.  Refusing when it is full makes
 * the encoder fail, and the block is then sent stored instead. */
static int CwPackWrite( void *user, const Byte *data, UInt32 len )
{
    CabWriter *cw = (CabWriter *)user;

    if ( len > CAB_PACK_CAP - cw->packLen ) return 0;
    memcpy( cw->pack + cw->packLen, data, len );
    cw->packLen += len;
    return 1;
}

/* The gathered block out as one CFDATA record.  The checksum is the one
 * CrNextBlock verifies: the data, then the two size fields. */
static int CwFlush( CabWriter *cw )
{
    static const Byte zeros[256];
    Byte        hdr[CAB_DATA_SIZE];
    const Byte *data = cw->raw;
    UInt32      len  = cw->rawLen;
    int         rc;

    if ( cw->rawLen == 0 ) return SZ_OK;
    if ( cw->blocks >= CAB_MAX_BLOCKS )
    {
        cw->why = cdWhyBlocks;
        return SZ_ERR_UNSUPPORTED;
    }

    if ( !cw->stored )
    {
        cw->pack[0] = 'C';
        cw->pack[1] = 'K';
        cw->packLen = 2;
        DeflReset( cw->defl, CwPackWrite, cw );
        rc = DeflFeed( cw->defl, cw->raw, cw->rawLen );
        if ( rc == SZ_OK ) rc = DeflFinish( cw->defl );

        /* Data that will not compress: one stored deflate block, final
         * (bit 0) and type 00, padded to the byte, then LEN and NLEN. */
        if ( rc != SZ_OK || cw->packLen > cw->rawLen + 7 )
        {
            cw->pack[2] = 0x01;
            CabPut16( cw->pack + 3, cw->rawLen );
            CabPut16( cw->pack + 5, ~cw->rawLen & 0xFFFFUL );
            memcpy( cw->pack + 7, cw->raw, cw->rawLen );
            cw->packLen = cw->rawLen + 7;
        }
        data = cw->pack;
        len  = cw->packLen;
    }

    CabPut16( hdr + 4, len );
    CabPut16( hdr + 6, cw->rawLen );
    CabPut32( hdr, CabChecksum( hdr + 4, 4, CabChecksum( data, len, 0 ) ) );

    rc = CwWrite( cw, hdr, CAB_DATA_SIZE );
    if ( rc == SZ_OK && cw->dataReserve )
        rc = CwWrite( cw, zeros, (UInt32)cw->dataReserve );
    if ( rc == SZ_OK ) rc = CwWrite( cw, data, len );
    if ( rc != SZ_OK ) return rc;

    cw->blocks++;
    cw->rawLen = 0;
    return SZ_OK;
}

/* Kept bytes into the block being gathered. */
static int CwPut( CabWriter *cw, const Byte *data, UInt32 len )
{
    while ( len > 0 )
    {
        UInt32 take = CAB_BLOCK_OUT - cw->rawLen;

        if ( take > len ) take = len;
        memcpy( cw->raw + cw->rawLen, data, take );
        cw->rawLen    += take;
        cw->folderPos += take;
        data          += take;
        len           -= take;
        if ( cw->rawLen == CAB_BLOCK_OUT )
        {
            int rc = CwFlush( cw );

            if ( rc != SZ_OK ) return rc;
        }
    }
    return SZ_OK;
}

/*---- The folders -----------------------------------------------------------*/

/* A folder whose files all stay: its records as they are.  A single cabinet
 * holds the whole of each folder, so there is one part. */
static int CdCopyFolder( CabArchive *cab, const CabFolder *fold, CabWriter *cw )
{
    const CabPart *part = &cab->parts[fold->firstPart];
    const CabVol  *vol  = &cab->vols[part->vol];
    long           at   = vol->base + (long)part->dataAt;
    UInt32         k;
    int            rc;

    for ( k = 0; k < part->numBlocks; k++ )
    {
        UInt32 cbData, rest;

        if ( VolSeek( cab->fp, at, SEEK_SET ) != 0 ||
             VolRead( cw->copy, 1, CAB_DATA_SIZE, cab->fp ) != CAB_DATA_SIZE )
            return SZ_ERR_READ;
        cbData = CabLe16( cw->copy + 4 );

        /* 8 + 255 + 65535 at most, inside CAB_BLOCK_IN. */
        rest = (UInt32)vol->dataReserve + cbData;
        if ( rest && VolRead( cw->copy + CAB_DATA_SIZE, 1, rest, cab->fp ) != rest )
            return SZ_ERR_READ;

        rc = CwWrite( cw, cw->copy, CAB_DATA_SIZE + rest );
        if ( rc == SZ_OK ) rc = CwWork( cw, cbData );
        if ( rc != SZ_OK ) return rc;
        at += (long)( CAB_DATA_SIZE + rest );
    }
    cw->blocks = part->numBlocks;
    return SZ_OK;
}

/* The next 'want' bytes of the folder: into the new block when 'keep' is
 * set, passed over when it is not.  CrCopy, except that the bytes go to the
 * writer and every decoded block counts as progress. */
static int CdPass( CabReader *rd, CabWriter *cw, UInt32 want, int keep )
{
    while ( want > 0 )
    {
        UInt32 take;
        int    rc;

        if ( rd->frameUsed == rd->frameLen )
        {
            rc = CrNextBlock( rd );
            if ( rc == SZ_OK ) rc = CwWork( cw, rd->frameLen );
            if ( rc != SZ_OK ) return rc;
            continue;
        }
        take = rd->frameLen - rd->frameUsed;
        if ( take > want ) take = want;
        if ( keep )
        {
            rc = CwPut( cw, rd->frame + rd->frameUsed, take );
            if ( rc != SZ_OK ) return rc;
        }
        rd->frameUsed += take;
        rd->pos       += take;
        want          -= take;
    }
    return SZ_OK;
}

/* A folder that keeps some of its files.  'order' is those files, sorted by
 * offset; each one's new offset goes into newOff[].  Decoding stops at the
 * end of the last one kept - whatever follows it is not needed. */
static int CdRecodeFolder( CabArchive *cab, int folder, const CabOrder *order,
                           int count, UInt32 *newOff, CabWriter *cw )
{
    CabReader rd;
    int       i, rc;

    cw->blocks    = 0;
    cw->folderPos = 0;
    cw->rawLen    = 0;
    cw->stored    = ( ( cab->folders[folder].type & CAB_COMP_MASK ) ==
                      CAB_COMP_NONE );

    rc = CrOpen( cab, folder, &rd );
    if ( rc != SZ_OK ) return rc;

    for ( i = 0; i < count && rc == SZ_OK; i++ )
    {
        const CabEntry *ent = &cab->entries[order[i].index];

        newOff[order[i].index] = cw->folderPos;
        cw->name = ent->name;
        rc = CwTell( cw );
        if ( rc == SZ_OK && ent->size > 0 )
        {
            /* Files that share bytes: back to the folder's start, as
             * CabExtractEntry does.  Each gets its own copy. */
            if ( ent->offset < rd.pos )
            {
                CrClose( &rd );
                rc = CrOpen( cab, folder, &rd );
            }
            if ( rc == SZ_OK ) rc = CdPass( &rd, cw, ent->offset - rd.pos, 0 );
            if ( rc == SZ_OK ) rc = CdPass( &rd, cw, ent->size, 1 );
        }
        cw->fileIndex++;
    }
    if ( rc == SZ_OK ) rc = CwFlush( cw );
    CrClose( &rd );
    return rc;
}

/*---- The CFFILE table ------------------------------------------------------
 * Read again from the cabinet rather than rebuilt from the entries, because
 * an entry's name has been through the code page and had its slashes turned
 * round; the new cabinet gets the bytes the old one had.  With 'out' NULL,
 * only measured.
 *--------------------------------------------------------------------------- */
static int CdFiles( CabArchive *cab, const Byte *del, const CdFold *folds,
                    const UInt32 *newOff, Byte *rec, FILE *out, UInt32 *bytes )
{
    const CabVol *vol = &cab->vols[0];
    int           k;

    *bytes = 0;
    if ( VolSeek( cab->fp, vol->base + (long)vol->filesAt, SEEK_SET ) != 0 )
        return SZ_ERR_READ;

    for ( k = 0; k < vol->numFiles; k++ )
    {
        UInt32 len = CAB_FILE_SIZE;
        int    ch;

        if ( VolRead( rec, 1, CAB_FILE_SIZE, cab->fp ) != CAB_FILE_SIZE )
            return SZ_ERR_READ;
        do
        {
            ch = VolGetc( cab->fp );
            if ( ch == EOF ) return SZ_ERR_READ;
            if ( len >= CAB_REC_CAP ) return SZ_ERR_DATA;
            rec[len++] = (Byte)ch;
        } while ( ch != 0 );

        if ( del[k] ) continue;
        if ( out )
        {
            const CdFold *fold = &folds[cab->entries[k].folder];

            if ( fold->how == CD_RECODE ) CabPut32( rec + 4, newOff[k] );
            CabPut16( rec + 8, (UInt32)fold->newIndex );
            if ( fwrite( rec, 1, (size_t)len, out ) != (size_t)len )
                return SZ_ERR_WRITE;
        }
        *bytes += len;
    }
    return SZ_OK;
}

/*---- The checks ------------------------------------------------------------*/

/* Can this cabinet lose files at all? */
static int CdCanDelete( CabArchive *cab, const char **why )
{
    int i;

    if ( !cab ) return SZ_ERR_FORMAT;
    if ( cab->numVols != 1 ||
         ( cab->vols[0].flags & ( CAB_FLAG_PREV | CAB_FLAG_NEXT ) ) )
    {
        *why = cdWhySet;
        return SZ_ERR_UNSUPPORTED;
    }
    /* Only CabOpenScan opens a cabinet anywhere but at the file's start. */
    if ( cab->vols[0].embed != 0 )
    {
        *why = cdWhyExe;
        return SZ_ERR_UNSUPPORTED;
    }
    /* One cabinet: every CFFILE record is an entry, in order, and each
     * names one of the cabinet's own folders. */
    if ( cab->numEntries != cab->vols[0].numFiles )
    {
        *why = cdWhyDamaged;
        return SZ_ERR_UNSUPPORTED;
    }
    for ( i = 0; i < cab->numEntries; i++ )
        if ( cab->entries[i].folder < 0 ||
             cab->entries[i].folder >= cab->numFolders )
        {
            *why = cdWhyDamaged;
            return SZ_ERR_UNSUPPORTED;
        }
    return SZ_OK;
}

/* Can CabReader decode this method, at this window? */
static int CdCanDecode( UInt16 type )
{
    int bits = CAB_WINDOW( type );

    switch ( type & CAB_COMP_MASK )
    {
    case CAB_COMP_NONE:
    case CAB_COMP_MSZIP:   return 1;
    case CAB_COMP_QUANTUM: return bits >= QTM_MIN_WINDOW && bits <= QTM_MAX_WINDOW;
    case CAB_COMP_LZX:     return bits >= LZX_MIN_WINDOW && bits <= LZX_MAX_WINDOW;
    }
    return 0;
}

/* Decide each folder's fate for this selection, numbering the ones that
 * stay.  *keptOut is the files left. */
static int CdPlan( CabArchive *cab, const Byte *del, CdFold *folds,
                   int *keptOut, int *foldersOut, const char **why )
{
    int kept = 0, numNew = 0, i, j;

    memset( folds, 0, (size_t)cab->numFolders * sizeof( CdFold ) );
    for ( i = 0; i < cab->numEntries; i++ )
    {
        const CabEntry *ent  = &cab->entries[i];
        CdFold         *fold = &folds[ent->folder];

        if ( del[i] )
        {
            fold->gone++;
            continue;
        }
        fold->kept++;
        kept++;
        if ( ent->size > 0 )
        {
            UInt32 end = ent->offset + ent->size;

            if ( end < ent->offset ) end = 0xFFFFFFFFUL;  /* for progress only */
            if ( end > fold->extent ) fold->extent = end;
        }
    }
    if ( kept == 0 )
    {
        *why = cdWhyAll;
        return SZ_ERR_UNSUPPORTED;
    }

    for ( j = 0; j < cab->numFolders; j++ )
    {
        CdFold *fold = &folds[j];

        if ( fold->kept == 0 )
        {
            fold->how = CD_DROP;
            continue;
        }
        if ( fold->gone == 0 )
            fold->how = CD_COPY;
        else
        {
            if ( !CdCanDecode( cab->folders[j].type ) )
            {
                *why = cdWhyMethod;
                return SZ_ERR_UNSUPPORTED;
            }
            fold->how = CD_RECODE;
        }
        fold->newIndex = numNew++;
    }
    *keptOut    = kept;
    *foldersOut = numNew;
    return SZ_OK;
}

/*---- Writing the new cabinet -----------------------------------------------
 * The header, folder and file tables are measured first and room left for
 * them; the data follows, a folder at a time; then the tables are written
 * into the room left, now that every folder's position and block count is
 * known.
 *--------------------------------------------------------------------------- */
static int CdWrite( CabArchive *cab, const Byte *del, CdFold *folds,
                    int kept, int numNew, FILE *out, CabWriter *cw )
{
    const CabVol *vol = &cab->vols[0];
    CabOrder     *order  = NULL;
    UInt32       *newOff = NULL;
    Byte         *rec    = NULL;
    Byte          hdr[CAB_HDR_SIZE + 4];
    UInt32        hdrLen, filesAt, fileBytes, dataStart, left;
    int           reserve, needDefl = 0, count = 0, pos, i, j, rc;

    order  = (CabOrder *)malloc( (size_t)kept * sizeof( CabOrder ) );
    newOff = (UInt32 *)calloc( (size_t)cab->numEntries, sizeof( UInt32 ) );
    rec    = (Byte *)malloc( CAB_REC_CAP );
    cw->raw  = (Byte *)malloc( CAB_BLOCK_OUT );
    cw->pack = (Byte *)malloc( CAB_PACK_CAP );
    cw->copy = (Byte *)malloc( CAB_BLOCK_IN );
    if ( !order || !newOff || !rec || !cw->raw || !cw->pack || !cw->copy )
    { rc = SZ_ERR_MEMORY; goto done; }

    for ( j = 0; j < cab->numFolders; j++ )
    {
        if ( folds[j].how == CD_RECODE &&
             ( cab->folders[j].type & CAB_COMP_MASK ) != CAB_COMP_NONE )
            needDefl = 1;
        if ( folds[j].how == CD_COPY )
        {
            UInt32 packed = cab->parts[cab->folders[j].firstPart].packed;

            if ( packed != 0xFFFFFFFFUL ) cw->totalKB += CdKB( packed );
        }
        else if ( folds[j].how == CD_RECODE )
            cw->totalKB += CdKB( folds[j].extent );
    }
    if ( needDefl )
    {
        rc = DeflCreate( &cw->defl );
        if ( rc != SZ_OK ) goto done;
    }

    /* The kept files, a folder at a time, in offset order. */
    for ( i = 0; i < cab->numEntries; i++ )
    {
        if ( del[i] ) continue;
        order[count].folder = cab->entries[i].folder;
        order[count].offset = cab->entries[i].offset;
        order[count].index  = i;
        count++;
    }
    qsort( order, count, sizeof( CabOrder ), CabOrderCmp );

    /* The room the tables take. */
    reserve = ( vol->folderReserve != 0 || vol->dataReserve != 0 );
    hdrLen  = CAB_HDR_SIZE + ( reserve ? 4 : 0 );
    filesAt = hdrLen + (UInt32)numNew * ( CAB_FOLDER_SIZE + vol->folderReserve );
    rc = CdFiles( cab, del, folds, newOff, rec, NULL, &fileBytes );
    if ( rc != SZ_OK ) goto done;
    dataStart = filesAt + fileBytes;

    memset( cw->raw, 0, CAB_BLOCK_OUT );
    for ( left = dataStart; left > 0; )
    {
        UInt32 take = ( left < CAB_BLOCK_OUT ) ? left : CAB_BLOCK_OUT;

        rc = CwWrite( cw, cw->raw, take );
        if ( rc != SZ_OK ) goto done;
        left -= take;
    }

    /* The data. */
    cw->dataReserve = vol->dataReserve;
    pos = 0;
    for ( j = 0; j < cab->numFolders; j++ )
    {
        CdFold *fold  = &folds[j];
        int     first = pos;

        while ( pos < count && order[pos].folder == j ) pos++;
        if ( fold->how == CD_DROP ) continue;

        fold->dataAt = cw->at;
        if ( fold->how == CD_COPY )
        {
            cw->name = cab->entries[order[first].index].name;
            rc = CwTell( cw );
            if ( rc == SZ_OK ) rc = CdCopyFolder( cab, &cab->folders[j], cw );
            fold->type = cab->folders[j].type;
            cw->fileIndex += pos - first;
        }
        else
        {
            rc = CdRecodeFolder( cab, j, order + first, pos - first, newOff, cw );
            fold->type = (UInt16)( cw->stored ? CAB_COMP_NONE : CAB_COMP_MSZIP );
        }
        if ( rc != SZ_OK ) goto done;
        fold->blocks = cw->blocks;
    }

    /* Each folder's share was rounded up, so the count ends a little short
     * of the total; show the bar full. */
    cw->doneKB = cw->totalKB;
    rc = CwTell( cw );
    if ( rc != SZ_OK ) goto done;

    /* And back to the start for the tables. */
    if ( fseek( out, 0L, SEEK_SET ) != 0 ) { rc = SZ_ERR_WRITE; goto done; }
    memset( hdr, 0, sizeof( hdr ) );
    memcpy( hdr, "MSCF", 4 );
    CabPut32( hdr + 8, cw->at );
    CabPut32( hdr + 16, filesAt );
    hdr[24] = 3;                                /* version 1.3 */
    hdr[25] = 1;
    CabPut16( hdr + 26, (UInt32)numNew );
    CabPut16( hdr + 28, (UInt32)kept );
    CabPut16( hdr + 30, reserve ? CAB_FLAG_RESERVE : 0 );
    CabPut16( hdr + 32, vol->setId );
    CabPut16( hdr + 34, vol->index );
    if ( reserve )
    {
        /* cbCFHeader stays 0: the header's own reserve is dropped. */
        hdr[38] = (Byte)vol->folderReserve;
        hdr[39] = (Byte)vol->dataReserve;
    }
    if ( fwrite( hdr, 1, (size_t)hdrLen, out ) != (size_t)hdrLen )
    { rc = SZ_ERR_WRITE; goto done; }

    for ( j = 0; j < cab->numFolders; j++ )
    {
        Byte frec[CAB_FOLDER_SIZE];

        if ( folds[j].how == CD_DROP ) continue;
        CabPut32( frec, folds[j].dataAt );
        CabPut16( frec + 4, folds[j].blocks );
        CabPut16( frec + 6, folds[j].type );
        if ( fwrite( frec, 1, CAB_FOLDER_SIZE, out ) != CAB_FOLDER_SIZE )
        { rc = SZ_ERR_WRITE; goto done; }

        /* The folder's reserved bytes come along, from the old record. */
        if ( vol->folderReserve )
        {
            long   at   = vol->base + (long)vol->foldersAt +
                          (long)j * ( CAB_FOLDER_SIZE + vol->folderReserve ) +
                          CAB_FOLDER_SIZE;
            size_t size = (size_t)vol->folderReserve;

            if ( VolSeek( cab->fp, at, SEEK_SET ) != 0 ||
                 VolRead( rec, 1, (UInt32)size, cab->fp ) != (UInt32)size )
            { rc = SZ_ERR_READ; goto done; }
            if ( fwrite( rec, 1, size, out ) != size )
            { rc = SZ_ERR_WRITE; goto done; }
        }
    }

    rc = CdFiles( cab, del, folds, newOff, rec, out, &fileBytes );
    if ( rc == SZ_OK &&
         ( ftell( out ) != (long)dataStart || fflush( out ) != 0 ) )
        rc = SZ_ERR_WRITE;

done:
    if ( cw->defl ) DeflFree( cw->defl );
    if ( cw->raw )  free( cw->raw );
    if ( cw->pack ) free( cw->pack );
    if ( cw->copy ) free( cw->copy );
    if ( order )    free( order );
    if ( newOff )   free( newOff );
    if ( rec )      free( rec );
    cw->defl = NULL;
    cw->raw  = cw->pack = cw->copy = NULL;
    return rc;
}

int CabDeleteWrite( CabArchive *cab, const Byte *del, FILE *out,
                    ArcCompProgress prog, void *user, const char **why )
{
    CabWriter cw;
    CdFold   *folds;
    int       kept = 0, numNew = 0, rc;

    rc = CdCanDelete( cab, why );
    if ( rc != SZ_OK || !del ) return rc;

    folds = (CdFold *)malloc( (size_t)cab->numFolders * sizeof( CdFold ) );
    if ( !folds ) return SZ_ERR_MEMORY;
    rc = CdPlan( cab, del, folds, &kept, &numNew, why );
    if ( rc != SZ_OK || !out )
    {
        free( folds );
        return rc;
    }

    memset( &cw, 0, sizeof( cw ) );
    cw.out       = out;
    cw.prog      = prog;
    cw.user      = user;
    cw.fileCount = kept;
    cw.name      = "";
    rc = CdWrite( cab, del, folds, kept, numNew, out, &cw );
    free( folds );

    /* In the terms ARCFILE.C expects: a block that fails its checksum, or a
     * folder that turns out not to decode, is the source's fault. */
    if ( rc == SZ_ERR_CRC || rc == SZ_ERR_FORMAT ) rc = SZ_ERR_DATA;
    if ( rc == SZ_ERR_NORAM ) rc = SZ_ERR_MEMORY;
    if ( rc == SZ_ERR_UNSUPPORTED ) *why = cw.why ? cw.why : cdWhyMethod;
    return rc;
}
