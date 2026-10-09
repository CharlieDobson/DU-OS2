/*===========================================================================
 * SETUP.CPP  -  OS/2 installer for the Dobson Utilities
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * Flow:   Welcome (drive/directory, validation)
 *      -> Progress (ExtractZip per file, RemoveObsoleteFiles, ValidateInstall,
 *                   desktop folder + program objects)
 *      -> Options (Enhanced Notepad on the Open menu of .TXT files)
 *      -> Complete
 *
 * The same four pages as the Win32s installer (S:\CPP\DU32\SETUP), driven
 * by the same two files beside the EXE: SETUP.INF (what to install, what to
 * call it) and DU.CWD (the PKZIP 2.04g payload).
 *
 * What is different is where the shortcuts go.  The Workplace Shell has no
 * Program Manager group and no .LNK files; it has objects.  Setup creates a
 * folder (class WPFolder) on the desktop and one program object (class
 * WPProgram) in it per [Shortcuts] line, through WinCreateObject.  A program
 * object is exactly what a user gets by dragging "Program" out of the
 * Templates folder and filling in the path: a real object that runs the EXE,
 * not a shadow of the file.  Its title is the descriptive name from the INF.
 *
 * Every object is given an OBJECTID so a second run finds the first run's
 * objects instead of adding to them.  The folder is <DU_OS2_FOLDER> and is
 * updated in place, so whatever the user put in it stays; each program
 * object is <DU_OS2_name> (from its EXE's base name) and is replaced
 * outright, which is what the Windows installers do by deleting and
 * re-adding the item.  [RemovedShortcuts] therefore lists EXE names here
 * rather than captions: the ID is built from the name and the object looked
 * up by it.
 *===========================================================================*/

#define INCL_WIN
#define INCL_WINWORKPLACE
#define INCL_DOSFILEMGR
#define INCL_DOSMISC
#define INCL_DOSPROCESS
#define INCL_DOSMODULEMGR     /* DosQueryModuleName                        */
#define INCL_DOSERRORS
#include <os2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "unzip.hpp"        /* ExtractZip                                 */
#include "insttool.hpp"     /* RemoveObsoleteFiles, ValidateInstall       */
#include "iniparse.hpp"     /* GetIniKeyString, GetIniKeyUInt             */
#include "res/resource.h"

/* WinOpenObject's default view.  Watcom's pmwp.h declares the function but
 * not this value. */
#ifndef OPEN_DEFAULT
#define OPEN_DEFAULT  0
#endif

#define MAXPATH       CCHMAXPATH
#define WMU_START     ( WM_USER + 1 )     /* progress page: begin the work  */

#define APP_TITLE     "Dobson Utilities Setup"
#define BAR_CLASS     "DUSetupBar"

/* Workplace Shell object IDs.  OBJID_PROGRAM_FMT takes the EXE base name. */
#define OBJID_FOLDER       "<DU_OS2_FOLDER>"
#define OBJID_PROGRAM_FMT  "<DU_OS2_%s>"
#define OBJID_ENHNOTE      "<DU_OS2_ENHNOTE>"

/*---- Globals -------------------------------------------------------------*/
static HAB     g_hab;
static char    g_modulePath[MAXPATH];    /* full path to this EXE          */
static char    g_infPath[MAXPATH];       /* full path to SETUP.INF         */
static char    g_cwdPath[MAXPATH];       /* full path to DU.CWD payload    */
static char    g_destPath[MAXPATH];      /* chosen install directory       */
static char    g_defDrive;               /* proposed drive letter ('C')    */
static char    g_defFolder[MAXPATH];     /* proposed directory, no drive   */
static char    g_groupName[128];         /* title of the desktop folder    */
static BOOL    g_bCancel;                /* set when user cancels progress */
static HOBJECT g_hFolder;                /* the desktop folder, once made  */
static HWND    g_hwndBar;                /* progress bar window            */
static int     g_barPercent;
static HSWITCH g_hswitch;                /* Window List entry of the page  */

/*===========================================================================
 * Helpers
 *===========================================================================*/

static ULONG Message( HWND owner, const char* text, ULONG style )
{
    return WinMessageBox( HWND_DESKTOP, owner, (PCSZ)text, (PCSZ)"Setup", 0,
                          style | MB_MOVEABLE );
}

/* Centre a window on the screen. */
static void CenterWindow( HWND hwnd )
{
    SWP  swp;
    LONG screenW, screenH;

    if ( !WinQueryWindowPos( hwnd, &swp ) ) return;
    screenW = WinQuerySysValue( HWND_DESKTOP, SV_CXSCREEN );
    screenH = WinQuerySysValue( HWND_DESKTOP, SV_CYSCREEN );

    WinSetWindowPos( hwnd, NULLHANDLE, ( screenW - swp.cx ) / 2,
                     ( screenH - swp.cy ) / 2, 0, 0, SWP_MOVE );
}

/* Pump pending messages so the dialog repaints / Cancel is seen.  A WM_QUIT
 * (the system shutting down, or the page closed from the Window List) is
 * taken as a cancel. */
