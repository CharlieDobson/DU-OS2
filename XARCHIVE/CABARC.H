/*===========================================================================
 * CABARC.H  -  Microsoft cabinet (.cab) parsing and extraction
 * Target: MSVC 2.2  Win32s, and the Open Watcom DOS/32A and OS/2 builds
 *
 * A cabinet holds FOLDERS - compressed streams, each one method - and FILES,
 * each a run of bytes at some offset inside one folder.  All four methods are
 * read: stored, MSZIP, Quantum and LZX.  So are cabinet SETS, where a folder,
 * and the files in it, carry on from one .cab into the next; opening any
 * cabinet of a set opens the whole set, and a missing one is reported as a
 * missing volume.  A cabinet inside a self-extracting .exe is found by
 * scanning, as a self-extracting 7z is.
 *
 * There is no per-file checksum in the format.  What a cabinet has is an
 * optional checksum on every data block, which is verified when present -
 * so the integrity test catches damage, but cannot say which file it hit
 * beyond "the one being decoded".
 *===========================================================================*/

#ifndef CABARC_H
#define CABARC_H

#include "arcdefs.h"
#include "szarc.h"      /* SzProgress, and the shared SZ_ERR_* result codes */

typedef struct {
    char   name[SZ_MAX_NAME];   /* stored path (back-slashes)               */
    UInt32 size;                /* uncompressed size                        */
    UInt16 modDate, modTime;    /* MS-DOS packed, local time                */
    UInt32 attrib;              /* DOS attribute bits                       */
    int    isDir;               /* always 0: cabinets hold no directories   */
    int    folder;              /* index of the (set-wide) folder it is in  */
    UInt32 offset;              /* where it starts in that folder's output  */
} CabEntry;

typedef struct CabArchive CabArchive;

/* Does this start like a cabinet?  'sig' is the first 'len' bytes. */
int  CabProbe( const unsigned char *sig, int len );

/* Open and parse.  SZ_ERR_SIG when there is no cabinet in the file,
 * SZ_ERR_VOLUME when it is one of a set and another member is missing. */
int  CabOpen( const char *path, CabArchive **out );

/* The same, but a cabinet that does not start the file is looked for - the
 * shape of a self-extracting .exe. */
int  CabOpenScan( const char *path, CabArchive **out );

int  CabNumEntries( CabArchive *cab );
const CabEntry *CabGetEntry( CabArchive *cab, int index );

/* A folder's packed size on its first member, as 7z does for a solid
 * block; 0xFFFFFFFF on the rest. */
UInt32 CabEntryPacked( CabArchive *cab, int index );

/* "Store", "MSZIP", "Quantum", "LZX", or "?" for a method this does not
 * know. */
const char *CabEntryMethod( CabArchive *cab, int index );

/* Cabinets in the set: 1 for an ordinary .cab. */
int  CabVolumeCount( CabArchive *cab );

/* What the last extraction or test could check: how many data blocks carried
 * a checksum (all of which matched, or it would have failed) and how many
 * carried none - the field is optional, and a writer may leave it 0. */
void CabChecksumCount( CabArchive *cab, UInt32 *checked, UInt32 *bare );

int  CabExtractAll( CabArchive *cab, const char *destDir,
                    SzProgress prog, void *user );
int  CabExtractItems( CabArchive *cab, const int *indices, int count,
                      const char *destDir, SzProgress prog, void *user );

/* Peak bytes one extraction needs; see ArcMemNeeded in ARCFILE.H. */
UInt32 CabMemNeeded( CabArchive *cab );

void CabClose( CabArchive *cab );

#endif /* CABARC_H */
