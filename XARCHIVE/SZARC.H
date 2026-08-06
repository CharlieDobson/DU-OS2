/*===========================================================================
 * SZARC.H  -  7z archive parsing and extraction (LZMA / LZMA2 / Copy subset)
 * Target: MSVC 2.2  Win32s
 *
 * Supports the common cases produced by 7-Zip: folders whose main coder is
 * LZMA, LZMA2 or stored/Copy, optionally preceded (for executables) by the
 * BCJ x86 branch filter.  No encryption, no PPMd, no other BCJ variants, no
 * multi-volume.  Solid folders (several files sharing one compressed stream)
 * are supported.  Anything outside the subset fails with SZ_ERR_UNSUPPORTED.
 *===========================================================================*/

#ifndef SZARC_H
#define SZARC_H

#include "arcdefs.h"

typedef struct {
    char   name[SZ_MAX_NAME];   /* archive-relative path (back-slashes)     */
    UInt32 size;                /* uncompressed size in bytes               */
    UInt32 crc;                 /* expected CRC-32 (valid if hasCrc)        */
    int    hasCrc;
    int    isDir;               /* directory entry                          */
    int    folderIndex;         /* coded folder, or -1 for dir/empty file   */
    UInt32 offsetInFolder;      /* byte offset within the folder's stream   */
    UInt32 mtimeLo, mtimeHi;    /* last-modified FILETIME (valid if hasMtime)*/
    int    hasMtime;
    UInt32 attrib;              /* Windows file attributes (valid if hasAttr)*/
    int    hasAttrib;
} SzEntry;

typedef struct SzArchive SzArchive;

/* Progress/yield callback; return 0 to cancel.  Called once per entry. */
typedef int (*SzProgress)( void *user, int fileIndex, int fileCount,
                           const char *name );

/* Open and fully parse the header.  On success *out owns an open handle and
 * must be released with SzClose().  Returns SZ_OK or an SZ_ERR_* code. */
int  SzOpen( const char *path, SzArchive **out );

int  SzGetNumEntries( SzArchive *a );
const SzEntry *SzGetEntry( SzArchive *a, int index );

/* Coder name for an entry's folder ("LZMA", "LZMA2", "Copy", "+BCJ" suffix);
 * "" for directory/empty entries.  Returns a pointer to a static buffer. */
const char *SzEntryMethod( SzArchive *a, int index );

/* Packed (compressed) size attributed to an entry.  7z packs per folder, so
 * this is the folder's pack size reported on the first member of each folder,
 * and 0xFFFFFFFF (unknown) for the rest and for directory/empty entries. */
UInt32 SzEntryPacked( SzArchive *a, int index );

/* Extract every entry beneath destDir, recreating sub-directories and
 * verifying CRCs.  Returns SZ_OK or an SZ_ERR_* code. */
int  SzExtractAll( SzArchive *a, const char *destDir,
                   SzProgress prog, void *user );

/* Extract a chosen subset of entries (indices into the entry list) beneath
 * destDir.  Folders shared by several selected files are decoded only once.
 * Returns SZ_OK or an SZ_ERR_* code. */
int  SzExtractItems( SzArchive *a, const int *indices, int count,
                     const char *destDir, SzProgress prog, void *user );

void SzClose( SzArchive *a );

/* Human-readable text for an SZ_* result code. */
const char *SzErrorText( int code );

#endif /* SZARC_H */