static void PumpMessages( void )
{
    QMSG qmsg;

    while ( WinPeekMsg( g_hab, &qmsg, NULLHANDLE, 0, 0, PM_REMOVE ) ) {
        if ( qmsg.msg == WM_QUIT ) {
            g_bCancel = TRUE;
            continue;
        }
        WinDispatchMsg( g_hab, &qmsg );
    }
}

/* Each page is a top-level dialog, which PM does not list in the Window
 * List by itself; this puts it there so a page that drops behind another
 * window can be brought back.  Called from every page's WM_INITDLG. */
static void PageInit( HWND hwnd )
{
    SWCNTRL swc;
    PID     pid = 0;
    TID     tid = 0;

    CenterWindow( hwnd );

    memset( &swc, 0, sizeof( swc ) );
    WinQueryWindowProcess( hwnd, &pid, &tid );
    swc.hwnd          = hwnd;
    swc.idProcess     = pid;
    swc.uchVisibility = SWL_VISIBLE;
    swc.fbJump        = SWL_JUMPABLE;
    swc.bProgType     = PROG_PM;
    strcpy( swc.szSwtitle, APP_TITLE );
    g_hswitch = WinAddSwitchEntry( &swc );
}

static void PageDone( void )
{
    if ( g_hswitch != NULLHANDLE ) {
        WinRemoveSwitchEntry( g_hswitch );
        g_hswitch = NULLHANDLE;
    }
}

/* Put the program icon on a page.  The template holds an empty static where
 * the icon goes and the icon control is made here, at run time, because a
 * dialog template is all-or-nothing: a control PM declines to create fails
 * the whole WinLoadDlg, and Setup would exit without a word.  Made this way,
 * a refused icon costs the picture and nothing else.  The first attempt is
 * the standard one, a static that loads the icon itself from "#<id>"; the
 * second hands it the pointer by handle. */
static void PlaceIcon( HWND hDlg )
{
    HWND hwndOld = WinWindowFromID( hDlg, IDC_ICON );
    HWND hwndIcon;
    SWP  swp;
    char ref[16];

    if ( hwndOld == NULLHANDLE || !WinQueryWindowPos( hwndOld, &swp ) ) return;
    WinDestroyWindow( hwndOld );

    sprintf( ref, "#%d", ID_SETUP );
    hwndIcon = WinCreateWindow( hDlg, (PCSZ)WC_STATIC, (PCSZ)ref, SS_ICON | WS_VISIBLE,
                                swp.x, swp.y, swp.cx, swp.cy, hDlg, HWND_TOP,
                                IDC_ICON, NULL, NULL );
    if ( hwndIcon == NULLHANDLE ) {
        HPOINTER hptr = WinLoadPointer( HWND_DESKTOP, NULLHANDLE, ID_SETUP );

        if ( hptr != NULLHANDLE ) {
            hwndIcon = WinCreateWindow( hDlg, (PCSZ)WC_STATIC, (PCSZ)"", SS_ICON | WS_VISIBLE,
                                        swp.x, swp.y, swp.cx, swp.cy, hDlg, HWND_TOP,
                                        IDC_ICON, NULL, NULL );
            if ( hwndIcon != NULLHANDLE ) {
                WinSendMsg( hwndIcon, SM_SETHANDLE, MPFROMLONG( hptr ), 0 );
            }
        }
    }
}

/*---- Progress bar --------------------------------------------------------
 * The bar is a window of its own so it repaints from WM_PAINT; relying on
 * the dialog to redraw a static control during the extraction loop is not
 * dependable.  The dialog's IDC_GAUGE is only a placeholder for its position.
 *-------------------------------------------------------------------------*/

MRESULT EXPENTRY BarWndProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    if ( msg == WM_PAINT ) {
        RECTL rcl, done, rest;
        HPS   hps = WinBeginPaint( hwnd, NULLHANDLE, NULL );

        WinQueryWindowRect( hwnd, &rcl );
        done = rcl;
        rest = rcl;
        done.xRight = rcl.xLeft + ( rcl.xRight - rcl.xLeft ) * g_barPercent / 100;
        rest.xLeft  = done.xRight;

        if ( done.xRight > done.xLeft ) {
            WinFillRect( hps, &done, SYSCLR_HILITEBACKGROUND );
        }
        if ( rest.xRight > rest.xLeft ) {
            WinFillRect( hps, &rest, SYSCLR_BUTTONMIDDLE );
        }
        WinDrawBorder( hps, &rcl, 1, 1, SYSCLR_WINDOWFRAME, 0, DB_STANDARD );

        WinEndPaint( hps );
        return (MRESULT)FALSE;
    }
    return WinDefWindowProc( hwnd, msg, mp1, mp2 );
}

/* Move the gauge to the given percentage and paint it now. */
static void SetGauge( int pct )
{
    if ( pct < 0 ) pct = 0;
    if ( pct > 100 ) pct = 100;
    if ( pct == g_barPercent ) return;

    g_barPercent = pct;
    if ( g_hwndBar != NULLHANDLE ) {
        WinInvalidateRect( g_hwndBar, NULL, FALSE );
        WinUpdateWindow( g_hwndBar );
    }
}

/* Does the path exist (file or directory)? */
static int PathExists( const char* path )
{
    FILESTATUS3 fs;

    return DosQueryPathInfo( (PCSZ)path, FIL_STANDARD, &fs, sizeof( fs ) ) == NO_ERROR;
}

