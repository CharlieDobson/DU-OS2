/*===========================================================================
 * DISKARC.H  -  FAT12/FAT16 floppy-image (.img / .dsk) reading
 * Target: MSVC 2.2  Win32s
 *
 * Treats a raw sector-dump floppy image (1.44M, 720K, 360K, ... and small
 * FAT16 hard-disk images) as a read-only archive: it walks the FAT and the
 * directory tree and presents every file as an entry, so the GUI can list and
 * "extract" (copy out) files just like a real archive.  Supports 8.3 short
 * names and VFAT long file names (LFN).  Presents the same open/list/extract
 * shape as SZARC/ZIPARC and returns the shared SZ_ERR_* result codes.
 *
 * There is no compression and no per-file CRC in a FAT filesystem, so the
 * "packed" size equals the file size and the integrity test simply reads every
 * file's cluster chain (a read error is the only way it can fail).
 *===========================================================================*/

#ifndef DISKARC_H
#define DISKARC_H

#include "arcdefs.h"
#include "szarc.h"      /* SzProgress, and the shared SZ_ERR_* result codes */

typedef struct {
    char   name[SZ_MAX_NAME];   /* image-relative path (back-slashes)       */
    UInt32 size;                /* file size in bytes                       */
    UInt32 packed;              /* == size (FAT stores files uncompressed)  */
    UInt32 crc;                 /* unused (FAT has no per-file CRC)          */
    int    methodCode;          /* always 0 (stored)                        */
    UInt16 modDate, modTime;    /* MS-DOS packed modification date/time     */
    UInt32 attrib;              /* DOS attribute bits (R/H/S/A/D)           */
    int    isDir;               /* directory entry                          */
    UInt32 firstCluster;        /* start cluster of the chain (0 = empty)   */
} DiskEntry;

typedef struct DiskArchive DiskArchive;

/* Cheap content probe: returns 1 if 'path' looks like a FAT12/FAT16 raw disk
 * image (valid boot-sector BPB), 0 otherwise.  Used by the front door to tell
 * a floppy image apart from a zip / self-extracting EXE. */
int  DiskProbe( const char *path );

/* Same probe against an in-memory image (e.g. the decompressed payload of a
 * WinImage .imz), inspecting only the first sector. */
int  DiskProbeBuf( const unsigned char *buf, UInt32 len );

/* Open and parse the filesystem tree.  *out must be released with
 * DiskClose().  Returns SZ_OK or an SZ_ERR_* code (SZ_ERR_SIG if not a
 * recognisable FAT12/16 image). */
int  DiskOpen( const char *path, DiskArchive **out );

/* Open an image already held in memory (a .imz's decompressed .ima).  If
 * ownBuf is non-zero DiskClose() frees buf; otherwise the caller keeps
 * ownership and must outlive the DiskArchive.  Same return codes as DiskOpen. */
int  DiskOpenMemory( unsigned char *buf, UInt32 len, int ownBuf,
                     DiskArchive **out );

int  DiskNumEntries( DiskArchive *d );
const DiskEntry *DiskGetEntry( DiskArchive *d, int index );

/* "FAT12" or "FAT16" for the Info box. */
const char *DiskFsName( DiskArchive *d );

/* Copy all / a chosen subset (entry indices) of files out beneath destDir.
 * destDir == NULL means "test only": read every cluster chain but write
 * nothing.  prog is called once per entry; return 0 from it to cancel. */
int  DiskExtractAll( DiskArchive *d, const char *destDir,
                     SzProgress prog, void *user );
int  DiskExtractItems( DiskArchive *d, const int *indices, int count,
                       const char *destDir, SzProgress prog, void *user );

void DiskClose( DiskArchive *d );

#endif /* DISKARC_H */
