/*===========================================================================
 * ARCCOMP.H  -  Making an archive: the front door every front end calls
 * Target: MSVC 2.2  Win32s
 *
 * The other half of ARCFILE.H.  A front end names what to put in (files,
 * folders, wildcards), what to leave out, and whether folder names are kept;
 * this finds the files, decides how each one should be packed, and writes a
 * .zip or a .7z:
 *
 *   ZIP   PKZIP 2.04g's own format and its -ex ("maximum") compression, so
 *         PKUNZIP 2.04g and everything since can read the result.  Each file
 *         is deflated, and stored instead when deflating would not make it
 *         smaller - which is decided both before (a file that is already
 *         compressed is recognised and not tried) and after (one that came
 *         out bigger is written again, stored).
 *
 *   7Z    A SOLID archive: all the compressible files in one LZMA stream, so
 *         a match can reach back into the file before.  The dictionary is as
 *         large as this machine can afford (and no larger than the files),
 *         which is what makes the archive good - and what decides how much
 *         memory the machine extracting it will need.  Executables go into a
 *         stream of their own with the x86 branch filter in front of LZMA;
 *         files that are already compressed are stored, not squeezed again.
 *
 * Attributes and modification times always go in.  Long names go in where
 * the file system has them (Win32, OS/2); DOS has 8.3 names, and those are
 * what a DOS-made archive holds.
 *
 * USE
 *
 *     ArcCompJob *j = ArcCompCreate();
 *     ArcCompSetFormat( j, ARC_CF_7Z );          or ArcCompFormatFromName
 *     ArcCompSetPaths( j, 1 );
 *     ArcCompSetClearArchive( j, 1 );            the A bit off afterwards
 *     ArcCompExclude( j, "*.BAK;@C:\\SKIP.TXT" );
 *     ArcCompAdd( j, "C:\\WORK\\SRC" );          a folder: all of it
 *     ArcCompAdd( j, "C:\\WORK\\*.TXT" );        a wildcard in one folder
 *     rc = ArcCompScan( j, "C:\\OUT\\WORK.7Z", prog, user );
 *     rc = ArcCompRun( j, "C:\\OUT\\WORK.7Z", prog, user );
 *     ...ArcCompFiles / ArcCompInBytes / ArcCompOutBytes / ArcCompSkipped...
 *     ArcCompFree( j );
 *
 * The archive is written to a temporary file beside the target and renamed
 * only when it is complete, so a cancelled or failed run leaves whatever was
 * there before untouched.  An archive already at the target is replaced: ask
 * the user before calling ArcCompRun.  The archive being written is never
 * put inside itself, even when it sits in a folder being archived.
 *===========================================================================*/

#ifndef ARCCOMP_H
#define ARCCOMP_H

#include "arcdefs.h"

#define ARC_CF_ZIP   1
#define ARC_CF_7Z    2

typedef struct ArcCompJob ArcCompJob;

/* Progress, and the way to cancel: return 0 and the job stops with
 * SZ_ERR_CANCEL.
 *
 *   fileIndex, fileCount  the entry being worked on, and how many there are.
 *                         During ArcCompScan fileCount is ARC_COMP_SCANNING:
 *                         the files are still being found, and fileIndex is
 *                         how many so far.  After the archive is made, while
 *                         the Archive attribute comes off the files in it
 *                         (ArcCompSetClearArchive), it is ARC_COMP_CLEARING
 *                         - and returning 0 then changes nothing, since the
 *                         archive is already made.
 *   name                  that entry's name as stored in the archive.
 *   doneKB, totalKB       input read so far, and in all (0 while scanning).
 *                         Kilobytes so a front end can multiply by 100
 *                         without overflowing. */
#define ARC_COMP_SCANNING  (-1)
#define ARC_COMP_CLEARING  (-2)

typedef int (*ArcCompProgress)( void *user, int fileIndex, int fileCount,
                                const char *name,
                                UInt32 doneKB, UInt32 totalKB );

ArcCompJob *ArcCompCreate( void );           /* NULL when out of memory */
void        ArcCompFree( ArcCompJob *j );

/* .7z means 7z; anything else means zip.  For a front end that offers both
 * and lets the name decide. */
int  ArcCompFormatFromName( const char *archivePath );
const char *ArcCompFormatName( int fmt );    /* "Zip" / "7z" */

void ArcCompSetFormat( ArcCompJob *j, int fmt );

/* 1 (the default): each entry keeps its folders, from the folder of the item
 * that was named.  0: a FLAT archive - names only, no folders stored, and a
 * second file with a name already used is left out (and counted). */
void ArcCompSetPaths( ArcCompJob *j, int keepPaths );