/* Create a directory tree (e.g. C:\DU-OS2\SUB).  Returns 1 if it now exists. */
static int MakeTree( const char* path )
{
    char  tmp[MAXPATH];
    char* p;

    strcpy( tmp, path );

    p = tmp;
    if ( p[0] && p[1] == ':' ) p += 2;   /* skip drive spec   */
    if ( *p == '\\' ) p++;               /* skip leading root */

    for ( ; *p; p++ ) {
        if ( *p == '\\' ) {
            *p = '\0';
            DosCreateDir( (PCSZ)tmp, NULL );
            *p = '\\';
        }
    }
    DosCreateDir( (PCSZ)tmp, NULL );

    return PathExists( path );
}

/* Free space on a drive ('A'..'Z') in KB; 0xFFFFFFFF if undeterminable.
 * Worked out in KB rather than bytes so a large HPFS or JFS volume with more
 * than 4 GB free cannot overflow and read as "not enough room". */
static unsigned long DriveFreeKB( char driveLetter )
{
    FSALLOCATE    fsa;
    unsigned long unitKB;

    if ( DosQueryFSInfo( (ULONG)( toupper( driveLetter ) - 'A' + 1 ), FSIL_ALLOC,
                         &fsa, sizeof( fsa ) ) != NO_ERROR ) {
        return 0xFFFFFFFFUL;
    }

    unitKB = (unsigned long)fsa.cSectorUnit * fsa.cbSector / 1024;
    if ( unitKB == 0 ) {
        return fsa.cUnitAvail / 2;               /* 512-byte units    */
    }
    if ( fsa.cUnitAvail > 0xFFFFFFFEUL / unitKB ) {
        return 0xFFFFFFFEUL;                     /* saturate: plenty  */
    }
    return fsa.cUnitAvail * unitKB;
}

/* Trim leading/trailing blanks in place. */
static void Trim( char* s )
{
    char* p = s;
    char* e;

    while ( *p == ' ' || *p == '\t' ) p++;
    if ( p != s ) memmove( s, p, strlen( p ) + 1 );
    e = s + strlen( s );
    while ( e > s && ( e[-1] == ' ' || e[-1] == '\t' ||
                       e[-1] == '\r' || e[-1] == '\n' ) ) {
        *--e = '\0';
    }
}

/* Read the non-blank, non-comment lines of [section] from SETUP.INF into buf
 * as a run of null-terminated strings (double-null terminated).  Returns the
 * line count.  OS/2 has no profile call for a text INI file, so this is the
 * reader for every list in the file: [Files], [Shortcuts], [RemovedShortcuts]. */
static int ReadSection( const char* section, char* buf, int size )
{
    FILE* fp;
    char  line[256];
    char  sec[64];
    int   inSection = 0, count = 0, used = 0, len;

    buf[0] = '\0';
    fp = fopen( g_infPath, "r" );
    if ( !fp ) return 0;

    sprintf( sec, "[%s]", section );

    while ( fgets( line, sizeof( line ), fp ) != NULL ) {
        Trim( line );
        if ( line[0] == '[' ) {
            inSection = ( stricmp( line, sec ) == 0 );
            continue;
        }
        if ( !inSection || line[0] == '\0' || line[0] == ';' ) {
            continue;
        }

        len = (int)strlen( line );
        if ( used + len + 2 > size ) {   /* leave room for double null */
            break;
        }
        strcpy( buf + used, line );
        used += len + 1;
        count++;
    }

    buf[used] = '\0';                  /* terminating empty string */
    fclose( fp );
    return count;
}

/* Split a [Shortcuts] line, EXENAME.EXE="Friendly Title", into its two
 * parts, dropping the quotes.  Returns 1 if both parts are present. */
static int ParseShortcut( const char* line, char* exe, int exeSize,
                          char* title, int titleSize )
{
    const char* eq = strchr( line, '=' );
    int         len;

    if ( !eq ) return 0;

    len = (int)( eq - line );
    if ( len >= exeSize ) len = exeSize - 1;
    memcpy( exe, line, len );
    exe[len] = '\0';
    Trim( exe );

    strncpy( title, eq + 1, titleSize - 1 );
    title[titleSize - 1] = '\0';
    Trim( title );

    len = (int)strlen( title );
    if ( len >= 2 && title[0] == '"' && title[len - 1] == '"' ) {
        title[len - 1] = '\0';
        memmove( title, title + 1, len - 1 );
    }

    return exe[0] != '\0' && title[0] != '\0';
}

/*===========================================================================
 * Workplace Shell - the desktop folder and its program objects
 *===========================================================================*/

/* The object ID of the program object for an EXE: "<DU_OS2_DISKSTAT>" for
 * DISKSTAT.EXE.  Built from the base name alone, so it is stable across
 * installs and the same whatever the title says. */
static void ObjectIdFor( char* id, int idSize, const char* exeName )
{
    char base[64];
    int  i;

    for ( i = 0; exeName[i] && exeName[i] != '.' && i < (int)sizeof( base ) - 1; i++ ) {
        base[i] = (char)toupper( exeName[i] );
    }
    base[i] = '\0';

    sprintf( id, OBJID_PROGRAM_FMT, base );
    (void)idSize;
}

