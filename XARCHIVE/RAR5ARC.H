/*===========================================================================
 * RAR5ARC.H  -  RAR 5.x parsing and extraction
 * Target: MSVC 2.2  Win32s
 *
 * PHASE 1: parses the RAR5 container (vint-encoded block headers), lists
 * entries, and extracts STORED (method 0) files.  Compressed entries list but
 * return SZ_ERR_UNSUPPORTED on extraction (the RAR5 unpack engine is a later
 * phase).  No encryption, no multi-volume.  Presents the same open/list/
 * extract shape as the other backends and shares SZ_ERR_* / SzProgress.
 *
 * RAR5 stores modification time as a Unix timestamp, which we convert to a
 * Windows FILETIME (as 7z uses), so the ARCFILE date/attr code treats a RAR5
 * entry like a 7z one.
 *===========================================================================*/

#ifndef RAR5ARC_H
#define RAR5ARC_H

#include "arcdefs.h"
#include "szarc.h"

typedef struct {
    char   name[SZ_MAX_NAME];
    UInt32 size;
    UInt32 packed;
    UInt32 crc;
    int    hasCrc;
    int    methodCode;          /* 0 = stored, 1..5 = compression level      */
    UInt32 mtimeLo, mtimeHi;    /* last-modified FILETIME (valid if hasMtime) */
    int    hasMtime;
    UInt32 attrib;              /* low byte = DOS attribute bits             */
    int    isDir;
} Rar5Entry;

typedef struct Rar5Archive Rar5Archive;

int  Rar5Open( const char *path, Rar5Archive **out );
int  Rar5NumEntries( Rar5Archive *r );
const Rar5Entry *Rar5GetEntry( Rar5Archive *r, int index );
int  Rar5ExtractAll( Rar5Archive *r, const char *destDir,
                     SzProgress prog, void *user );
int  Rar5ExtractItems( Rar5Archive *r, const int *indices, int count,
                       const char *destDir, SzProgress prog, void *user );
void Rar5Close( Rar5Archive *r );

#endif /* RAR5ARC_H */
