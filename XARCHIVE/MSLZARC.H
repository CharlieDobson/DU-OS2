/*===========================================================================
 * MSLZARC.H  -  Files packed by Microsoft's COMPRESS.EXE (SZDD and KWAJ)
 * Target: MSVC 2.2  Win32s, and the Open Watcom DOS/32A and OS/2 builds
 *
 * The SETUP.EX_ / README.TX_ files on Microsoft's floppies.  Each holds ONE
 * compressed file, so the backend presents a one-entry archive whose entry
 * is named after the original file.
 *
 *   SZDD  COMPRESS -r and the MS-DOS 6 / Windows 3.1 setup disks.  LZSS with
 *         a 4 KB window; the header keeps the length and the one character
 *         the -r option replaced with '_' (SETUP.EX_ holds 'E').  Also the
 *         older QBasic-era variant, signature "SZ " with no such character.
 *   KWAJ  COMPRESS 1.1x and later.  A header with optional fields - length,
 *         original name and extension, free text - and a choice of method:
 *         0 stored, 1 XOR 0xFF, 2 the SZDD LZSS, 3 LZSS + Huffman (the "Jeff
 *         Johnson" algorithm), 4 MSZIP.
 *
 * Neither format records a date, so the entry carries the compressed file's
 * own, which is also what EXPAND.EXE gives the file it writes.  Neither has
 * a checksum either: the integrity test can only confirm that the data
 * decodes to the length the header promises.
 *===========================================================================*/

#ifndef MSLZARC_H
#define MSLZARC_H

#include "arcdefs.h"
#include "szarc.h"      /* SzProgress, and the shared SZ_ERR_* result codes */

typedef struct {
    char   name[SZ_MAX_NAME];   /* the original file's name                 */
    UInt32 size;                /* uncompressed size                        */
    UInt32 packed;              /* compressed bytes after the header        */
    UInt16 modDate, modTime;    /* MS-DOS packed: the compressed file's own */
    UInt32 attrib;              /* always 0: nothing is recorded            */
    int    isDir;               /* always 0                                 */
} MslzEntry;

typedef struct MslzArchive MslzArchive;

/* Does this start like one of these files?  'sig' is the first 'len' bytes
 * (8 are needed).  Cheap, for ArcOpen's sniffing. */
int  MslzProbe( const unsigned char *sig, int len );

/* SZ_OK, SZ_ERR_SIG when it is neither format, SZ_ERR_UNSUPPORTED for a
 * method this does not know. */
int  MslzOpen( const char *path, MslzArchive **out );

int  MslzNumEntries( MslzArchive *arc );
const MslzEntry *MslzGetEntry( MslzArchive *arc, int index );

/* "SZDD" or "KWAJ", and the method in the list view's terms. */
const char *MslzFormatName( MslzArchive *arc );
const char *MslzMethod( MslzArchive *arc );

/* The free text a KWAJ header may carry (COMPRESS -t), or NULL. */
const char *MslzComment( MslzArchive *arc );

/* Did the header give the length?  It is the only thing a test can check the
 * output against; a KWAJ file made with COMPRESS -l does not have one. */
int  MslzHasLength( MslzArchive *arc );

int  MslzExtractAll( MslzArchive *arc, const char *destDir,
                     SzProgress prog, void *user );
int  MslzExtractItems( MslzArchive *arc, const int *indices, int count,
                       const char *destDir, SzProgress prog, void *user );

/* Peak bytes one extraction needs; see ArcMemNeeded in ARCFILE.H. */
UInt32 MslzMemNeeded( MslzArchive *arc );

void MslzClose( MslzArchive *arc );

#endif /* MSLZARC_H */