/* Create the folder on the desktop, or find the one an earlier run made.
 * CO_UPDATEIFEXISTS keeps an existing folder and everything in it. */
static HOBJECT CreateDesktopFolder( void )
{
    HOBJECT hobj;

    hobj = WinCreateObject( (PCSZ)"WPFolder", (PCSZ)g_groupName,
                            (PCSZ)"OBJECTID=" OBJID_FOLDER,
                            (PCSZ)"<WP_DESKTOP>", CO_UPDATEIFEXISTS );
    if ( hobj != NULLHANDLE ) {
        WinSaveObject( hobj, FALSE );
    }
    return hobj;
}

/* Destroy program objects retired in earlier versions.  [RemovedShortcuts]
 * lists one EXE name per line; the object is found by the ID built from it.
 * Objects the user made themselves carry no such ID and are untouched. */
static void RemoveOldObjects( void )
{
    char  lines[2048];
    char  id[80];
    char* p;

    ReadSection( "RemovedShortcuts", lines, sizeof( lines ) );

    for ( p = lines; *p; p += strlen( p ) + 1 ) {
        HOBJECT hobj;

        ObjectIdFor( id, sizeof( id ), p );
        hobj = WinQueryObject( (PCSZ)id );
        if ( hobj != NULLHANDLE ) {
            WinDestroyObject( hobj );
        }
    }
}

/* Read [Shortcuts] from SETUP.INF and create a program object in the folder
 * for each.  CO_REPLACEIFEXISTS throws away a previous run's object of the
 * same ID and makes a fresh one, so the path and title are always current
 * and a reinstall never doubles up.  Returns the number created. */
static int AddProgramObjects( void )
{
    char  lines[2048];
    char  exe[MAXPATH];
    char  title[160];
    char  id[80];
    char  setup[MAXPATH * 2 + 160];
    char* p;
    int   made = 0;

    ReadSection( "Shortcuts", lines, sizeof( lines ) );

    for ( p = lines; *p; p += strlen( p ) + 1 ) {
        HOBJECT hobj;

        if ( !ParseShortcut( p, exe, sizeof( exe ), title, sizeof( title ) ) ) {
            continue;
        }

        ObjectIdFor( id, sizeof( id ), exe );

        /* The same settings the Program template asks for: the EXE, its
         * working directory, and that it is a PM program.  Every utility in
         * the OS/2 set is one. */
        sprintf( setup, "EXENAME=%s\\%s;STARTUPDIR=%s;PROGTYPE=PM;OBJECTID=%s",
                 g_destPath, exe, g_destPath, id );

        hobj = WinCreateObject( (PCSZ)"WPProgram", (PCSZ)title, (PCSZ)setup,
                                (PCSZ)OBJID_FOLDER, CO_REPLACEIFEXISTS );
        if ( hobj != NULLHANDLE ) {
            WinSaveObject( hobj, FALSE );
            made++;
        }
    }

    return made;
}

/* Create the desktop folder and fill it.  Returns 1 if the folder exists;
 * the program objects report their own count through the caller's message. */
static int CreateProgramGroup( HWND hDlg )
{
    char m[MAXPATH + 128];
    int  made;

    g_hFolder = CreateDesktopFolder();
    if ( g_hFolder == NULLHANDLE ) {
        sprintf( m, "The desktop folder \"%s\" could not be created.\n\n"
                    "The programs are installed in %s and can be run "
                    "from there.", g_groupName, g_destPath );
        Message( hDlg, m, MB_OK | MB_WARNING );
        return 0;
    }

    RemoveOldObjects();
    made = AddProgramObjects();
    if ( made == 0 ) {
        sprintf( m, "No program objects could be created in the \"%s\" "
                    "folder.\n\nThe programs are installed in %s and can "
                    "be run from there.", g_groupName, g_destPath );
        Message( hDlg, m, MB_OK | MB_WARNING );
    }
    return 1;
}

/*===========================================================================
 * Installation work (runs on the progress page)
 *===========================================================================*/

/* Context handed to the ExtractZip progress callback. */
typedef struct {
    HWND hDlg;
    int  fileIndex;   /* 0-based index of the file being extracted */
    int  fileCount;   /* total number of files                     */
} PROGCTX;

/* Called by ExtractZip as bytes are written.  Advances the gauge across the
 * whole job (completed files + fraction of the current file), keeps PM
 * responsive, and reports a Cancel by returning 0 to abort extraction. */
static int ProgressCallback( unsigned long done, unsigned long total,
                             void* user )
{
    PROGCTX* ctx = (PROGCTX*)user;
    long     filePct, overall;

    if ( ctx->fileCount > 0 ) {
        filePct = ( total > 0UL ) ? (long)( done * 100UL / total ) : 100L;
        overall = ( (long)ctx->fileIndex * 100L + filePct ) / ctx->fileCount;
        SetGauge( (int)overall );
    }

    PumpMessages();
    return g_bCancel ? 0 : 1;   /* 0 aborts the extraction */
}

static void SetStatus( HWND hDlg, const char* text )
{
    WinSetDlgItemText( hDlg, IDC_STATUS, (PCSZ)text );
    PumpMessages();
}

