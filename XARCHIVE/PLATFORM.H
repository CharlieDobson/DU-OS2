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

#endif /* PLATFORM_H */
