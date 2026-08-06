/*===========================================================================
 * OS2PLAT.C  -  OS/2 replacement for PLATFORM.C (Open Watcom, -bt=os2).
 * Compiled INSTEAD OF the Win32s PLATFORM.C; declares the same platform.h
 * interface so the shared backends need no change.
 *
 * Timestamp restore goes through DosSetPathInfo / FIL_STANDARD.  OS/2's FDATE
 * and FTIME are bit-fields with exactly the MS-DOS packing (day:5 month:4
 * year:7 / twosecs:5 minutes:6 hours:5), so the DOS-packed stamps that zip,
 * RAR and FAT already carry drop straight in; 7z and RAR5 carry a UTC
 * FILETIME, which is converted to local time and decomposed first.
 *
 * IsModernShell / InitCtl3d / CleanupCtl3d are Win32-only notions (Program
 * Manager detection and the CTL3D32 3-D control look).  PM controls are
 * already three-dimensional and there is no shell to sniff, so they survive
 * here only as no-ops that keep platform.h a single shared header - the PM
 * front end never calls them.
 *===========================================================================*/
#define INCL_DOSFILEMGR
#define INCL_DOSERRORS
#include <os2.h>

#include "platform.h"    /* pulls in the local windows.h shim */

/* Apply an MS-DOS packed date/time to a file's last-write stamp.  The rest of
 * the FILESTATUS3 has to be valid, so it is read back first and only the two
 * last-write fields are altered. */
static void ApplyStamp( const char *path, WORD dosDate, WORD dosTime )
{
    FILESTATUS3 fs;

    if ( DosQueryPathInfo( (PCSZ)path, FIL_STANDARD, &fs, sizeof( fs ) ) != 0 )
        return;

    memcpy( &fs.fdateLastWrite, &dosDate, sizeof( FDATE ) );
    memcpy( &fs.ftimeLastWrite, &dosTime, sizeof( FTIME ) );

    DosSetPathInfo( (PCSZ)path, FIL_STANDARD, &fs, sizeof( fs ), 0 );
}

void SetFileDosMTime( const char *path, WORD dosDate, WORD dosTime )
{
    if ( dosDate == 0 && dosTime == 0 )
        return;
    ApplyStamp( path, dosDate, dosTime );
}

void SetFileMTime( const char *path, const FILETIME *ft )
{
    FILETIME   local;
    SYSTEMTIME st;
    WORD       date, time;

    /* 7z / RAR5 store UTC; the file system records local time. */
    if ( !FileTimeToLocalFileTime( ft, &local ) )
        return;
    if ( !FileTimeToSystemTime( &local, &st ) )
        return;
    if ( st.wYear < 1980 || st.wYear > 2107 )   /* outside the FAT/HPFS epoch */
        return;

    date = (WORD)( ( ( st.wYear - 1980 ) << 9 ) |
                   ( st.wMonth << 5 ) | st.wDay );
    time = (WORD)( ( st.wHour << 11 ) |
                   ( st.wMinute << 5 ) | ( st.wSecond >> 1 ) );
    ApplyStamp( path, date, time );
}

/*---- Win32-only shell helpers, kept as no-ops ---------------------------- */
int  IsModernShell( void )            { return 0; }
void InitCtl3d( HINSTANCE hInst )     { (void)hInst; }
void CleanupCtl3d( HINSTANCE hInst )  { (void)hInst; }