static int DoInstall( HWND hDlg )
{
    static char files[2048];
    static char status[MAXPATH + 32];
    char*       f;
    int         total, idx;
    PROGCTX     ctx;

    g_bCancel = FALSE;

    /* Read the [Files] list */
    total = ReadSection( "Files", files, sizeof( files ) );

    if ( total == 0 ) {
        Message( hDlg, "No files listed in SETUP.INF.", MB_OK | MB_ERROR );
        return 0;
    }

    /* Extract each file from the DU.CWD payload */
    ctx.hDlg      = hDlg;
    ctx.fileCount = total;

    idx = 0;
    for ( f = files; *f; f += strlen( f ) + 1 ) {
        int rc;

        ctx.fileIndex = idx;
        sprintf( status, "Extracting %s ...", f );
        SetStatus( hDlg, status );

        if ( g_bCancel ) {
            return 0;
        }

        rc = ExtractZip( g_cwdPath, f, g_destPath, ProgressCallback, &ctx );
        if ( rc == UZ_ERR_CANCEL ) {
            return 0;            /* user cancelled mid-file */
        }
        if ( rc != UZ_OK ) {
            sprintf( status, "Could not extract %s (error %d).", f, rc );
            Message( hDlg, status, MB_OK | MB_ERROR );
            return 0;
        }
        idx++;
    }

    /* Remove files left over from earlier versions */
    SetStatus( hDlg, "Removing obsolete files ..." );
    RemoveObsoleteFiles( g_destPath );

    /* Verify everything listed in [Files] is present */
    SetStatus( hDlg, "Verifying installation ..." );
    if ( !ValidateInstall( g_destPath ) ) {
        Message( hDlg, "Installation could not be verified: one or more "
                       "files are missing.", MB_OK | MB_ERROR );
        return 0;
    }

    /* Create the desktop folder and its program objects */
    SetStatus( hDlg, "Creating desktop folder ..." );
    CreateProgramGroup( hDlg );

    SetGauge( 100 );
    return 1;
}

/*===========================================================================
 * System integration options (applied on the Options page)
 *===========================================================================*/

/* Put the Enhanced Notepad on the Open menu of every .TXT file.  The
 * Workplace Shell associates programs with files through the program
 * object: ASSOCFILTER on the Notepad's object does it, and the object was
 * made moments ago with a known ID.  Returns 1 on success. */
static int ApplyTextAssociation( void )
{
    HOBJECT hobj = WinQueryObject( (PCSZ)OBJID_ENHNOTE );

    if ( hobj == NULLHANDLE ) return 0;
    if ( !WinSetObjectData( hobj, (PCSZ)"ASSOCFILTER=*.TXT" ) ) return 0;
    WinSaveObject( hobj, FALSE );
    return 1;
}

/*===========================================================================
 * Dialog procedures
 *===========================================================================*/

/* Is this a drive Setup can install on: a hard disk or a network drive that
 * is ready, and not a CD-ROM.  Diskettes are skipped by letter.  Hard errors
 * are disabled at startup, so an empty removable drive fails the space query
 * quietly instead of raising the system's popup. */
static int UsableDrive( char letter )
{
    union {
        FSQBUFFER2 fsq;
        char       pad[sizeof( FSQBUFFER2 ) + 3 * CCHMAXPATH];
    } buf;
    ULONG       cb = sizeof( buf );
    FSALLOCATE  fsa;
    char        drive[3];
    const char* fsName;

    if ( letter == 'A' || letter == 'B' ) return 0;

    drive[0] = letter;
    drive[1] = ':';
    drive[2] = '\0';

    memset( &buf, 0, sizeof( buf ) );
    if ( DosQueryFSAttach( (PCSZ)drive, 0, FSAIL_QUERYNAME, &buf.fsq, &cb ) != NO_ERROR ) {
        return 0;
    }
    if ( buf.fsq.iType != FSAT_LOCALDRV && buf.fsq.iType != FSAT_REMOTEDRV ) {
        return 0;
    }

    /* szName holds the drive; the attached file system's name follows it. */
    fsName = (const char*)buf.fsq.szName + buf.fsq.cbName + 1;
    if ( stricmp( fsName, "CDFS" ) == 0 ) {
        return 0;
    }

    if ( DosQueryFSInfo( (ULONG)( letter - 'A' + 1 ), FSIL_ALLOC,
                         &fsa, sizeof( fsa ) ) != NO_ERROR ) {
        return 0;
    }
    return 1;
}

/* Populate the drive combo with the usable drives and select the proposed
 * one - or, if that is not present, the boot drive, or the first entry. */