/* With a wildcard item, look for matches in every folder below it too, the
 * way PKZIP's -r does.  Off by default: a named folder is always taken whole
 * anyway, and so is a folder a wildcard matches. */
void ArcCompSetRecurse( ArcCompJob *j, int recurse );

/* Take the Archive attribute off each file that went in, once the archive
 * is complete - the way a backup marks what it has saved, so the next one
 * can take only the files changed since (XCOPY /A and /M go by it).  Off by
 * default.  Folders and files left out keep theirs, and so does every file
 * when the run fails or is cancelled. */
void ArcCompSetClearArchive( ArcCompJob *j, int clear );

/* Something to leave out.  'spec' may hold several, separated by ';':
 *
 *   NAME.EXT, *.BAK, DATA??.*   matched against each file's and each
 *                               folder's own name, at any depth.  A folder
 *                               left out takes everything in it along.
 *   SRC\*.OBJ                   with a '\' (or '/'), matched against the
 *                               path inside the archive.
 *   C:\TEMP\*                   with a drive or a leading '\', matched
 *                               against the file's full path.
 *   @LIST.TXT                   every line of LIST.TXT is one of the above.
 *                               Blank lines and lines starting ';' are
 *                               skipped, and so is anything after " ;" -
 *                               the PKZIP list-file rules.
 *
 * Case is ignored, and a name with no '.' matches "*.*" as DOS would have it.
 * Returns SZ_OK, SZ_ERR_OPEN when a list file cannot be read (ArcCompProblem
 * names it), or SZ_ERR_MEMORY. */
int  ArcCompExclude( ArcCompJob *j, const char *spec );

/* Something to put in: a file, a folder (with everything under it), or a
 * wildcard in a folder (the files AND folders it matches).  Relative paths
 * are taken from the current directory.  Nothing is looked at until
 * ArcCompScan.  SZ_OK or SZ_ERR_MEMORY. */
int  ArcCompAdd( ArcCompJob *j, const char *path );

/* Find everything, apply the exclusions, and - for a 7z - look at the start
 * of each file to decide how it should be packed.  'archivePath' is the
 * archive to be written, so that it can be kept out of itself.
 *
 * SZ_OK, SZ_ERR_CANCEL, SZ_ERR_MEMORY, SZ_ERR_NOFILES (nothing to archive -
 * ArcCompMissing says whether that was because named items were not there),
 * or SZ_ERR_TOOBIG (more than a zip can list, or more data than this program
 * can write in one archive). */
int  ArcCompScan( ArcCompJob *j, const char *archivePath,
                  ArcCompProgress prog, void *user );

/* Entries found by ArcCompScan, and their total size. */
int    ArcCompCount( ArcCompJob *j );
UInt32 ArcCompTotalKB( ArcCompJob *j );

/* Write the archive (running ArcCompScan first if it has not been).
 * SZ_OK, SZ_ERR_CANCEL, SZ_ERR_WRITE (the archive could not be created or
 * the disk filled), SZ_ERR_READ (a file could not be read part way through -
 * ArcCompProblem names it), SZ_ERR_MEMORY / SZ_ERR_NORAM, or whatever
 * ArcCompScan returned. */
int  ArcCompRun( ArcCompJob *j, const char *archivePath,
                 ArcCompProgress prog, void *user );

/* What happened. */
int    ArcCompFiles( ArcCompJob *j );        /* entries written           */
UInt32 ArcCompInBytes( ArcCompJob *j );      /* bytes of file data read   */
UInt32 ArcCompOutBytes( ArcCompJob *j );     /* size of the archive       */

/* Entries left out, by reason: a stored name already taken (flat archives,
 * or the same name from two places), a file that vanished or could not be
 * opened by the time it was read, a name too long for an archive entry, and
 * a file of 2 GB or more. */
int  ArcCompSkipped( ArcCompJob *j );        /* all of the below          */
int  ArcCompDuplicates( ArcCompJob *j );
int  ArcCompUnreadable( ArcCompJob *j );
int  ArcCompTooLong( ArcCompJob *j );
int  ArcCompTooBig( ArcCompJob *j );

/* Items named with ArcCompAdd that matched nothing at all. */
int  ArcCompMissing( ArcCompJob *j );

/* The first file (or list file, or missing item) that caused a problem, or
 * "" - for the message that goes with an error or a skipped count. */
const char *ArcCompProblem( ArcCompJob *j );

/* The dictionary a 7z was (or would be) written with, in bytes, and what
 * extracting it will need.  0 until known. */
UInt32 ArcCompDictSize( ArcCompJob *j );

#endif /* ARCCOMP_H */
