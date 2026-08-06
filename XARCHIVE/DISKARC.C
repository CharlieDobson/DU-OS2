/*===========================================================================
 * DISKARC.C  -  FAT12/FAT16 floppy-image (.img / .dsk) reading
 * Target: MSVC 2.2  Win32s
 *
 * Reads a raw sector dump of a FAT12 or FAT16 volume (the usual .img / .dsk
 * floppy images, plus small FAT16 disk images) and exposes each file as an
 * archive entry.  It parses the BIOS Parameter Block from the boot sector,
 * loads the first FAT, walks the directory tree (root region for FAT12/16,
 * then cluster-chained sub-directories) and follows each file's cluster chain
 * to copy it out.  Both 8.3 short names and VFAT long file names (LFN) are
 * decoded.  No compression, no CRC: the "packed" size equals the file size.
 *
 * References used: the public FAT specification (Microsoft "FAT: General
 * Overview of On-Disk Format").  Original code; no third-party source.
 *===========================================================================*/

#include <windows.h>     /* lstrcpyn */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>      /* _mkdir */

#include "diskarc.h"
#include "platform.h"    /* SetFileDosMTime */

/*---- Limits -------------------------------------------------------------- */
#define MAX_FAT_BYTES   (8UL * 1024 * 1024)   /* refuse absurd FAT sizes     */
#define MAX_DIR_BYTES   (2UL * 1024 * 1024)   /* per-directory chain cap     */
#define MAX_DEPTH       64                    /* sub-directory nesting cap   */
#define DIRENT_SIZE     32

/*---- FAT directory attribute bits ---------------------------------------- */
#define ATTR_READONLY   0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME     0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LFN        0x0F   /* RO|Hidden|System|Volume => long-name entry */

struct DiskArchive {
    FILE          *fp;               /* file source (NULL for memory source) */
    unsigned char *mem;              /* in-memory image (NULL for file src)  */
    UInt32         memLen;
    int            ownMem;           /* free mem in DiskClose?               */
    int            fatType;          /* 12 or 16                             */
    UInt32         bytesPerSector;
    UInt32         sectorsPerCluster;
    UInt32         reservedSectors;
    UInt32         numFATs;
    UInt32         rootEntries;
    UInt32         sectorsPerFAT;
    UInt32         totalSectors;
    UInt32         rootDirSectors;
    UInt32         rootDirStart;      /* first sector of the root directory  */
    UInt32         firstDataSector;   /* first sector of cluster #2          */
    UInt32         countOfClusters;
    UInt32         clusterBytes;
    unsigned char *fat;               /* first FAT copy, loaded whole        */
    UInt32         fatBytes;
    int            numEntries;
    int            entryCap;          /* entries the table can hold          */
    DiskEntry     *entries;           /* grown on demand up to SZ_MAX_FILES  */
};

/* Make room for one more directory entry.  The table doubles from a small
 * start: a fixed SZ_MAX_FILES table would cost ~5 MB per image mounted, and
 * a floppy's root holds a couple of hundred files at most. */
static int DiskGrow( DiskArchive *d )
{
    int        n;
    DiskEntry *ne;

    if ( d->numEntries < d->entryCap ) return 1;

    n = d->entryCap ? d->entryCap * 2 : 64;
    if ( n > SZ_MAX_FILES ) n = SZ_MAX_FILES;
    if ( n <= d->numEntries ) return 0;

    ne = (DiskEntry *)realloc( d->entries, (size_t)n * sizeof( DiskEntry ) );
    if ( !ne ) return 0;

    memset( ne + d->entryCap, 0,
            (size_t)( n - d->entryCap ) * sizeof( DiskEntry ) );
    d->entries  = ne;
    d->entryCap = n;
    return 1;
}

/* Hand back the slack the doubling left over once the walk is done. */
static void DiskTrim( DiskArchive *d )
{
    int        n = d->numEntries ? d->numEntries : 1;
    DiskEntry *ne;

    if ( n >= d->entryCap ) return;
    ne = (DiskEntry *)realloc( d->entries, (size_t)n * sizeof( DiskEntry ) );
    if ( ne ) { d->entries = ne; d->entryCap = n; }
}