static void FillDrives( HWND hDlg )
{
    ULONG curDrive = 0, map = 0, bootDrive = 0;
    char  letter;
    char  s[3];
    SHORT want = -1, boot = -1, idx;

    DosQueryCurrentDisk( &curDrive, &map );
    DosQuerySysInfo( QSV_BOOT_DRIVE, QSV_BOOT_DRIVE, &bootDrive, sizeof( bootDrive ) );

    for ( letter = 'A'; letter <= 'Z'; letter++ ) {
        if ( !( map & ( 1UL << ( letter - 'A' ) ) ) ) continue;
        if ( !UsableDrive( letter ) ) continue;

        s[0] = letter;
        s[1] = ':';
        s[2] = '\0';
        idx = (SHORT)LONGFROMMR( WinSendDlgItemMsg( hDlg, IDC_DRIVE, LM_INSERTITEM,
                                                     MPFROMLONG( LIT_END ),
                                                     MPFROMP( s ) ) );
        if ( letter == g_defDrive ) want = idx;
        if ( letter == (char)( 'A' + bootDrive - 1 ) ) boot = idx;
    }

    if ( want < 0 ) want = boot;
    if ( want < 0 ) want = 0;
    WinSendDlgItemMsg( hDlg, IDC_DRIVE, LM_SELECTITEM, MPFROMLONG( want ),
                       MPFROMSHORT( TRUE ) );
}

MRESULT EXPENTRY WelcomeDlgProc( HWND hDlg, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg ) {
    case WM_INITDLG: {
        char note[200];

        PageInit( hDlg );
        PlaceIcon( hDlg );

        /* A PM entry field takes 32 characters unless told otherwise. */
        WinSendDlgItemMsg( hDlg, IDC_FOLDER, EM_SETTEXTLIMIT,
                           MPFROMSHORT( MAXPATH - 4 ), 0 );
        FillDrives( hDlg );
        WinSetDlgItemText( hDlg, IDC_FOLDER, (PCSZ)g_defFolder );

        /* Second line of "Program objects go in the desktop folder": the
         * name on a line of its own, so a long one is not cut off. */
        sprintf( note, "\"%s\".", g_groupName );
        WinSetDlgItemText( hDlg, IDC_NOTE, (PCSZ)note );
        return (MRESULT)FALSE;
    }

    case WM_COMMAND:
        switch ( SHORT1FROMMP( mp1 ) ) {
        case DID_OK: {
            char          drv[8];
            char          folder[MAXPATH];
            SHORT         sel;
            int           len;
            unsigned long freeKB;
            unsigned int  reqKB = 0;

            sel = (SHORT)LONGFROMMR( WinSendDlgItemMsg( hDlg, IDC_DRIVE, LM_QUERYSELECTION,
                                                         MPFROMLONG( LIT_FIRST ), 0 ) );
            if ( sel < 0 ) {
                Message( hDlg, "Please select a drive.", MB_OK | MB_WARNING );
                return (MRESULT)FALSE;
            }
            WinSendDlgItemMsg( hDlg, IDC_DRIVE, LM_QUERYITEMTEXT,
                               MPFROM2SHORT( sel, sizeof( drv ) ), MPFROMP( drv ) );

            WinQueryDlgItemText( hDlg, IDC_FOLDER, sizeof( folder ), (PCSZ)folder );
            Trim( folder );
            if ( folder[0] == '\0' ) {
                Message( hDlg, "Please enter a directory.", MB_OK | MB_WARNING );
                return (MRESULT)FALSE;
            }

            /* Build the full destination path (drv = "C:") */
            if ( folder[0] == '\\' ) {
                sprintf( g_destPath, "%s%s", drv, folder );
            } else {
                sprintf( g_destPath, "%s\\%s", drv, folder );
            }

            len = (int)strlen( g_destPath );
            if ( len > 3 && g_destPath[len - 1] == '\\' ) {
                g_destPath[len - 1] = '\0';
            }

            /* Disk-space check on the target drive */
            GetIniKeyUInt( &reqKB, g_infPath, "[DiskSpace]", "required" );
            freeKB = DriveFreeKB( drv[0] );
            if ( freeKB != 0xFFFFFFFFUL && freeKB < (unsigned long)reqKB ) {
                char m[160];
                sprintf( m, "Not enough disk space on drive %s.\n"
                            "%lu KB required, %lu KB available.",
                         drv, (unsigned long)reqKB, freeKB );
                Message( hDlg, m, MB_OK | MB_ERROR );
                return (MRESULT)FALSE;   /* stay on this page */
            }

            /* Create the directory if it does not exist */
            if ( !PathExists( g_destPath ) ) {
                char m[MAXPATH + 64];
                sprintf( m, "The directory %s does not exist.\n"
                            "Do you want to create it?", g_destPath );
                if ( Message( hDlg, m, MB_YESNO | MB_QUERY ) != MBID_YES ) {
                    return (MRESULT)FALSE;
                }

                if ( !MakeTree( g_destPath ) ) {
                    Message( hDlg, "The directory could not be created.",
                             MB_OK | MB_ERROR );
                    return (MRESULT)FALSE;
                }
            }

            WinDismissDlg( hDlg, DID_OK );
            return (MRESULT)FALSE;
        }

        case DID_CANCEL:
            WinDismissDlg( hDlg, DID_CANCEL );
            return (MRESULT)FALSE;
        }
        break;
    }

    return WinDefDlgProc( hDlg, msg, mp1, mp2 );
}

