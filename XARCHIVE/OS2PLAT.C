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
#define INCL_DOSDATETIME      /* DosGetDateTime, for the archive writer */
#include <os2.h>
#include <stdlib.h>

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

/* Change only the attribute byte.  The dates and times go back as zero,
 * which DosSetPathInfo reads as "leave this one as it is", and the directory
 * bit never goes back at all: it is the file system's to set, not ours. */
static void ApplyAttr( const char *path, ULONG attr, ULONG keep )
{
    FILESTATUS3 fs;

    if ( DosQueryPathInfo( (PCSZ)path, FIL_STANDARD, &fs, sizeof( fs ) ) != 0 )
        return;
    memset( &fs.fdateCreation, 0, sizeof( FDATE ) );
    memset( &fs.ftimeCreation, 0, sizeof( FTIME ) );
    memset( &fs.fdateLastAccess, 0, sizeof( FDATE ) );
    memset( &fs.ftimeLastAccess, 0, sizeof( FTIME ) );
    memset( &fs.fdateLastWrite, 0, sizeof( FDATE ) );
    memset( &fs.ftimeLastWrite, 0, sizeof( FTIME ) );
    fs.attrFile = ( fs.attrFile & keep ) | attr;
    DosSetPathInfo( (PCSZ)path, FIL_STANDARD, &fs, sizeof( fs ), 0 );
}

void SetFileDosAttr( const char *path, DWORD attr, int isDir )
{
    if ( isDir )
    {
        attr &= FILE_HIDDEN | FILE_SYSTEM;
        if ( attr == 0 ) return;
        ApplyAttr( path, attr, FILE_READONLY | FILE_ARCHIVED );
    }
    else
        ApplyAttr( path, attr & ( FILE_READONLY | FILE_HIDDEN | FILE_SYSTEM |
                                  FILE_ARCHIVED ), 0 );
}

void ClearFileAttr( const char *path )
{
    FILESTATUS3 fs;

    if ( DosQueryPathInfo( (PCSZ)path, FIL_STANDARD, &fs, sizeof( fs ) ) != 0 )
        return;
    if ( !( fs.attrFile & ( FILE_READONLY | FILE_HIDDEN | FILE_SYSTEM ) ) ) return;
    ApplyAttr( path, 0, FILE_ARCHIVED );
}

void ClearArchiveBit( const char *path )
{
    FILESTATUS3 fs;

    if ( DosQueryPathInfo( (PCSZ)path, FIL_STANDARD, &fs, sizeof( fs ) ) != 0 )
        return;
    if ( !( fs.attrFile & FILE_ARCHIVED ) ) return;
    ApplyAttr( path, 0, FILE_READONLY | FILE_HIDDEN | FILE_SYSTEM );
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

/*---- Reading a folder (see PLATFORM.H) ------------------------------------ *
 * OS/2 keeps local time in DOS-packed FDATE/FTIME, so the zip form drops
 * straight out; the 7z form is that local time moved to UTC by the zone in
 * DosGetDateTime (minutes WEST of UTC, 0xFFFF = not set, taken as UTC) -
 * the inverse of COMPAT.C's FileTimeToLocalFileTime, which is what the
 * extractor applies on the way back.
 *--------------------------------------------------------------------------- */
#define FIND_ALL ( FILE_READONLY | FILE_HIDDEN | FILE_SYSTEM | \
                   FILE_DIRECTORY | FILE_ARCHIVED )

typedef struct {
    HDIR         hdir;
    FILEFINDBUF3 fb;
} PlatFindState;

static void PlatFill( const FILEFINDBUF3 *fb, PlatFind *f )
{
    SYSTEMTIME st;
    DATETIME   dt;
    WORD       date, time;

    strncpy( f->name, fb->achName, PLAT_NAME_MAX - 1 );
    f->name[PLAT_NAME_MAX - 1] = '\0';
    f->attr = (DWORD)( fb->attrFile & 0x3F );
    f->size = ( fb->attrFile & FILE_DIRECTORY ) ? 0 : (DWORD)fb->cbFile;
    f->tooBig = ( f->size >= 0x80000000UL );
    memcpy( &date, &fb->fdateLastWrite, sizeof( WORD ) );
    memcpy( &time, &fb->ftimeLastWrite, sizeof( WORD ) );
    f->dosDate = date;
    f->dosTime = time;

    st.wYear   = (WORD)( ( ( date >> 9 ) & 0x7F ) + 1980 );
    st.wMonth  = (WORD)( ( date >> 5 ) & 0x0F );
    st.wDay    = (WORD)( date & 0x1F );
    st.wHour   = (WORD)( ( time >> 11 ) & 0x1F );
    st.wMinute = (WORD)( ( time >> 5 ) & 0x3F );
    st.wSecond = (WORD)( ( time & 0x1F ) * 2 );
    st.wMilliseconds = 0;
    st.wDayOfWeek    = 0;
    if ( st.wMonth < 1 ) st.wMonth = 1;
    if ( st.wDay < 1 )   st.wDay = 1;
    SystemTimeToFileTime( &st, &f->mtime );

    if ( DosGetDateTime( &dt ) == NO_ERROR && (USHORT)dt.timezone != 0xFFFF )
    {
        unsigned long long t = ( (unsigned long long)f->mtime.dwHighDateTime << 32 )
                             | f->mtime.dwLowDateTime;
        t += (long long)(SHORT)dt.timezone * 60LL * 10000000LL;
        f->mtime.dwLowDateTime  = (DWORD)( t & 0xFFFFFFFFULL );
        f->mtime.dwHighDateTime = (DWORD)( t >> 32 );
    }
}

void *PlatFindFirst( const char *pattern, PlatFind *f )
{
    PlatFindState *s = (PlatFindState *)malloc( sizeof( PlatFindState ) );
    ULONG count = 1;

    if ( !s ) return NULL;
    s->hdir = HDIR_CREATE;
    if ( DosFindFirst( (PCSZ)pattern, &s->hdir, FIND_ALL, &s->fb,
                       sizeof( s->fb ), &count, FIL_STANDARD ) != NO_ERROR ||
         count == 0 )
    {
        free( s );
        return NULL;
    }
    PlatFill( &s->fb, f );
    return s;
}

int PlatFindNext( void *h, PlatFind *f )
{
    PlatFindState *s = (PlatFindState *)h;
    ULONG count = 1;

    if ( DosFindNext( s->hdir, &s->fb, sizeof( s->fb ), &count ) != NO_ERROR ||
         count == 0 )
        return 0;
    PlatFill( &s->fb, f );
    return 1;
}

void PlatFindClose( void *h )
{
    PlatFindState *s = (PlatFindState *)h;
    if ( !s ) return;
    DosFindClose( s->hdir );
    free( s );
}

/* Physical memory not locked down by the system: QSV_TOTPHYSMEM less
 * QSV_TOTRESMEM.  Not "free" in the strict sense - OS/2 will page other
 * programs out to make room - but it is the most that a compression job can
 * touch without the swapper becoming part of every match it looks up. */
DWORD PlatPhysFree( void )
{
    ULONG v[2];

    if ( DosQuerySysInfo( QSV_TOTPHYSMEM, QSV_TOTRESMEM, v, sizeof( v ) ) != NO_ERROR )
        return 0;
    return ( v[0] > v[1] ) ? (DWORD)( v[0] - v[1] ) : 0;
}