/*---- Little-endian field readers ----------------------------------------- */
static UInt32 GetU16( const unsigned char *p )
{
    return (UInt32)p[0] | ( (UInt32)p[1] << 8 );
}

static UInt32 GetU32( const unsigned char *p )
{
    return (UInt32)p[0] | ( (UInt32)p[1] << 8 ) |
           ( (UInt32)p[2] << 16 ) | ( (UInt32)p[3] << 24 );
}

/*---- Boot-sector / BPB geometry ------------------------------------------ *
 * Fills 'd' geometry from a 512-byte (or larger) boot buffer and validates
 * it.  Returns SZ_OK if this looks like a FAT12/16 volume, else SZ_ERR_SIG.
 * fileLen is the physical image size (for a light sanity bound).
 *-------------------------------------------------------------------------- */
static int ParseBoot( const unsigned char *bs, long fileLen, DiskArchive *d )
{
    UInt32 bps, spc, resv, nfat, rootEnt, spf;
    UInt32 tot16, tot32, total, media;
    UInt32 rootSecs, dataSecs, clusters, firstData;

    bps     = GetU16( bs + 0x0B );
    spc     = bs[0x0D];
    resv    = GetU16( bs + 0x0E );
    nfat    = bs[0x10];
    rootEnt = GetU16( bs + 0x11 );
    tot16   = GetU16( bs + 0x13 );
    media   = bs[0x15];
    spf     = GetU16( bs + 0x16 );
    tot32   = GetU32( bs + 0x20 );

    /* Bytes/sector must be a valid power-of-two sector size. */
    if ( bps != 512 && bps != 1024 && bps != 2048 && bps != 4096 )
        return SZ_ERR_SIG;
    /* Sectors/cluster must be a power of two, 1..128. */
    if ( spc == 0 || ( spc & ( spc - 1 ) ) != 0 || spc > 128 )
        return SZ_ERR_SIG;
    if ( resv == 0 )              return SZ_ERR_SIG;
    if ( nfat < 1 || nfat > 2 )   return SZ_ERR_SIG;
    if ( media < 0xF0 )           return SZ_ERR_SIG;
    /* FAT12/16 carry the FAT size in the 16-bit field and a non-zero root
     * entry count; FAT32 zeroes both, which we reject here. */
    if ( spf == 0 )               return SZ_ERR_SIG;
    if ( rootEnt == 0 )           return SZ_ERR_SIG;

    total = tot16 ? tot16 : tot32;
    if ( total == 0 )             return SZ_ERR_SIG;

    rootSecs  = ( rootEnt * DIRENT_SIZE + ( bps - 1 ) ) / bps;
    firstData = resv + nfat * spf + rootSecs;
    if ( firstData >= total )     return SZ_ERR_SIG;
    dataSecs  = total - firstData;
    clusters  = dataSecs / spc;

    /* FAT type is defined purely by the cluster count (per the FAT spec). */
    if ( clusters < 4085 )        d->fatType = 12;
    else if ( clusters < 65525 )  d->fatType = 16;
    else                          return SZ_ERR_SIG;   /* FAT32 / not us */

    /* Light physical bound: the image should hold at least the system area
     * plus one data sector (truncated images are rejected). */
    if ( fileLen > 0 &&
         (long)( firstData + spc ) * (long)bps > fileLen )
        return SZ_ERR_SIG;

    d->bytesPerSector    = bps;
    d->sectorsPerCluster = spc;
    d->reservedSectors   = resv;
    d->numFATs           = nfat;
    d->rootEntries       = rootEnt;
    d->sectorsPerFAT     = spf;
    d->totalSectors      = total;
    d->rootDirSectors    = rootSecs;
    d->rootDirStart      = resv + nfat * spf;
    d->firstDataSector   = firstData;
    d->countOfClusters   = clusters;
    d->clusterBytes      = spc * bps;
    return SZ_OK;
}