MRESULT EXPENTRY ProgressDlgProc( HWND hDlg, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg ) {
    case WM_INITDLG: {
        HWND hwndOld = WinWindowFromID( hDlg, IDC_GAUGE );
        SWP  swp;

        PageInit( hDlg );

        /* Swap the placeholder static for the bar window, same place. */
        g_hwndBar    = NULLHANDLE;
        g_barPercent = 0;
        if ( hwndOld != NULLHANDLE && WinQueryWindowPos( hwndOld, &swp ) ) {
            WinDestroyWindow( hwndOld );
            g_hwndBar = WinCreateWindow( hDlg, (PCSZ)BAR_CLASS, (PCSZ)"",
                                         WS_VISIBLE, swp.x, swp.y, swp.cx, swp.cy,
                                         hDlg, HWND_TOP, IDC_GAUGE, NULL, NULL );
        }

        WinPostMsg( hDlg, WMU_START, 0, 0 );
        return (MRESULT)FALSE;
    }

    case WMU_START:
        /* Make sure the page and its controls have painted before the long,
         * blocking extraction begins; the posted WMU_START can arrive ahead
         * of the first WM_PAINTs. */
        WinShowWindow( hDlg, TRUE );
        WinUpdateWindow( hDlg );
        PumpMessages();

        if ( DoInstall( hDlg ) ) {
            WinDismissDlg( hDlg, DID_OK );
        } else {
            WinDismissDlg( hDlg, DID_CANCEL );
        }
        return (MRESULT)FALSE;

    case WM_COMMAND:
        if ( SHORT1FROMMP( mp1 ) == DID_CANCEL ) {
            g_bCancel = TRUE;     /* DoInstall finishes the current step */
            WinEnableWindow( WinWindowFromID( hDlg, DID_CANCEL ), FALSE );
            return (MRESULT)FALSE;
        }
        break;

    /* The close box is a Cancel too: the work decides when the page goes. */
    case WM_CLOSE:
        g_bCancel = TRUE;
        return (MRESULT)FALSE;
    }

    return WinDefDlgProc( hDlg, msg, mp1, mp2 );
}

MRESULT EXPENTRY OptionsDlgProc( HWND hDlg, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg ) {
    case WM_INITDLG:
        PageInit( hDlg );

        /* Offered checked - unless the Notepad's object is not there (it was
         * not in [Shortcuts], or the folder could not be made), in which
         * case there is nothing to attach the association to. */
        if ( WinQueryObject( (PCSZ)OBJID_ENHNOTE ) != NULLHANDLE ) {
            WinCheckButton( hDlg, IDC_OPT_ENHNOTE, 1 );
        } else {
            WinCheckButton( hDlg, IDC_OPT_ENHNOTE, 0 );
            WinEnableWindow( WinWindowFromID( hDlg, IDC_OPT_ENHNOTE ), FALSE );
        }
        return (MRESULT)FALSE;

    case WM_COMMAND:
        switch ( SHORT1FROMMP( mp1 ) ) {
        case DID_OK:
            if ( WinQueryButtonCheckstate( hDlg, IDC_OPT_ENHNOTE ) ) {
                if ( !ApplyTextAssociation() ) {
                    Message( hDlg, "The setting could not be saved.",
                             MB_OK | MB_WARNING );
                }
            }
            WinDismissDlg( hDlg, DID_OK );
            return (MRESULT)FALSE;

        case DID_CANCEL:
            /* Skipping the integration is fine; the install itself is done. */
            WinDismissDlg( hDlg, DID_CANCEL );
            return (MRESULT)FALSE;
        }
        break;
    }

    return WinDefDlgProc( hDlg, msg, mp1, mp2 );
}

MRESULT EXPENTRY CompleteDlgProc( HWND hDlg, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg ) {
    case WM_INITDLG: {
        char line[MAXPATH + 8];

        PageInit( hDlg );
        PlaceIcon( hDlg );

        if ( g_hFolder != NULLHANDLE ) {
            sprintf( line, "\"%s\".", g_groupName );
        } else {
            WinSetDlgItemText( hDlg, IDC_NOTE1,
                               (PCSZ)"The programs are in the directory" );
            sprintf( line, "%s.", g_destPath );
        }
        WinSetDlgItemText( hDlg, IDC_NOTE, (PCSZ)line );
        return (MRESULT)FALSE;
    }

    case WM_COMMAND:
        if ( SHORT1FROMMP( mp1 ) == DID_OK || SHORT1FROMMP( mp1 ) == DID_CANCEL ) {
            WinDismissDlg( hDlg, DID_OK );
            return (MRESULT)FALSE;
        }
        break;
    }

    return WinDefDlgProc( hDlg, msg, mp1, mp2 );
}

/*===========================================================================
 * Entry point
 *===========================================================================*/

/* Resolve module path, SETUP.INF path, and make the module dir current
 * (so ValidateInstall, which opens "SETUP.INF" relative to the CWD, works). */
static void InitPaths( void )
{
    PTIB  ptib;
    PPIB  ppib;
    char  dir[MAXPATH];
    char* bs;

    g_modulePath[0] = '\0';
    if ( DosGetInfoBlocks( &ptib, &ppib ) == NO_ERROR ) {
        DosQueryModuleName( ppib->pib_hmte, sizeof( g_modulePath ), g_modulePath );
    }

    strcpy( dir, g_modulePath );
    bs = strrchr( dir, '\\' );
    if ( bs ) {
        *bs = '\0';
    } else {
        dir[0] = '\0';
    }

    if ( dir[0] ) {
        sprintf( g_infPath, "%s\\SETUP.INF", dir );
        sprintf( g_cwdPath, "%s\\DU.CWD", dir );
        if ( dir[1] == ':' ) {
            DosSetDefaultDisk( (ULONG)( toupper( dir[0] ) - 'A' + 1 ) );
            if ( dir[2] == '\0' ) strcat( dir, "\\" );    /* the root */
        }
        DosSetCurrentDir( (PCSZ)dir );
    } else {
        strcpy( g_infPath, "SETUP.INF" );
        strcpy( g_cwdPath, "DU.CWD" );
    }
}

