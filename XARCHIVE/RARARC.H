/*===========================================================================
 * RARARC.H  -  RAR (2.x/3.x, "RAR4") parsing and extraction
 * Target: MSVC 2.2  Win32s
 *
 * PHASE 1: parses the RAR4 container (marker + block headers), lists entries,
 * and extracts STORED (method 0x30) files.  Compressed entries list correctly
 * but currently return SZ_ERR_UNSUPPORTED on extraction - the RAR unpack
 * engine (LZ/PPMd/filter VM) is a later phase.  RAR5 archives are detected and
 * reported as unsupported until their (separate) container is added.
 *
 * No encryption, no multi-volume.  Presents the same open/list/extract shape
 * as SZARC/ZIPARC and shares the SZ_ERR_* codes + SzProgress callback.
 *===========================================================================*/

#ifndef RARARC_H
#define RARARC_H

#include "arcdefs.h"
#include "szarc.h"      /* SzProgress + shared SZ_ERR_* result codes */

typedef struct {
    char   name[SZ_MAX_NAME];   /* archive-relative path (back-slashes)     */
    UInt32 size;                /* uncompressed size                        */
    UInt32 packed;              /* compressed size                          */
    UInt32 crc;                 /* expected CRC-32                          */
    int    methodCode;          /* RAR method byte (0x30 = stored)          */
    int    unpVer;              /* unpack version (29/36 = RAR3 "v3")       */
    UInt16 modDate, modTime;    /* MS-DOS packed modification date/time     */
    UInt32 attrib;              /* file attributes (low byte = DOS bits)    */
    int    isDir;               /* directory entry                          */
} RarEntry;

typedef struct RarArchive RarArchive;

/* Open and parse the block headers.  *out must be released with RarClose().
 * Returns SZ_OK or an SZ_ERR_* code (SZ_ERR_SIG if not a RAR archive,
 * SZ_ERR_UNSUPPORTED for RAR5 until that container is implemented). */
int  RarOpen( const char *path, RarArchive **out );

int  RarNumEntries( RarArchive *r );
const RarEntry *RarGetEntry( RarArchive *r, int index );

int  RarExtractAll( RarArchive *r, const char *destDir,
                    SzProgress prog, void *user );
int  RarExtractItems( RarArchive *r, const int *indices, int count,
                      const char *destDir, SzProgress prog, void *user );

void RarClose( RarArchive *r );

#endif /* RARARC_H */