int DiskProbe( const char *path )
{
    FILE         *fp;
    unsigned char bs[512];
    DiskArchive   tmp;
    long          fileLen;
    int           rc;

    fp = fopen( path, "rb" );
    if ( !fp ) return 0;
    fseek( fp, 0L, SEEK_END );
    fileLen = ftell( fp );
    fseek( fp, 0L, SEEK_SET );
    if ( fread( bs, 1, 512, fp ) != 512 ) { fclose( fp ); return 0; }
    fclose( fp );

    /* The boot sector should begin with a jump (EB xx 90 or E9) and end with
     * the 0x55AA signature; combined with a sane BPB this reliably tells a
     * formatted FAT image apart from a zip or an MZ executable. */
    if ( !( bs[0] == 0xEB || bs[0] == 0xE9 ) ) return 0;
    if ( bs[510] != 0x55 || bs[511] != 0xAA )  return 0;

    memset( &tmp, 0, sizeof( tmp ) );
    rc = ParseBoot( bs, fileLen, &tmp );
    return ( rc == SZ_OK ) ? 1 : 0;
}

int DiskProbeBuf( const unsigned char *buf, UInt32 len )
{
    DiskArchive tmp;

    if ( len < 512 ) return 0;
    if ( !( buf[0] == 0xEB || buf[0] == 0xE9 ) ) return 0;
    if ( buf[510] != 0x55 || buf[511] != 0xAA )  return 0;

    memset( &tmp, 0, sizeof( tmp ) );
    return ( ParseBoot( buf, (long)len, &tmp ) == SZ_OK ) ? 1 : 0;
}

/*---- Sector I/O and FAT lookups ------------------------------------------ */

/* Read 'len' bytes at absolute offset 'off' from whichever source backs this
 * archive (a FILE or an in-memory image). */
static int ReadRaw( DiskArchive *d, long off, UInt32 len, unsigned char *buf )
{
    if ( d->mem )
    {
        if ( off < 0 || (UInt32)off > d->memLen || (UInt32)off + len > d->memLen )
            return SZ_ERR_READ;
        memcpy( buf, d->mem + off, len );
        return SZ_OK;
    }
    if ( fseek( d->fp, off, SEEK_SET ) != 0 )       return SZ_ERR_READ;
    if ( fread( buf, 1, len, d->fp ) != len )       return SZ_ERR_READ;
    return SZ_OK;
}

static int ReadSectors( DiskArchive *d, UInt32 sector, UInt32 count,
                        unsigned char *buf )
{
    return ReadRaw( d, (long)sector * (long)d->bytesPerSector,
                    count * d->bytesPerSector, buf );
}

/* Value of the FAT slot for 'cluster' (raw 12/16-bit entry). */
static UInt32 FatEntry( DiskArchive *d, UInt32 cluster )
{
    UInt32 off, val;

    if ( d->fatType == 16 )
    {
        off = cluster * 2;
        if ( off + 1 >= d->fatBytes ) return 0xFFFF;
        return GetU16( d->fat + off );
    }
    /* FAT12: 1.5 bytes per entry. */
    off = cluster + ( cluster >> 1 );
    if ( off + 1 >= d->fatBytes ) return 0xFFF;
    val = GetU16( d->fat + off );
    if ( cluster & 1 ) val >>= 4;
    else               val &= 0x0FFF;
    return val;
}

/* True if 'val' marks the end of a cluster chain (or is a bad/free slot we
 * should stop on). */
static int IsEndCluster( DiskArchive *d, UInt32 val )
{
    if ( d->fatType == 16 )
        return ( val >= 0xFFF8 || val == 0 || val == 0xFFF7 );
    return ( val >= 0x0FF8 || val == 0 || val == 0x0FF7 );
}

static int ClusterValid( DiskArchive *d, UInt32 cluster )
{
    return ( cluster >= 2 && cluster < d->countOfClusters + 2 );
}

static UInt32 ClusterSector( DiskArchive *d, UInt32 cluster )
{
    return d->firstDataSector + ( cluster - 2 ) * d->sectorsPerCluster;
}

/*---- Name decoding ------------------------------------------------------- */

