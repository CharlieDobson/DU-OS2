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
#define INCL_DOSMISC          /* DosQuerySysInfo, for the memory budget */
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

/*---- 8.3 filesystem probe (for ArcFsName's name mangling) -----------------
 * Called by the shared ARCFILE.C (extern under #ifdef __OS2__) at the start
 * of every extraction: 1 when the drive holding 'path' takes only 8.3 names
 * (FAT), 0 when it takes long names (HPFS, and anything else that is not
 * FAT - JFS, NFS, CDFS all allow long names).  A NULL or relative path means
 * the current drive.  On any query failure the answer is 1: mangled names
 * are legal everywhere, long names on FAT are not.
 *-------------------------------------------------------------------------- */
int Os2NamesNeed83( const char *path )
{
    union {
        FSQBUFFER2 fsq;
        char       pad[sizeof( FSQBUFFER2 ) + 3 * CCHMAXPATH];
    } buf;
    ULONG cb = sizeof( buf );
    char  drive[3];
    char *fsName;

    if ( path && path[0] && path[1] == ':' )
        drive[0] = path[0];
    else
    {
        ULONG ulDrive = 0, ulMap = 0;
        if ( DosQueryCurrentDisk( &ulDrive, &ulMap ) != NO_ERROR )
            return 1;
        drive[0] = (char)( 'A' + ulDrive - 1 );
    }
    drive[1] = ':';
    drive[2] = '\0';

    memset( &buf, 0, sizeof( buf ) );
    if ( DosQueryFSAttach( (PCSZ)drive, 0, FSAIL_QUERYNAME,
                           &buf.fsq, &cb ) != NO_ERROR )
        return 1;

    /* szName holds the drive; the attached filesystem's name follows it. */
    fsName = (char *)buf.fsq.szName + buf.fsq.cbName + 1;
    return ( stricmp( fsName, "FAT" ) == 0 );
}

/*---- How much memory is going spare --------------------------------------- *
 * Called by the shared ARCFILE.C (extern under #ifdef __OS2__) to bound the
 * heap probe that sets the extractor's in-RAM budget.  Without an answer that
 * probe would ask malloc for hundreds of megabytes, and OS/2 would very
 * likely SAY YES - growing the swap file to cover it - which is worse than
 * refusing, because the archive then extracts at the speed of the disk.
 *
 *   QSV_TOTAVAILMEM  free physical memory plus what the swapper can still
 *                    hand out: the "will this thrash" number.
 *   QSV_MAXPRMEM     the largest single private allocation possible: the
 *                    "can one malloc even be this big" number.
 *
 * The smaller of the two, since the dictionary has to satisfy both.  0 on any
 * failure (OS/2 2.0 does not answer index 19), which leaves the caller to
 * probe unaided exactly as before.
 *-------------------------------------------------------------------------- */
unsigned int Os2MemFree( void )
{
    ULONG v[QSV_MAXPRMEM - QSV_TOTPHYSMEM + 1];
    ULONG avail, maxpr;

    memset( v, 0, sizeof( v ) );
    if ( DosQuerySysInfo( QSV_TOTPHYSMEM, QSV_MAXPRMEM,
                          v, sizeof( v ) ) != NO_ERROR )
        return 0;

    avail = v[QSV_TOTAVAILMEM - QSV_TOTPHYSMEM];
    maxpr = v[QSV_MAXPRMEM    - QSV_TOTPHYSMEM];

    if ( avail == 0 ) avail = maxpr;
    if ( maxpr == 0 ) maxpr = avail;
    if ( avail == 0 ) return 0;
    return (unsigned int)( ( avail < maxpr ) ? avail : maxpr );
}

/*---- Win32-only shell helpers, kept as no-ops ---------------------------- */
int  IsModernShell( void )            { return 0; }
void InitCtl3d( HINSTANCE hInst )     { (void)hInst; }
void CleanupCtl3d( HINSTANCE hInst )  { (void)hInst; }
