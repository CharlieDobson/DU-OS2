/*===========================================================================
 * PLATFORM.H  -  Shell detection and CTL3D32 management
 * Target: MSVC 2.2  Win32s
 *===========================================================================*/

#ifndef PLATFORM_H
#define PLATFORM_H

#include <windows.h>

/* TRUE on the Explorer shell (Windows 9x / NT 4+), FALSE on Program Manager
 * shells (Win32s on Windows 3.1x, Windows NT 3.x). */
int IsModernShell( void );

/* Load and register CTL3D32 for the 3-D control look, but only on the
 * Program Manager shells that need it (Windows 3.1x / NT 3.x).  No-op on
 * Windows 9x / NT 4+, which draw 3-D controls natively.  Absence of the DLL
 * is not an error. */
void InitCtl3d( HINSTANCE hInst );
void CleanupCtl3d( HINSTANCE hInst );

/* Set a file's last-modified time.  'ft' is a UTC FILETIME (the form 7z stores
 * directly).  Silently does nothing on failure. */
void SetFileMTime( const char *path, const FILETIME *ft );

/* Set a file's last-modified time from an MS-DOS packed date/time (as zip and
 * RAR store it, in local time).  No-op when both are zero. */
void SetFileDosMTime( const char *path, WORD dosDate, WORD dosTime );

/* Give an extracted file the DOS attributes its archive recorded - 01 R,
 * 02 H, 04 S, 20 A, anything else ignored - exactly, so a file stored
 * without the archive bit comes out without it.  A folder takes only H and
 * S, and one with neither is left alone.  Call it LAST: the time of a file
 * that is already read-only can no longer be set.  Silently does nothing on
 * failure. */
void SetFileDosAttr( const char *path, DWORD attr, int isDir );

/* Take R, H and S off a file that is about to be written over, so that it
 * can be.  Read-only stops a write everywhere, and Windows NT also refuses
 * to recreate a hidden or system file.  No-op when none are set. */
void ClearFileAttr( const char *path );

/* Take the archive bit off a file that has just gone into an archive, and
 * leave R, H and S as they are.  Silently does nothing when the bit is not
 * set or cannot be changed (a write-protected disk, a CD-ROM). */
void ClearArchiveBit( const char *path );

/*---- Reading a folder, for the archive writer ------------------------------ *
 * One entry of a directory search, with everything the writer needs taken
 * from the search itself - so a folder of a thousand files costs a thousand
 * directory reads and not a thousand opens.  Each system answers in its own
 * terms (Win32 keeps UTC FILETIMEs, DOS and OS/2 keep local DOS-packed
 * stamps) and the implementation fills in the other form:
 *
 *   PLATFORM.C (Win32)  FindFirstFile; long names; FILETIME -> local DOS.
 *   DOSPLAT.C  (DOS)    _dos_findfirst; 8.3 names; DOS stamp -> FILETIME
 *                       with no time-zone shift, which is exactly what the
 *                       extractor assumes on the way back out (COMPAT.C).
 *   OS2PLAT.C  (OS/2)   DosFindFirst; long names on HPFS; local DOS stamp ->
 *                       UTC through the time zone DosGetDateTime reports.
 *--------------------------------------------------------------------------- */
#define PLAT_NAME_MAX  260

typedef struct {
    char     name[PLAT_NAME_MAX];  /* the entry's own name, long where the
                                      volume has long names                 */
    DWORD    attr;                 /* DOS bits: 01 R  02 H  04 S  10 D  20 A */
    DWORD    size;                 /* bytes; 0 for a folder                  */
    int      tooBig;               /* 2 GB or more: past what the archive
                                      writer can seek in (a 'long' offset)  */
    WORD     dosDate;              /* last write, local time, MS-DOS packed  */
    WORD     dosTime;
    FILETIME mtime;                /* last write, UTC                        */
} PlatFind;

/* Search 'pattern' - a path, usually ending in "*.*".  Returns a handle with
 * the first entry in *f, or NULL when nothing matched.  Volume labels are
 * never returned; "." and ".." are, and the caller skips them.  Hidden and
 * system files ARE returned: an archive keeps every attribute. */
void *PlatFindFirst( const char *pattern, PlatFind *f );
int   PlatFindNext( void *h, PlatFind *f );           /* 1 = another entry */
void  PlatFindClose( void *h );

/* Physical memory a long job can use without being paged, or 0 when the
 * system cannot say - or has no paging to worry about, which is DOS.  The
 * archive writer keeps its LZMA dictionary under this, because the match
 * finder reads all over its tables and a paged one runs at disk speed. */
DWORD PlatPhysFree( void );

#endif /* PLATFORM_H */