/* Checksum an 11-byte raw short name the way LFN entries do. */
static unsigned char ShortSum( const unsigned char *raw11 )
{
    unsigned char sum = 0;
    int i;
    for ( i = 0; i < 11; i++ )
        sum = (unsigned char)( ( ( sum & 1 ) ? 0x80 : 0 ) + ( sum >> 1 ) + raw11[i] );
    return sum;
}

/* Format a raw 11-byte 8.3 name into "NAME.EXT", honouring the WinNT
 * lower-case flags (byte 0x0C: 0x08 = lower base, 0x10 = lower ext). */
static void FormatShort( const unsigned char *raw11, unsigned char ntFlags,
                         char *out )
{
    char base[9], ext[4];
    int  i, n;
    char *p = out;

    n = 0;
    for ( i = 0; i < 8; i++ )
        if ( raw11[i] != ' ' ) base[n++] = (char)raw11[i];
    base[n] = '\0';
    if ( (unsigned char)base[0] == 0x05 ) base[0] = (char)0xE5;   /* KANJI */
    if ( ntFlags & 0x08 )
        for ( i = 0; base[i]; i++ )
            if ( base[i] >= 'A' && base[i] <= 'Z' ) base[i] += 32;

    n = 0;
    for ( i = 8; i < 11; i++ )
        if ( raw11[i] != ' ' ) ext[n++] = (char)raw11[i];
    ext[n] = '\0';
    if ( ntFlags & 0x10 )
        for ( i = 0; ext[i]; i++ )
            if ( ext[i] >= 'A' && ext[i] <= 'Z' ) ext[i] += 32;

    for ( i = 0; base[i]; i++ ) *p++ = base[i];
    if ( ext[0] )
    {
        *p++ = '.';
        for ( i = 0; ext[i]; i++ ) *p++ = ext[i];
    }
    *p = '\0';
}

/* Pull the 13 UCS-2 code units out of one LFN entry and place them, converted
 * to ANSI, into lfn[] at the slot given by the sequence number.  Untranslatable
 * characters become '_'; 0x0000/0xFFFF are left as the pre-zeroed terminator. */
