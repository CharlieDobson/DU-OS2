/*===========================================================================
 * ARCCOMPI.H  -  Inside ARCCOMP: what the scanner hands the two writers
 * Target: MSVC 2.2  Win32s
 *
 * Private to ARCCOMP.C, ZIPWRITE.C and SZWRITE.C.  A front end includes
 * ARCCOMP.H and never this.
 *
 * ONE ENTRY PER FILE OR FOLDER, and an entry does not carry its source path:
 * it carries the path RELATIVE to the folder its item was named in, plus the
 * number of that folder.  A flat archive stores only the last component of
 * the relative path, a pathed one stores all of it, and the file on disk is
 * the base folder joined to it - so one string serves all three, and ten
 * thousand files cost ten thousand short strings rather than three long ones
 * each.  That is the difference between fitting and not on a 4 MB machine.
 *===========================================================================*/

#ifndef ARCCOMPI_H
#define ARCCOMPI_H

#include <stdio.h>
#include "arccomp.h"

/* How a file is to be packed, decided from its first few kilobytes. */
#define CK_DATA     0       /* compress it                                 */
#define CK_EXE      1       /* x86 code: the branch filter, then LZMA (7z)  */
#define CK_PACKED   2       /* already compressed: store it                 */

/* What became of an entry while the archive was written. */
#define CS_PENDING  0
#define CS_DONE     1
#define CS_SKIPPED  2       /* could not be opened when its turn came       */
#define CS_DROPPED  3       /* left out at scan time (a duplicate name)     */

typedef struct {
    char   *rel;            /* path from the base folder, '\' separated     */
    UInt16  base;           /* index into job->bases                        */
    Byte    isDir;
    Byte    kind;           /* CK_*                                         */
    Byte    text;           /* looks like text (zip's internal attribute)   */
    Byte    state;          /* CS_*                                         */
    Byte    method;         /* zip: 0 stored, 8 deflated                    */
    Byte    folder;         /* 7z: which coded folder                       */
    UInt32  attr;           /* DOS attribute bits                           */
    UInt32  size;           /* as found by the scan                         */
    UInt16  dosDate;        /* last write, local, packed                    */
    UInt16  dosTime;
    UInt32  mtLo, mtHi;     /* last write, UTC FILETIME                     */

    /* Filled in as the entry is written. */
    UInt32  crc;
    UInt32  realSize;       /* bytes actually read                          */
    UInt32  packed;         /* zip: bytes of compressed data                */
    UInt32  offset;         /* zip: where its local header starts           */
} CompEntry;

struct ArcCompJob {
    int        fmt;
    int        keepPaths;
    int        recurse;
    int        clearArc;    /* take the A bit off what went in              */

    char     **items;      /* as given to ArcCompAdd                       */
    int        nItems;

    char     **bases;       /* full folder paths, each ending in '\'        */
    int        nBases;

    char     **excl;        /* exclusion patterns                           */
    int        nExcl;

    CompEntry *ents;
    UInt32     nEnts;
    UInt32     capEnts;
    int        scanned;

    UInt32    *order;       /* the order the writer goes through ents in    */
    UInt32     nOrder;

    char       outFull[SZ_MAX_NAME];   /* the archive's own full path       */
    UInt32     totalBytes;             /* of everything to be read          */

    /* Results. */
    int        nFiles;
    UInt32     inBytes;
    UInt32     outBytes;
    int        nDup, nUnread, nLong, nBig, nMissing;
    char       problem[SZ_MAX_NAME];
    UInt32     dictSize;

    /* Progress, while a run is going. */
    ArcCompProgress prog;
    void      *user;
    UInt32     doneBytes;
    int        cancelled;
};

/* The name an entry is stored under: its whole relative path, or just the
 * last part of it in a flat archive.  Points into the entry. */
const char *CompStoredName( ArcCompJob *j, const CompEntry *e );

/* The entry's file on disk, built into 'dst'. */
void   CompSourcePath( ArcCompJob *j, const CompEntry *e, char *dst, int size );

/* Open an entry's file for reading; NULL (and the entry marked CS_SKIPPED,
 * and counted, and named in ArcCompProblem if it is the first) when it is
 * not there any more or cannot be opened. */
FILE  *CompOpenSource( ArcCompJob *j, CompEntry *e );

/* Report progress; returns 0 when the user has asked to stop (and remembers
 * that, so every later call says 0 too). */
int    CompProgress( ArcCompJob *j, int index, int count, const char *name );

/* Note the first problem file. */
void   CompSetProblem( ArcCompJob *j, const char *name );

/* Decide CK_* (and the text flag) from the first bytes of a file. */
int    CompSniff( const Byte *buf, UInt32 len, int *isText );

/* Writers.  Each writes the whole archive to 'f', which is open for binary
 * writing and positioned at 0, and fills j->outBytes.  SZ_OK or SZ_ERR_*. */
int    ZipWriteArchive( ArcCompJob *j, FILE *f );
int    SzWriteArchive( ArcCompJob *j, FILE *f );

/* Read buffer size for file data. */
#define COMP_IOBUF  32768U

#endif /* ARCCOMPI_H */