/* The folder the program objects go in is named by SETUP.INF [Group] Name=. */
static void ReadGroupName( void )
{
    char name[_MAX_PATH];

    GetIniKeyString( name, g_infPath, "[Group]", "Name" );
    Trim( name );
    if ( name[0] == '\0' ) {
        strcpy( name, "Dobson Utilities" );
    }
    strncpy( g_groupName, name, sizeof( g_groupName ) - 1 );
    g_groupName[sizeof( g_groupName ) - 1] = '\0';
}

/* The drive and directory proposed on the Welcome page: C:\DU-OS2.  If C: is
 * not a drive Setup can install on, FillDrives offers the boot drive. */
static void ChooseDefaultLocation( void )
{
    g_defDrive = 'C';
    strcpy( g_defFolder, "\\DU-OS2" );
}

/* Run one page.  A page that will not open is reported, with the PM error,
 * rather than left to look like Setup exiting for no reason: WinDlgBox says
 * only DID_ERROR when the template cannot be built. */
static ULONG RunPage( ULONG dlgId, PFNWP proc, const char* pageName )
{
    ULONG rc = WinDlgBox( HWND_DESKTOP, HWND_DESKTOP, proc, NULLHANDLE, dlgId, NULL );

    PageDone();

    if ( rc == DID_ERROR ) {
        char  m[200];
        ULONG err = (ULONG)WinGetLastError( g_hab ) & 0xFFFFL;

        sprintf( m, "Setup could not open its %s page (dialog %lu).\n\n"
                    "PM error 0x%04lX.", pageName, dlgId, err );
        Message( HWND_DESKTOP, m, MB_OK | MB_ERROR );
    }
    return rc;
}

/* Verify the payload (DU.CWD) and SETUP.INF are present alongside us.
 * Returns 1 if both exist, otherwise shows a fatal error and returns 0. */
static int CheckPrerequisites( void )
{
    if ( !PathExists( g_cwdPath ) || !PathExists( g_infPath ) ) {
        Message( HWND_DESKTOP,
                 "Required setup files are missing.\n\n"
                 "DU.CWD and SETUP.INF must be in the same directory "
                 "as this program.", MB_OK | MB_ERROR );
        return 0;
    }
    return 1;
}

int main( void )
{
    HMQ hmq;

    g_hab = WinInitialize( 0 );
    if ( g_hab == NULLHANDLE ) return 1;

    hmq = WinCreateMsgQueue( g_hab, 0 );
    if ( hmq == NULLHANDLE ) {
        WinTerminate( g_hab );
        return 1;
    }

    /* Probing a drive with nothing in it must not raise the system's
     * hard-error popup. */
    DosError( FERR_DISABLEHARDERR );

    InitPaths();

    /* Both the payload and the manifest must be present, or we cannot run */
    if ( !CheckPrerequisites() ) {
        WinDestroyMsgQueue( hmq );
        WinTerminate( g_hab );
        return 1;
    }

    ReadGroupName();
    ChooseDefaultLocation();
    if ( !WinRegisterClass( g_hab, (PCSZ)BAR_CLASS, BarWndProc, CS_SIZEREDRAW, 0 ) ) {
        Message( HWND_DESKTOP, "Window class registration failed.", MB_OK | MB_ERROR );
        WinDestroyMsgQueue( hmq );
        WinTerminate( g_hab );
        return 1;
    }

    /* Page 1: welcome / destination */
    if ( RunPage( IDD_WELCOME, WelcomeDlgProc, "Welcome" ) != DID_OK ) {
        WinDestroyMsgQueue( hmq );
        WinTerminate( g_hab );
        return 0;   /* user cancelled */
    }

    /* Page 2: progress (does the actual work) */
    if ( RunPage( IDD_PROGRESS, ProgressDlgProc, "Installing" ) != DID_OK ) {
        WinDestroyMsgQueue( hmq );
        WinTerminate( g_hab );
        return 0;   /* failed or cancelled - message already shown */
    }

    /* Page 3: integration option (Enhanced Notepad on the Open menu).
     * Applied on Next; Cancel simply skips it - the install is already done. */
    RunPage( IDD_OPTIONS, OptionsDlgProc, "Integration Options" );

    /* Page 4: completion */
    RunPage( IDD_COMPLETE, CompleteDlgProc, "Setup Complete" );

    /* Show the result: open the folder, the way Program Manager showed a
     * group it had just made. */
    if ( g_hFolder != NULLHANDLE ) {
        WinOpenObject( g_hFolder, OPEN_DEFAULT, TRUE );
    }

    WinDestroyMsgQueue( hmq );
    WinTerminate( g_hab );
    return 0;
}