static void LfnPart( const unsigned char *rec, char *lfn )
{
    static const int ofs[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
    int    seq = rec[0] & 0x1F;
    int    base, k;
    UInt32 cu;

    if ( seq < 1 || seq > 20 ) return;
    base = ( seq - 1 ) * 13;
    for ( k = 0; k < 13; k++ )
    {
        cu = GetU16( rec + ofs[k] );
        if ( cu == 0x0000 || cu == 0xFFFF ) continue;   /* leave the '\0' */
        if ( base + k >= SZ_MAX_NAME - 1 ) continue;
        lfn[base + k] = ( cu < 0x100 ) ? (char)cu : '_';
    }
}

/*---- Directory walking --------------------------------------------------- */
static void ParseDir( DiskArchive *d, const unsigned char *buf, UInt32 len,
                      const char *prefix, int depth );

/* Read a sub-directory's whole cluster chain into a heap buffer.  Caller frees.
 * *outLen receives the byte length.  Returns NULL on error / empty. */
static unsigned char *ReadDirChain( DiskArchive *d, UInt32 startCluster,
                                    UInt32 *outLen )
{
    unsigned char *buf = NULL, *nb;
    UInt32 cap = 0, len = 0;
    UInt32 cluster = startCluster;
    UInt32 guard = 0;

    *outLen = 0;
    while ( ClusterValid( d, cluster ) && guard <= d->countOfClusters )
    {
        if ( len + d->clusterBytes > MAX_DIR_BYTES ) break;
        if ( len + d->clusterBytes > cap )
        {
            cap = len + d->clusterBytes;
            nb  = (unsigned char *)realloc( buf, cap );
            if ( !nb ) { free( buf ); return NULL; }
            buf = nb;
        }
        if ( ReadSectors( d, ClusterSector( d, cluster ),
                          d->sectorsPerCluster, buf + len ) != SZ_OK )
        { free( buf ); return NULL; }
        len += d->clusterBytes;

        cluster = FatEntry( d, cluster );
        if ( IsEndCluster( d, cluster ) ) break;
        guard++;
    }
    *outLen = len;
    return buf;
}

/* Append one enumerated file/dir; recurse into sub-directories. */
static void AddEntry( DiskArchive *d, const char *prefix, const char *name,
                      const unsigned char *rec, int depth )
{
    DiskEntry *e;
    UInt32     cluster;
    unsigned   attr = rec[0x0B];
    char       path[SZ_MAX_NAME];
    int        pn;

    if ( d->numEntries >= SZ_MAX_FILES ) return;
    if ( !DiskGrow( d ) ) return;

    /* Build "prefix\name" (or just "name" at the root). */
    pn = 0;
    if ( prefix && prefix[0] )
    {
        while ( prefix[pn] && pn < SZ_MAX_NAME - 2 ) { path[pn] = prefix[pn]; pn++; }
        if ( pn > 0 && path[pn-1] != '\\' ) path[pn++] = '\\';
    }
    { int i = 0; while ( name[i] && pn < SZ_MAX_NAME - 1 ) path[pn++] = name[i++]; }
    path[pn] = '\0';

    cluster = ( GetU16( rec + 0x14 ) << 16 ) | GetU16( rec + 0x1A );

    e = &d->entries[d->numEntries++];
    lstrcpyn( e->name, path, SZ_MAX_NAME );
    e->size         = GetU32( rec + 0x1C );
    e->crc          = 0;
    e->methodCode   = 0;
    e->modTime      = (UInt16)GetU16( rec + 0x16 );
    e->modDate      = (UInt16)GetU16( rec + 0x18 );
    e->attrib       = attr;
    e->isDir        = ( attr & ATTR_DIRECTORY ) ? 1 : 0;
    e->firstCluster = cluster;
    e->size         = e->isDir ? 0 : e->size;
    e->packed       = e->size;

    if ( e->isDir && depth < MAX_DEPTH && ClusterValid( d, cluster ) )
    {
        UInt32         subLen;
        unsigned char *sub = ReadDirChain( d, cluster, &subLen );
        if ( sub )
        {
            ParseDir( d, sub, subLen, path, depth + 1 );
            free( sub );
        }
    }
}

static void ParseDir( DiskArchive *d, const unsigned char *buf, UInt32 len,
                      const char *prefix, int depth )
{
    UInt32 pos;
    char   lfn[SZ_MAX_NAME];
    int    lfnActive = 0;
    unsigned char lfnSum = 0;

    for ( pos = 0; pos + DIRENT_SIZE <= len; pos += DIRENT_SIZE )
    {
        const unsigned char *rec = buf + pos;
        unsigned char first = rec[0];
        unsigned char attr;
        char short83[13];

        if ( first == 0x00 ) break;              /* no more entries */
        if ( first == 0xE5 ) { lfnActive = 0; continue; }   /* deleted */

        attr = rec[0x0B];

        if ( attr == ATTR_LFN )                  /* long-name fragment */
        {
            if ( !lfnActive )
            {
                memset( lfn, 0, sizeof( lfn ) );
                lfnSum    = rec[0x0D];
                lfnActive = 1;
            }
            LfnPart( rec, lfn );
            continue;
        }

        if ( attr & ATTR_VOLUME ) { lfnActive = 0; continue; } /* vol label */
        if ( first == 0x2E )      { lfnActive = 0; continue; } /* . or ..   */
        if ( d->numEntries >= SZ_MAX_FILES ) break;

        FormatShort( rec, rec[0x0C], short83 );

        if ( lfnActive && lfn[0] && ShortSum( rec ) == lfnSum )
            AddEntry( d, prefix, lfn, rec, depth );
        else
            AddEntry( d, prefix, short83, rec, depth );

        lfnActive = 0;
    }
}

/*===========================================================================
 * Open / close
 *===========================================================================*/

/* Shared loader: 'd' already has its source (fp or mem) wired up.  Reads and
 * validates the boot sector, loads the FAT, and walks the root directory.
 * On failure the DiskArchive is destroyed and *out left NULL. */
static int DiskLoad( DiskArchive *d, long imgLen, DiskArchive **out )
{
    unsigned char  bs[512];
    unsigned char *rootBuf;
    int            rc;

    if ( ReadRaw( d, 0L, 512, bs ) != SZ_OK ) { DiskClose( d ); return SZ_ERR_READ; }

    rc = ParseBoot( bs, imgLen, d );
    if ( rc != SZ_OK ) { DiskClose( d ); return rc; }

    /* Load the first FAT copy whole (cheap for era-sized volumes). */
    d->fatBytes = d->sectorsPerFAT * d->bytesPerSector;
    if ( d->fatBytes == 0 || d->fatBytes > MAX_FAT_BYTES )
    { DiskClose( d ); return SZ_ERR_TOOBIG; }

    d->fat = (unsigned char *)malloc( d->fatBytes );
    if ( !d->fat )                      /* the entry table grows as we walk */
    { DiskClose( d ); return SZ_ERR_MEMORY; }

    if ( ReadSectors( d, d->reservedSectors,
                      d->sectorsPerFAT, d->fat ) != SZ_OK )
    { DiskClose( d ); return SZ_ERR_READ; }

    /* Read and parse the fixed-size root directory region. */
    rootBuf = (unsigned char *)malloc( d->rootDirSectors * d->bytesPerSector );
    if ( !rootBuf ) { DiskClose( d ); return SZ_ERR_MEMORY; }
    if ( ReadSectors( d, d->rootDirStart, d->rootDirSectors, rootBuf ) != SZ_OK )
    { free( rootBuf ); DiskClose( d ); return SZ_ERR_READ; }

    ParseDir( d, rootBuf, d->rootDirSectors * d->bytesPerSector, "", 0 );
    free( rootBuf );

    DiskTrim( d );
    *out = d;
    return SZ_OK;
}

int DiskOpen( const char *path, DiskArchive **out )
{
    DiskArchive *d;
    long         fileLen;

    *out = NULL;
    d = (DiskArchive *)calloc( 1, sizeof( DiskArchive ) );
    if ( !d ) return SZ_ERR_MEMORY;

    d->fp = fopen( path, "rb" );
    if ( !d->fp ) { free( d ); return SZ_ERR_OPEN; }

    fseek( d->fp, 0L, SEEK_END );
    fileLen = ftell( d->fp );
    fseek( d->fp, 0L, SEEK_SET );

    return DiskLoad( d, fileLen, out );
}

int DiskOpenMemory( unsigned char *buf, UInt32 len, int ownBuf,
                    DiskArchive **out )
{
    DiskArchive *d;

    *out = NULL;
    if ( !buf ) return SZ_ERR_FORMAT;

    d = (DiskArchive *)calloc( 1, sizeof( DiskArchive ) );
    if ( !d ) { if ( ownBuf ) free( buf ); return SZ_ERR_MEMORY; }

    d->mem    = buf;
    d->memLen = len;
    d->ownMem = ownBuf;

    return DiskLoad( d, (long)len, out );
}

int DiskNumEntries( DiskArchive *d )
{
    return d ? d->numEntries : 0;
}

const DiskEntry *DiskGetEntry( DiskArchive *d, int index )
{
    if ( !d || index < 0 || index >= d->numEntries ) return NULL;
    return &d->entries[index];
}

const char *DiskFsName( DiskArchive *d )
{
    if ( !d ) return "FAT";
    return ( d->fatType == 12 ) ? "FAT12" : "FAT16";
}

/*===========================================================================
 * Path helpers (mirrors of the ZIPARC copies)
 *===========================================================================*/
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

static void BuildOut( char *dst, int dstSize,
                      const char *destDir, const char *name )
{
    int n = 0;
    if ( destDir && destDir[0] )
    {
        while ( destDir[n] && n < dstSize - 2 ) { dst[n] = destDir[n]; n++; }
        if ( n > 0 && dst[n-1] != '\\' ) dst[n++] = '\\';
    }
    while ( *name && n < dstSize - 1 ) dst[n++] = *name++;
    dst[n] = '\0';
}

/*===========================================================================
 * Extraction
 *===========================================================================*/
static int DiskExtractIndex( DiskArchive *d, int idx, const char *destDir )
{
    DiskEntry     *e = &d->entries[idx];
    char           outPath[SZ_MAX_NAME * 4];
    FILE          *out;
    unsigned char *clbuf;
    UInt32         remain, cluster, guard;
    int            rc;

    if ( destDir ) BuildOut( outPath, sizeof( outPath ), destDir, e->name );

    if ( e->isDir )
    {
        if ( destDir ) MakeDirs( outPath, 1 );
        return SZ_OK;
    }

    if ( destDir )
    {
        MakeDirs( outPath, 0 );
        out = fopen( outPath, "wb" );
        if ( !out ) return SZ_ERR_WRITE;
    }
    else
        out = NULL;

    rc      = SZ_OK;
    remain  = e->size;
    cluster = e->firstCluster;
    guard   = 0;
    clbuf   = ( remain > 0 ) ? (unsigned char *)malloc( d->clusterBytes ) : NULL;
    if ( remain > 0 && !clbuf ) { if ( out ) fclose( out ); if ( destDir ) remove( outPath ); return SZ_ERR_MEMORY; }

    while ( remain > 0 )
    {
        UInt32 chunk;

        if ( !ClusterValid( d, cluster ) ) { rc = SZ_ERR_DATA; break; }
        if ( guard++ > d->countOfClusters + 1 ) { rc = SZ_ERR_DATA; break; }

        if ( ReadSectors( d, ClusterSector( d, cluster ),
                          d->sectorsPerCluster, clbuf ) != SZ_OK )
        { rc = SZ_ERR_READ; break; }

        chunk = ( remain < d->clusterBytes ) ? remain : d->clusterBytes;
        if ( out && fwrite( clbuf, 1, chunk, out ) != chunk )
        { rc = SZ_ERR_WRITE; break; }
        remain -= chunk;
        if ( remain == 0 ) break;

        cluster = FatEntry( d, cluster );
        if ( IsEndCluster( d, cluster ) ) { rc = SZ_ERR_DATA; break; }
    }

    if ( clbuf ) free( clbuf );
    if ( out )   fclose( out );

    if ( rc == SZ_OK )
    {
        if ( destDir ) SetFileDosMTime( outPath, e->modDate, e->modTime );
    }
    else if ( destDir )
        remove( outPath );
    return rc;
}

int DiskExtractAll( DiskArchive *d, const char *destDir,
                    SzProgress prog, void *user )
{
    int i, rc;
    if ( !d ) return SZ_ERR_FORMAT;
    for ( i = 0; i < d->numEntries; i++ )
    {
        if ( !d->entries[i].name[0] ) continue;
        if ( prog && !prog( user, i, d->numEntries, d->entries[i].name ) )
            return SZ_ERR_CANCEL;
        rc = DiskExtractIndex( d, i, destDir );
        if ( rc ) return rc;
    }
    return SZ_OK;
}

int DiskExtractItems( DiskArchive *d, const int *indices, int count,
                      const char *destDir, SzProgress prog, void *user )
{
    int k, idx, rc;
    if ( !d ) return SZ_ERR_FORMAT;
    for ( k = 0; k < count; k++ )
    {
        idx = indices[k];
        if ( idx < 0 || idx >= d->numEntries ) continue;
        if ( !d->entries[idx].name[0] ) continue;
        if ( prog && !prog( user, idx, d->numEntries, d->entries[idx].name ) )
            return SZ_ERR_CANCEL;
        rc = DiskExtractIndex( d, idx, destDir );
        if ( rc ) return rc;
    }
    return SZ_OK;
}

void DiskClose( DiskArchive *d )
{
    if ( d )
    {
        if ( d->fp )              fclose( d->fp );
        if ( d->mem && d->ownMem ) free( d->mem );
        if ( d->fat )             free( d->fat );
        if ( d->entries )         free( d->entries );
        free( d );
    }
}
