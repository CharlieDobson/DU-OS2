/*===========================================================================
 * ZIPARC.H  -  PKZIP (.zip) parsing and extraction
 * Target: MSVC 2.2  Win32s
 *
 * A subset extractor for PKZIP 2.04g archives: stored (method 0) and deflated
 * (method 8) entries, with sub-directory paths and self-extracting (stub-
 * prefixed) archives.  No encryption, no Zip64, no multi-disk.  Ported from
 * the DOS/Win16 UNZIP module; presents the same open/list/extract shape as
 * SZARC so the GUI can treat 7z and zip uniformly.  Result codes and progress
 * callback are shared with SZARC (SZ_ERR_* / SzProgress).
 *===========================================================================*/

#ifndef ZIPARC_H
#define ZIPARC_H

#include "arcdefs.h"
#include "szarc.h"      /* SzProgress, and the shared SZ_ERR_* result codes */

typedef struct {
    char   name[SZ_MAX_NAME];   /* archive-relative path (back-slashes)     */
    UInt32 size;                /* uncompressed size                        */
    UInt32 packed;              /* compressed size                          */
    UInt32 crc;                 /* expected CRC-32                          */
    int    methodCode;          /* 0 = stored, 8 = deflated                 */
    UInt16 modDate, modTime;    /* MS-DOS packed modification date/time     */
    UInt32 attrib;              /* external attributes (low byte = DOS bits) */
    int    isDir;               /* directory entry                          */
} ZipEntry;

typedef struct ZipArchive ZipArchive;

/* Open and parse the central directory.  *out must be released with
 * ZipClose().  Returns SZ_OK or an SZ_ERR_* code (SZ_ERR_SIG if not a zip). */
int  ZipOpen( const char *path, ZipArchive **out );

int  ZipNumEntries( ZipArchive *z );
const ZipEntry *ZipGetEntry( ZipArchive *z, int index );

/* Decompress one entry entirely into a freshly malloc'd buffer (used to unwrap
 * a WinImage .imz, which is a zip holding a single disk image).  On SZ_OK,
 * *outBuf must be free()d by the caller and *outLen is the byte length; the
 * CRC is verified.  Fails with SZ_ERR_UNSUPPORTED for encrypted / non
 * store-or-deflate entries. */
int  ZipExtractToMemory( ZipArchive *z, int index,
                         unsigned char **outBuf, UInt32 *outLen );

/* Extract all / a chosen subset (entry indices) beneath destDir, verifying
 * CRCs.  prog is called once per entry; return 0 from it to cancel. */
int  ZipExtractAll( ZipArchive *z, const char *destDir,
                    SzProgress prog, void *user );
int  ZipExtractItems( ZipArchive *z, const int *indices, int count,
                      const char *destDir, SzProgress prog, void *user );

void ZipClose( ZipArchive *z );

#endif /* ZIPARC_H */
