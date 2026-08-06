/*===========================================================================
 * windows.h  -  OS/2 compatibility shim (Open Watcom 1.9, -bt=os2)
 *
 * This is NOT the real windows.h.  A -bt=os2 build has no windows.h anywhere
 * on its include path, so this file is what the copied format backends pick
 * up when they say  #include <windows.h>  -- which is exactly why those
 * backends (ARCFILE / SZARC / ZIPARC / RARARC / RAR3DEC / RAR5ARC / DISKARC /
 * LZMADEC / CRC32) are byte-identical to their Win32s originals in
 * the Win32s XArchive tree and must stay that way.  The DOS/32 build uses the
 * same trick with a shim of its own.
 *
 * The Win32 surface those backends actually touch is tiny and was measured,
 * not guessed:
 *
 *      lstrcpyn  lstrcpy  lstrcat  lstrlen  wsprintf
 *      WideCharToMultiByte                (7z stores UTF-16 names)
 *      SystemTimeToFileTime
 *      FileTimeToSystemTime
 *      FileTimeToLocalFileTime
 *      FILETIME  SYSTEMTIME  WORD  WCHAR  LPSTR  CP_ACP
 *
 * The string helpers map onto the C run-time; the rest is implemented in
 * COMPAT.C.
 *
 * os2def.h is included first, deliberately.  It is pure typedefs with its own
 * include guard, and pulling it in here means BOOL / TRUE / FALSE / NULL come
 * from OS/2 and cannot clash with os2.h in a translation unit (such as
 * OS2PLAT.C) that needs both this shim and the real PM headers.
 *===========================================================================*/
#ifndef XA_OS2_WINDOWS_SHIM_H
#define XA_OS2_WINDOWS_SHIM_H

#include <os2def.h>      /* BOOL, TRUE/FALSE, NULL -- the OS/2 definitions */
#include <stdio.h>       /* sprintf  */
#include <string.h>      /* strcpy, strcat, strlen */

/*---- Win32 integer / handle types not covered by os2def.h ---------------- */
typedef unsigned long   DWORD;
typedef unsigned short  WORD;
typedef unsigned short  WCHAR;
typedef char           *LPSTR;
typedef const char     *LPCSTR;
typedef void           *HINSTANCE;

#define CP_ACP 0

/*---- FILETIME / SYSTEMTIME ------------------------------------------------
 * Layouts match Win32's: 7z and RAR5 timestamps are read straight out of the
 * archive into a FILETIME, so the field order matters.
 *-------------------------------------------------------------------------- */
typedef struct {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

typedef struct {
    WORD wYear, wMonth, wDayOfWeek, wDay;
    WORD wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

/*---- String helpers (mapped to the C runtime) ---------------------------- */
char *xa_lstrcpyn( char *dst, const char *src, int count );

#define lstrcpyn   xa_lstrcpyn
#define lstrcpy(d,s)  strcpy((d),(s))
#define lstrcat(d,s)  strcat((d),(s))
#define lstrlen(s)    ((int)strlen(s))
#define wsprintf      sprintf

/*---- The handful of Win32 calls the backends make ------------------------ */
int  WideCharToMultiByte( unsigned cp, DWORD flags,
                          const WCHAR *wstr, int wlen,
                          char *dst, int dstBytes,
                          const char *defChar, int *usedDef );

BOOL SystemTimeToFileTime( const SYSTEMTIME *st, FILETIME *ft );
BOOL FileTimeToSystemTime( const FILETIME *ft, SYSTEMTIME *st );
BOOL FileTimeToLocalFileTime( const FILETIME *ft, FILETIME *lft );

#endif /* XA_OS2_WINDOWS_SHIM_H */
