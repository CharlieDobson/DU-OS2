/*===========================================================================
 * PMMAIN.C  -  XArchive for OS/2 2.x / Warp (32-bit Presentation Manager)
 *
 * Front end only: open an archive, list its contents, extract all of it or a
 * selection, test its integrity, summarise it.  Every format backend
 * (ARCFILE / SZARC / ZIPARC / RARARC / RAR3DEC / RAR5ARC / DISKARC /
 * LZMADEC / CRC32) is the Win32s source BYTE-FOR-BYTE, reading its handful of
 * Win32 calls out of the local windows.h shim.  Those files must stay
 * identical to the Win32s XArchive tree -- do not edit them, not even the
 * comments.
 * This file is the whole of the port's user interface.
 *
 *   WinMain / RegisterClass      ->  main / WinRegisterClass
 *   CreateWindow(WS_OVERLAPPED..)->  WinCreateStdWindow(FCF_*)
 *   Get/DispatchMessage          ->  WinGetMsg / WinDispatchMsg
 *   PeekMessage pump             ->  WinPeekMsg / WinDispatchMsg
 *   COMCTL32 list view (report)  ->  WC_CONTAINER in details view
 *   COMCTL32 toolbar             ->  a strip of ordinary WC_BUTTONs
 *   GetOpenFileName              ->  WinFileDlg
 *   DlgDirList folder picker     ->  hand-filled listbox (PM has no
 *                                    DlgDirList) over DosFindFirst
 *   DialogBox / CreateDialog     ->  WinDlgBox / WinLoadDlg
 *   MessageBox                   ->  WinMessageBox
 *   WM_DROPFILES                 ->  DM_DRAGOVER / DM_DROP  (+ the container's
 *                                    CN_DRAGOVER / CN_DROP relay)
 *   WM_INITMENUPOPUP + position  ->  WM_INITMENU + MM_SETITEMATTR
 *   owner-draw progress bar      ->  a small registered "XArcBar" class
 *   CTL3D32 gating               ->  dropped; PM controls already are 3-D
 *
 * PM's origin is the BOTTOM-left and y grows up, so the toolbar strip sits at
 * yTop - toolbar height and the container starts at y = 0.
 *
 * Compiler: Open Watcom 1.9  (wcc386 -bt=os2)
 *===========================================================================*/

#define INCL_WIN
#define INCL_GPI
#define INCL_WINSTDFILE
#define INCL_WINSTDCNR
#define INCL_WINSTDDRAG
#define INCL_DOSFILEMGR
#define INCL_DOSMISC
#define INCL_DOSERRORS

#include <os2.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <malloc.h>      /* _heapmin */

#include "arcdefs.h"
#include "arcfile.h"
#include "arcpref.h"
#include "pmworker.h"
#include "res/resource.h"

/*===========================================================================
 * Container record
 *
 * One per archive entry.  CCS_MINIRECORDCORE keeps the fixed part down to 28
 * bytes, which matters: SZ_MAX_FILES is 16384.
 *
 * pszName may point straight into the open archive's own entry table, which
 * stays put until ArcClose.  Everything else is copied into the record - and
 * the method string ESPECIALLY so: SzEntryMethod() builds its answer in a
 * static buffer and returns a pointer to it, which is fine for the Windows
 * build (ListView_SetItemText copies the text there and then) but would leave
 * every 7z row in the container pointing at one shared buffer, all showing
 * whatever the last entry happened to be.
 *===========================================================================*/
typedef struct _ARCREC {
    MINIRECORDCORE rc;
    PSZ   pszName;
    PSZ   pszSize;
    PSZ   pszPacked;
    PSZ   pszDate;
    PSZ   pszAttr;
    PSZ   pszMethod;
    LONG  lIndex;                /* entry index, for the selection walk */
    char  szSize[16];
    char  szPacked[16];
    char  szDate[24];
    char  szAttr[16];
    char  szMethod[32];
} ARCREC, *PARCREC;

/*===========================================================================
 * Globals
 *===========================================================================*/
static HAB      g_hab       = NULLHANDLE;
static HWND     g_hwndFrame = NULLHANDLE;
static HWND     g_hwndClient= NULLHANDLE;
static HWND     g_hwndCnr   = NULLHANDLE;
static ArcFile *g_arc       = NULL;
static char     g_arcPath[CCHMAXPATH] = { 0 };

/* Toolbar: the button IDs are the menu command IDs, so WM_COMMAND is shared. */
#define TB_COUNT 5
static const ULONG g_tbId[TB_COUNT] = {
    IDM_FILE_OPEN, IDM_ARCHIVE_INFO, IDM_ARCHIVE_TEST,
    IDM_ARCHIVE_EXTRACT, IDM_FILE_EXIT
};
static const char *g_tbText[TB_COUNT] = {
    "~Open", "Info", "Test", "Extract", "Exit"
};
static HWND g_hwndBtn[TB_COUNT];
static LONG g_btnW = 72, g_btnH = 26, g_tbH = 34;

/* Progress state (valid while the progress box is up).
 *
 * g_hwndProgress and g_hwndBar belong to the UI thread.  g_progPercent and
 * g_progName are written by the WORKER and read by the UI thread's timer --
 * see ProgressSet (worker) and ProgressTick (UI).
 *
 * g_progName is one byte longer than the longest name it can be given and
 * that byte is never written, so it is always terminated inside its own
 * storage however a read and a write interleave.  The worst case is one tick
 * showing a mix of two names. */
static HWND          g_hwndProgress = NULLHANDLE;
static HWND          g_hwndBar      = NULLHANDLE;
static volatile LONG g_progPercent  = 0;
static char          g_progName[CCHMAXPATH + 1];

/*---- The worker ----------------------------------------------------------
 *
 * Extraction, integrity testing and the archive-header parse each used to run
 * on the UI thread with a WinPeekMsg pump in the progress callback.  That
 * pump could not help inside a single big member -- a 200 MB file in a solid
 * RAR is one call into the decoder -- and because PM has one input queue, the
 * whole desktop stopped with it.  It also re-entered the application: the
 * dispatch inside ProgressSet let the user hit Extract again mid-extraction.
 *
 *      RunExtraction / DoTest / OpenArchiveFile  start a worker and return
 *      ArcWorkerBody                             the Arc* call itself
 *      ArcOnDone                                 report, and list the archive
 *
 * PopulateList stays on the UI thread: it fills a PM container, which a
 * worker may not touch.  Only the header parse moved.
 *-------------------------------------------------------------------------*/

#define ARCJOB_EXTRACT      1
#define ARCJOB_TEST         2
#define ARCJOB_OPEN         3

#define TID_ARCPROGRESS     1
#define ARC_TIMER_MS        150

/* The one question an extraction asks (PmWorkerAsk's ulQuestion). */
#define ARCASK_OVERWRITE    1

static PMWORKER g_worker;
static int      g_workerReady = 0;

/* The output path an overwrite question is about.  Written by the WORKER
 * immediately before PmWorkerAsk raises the ask latch, read by the UI
 * thread's dialog; safe unlocked because the worker then blocks inside
 * PmWorkerAsk until PmWorkerAnswer, so the buffer cannot change while the
 * dialog is up. */
static char g_owAskPath[SZ_MAX_NAME * 4];

static struct {
    int      kind;
    int     *sel;                       /* extract selection; worker frees  */
    int      selCount;
    char     dest[CCHMAXPATH];
    char     openPath[CCHMAXPATH];
    ArcFile *openArc;                   /* ARCJOB_OPEN result               */
    int      rc;
} g_job;

/* Filled by DoInfo(), shown by the Info dialog. */
static char g_infoText[1024];

/* Extract-dialog state: the chosen destination, a New-Folder name, and the
 * title that says what this run is about to take out of the archive. */
static char g_extractPath[CCHMAXPATH];
static char g_newFolderName[CCHMAXPATH];
static char g_extractTitle[64] = "Extract To";

/* Folder picker: suggested dir in / chosen dir out, plus the CWD to restore. */
static char g_folderPick[CCHMAXPATH];
static char g_fpSaveCwd[CCHMAXPATH];

/* A path handed over by a drop, opened once the drag transaction is done. */
static char g_dropPath[CCHMAXPATH];
#define WM_XA_OPENDROP  (WM_USER + 1)

/* Container subclass: the container's own window procedure, and the private
 * message that undoes PM's select-the-first-record-on-focus (see CnrSubProc).
 * g_cnrClicked marks a selection the user made with the mouse, which that
 * undo has to leave alone. */
static PFNWP g_pfnCnrOld  = NULL;
static BOOL  g_cnrClicked = FALSE;
#define WM_XA_NOSEL     (WM_USER + 2)

static const char g_szClass[]  = "XArchive";
static const char g_szBarClass[] = "XArcBar";
static const char g_szTitle[]  = "XArchive";

/*---- Forward declarations ------------------------------------------------ */
MRESULT EXPENTRY ClientWndProc  ( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY CnrSubProc     ( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY BarWndProc     ( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY AboutDlgProc   ( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY ProgressDlgProc( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY InfoDlgProc    ( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY ExtractDlgProc ( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY NewFolderDlgProc( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY FolderPickProc ( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY OverwriteDlgProc( HWND, ULONG, MPARAM, MPARAM );

static void OpenArchiveFile( HWND hwnd, const char *path );
static void ProgressBegin  ( HWND owner, const char *caption,
                             const char *label, BOOL cancelable );
static void ProgressSet    ( LONG percent, const char *text );
static void ProgressTick   ( void );
static void ProgressEnd    ( HWND owner );
static void ArcWorkerBody  ( void *arg );
static void ArcOnDone      ( HWND hwnd, BOOL cancelled );
static void ArcOnAsk       ( HWND hwnd, ULONG question );
static BOOL ArcStartJob    ( HWND hwnd, int kind, const char *caption,
                             const char *label, BOOL cancelable );
static BOOL BrowseForFolder( HWND owner, char *dir, int dirSize );
static BOOL NewFolderDlg   ( HWND owner, const char *parentPath,
                             char *out, int outSize );
static BOOL CreateDirTree  ( const char *path );
static void UpdateToolbarState( void );
static void PopulateList   ( void );

/*===========================================================================
 * Small helpers
 *===========================================================================*/

/* A message box in the app's voice. */
static ULONG Say( HWND hwnd, const char *text, ULONG flags )
{
    return WinMessageBox( HWND_DESKTOP, hwnd, (PCSZ)text, (PCSZ)g_szTitle,
                          0, flags | MB_MOVEABLE );
}

/* Return a pointer to the file-name component of a path. */
static const char *FileNamePart( const char *path )
{
    const char *p    = path;
    const char *last = path;
    while ( *p )
    {
        if ( *p == '\\' || *p == '/' || *p == ':' )
            last = p + 1;
        p++;
    }
    return last;
}

/* Refresh the caption to reflect the open archive (or lack of one). */
static void UpdateTitle( void )
{
    char buf[CCHMAXPATH + 64];
    if ( g_arc )
        sprintf( buf, "%s - %s (%d files)", g_szTitle,
                 FileNamePart( g_arcPath ), ArcNumEntries( g_arc ) );
    else
        strcpy( buf, g_szTitle );
    WinSetWindowText( g_hwndFrame, (PCSZ)buf );
}

/* The process's current directory, drive letter and all. */
static void GetCwd( char *buf, int size )
{
    ULONG drive = 0, map = 0, len = (ULONG)size;
    char  dir[CCHMAXPATH];

    buf[0] = '\0';
    if ( DosQueryCurrentDisk( &drive, &map ) != 0 || drive == 0 )
        return;

    dir[0] = '\0';
    len = sizeof( dir );
    if ( DosQueryCurrentDir( 0, (PBYTE)dir, &len ) != 0 )
        dir[0] = '\0';

    /* DosQueryCurrentDir omits both the drive and the leading backslash. */
    sprintf( buf, "%c:\\%s", (char)( 'A' + drive - 1 ), dir );
    buf[size - 1] = '\0';
}

/* Make 'path' the current directory, following it across drives. */
static BOOL SetCwd( const char *path )
{
    if ( !path || !path[0] )
        return FALSE;

    if ( path[1] == ':' )
    {
        ULONG drive = (ULONG)( toupper( (unsigned char)path[0] ) - 'A' + 1 );
        if ( DosSetDefaultDisk( drive ) != 0 )
            return FALSE;
        if ( path[2] == '\0' )      /* "C:" alone: drive only, keep its dir */
            return TRUE;
    }
    return ( DosSetCurrentDir( (PCSZ)path ) == 0 );
}

/* TRUE if 'path' names an existing directory. */
static BOOL IsDir( const char *path )
{
    FILESTATUS3 fs;
    if ( DosQueryPathInfo( (PCSZ)path, FIL_STANDARD, &fs, sizeof( fs ) ) != 0 )
        return FALSE;
    return ( fs.attrFile & FILE_DIRECTORY ) ? TRUE : FALSE;
}

/*===========================================================================
 * The container (the list-view replacement)
 *===========================================================================*/

/* Declare the six detail columns.  cxWidth 0 lets the container size each
 * column to its widest entry. */
static void CnrSetupColumns( HWND hwndCnr )
{
    static const struct {
        const char *title;
        ULONG       align;
        ULONG       off;
    } cols[6] = {
        { "Name",     CFA_LEFT,  FIELDOFFSET( ARCREC, pszName   ) },
        { "Size",     CFA_RIGHT, FIELDOFFSET( ARCREC, pszSize   ) },
        { "Packed",   CFA_RIGHT, FIELDOFFSET( ARCREC, pszPacked ) },
        { "Modified", CFA_LEFT,  FIELDOFFSET( ARCREC, pszDate   ) },
        { "Attr",     CFA_LEFT,  FIELDOFFSET( ARCREC, pszAttr   ) },
        { "Method",   CFA_LEFT,  FIELDOFFSET( ARCREC, pszMethod ) }
    };

    PFIELDINFO      pfiFirst, pfi;
    FIELDINFOINSERT fii;
    CNRINFO         ci;
    int             i;

    pfiFirst = (PFIELDINFO)WinSendMsg( hwndCnr, CM_ALLOCDETAILFIELDINFO,
                                       MPFROMLONG( 6L ), NULL );
    if ( pfiFirst == NULL )
        return;

    pfi = pfiFirst;
    for ( i = 0; i < 6 && pfi != NULL; i++ )
    {
        pfi->cb         = sizeof( FIELDINFO );
        pfi->flData     = CFA_STRING | CFA_SEPARATOR | CFA_HORZSEPARATOR |
                          cols[i].align;
        pfi->flTitle    = CFA_CENTER | CFA_FITITLEREADONLY;
        pfi->pTitleData = (PVOID)cols[i].title;
        pfi->offStruct  = cols[i].off;
        pfi->pUserData  = NULL;
        pfi->cxWidth    = 0;
        pfi = pfi->pNextFieldInfo;
    }

    memset( &fii, 0, sizeof( fii ) );
    fii.cb                   = sizeof( FIELDINFOINSERT );
    fii.pFieldInfoOrder      = (PFIELDINFO)CMA_FIRST;
    fii.fInvalidateFieldInfo = TRUE;
    fii.cFieldInfoInsert     = 6;
    WinSendMsg( hwndCnr, CM_INSERTDETAILFIELDINFO,
                MPFROMP( pfiFirst ), MPFROMP( &fii ) );

    memset( &ci, 0, sizeof( ci ) );
    ci.cb           = sizeof( CNRINFO );
    ci.flWindowAttr = CV_DETAIL | CV_MINI | CA_DETAILSVIEWTITLES;
    WinSendMsg( hwndCnr, CM_SETCNRINFO,
                MPFROMP( &ci ), MPFROMLONG( CMA_FLWINDOWATTR ) );
}

/* Throw away every record (and the strings hanging off them). */
static void CnrClear( HWND hwndCnr )
{
    if ( hwndCnr != NULLHANDLE )
        WinSendMsg( hwndCnr, CM_REMOVERECORD, NULL,
                    MPFROM2SHORT( 0, CMA_FREE | CMA_INVALIDATE ) );
}

/* How many records are selected. */
static int CnrSelectedCount( HWND hwndCnr )
{
    PARCREC p;
    int     n = 0;

    p = (PARCREC)WinSendMsg( hwndCnr, CM_QUERYRECORDEMPHASIS,
                             MPFROMP( CMA_FIRST ), MPFROMSHORT( CRA_SELECTED ) );
    while ( p != NULL && (LONG)p != -1L )
    {
        n++;
        p = (PARCREC)WinSendMsg( hwndCnr, CM_QUERYRECORDEMPHASIS,
                                 MPFROMP( p ), MPFROMSHORT( CRA_SELECTED ) );
    }
    return n;
}

/* Fill 'out' with the entry indices of the selected records. */
static int CnrSelectedIndices( HWND hwndCnr, int *out, int max )
{
    PARCREC p;
    int     n = 0;

    p = (PARCREC)WinSendMsg( hwndCnr, CM_QUERYRECORDEMPHASIS,
                             MPFROMP( CMA_FIRST ), MPFROMSHORT( CRA_SELECTED ) );
    while ( p != NULL && (LONG)p != -1L && n < max )
    {
        out[n++] = (int)p->lIndex;
        p = (PARCREC)WinSendMsg( hwndCnr, CM_QUERYRECORDEMPHASIS,
                                 MPFROMP( p ), MPFROMSHORT( CRA_SELECTED ) );
    }
    return n;
}

/* Drop the selection emphasis from every record, leaving "nothing selected"
 * - which is what tells Extract to take the whole archive.
 *
 * The walk has to re-ask for the first selected record each time round, since
 * clearing the one we are holding takes it out of the chain the enumeration
 * follows.  Stopping when the same record comes back twice means a record
 * that refuses to give up its emphasis costs one wasted iteration rather than
 * an endless loop. */
static void CnrClearSelection( HWND hwndCnr )
{
    PARCREC p, prev = NULL;

    if ( hwndCnr == NULLHANDLE )
        return;

    for ( ;; )
    {
        p = (PARCREC)WinSendMsg( hwndCnr, CM_QUERYRECORDEMPHASIS,
                                 MPFROMP( CMA_FIRST ),
                                 MPFROMSHORT( CRA_SELECTED ) );
        if ( p == NULL || (LONG)p == -1L || p == prev )
            break;
        prev = p;
        WinSendMsg( hwndCnr, CM_SETRECORDEMPHASIS, MPFROMP( p ),
                    MPFROM2SHORT( FALSE, CRA_SELECTED ) );
    }
}

/* Select the first record, cursor and all.  FALSE when the list is empty. */
static BOOL CnrSelectFirst( HWND hwndCnr )
{
    PARCREC p;

    if ( hwndCnr == NULLHANDLE )
        return FALSE;

    p = (PARCREC)WinSendMsg( hwndCnr, CM_QUERYRECORD, MPFROMP( NULL ),
                             MPFROM2SHORT( CMA_FIRST, CMA_ITEMORDER ) );
    if ( p == NULL || (LONG)p == -1L )
        return FALSE;

    WinSendMsg( hwndCnr, CM_SETRECORDEMPHASIS, MPFROMP( p ),
                MPFROM2SHORT( TRUE, CRA_SELECTED | CRA_CURSORED ) );
    return TRUE;
}

/*---------------------------------------------------------------------------
 * Container subclass
 *
 * Two things PM will not do on its own:
 *
 *   - Leave an untouched list unselected.  A CCS_EXTENDSEL container puts its
 *     cursor on the first record the moment it is given the focus, and in
 *     extended-selection mode the selection travels with the cursor - so a
 *     list nobody has touched arrives at Extract looking as though the user
 *     had picked file #1, and only that file comes out.  Every time the focus
 *     lands on a container that had nothing selected, that implicit selection
 *     is taken off again.  The undo is POSTED rather than done here so it runs
 *     after the container has finished its own focus handling; a click, which
 *     brings a selection of its own along a moment later, sets g_cnrClicked
 *     and is left alone.
 *
 *   - Do anything useful with the first arrow key when nothing is selected.
 *     Down (or Up) then means "start at the top", so it selects the first
 *     record instead of quietly moving an invisible cursor.
 *-------------------------------------------------------------------------*/
MRESULT EXPENTRY CnrSubProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg )
    {
    case WM_CHAR:
    {
        USHORT fs = SHORT1FROMMP( mp1 );
        USHORT vk = SHORT2FROMMP( mp2 );

        /* Ctrl+arrow is PM's "move the cursor, keep the selection", so it is
         * left to do exactly that. */
        if ( !( fs & KC_KEYUP ) && ( fs & KC_VIRTUALKEY ) &&
             !( fs & KC_CTRL ) && ( vk == VK_DOWN || vk == VK_UP ) &&
             CnrSelectedCount( hwnd ) == 0 && CnrSelectFirst( hwnd ) )
            return (MRESULT)TRUE;                 /* key consumed */
        break;
    }

    case WM_BUTTON1DOWN:
    case WM_BUTTON1DBLCLK:
    case WM_BUTTON2DOWN:
        g_cnrClicked = TRUE;
        break;

    case WM_SETFOCUS:
        if ( SHORT1FROMMP( mp2 ) )                /* gaining the focus */
        {
            BOOL    bEmpty = ( CnrSelectedCount( hwnd ) == 0 );
            MRESULT mr     = g_pfnCnrOld( hwnd, msg, mp1, mp2 );
            if ( bEmpty )
                WinPostMsg( hwnd, WM_XA_NOSEL, MPVOID, MPVOID );
            return mr;
        }
        g_cnrClicked = FALSE;                     /* losing it: start clean */
        break;

    case WM_XA_NOSEL:
        if ( g_cnrClicked )
            g_cnrClicked = FALSE;                 /* the user's own selection */
        else
            CnrClearSelection( hwnd );
        return (MRESULT)FALSE;
    }

    return g_pfnCnrOld( hwnd, msg, mp1, mp2 );
}

/* Fill the container: one record per entry across the six columns.  All the
 * records are allocated in a single CM_ALLOCRECORD and inserted in a single
 * CM_INSERTRECORD, which is what keeps a 16000-entry archive quick. */
static void PopulateList( void )
{
    int          n, i;
    PARCREC      pFirst, p;
    RECORDINSERT ri;

    CnrClear( g_hwndCnr );
    if ( !g_arc )
        return;

    n = ArcNumEntries( g_arc );
    if ( n <= 0 )
        return;

    pFirst = (PARCREC)WinSendMsg( g_hwndCnr, CM_ALLOCRECORD,
                    MPFROMLONG( sizeof( ARCREC ) - sizeof( MINIRECORDCORE ) ),
                    MPFROMLONG( (ULONG)n ) );
    if ( pFirst == NULL )
    {
        Say( g_hwndClient, "Not enough memory to list this archive.",
             MB_OK | MB_ICONEXCLAMATION );
        return;
    }

    p = pFirst;
    for ( i = 0; i < n && p != NULL; i++ )
    {
        const char *name = ArcEntryName( g_arc, i );
        int         isDir;
        UInt32      packed;

        if ( g_hwndProgress != NULLHANDLE && ( i & 63 ) == 0 )
            ProgressSet( ( (LONG)i * 100 ) / n, name );

        isDir = ArcEntryIsDir( g_arc, i );

        if ( isDir )
            p->szSize[0] = '\0';
        else
            sprintf( p->szSize, "%lu",
                     (unsigned long)ArcEntrySize( g_arc, i ) );

        packed = ArcEntryPacked( g_arc, i );
        if ( isDir || packed == 0xFFFFFFFFUL )
            p->szPacked[0] = '\0';
        else
            sprintf( p->szPacked, "%lu", (unsigned long)packed );

        ArcEntryDate( g_arc, i, p->szDate, sizeof( p->szDate ) );
        ArcEntryAttr( g_arc, i, p->szAttr, sizeof( p->szAttr ) );

        if ( isDir )
            p->szMethod[0] = '\0';
        else
        {
            const char *m = ArcEntryMethod( g_arc, i );   /* may be static! */
            strncpy( p->szMethod, m ? m : "", sizeof( p->szMethod ) - 1 );
            p->szMethod[sizeof( p->szMethod ) - 1] = '\0';
        }

        p->lIndex     = i;
        p->pszName    = (PSZ)( name ? name : "" );
        p->pszSize    = (PSZ)p->szSize;
        p->pszPacked  = (PSZ)p->szPacked;
        p->pszDate    = (PSZ)p->szDate;
        p->pszAttr    = (PSZ)p->szAttr;
        p->pszMethod  = (PSZ)p->szMethod;

        p->rc.pszIcon = p->pszName;      /* used by CV_TEXT and CM_SEARCHSTRING */

        p = (PARCREC)p->rc.preccNextRecord;
    }

    memset( &ri, 0, sizeof( ri ) );
    ri.cb                = sizeof( RECORDINSERT );
    ri.pRecordOrder      = (PRECORDCORE)CMA_END;
    ri.pRecordParent     = NULL;
    ri.fInvalidateRecord = TRUE;
    ri.zOrder            = (USHORT)CMA_TOP;
    ri.cRecordsInsert    = (ULONG)n;
    WinSendMsg( g_hwndCnr, CM_INSERTRECORD, MPFROMP( pFirst ), MPFROMP( &ri ) );

    /* A freshly listed archive starts with nothing picked out, so Extract
     * means the whole thing until the user says otherwise.  (Records arriving
     * in a focused container can pick up the cursor - and with it the
     * selection - as they land.) */
    CnrClearSelection( g_hwndCnr );
    g_cnrClicked = FALSE;
}

/*===========================================================================
 * Opening and closing
 *===========================================================================*/

/* Close any open archive.
 *
 * Freeing the entry tables hands the blocks back to the C run-time, which by
 * default keeps the pages committed for the next allocation - so the process
 * still looks large in the system's memory figures after a Close.  _heapmin()
 * releases those now-unused pages, which is what the user expects Close to
 * do.
 *
 * The container goes first, deliberately: its records hold pointers into the
 * archive's own name table, so they must be gone before ArcClose frees it. */
static void CloseArchive( void )
{
    CnrClear( g_hwndCnr );
    if ( g_arc )
    {
        ArcClose( g_arc );
        g_arc = NULL;
    }
    g_arcPath[0] = '\0';
    _heapmin();
}

/* Parse an archive and show its contents (shared by Open, drop and the
 * command line).  A progress box is up while the headers are parsed and the
 * potentially large list is built. */
static void OpenArchiveFile( HWND hwnd, const char *path )
{
    strncpy( g_job.openPath, path, sizeof( g_job.openPath ) - 1 );
    g_job.openPath[sizeof( g_job.openPath ) - 1] = '\0';

    g_job.sel = NULL;

    /* The header parse goes to the worker; ArcOnDone does CloseArchive,
       PopulateList and the title afterwards, on this thread.  Not cancelable:
       a half-parsed archive is not a state ArcOpen can hand back. */
    if ( !ArcStartJob( hwnd, ARCJOB_OPEN, "Opening", "Reading:", FALSE ) )
        return;

    ProgressSet( 0, FileNamePart( path ) );
}

/* Open dialog -> parse -> list.
 *
 * PM's file dialog takes ONE wildcard in szFullFile -- there is no list of
 * named multi-pattern filters.  Since
 * ArcOpen identifies the format from the file's content rather than its
 * extension, showing everything is the honest default - the recognised
 * extensions are named in the dialog title instead. */
static void DoOpen( HWND hwnd )
{
    FILEDLG fd;

    memset( &fd, 0, sizeof( fd ) );
    fd.cbSize   = sizeof( FILEDLG );
    fd.fl       = FDS_CENTER | FDS_OPEN_DIALOG;
    fd.pszTitle = (PSZ)"Open Archive  (.7z .zip .rar .img .ima .imz .dsk)";
    strcpy( fd.szFullFile, "*" );

    if ( WinFileDlg( HWND_DESKTOP, hwnd, &fd ) == NULLHANDLE )
        return;
    if ( fd.lReturn != DID_OK )
        return;

    OpenArchiveFile( hwnd, fd.szFullFile );
}

/* Suggested extraction target: the folder the archive lives in.  The user can
 * edit it, browse, or make a new folder in the Extract dialog. */
static void DeriveDestDir( char *dest, int destSize )
{
    char *bs;

    strncpy( dest, g_arcPath, (size_t)destSize - 1 );
    dest[destSize - 1] = '\0';

    bs = strrchr( dest, '\\' );
    if ( bs )
        *bs = '\0';                       /* strip the file name */
    else
        GetCwd( dest, destSize );

    if ( dest[0] && dest[1] == ':' && dest[2] == '\0' )
    {                                     /* "C:" -> "C:\" */
        dest[2] = '\\';
        dest[3] = '\0';
    }
}

/*===========================================================================
 * Progress box
 *===========================================================================*/

/* Paint the bar over rect rc, filled to g_progPercent. */
static void PaintBar( HPS hps, const RECTL *rc )
{
    RECTL done = *rc, rest = *rc, edge = *rc;
    LONG  w    = rc->xRight - rc->xLeft;
    LONG  fill = ( w * g_progPercent ) / 100;

    done.xRight = rc->xLeft + fill;
    rest.xLeft  = rc->xLeft + fill;

    if ( done.xRight > done.xLeft )
        WinFillRect( hps, &done, SYSCLR_HILITEBACKGROUND );
    if ( rest.xRight > rest.xLeft )
        WinFillRect( hps, &rest, SYSCLR_BUTTONMIDDLE );

    WinDrawBorder( hps, &edge, 1, 1, SYSCLR_WINDOWFRAME, 0, DB_STANDARD );
}

/* The bar is a window of its own so it can repaint from WM_PAINT: relying on
 * the owner to redraw it during a tight extract loop is unreliable. */
MRESULT EXPENTRY BarWndProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    if ( msg == WM_PAINT )
    {
        RECTL rcl;
        HPS   hps = WinBeginPaint( hwnd, NULLHANDLE, NULL );
        WinQueryWindowRect( hwnd, &rcl );
        PaintBar( hps, &rcl );
        WinEndPaint( hps );
        return (MRESULT)FALSE;
    }
    return WinDefWindowProc( hwnd, msg, mp1, mp2 );
}

/* Progress dialog: swaps the placeholder static for a real bar window and
 * latches the Cancel request. */
MRESULT EXPENTRY ProgressDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg )
    {
    case WM_INITDLG:
    {
        HWND hwndOld = WinWindowFromID( hwnd, IDC_PROG_BAR );
        SWP  swp;

        if ( hwndOld != NULLHANDLE && WinQueryWindowPos( hwndOld, &swp ) )
        {
            WinDestroyWindow( hwndOld );
            g_hwndBar = WinCreateWindow( hwnd, (PCSZ)g_szBarClass, (PCSZ)"",
                            WS_VISIBLE, swp.x, swp.y, swp.cx, swp.cy,
                            hwnd, HWND_TOP, IDC_PROG_BAR, NULL, NULL );
        }
        return (MRESULT)FALSE;
    }

    case WM_COMMAND:
        if ( SHORT1FROMMP( mp1 ) == DID_CANCEL )
        {
            PmWorkerRequestCancel( &g_worker );
            WinEnableWindow( WinWindowFromID( hwnd, DID_CANCEL ), FALSE );
            return (MRESULT)FALSE;
        }
        break;

    /* Swallow the close box: the operation decides when this box goes away. */
    case WM_CLOSE:
        PmWorkerRequestCancel( &g_worker );
        return (MRESULT)FALSE;
    }
    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}

/* Show the modeless progress box over 'owner' with the given caption and
 * left-hand label ("Extracting:", "Testing:", "Reading:").  The frame is
 * disabled until ProgressEnd().  'cancelable' greys Cancel when FALSE. */
static void ProgressBegin( HWND owner, const char *caption,
                           const char *label, BOOL cancelable )
{
    g_progPercent = 0;
    g_progName[0] = '\0';
    g_hwndBar     = NULLHANDLE;

    g_hwndProgress = WinLoadDlg( HWND_DESKTOP, owner, ProgressDlgProc,
                                 NULLHANDLE, IDD_PROGRESS, NULL );
    if ( g_hwndProgress == NULLHANDLE )
        return;

    WinSetWindowText( g_hwndProgress, (PCSZ)caption );
    WinSetDlgItemText( g_hwndProgress, IDC_PROG_LABEL, (PCSZ)label );
    WinSetDlgItemText( g_hwndProgress, IDC_PROG_FILE, (PCSZ)"" );
    WinEnableWindow( WinWindowFromID( g_hwndProgress, DID_CANCEL ), cancelable );

    WinEnableWindow( g_hwndFrame, FALSE );
    WinShowWindow( g_hwndProgress, TRUE );
    WinUpdateWindow( g_hwndProgress );
}

/* Record the bar position and the file name.  WORKER THREAD: no PM calls,
 * no pump.  The old version painted the bar and then dispatched every pending
 * message from inside the decoder's progress callback, which is how a second
 * Extract could be started while the first was still running. */
static void ProgressSet( LONG percent, const char *text )
{
    if ( percent < 0 )   percent = 0;
    if ( percent > 100 ) percent = 100;
    g_progPercent = percent;

    if ( text )
    {
        strncpy( g_progName, text, CCHMAXPATH );
        g_progName[CCHMAXPATH] = '\0';
    }
}

/* Show what the worker last recorded.  UI THREAD, from the progress timer. */
static void ProgressTick( void )
{
    if ( g_hwndProgress == NULLHANDLE )
        return;

    WinSetDlgItemText( g_hwndProgress, IDC_PROG_FILE, (PCSZ)g_progName );

    if ( g_hwndBar != NULLHANDLE )
    {
        RECTL rcl;
        HPS   hps = WinGetPS( g_hwndBar );
        if ( hps != NULLHANDLE )
        {
            WinQueryWindowRect( g_hwndBar, &rcl );
            PaintBar( hps, &rcl );
            WinReleasePS( hps );
        }
    }
}

/* Tear the box down and re-enable the frame. */
static void ProgressEnd( HWND owner )
{
    (void)owner;
    if ( g_hwndProgress != NULLHANDLE )
    {
        WinEnableWindow( g_hwndFrame, TRUE );
        WinDestroyWindow( g_hwndProgress );
        g_hwndProgress = NULLHANDLE;
        g_hwndBar      = NULLHANDLE;
        WinSetActiveWindow( HWND_DESKTOP, g_hwndFrame );
    }
}

/* Progress callback (SzProgress) for extraction and integrity testing.
 * Returns 0 to request cancellation.  WORKER THREAD.
 *
 * The cancel answer comes from the worker's own flag rather than a separate
 * g_cancel, so that PmWorkerWait -- which is how WM_CLOSE stops a running
 * job -- reaches it too, not just the dialog's Cancel button. */
static int ExtractProgress( void *user, int fileIndex, int fileCount,
                            const char *name )
{
    LONG pct = ( fileCount > 0 )
               ? ( (LONG)fileIndex * 100 ) / fileCount : 0;
    (void)user;
    ProgressSet( pct, name );
    return PmWorkerCancelled( &g_worker ) ? 0 : 1;
}

/* Overwrite prompt hook (ArcOverwriteFn), registered once in main().
 * WORKER THREAD - no PM calls here.  Park the path where the UI thread's
 * dialog can read it, then block in PmWorkerAsk until ArcOnAsk answers.
 * If the job is cancelled while waiting (or the ask machinery is not
 * available), the answer is "No": nothing gets overwritten, and the progress
 * callback then turns the pending cancel into SZ_ERR_CANCEL. */
static int OverwriteHook( void *user, const char *path )
{
    (void)user;
    strncpy( g_owAskPath, path, sizeof( g_owAskPath ) - 1 );
    g_owAskPath[sizeof( g_owAskPath ) - 1] = '\0';
    return (int)PmWorkerAsk( &g_worker, ARCASK_OVERWRITE, ARC_OW_NO );
}

/* The overwrite dialog.  Four answers; Esc (DID_CANCEL) and the close box
 * mean "No".  Dismissed with ARC_OW_* + 1, because a WinDismissDlg value of
 * 0 could not be told from WinDlgBox failing outright. */
MRESULT EXPENTRY OverwriteDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg )
    {
    case WM_INITDLG:
        WinSetDlgItemText( hwnd, IDC_OW_FILE, (PCSZ)g_owAskPath );
        return (MRESULT)FALSE;

    case WM_COMMAND:
        switch ( SHORT1FROMMP( mp1 ) )
        {
        case IDC_OW_YES:    WinDismissDlg( hwnd, ARC_OW_YES + 1 );    return (MRESULT)FALSE;
        case IDC_OW_NO:     WinDismissDlg( hwnd, ARC_OW_NO + 1 );     return (MRESULT)FALSE;
        case IDC_OW_YESALL: WinDismissDlg( hwnd, ARC_OW_YESALL + 1 ); return (MRESULT)FALSE;
        case IDC_OW_NOALL:  WinDismissDlg( hwnd, ARC_OW_NOALL + 1 );  return (MRESULT)FALSE;
        case DID_CANCEL:    WinDismissDlg( hwnd, ARC_OW_NO + 1 );     return (MRESULT)FALSE;
        }
        break;

    case WM_CLOSE:
        WinDismissDlg( hwnd, ARC_OW_NO + 1 );
        return (MRESULT)FALSE;
    }
    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}

/* A worker question arrived (UI THREAD, from WMU_WORKER_ASK or the progress
 * timer - PmWorkerPollAsk latches, so whichever runs first takes it and the
 * other finds nothing).  MUST end in PmWorkerAnswer: the worker is blocked
 * until it does. */
static void ArcOnAsk( HWND hwnd, ULONG question )
{
    (void)hwnd;
    if ( question == ARCASK_OVERWRITE )
    {
        HWND  owner = ( g_hwndProgress != NULLHANDLE ) ? g_hwndProgress
                                                       : g_hwndFrame;
        ULONG r = WinDlgBox( HWND_DESKTOP, owner, OverwriteDlgProc,
                             NULLHANDLE, IDD_OVERWRITE, NULL );
        PmWorkerAnswer( &g_worker,
                        ( r >= 1 && r <= 4 ) ? r - 1 : ARC_OW_NO );
    }
    else
        PmWorkerAnswer( &g_worker, ARC_OW_NO );   /* unknown question */
}

/*===========================================================================
 * Worker plumbing
 *===========================================================================*/

static void EnsureWorker( void )
{
    if ( !g_workerReady )
    {
        PmWorkerInit( &g_worker );
        g_workerReady = 1;
    }
}

static BOOL ArcBusy( void )
{
    EnsureWorker();
    return PmWorkerIsBusy( &g_worker );
}

/* Stop a running job before the window it posts to is destroyed. */
static BOOL ArcShutdown( void )
{
    EnsureWorker();

    if ( !PmWorkerIsBusy( &g_worker ) )
        return TRUE;

    return PmWorkerWait( &g_worker, 5000 );
}

/* The worker thread's entry point.  !! NO PM CALLS !! */
static void ArcWorkerBody( void *arg )
{
    (void)arg;

    switch ( g_job.kind )
    {
    case ARCJOB_EXTRACT:
        if ( g_job.sel )
            g_job.rc = ArcExtractItems( g_arc, g_job.sel, g_job.selCount,
                                        g_job.dest, ExtractProgress, NULL );
        else
            g_job.rc = ArcExtractAll( g_arc, g_job.dest,
                                      ExtractProgress, NULL );
        break;

    case ARCJOB_TEST:
        g_job.rc = ArcTestAll( g_arc, ExtractProgress, NULL );
        break;

    case ARCJOB_OPEN:
        /* Header parse only.  The container fill (PopulateList) is PM work
           and stays on the UI thread -- see ArcOnDone. */
        g_job.rc = ArcOpen( g_job.openPath, &g_job.openArc );
        break;
    }

    if ( g_job.sel )
    {
        free( g_job.sel );
        g_job.sel = NULL;
    }

    PmWorkerEnd( &g_worker, (ULONG)g_job.rc );
}

/* Put the progress box up and start the worker.  FALSE if it could not
 * start, in which case nothing was shown and g_job.sel has been freed. */
static BOOL ArcStartJob( HWND hwnd, int kind, const char *caption,
                         const char *label, BOOL cancelable )
{
    EnsureWorker();

    if ( PmWorkerIsBusy( &g_worker ) )
    {
        Say( hwnd, "Another operation is still running.",
             MB_OK | MB_INFORMATION );
        if ( g_job.sel ) { free( g_job.sel ); g_job.sel = NULL; }
        return FALSE;
    }

    g_job.kind    = kind;
    g_job.rc      = SZ_OK;
    g_job.openArc = NULL;

    ProgressBegin( hwnd, caption, label, cancelable );

    if ( !PmWorkerStart( &g_worker, ArcWorkerBody, NULL,
                         PMWORKER_STACK, g_hwndClient ) )
    {
        ProgressEnd( hwnd );
        if ( g_job.sel ) { free( g_job.sel ); g_job.sel = NULL; }
        Say( hwnd, "Could not start the operation.",
             MB_OK | MB_ICONEXCLAMATION );
        return FALSE;
    }

    WinStartTimer( g_hab, g_hwndClient, TID_ARCPROGRESS, ARC_TIMER_MS );
    return TRUE;
}

/*===========================================================================
 * Extraction
 *===========================================================================*/

/* Start an extraction with the progress box up: sel == NULL extracts the whole
 * archive, otherwise the given selCount entry indices.
 *
 * ASYNCHRONOUS.  Takes ownership of sel in every case -- the worker frees it,
 * because this returns long before the extraction is finished.  The result is
 * reported by ArcOnDone. */
static void RunExtraction( HWND hwnd, int *sel, int selCount,
                           const char *dest )
{
    g_job.sel      = sel;
    g_job.selCount = selCount;

    strncpy( g_job.dest, dest, sizeof( g_job.dest ) - 1 );
    g_job.dest[sizeof( g_job.dest ) - 1] = '\0';

    ArcStartJob( hwnd, ARCJOB_EXTRACT, "Extracting", "Extracting:", TRUE );
}

/*---------------------------------------------------------------------------
 * ArcOnDone - a job finished (UI THREAD, from WMU_WORKER_DONE)
 *-------------------------------------------------------------------------*/

static void ArcOnDone( HWND hwnd, BOOL cancelled )
{
    char msg[CCHMAXPATH + 200];
    int  rc   = g_job.rc;
    int  kind = g_job.kind;

    (void)cancelled;

    WinStopTimer( g_hab, g_hwndClient, TID_ARCPROGRESS );

    /* ARCJOB_OPEN does its PM half here: the header parse ran on the worker,
       but filling the container has to happen on this thread.  The progress
       box stays up across it, which is what it was there for. */
    if ( kind == ARCJOB_OPEN && rc == SZ_OK )
    {
        CloseArchive();
        g_arc = g_job.openArc;
        g_job.openArc = NULL;

        strncpy( g_arcPath, g_job.openPath, sizeof( g_arcPath ) - 1 );
        g_arcPath[sizeof( g_arcPath ) - 1] = '\0';

        PopulateList();
    }

    ProgressEnd( hwnd );

    g_job.kind = 0;

    switch ( kind )
    {
    case ARCJOB_OPEN:
        if ( rc != SZ_OK )
        {
            sprintf( msg, "Could not open the archive:\n\n%s",
                     ArcErrorText( rc ) );
            Say( hwnd, msg, MB_OK | MB_ICONEXCLAMATION );
        }
        UpdateTitle();
        UpdateToolbarState();
        break;

    case ARCJOB_EXTRACT:
        if ( rc == SZ_OK )
        {
            sprintf( msg, "Extraction complete:\n\n%s", g_job.dest );
            Say( hwnd, msg, MB_OK | MB_INFORMATION );
        }
        else if ( rc == SZ_ERR_CANCEL )
            Say( hwnd, "Extraction cancelled.", MB_OK | MB_INFORMATION );
        else if ( rc == SZ_ERR_UNSUPPORTED )
        {
            sprintf( msg, "Extraction stopped - unsupported feature:\n\n%s",
                     ArcUnsupportedHint( g_arc ) );
            Say( hwnd, msg, MB_OK | MB_ICONEXCLAMATION );
        }
        else
        {
            sprintf( msg, "Extraction failed:\n\n%s", ArcErrorText( rc ) );
            Say( hwnd, msg, MB_OK | MB_ICONEXCLAMATION );
        }
        break;

    case ARCJOB_TEST:
        if ( rc == SZ_OK )
            Say( hwnd, "Integrity test passed.\n\n"
                       "All files decoded and their CRCs matched.",
                 MB_OK | MB_INFORMATION );
        else if ( rc == SZ_ERR_CANCEL )
            Say( hwnd, "Integrity test cancelled.", MB_OK | MB_INFORMATION );
        else if ( rc == SZ_ERR_UNSUPPORTED )
        {
            sprintf( msg, "Integrity could not be fully verified.\n\n%s",
                     ArcUnsupportedHint( g_arc ) );
            Say( hwnd, msg, MB_OK | MB_ICONEXCLAMATION );
        }
        else
        {
            sprintf( msg, "Integrity test FAILED:\n\n%s", ArcErrorText( rc ) );
            Say( hwnd, msg, MB_OK | MB_ICONEXCLAMATION );
        }
        break;
    }
}

/* Create a directory and any missing parents.  TRUE if it exists afterwards. */
static BOOL CreateDirTree( const char *path )
{
    char  buf[CCHMAXPATH];
    char *p;

    strncpy( buf, path, sizeof( buf ) - 1 );
    buf[sizeof( buf ) - 1] = '\0';

    p = buf;
    if ( p[0] && p[1] == ':' ) p += 2;            /* skip the drive letter */
    if ( *p == '\\' )          p++;               /* skip the root slash   */
    for ( ; *p; p++ )
        if ( *p == '\\' )
        {
            *p = '\0';
            DosCreateDir( (PCSZ)buf, NULL );      /* "exists" is not an error */
            *p = '\\';
        }
    DosCreateDir( (PCSZ)buf, NULL );              /* final component */

    return IsDir( path );
}

/* New-Folder dialog: capture the typed name into g_newFolderName. */
MRESULT EXPENTRY NewFolderDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg )
    {
    case WM_INITDLG:
        /* A PM entry field defaults to a 32-character limit. */
        WinSendDlgItemMsg( hwnd, IDC_NEWDIR_NAME, EM_SETTEXTLIMIT,
                           MPFROMSHORT( (SHORT)( sizeof( g_newFolderName ) - 1 ) ),
                           MPVOID );
        WinSetFocus( HWND_DESKTOP, WinWindowFromID( hwnd, IDC_NEWDIR_NAME ) );
        return (MRESULT)TRUE;                     /* we set the focus */

    case WM_COMMAND:
        switch ( SHORT1FROMMP( mp1 ) )
        {
        case DID_OK:
            WinQueryDlgItemText( hwnd, IDC_NEWDIR_NAME,
                                 (LONG)sizeof( g_newFolderName ),
                                 (PSZ)g_newFolderName );
            if ( !g_newFolderName[0] )
            {
                Say( hwnd, "Please enter a folder name.",
                     MB_OK | MB_INFORMATION );
                return (MRESULT)FALSE;
            }
            WinDismissDlg( hwnd, DID_OK );
            return (MRESULT)FALSE;
        case DID_CANCEL:
            WinDismissDlg( hwnd, DID_CANCEL );
            return (MRESULT)FALSE;
        }
        break;
    }
    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}

/* Prompt for a new folder name, create it (under parentPath unless the name
 * is absolute), and return the full path in 'out'. */
static BOOL NewFolderDlg( HWND owner, const char *parentPath,
                          char *out, int outSize )
{
    int abs, n;

    g_newFolderName[0] = '\0';
    if ( WinDlgBox( HWND_DESKTOP, owner, NewFolderDlgProc, NULLHANDLE,
                    IDD_NEWFOLDER, NULL ) != DID_OK )
        return FALSE;

    abs = ( g_newFolderName[0] == '\\' ) ||
          ( g_newFolderName[0] && g_newFolderName[1] == ':' );

    if ( abs || !parentPath || !parentPath[0] )
    {
        strncpy( out, g_newFolderName, (size_t)outSize - 1 );
        out[outSize - 1] = '\0';
    }
    else
    {
        strncpy( out, parentPath, (size_t)outSize - 1 );
        out[outSize - 1] = '\0';
        n = (int)strlen( out );
        if ( n > 0 && out[n-1] != '\\' && n < outSize - 1 )
        { out[n++] = '\\'; out[n] = '\0'; }
        strncpy( out + strlen( out ), g_newFolderName,
                 (size_t)( outSize - 1 - (int)strlen( out ) ) );
        out[outSize - 1] = '\0';
    }

    if ( !CreateDirTree( out ) )
    {
        Say( owner, "Could not create that folder.", MB_OK | MB_ICONEXCLAMATION );
        return FALSE;
    }
    return TRUE;
}

/*---- Destination folder browser -------------------------------------------
 * PM has nothing like DlgDirList, so the listbox is filled by hand: the
 * parent entry, then the drives from DosQueryCurrentDisk's logical-drive
 * bitmap, then this directory's subdirectories from DosFindFirst.  Browsing
 * changes the process's current directory and restores it when the box
 * closes.
 *
 * The name array is file-static rather than a dialog local: 1024 CCHMAXPATH
 * names is a quarter of a megabyte, which has no business on a stack.
 *-------------------------------------------------------------------------- */

#define FP_MAXDIRS 1024
static char g_fpDirs[FP_MAXDIRS][CCHMAXPATH];
static int  g_fpDirCount;

static int FpCompare( const void *a, const void *b )
{
    return stricmp( (const char *)a, (const char *)b );
}

/* Change into a selected list entry.  Entries are shown as "[..]", "[-C-]"
 * for a drive and "[NAME]" for a subdirectory, so the brackets come off
 * first; a drive needs an explicit root, since setting the default disk alone
 * would land in that drive's current directory instead. */
static void FolderPickEnter( const char *item )
{
    char work[CCHMAXPATH];
    int  n;

    if ( !item || !item[0] )
        return;

    strncpy( work, item, sizeof( work ) - 1 );
    work[sizeof( work ) - 1] = '\0';

    n = (int)strlen( work );
    if ( n >= 2 && work[0] == '[' && work[n-1] == ']' )
    {
        work[n-1] = '\0';
        memmove( work, work + 1, strlen( work + 1 ) + 1 );
        n -= 2;
    }

    if ( n == 3 && work[0] == '-' && work[2] == '-' )     /* "-C-" : a drive */
    {
        char root[4];
        root[0] = work[1]; root[1] = ':'; root[2] = '\\'; root[3] = '\0';
        SetCwd( root );
        return;
    }

    SetCwd( work );                                       /* ".." or a name  */
}

/* (Re)fill the list for the current directory. */
static void FolderPickRefresh( HWND hDlg )
{
    char        cur[CCHMAXPATH], item[CCHMAXPATH + 4];
    ULONG       drive = 0, map = 0;
    HDIR        hdir  = HDIR_CREATE;
    FILEFINDBUF3 ffb;
    ULONG       count = 1;
    HWND        hwndLB = WinWindowFromID( hDlg, IDC_FP_LIST );
    int         i;

    WinSendMsg( hwndLB, LM_DELETEALL, MPVOID, MPVOID );

    GetCwd( cur, sizeof( cur ) );
    WinSetDlgItemText( hDlg, IDC_FP_PATH, (PCSZ)cur );

    /* the parent, unless we are already at a root */
    if ( !( cur[0] && cur[1] == ':' && cur[2] == '\\' && cur[3] == '\0' ) )
        WinSendMsg( hwndLB, LM_INSERTITEM, MPFROMLONG( LIT_END ),
                    MPFROMP( "[..]" ) );

    /* every drive the system knows about */
    if ( DosQueryCurrentDisk( &drive, &map ) == 0 )
        for ( i = 0; i < 26; i++ )
            if ( map & ( 1UL << i ) )
            {
                sprintf( item, "[-%c-]", (char)( 'A' + i ) );
                WinSendMsg( hwndLB, LM_INSERTITEM, MPFROMLONG( LIT_END ),
                            MPFROMP( item ) );
            }

    /* subdirectories, gathered then sorted.  "*.*" matches only names with a
     * dot on HPFS, so the pattern has to be "*". */
    g_fpDirCount = 0;
    if ( DosFindFirst( (PCSZ)"*", &hdir,
                       MUST_HAVE_DIRECTORY | FILE_DIRECTORY | FILE_HIDDEN |
                       FILE_SYSTEM | FILE_READONLY,
                       &ffb, sizeof( ffb ), &count, FIL_STANDARD ) == 0 )
    {
        do
        {
            if ( strcmp( ffb.achName, "." ) != 0 &&
                 strcmp( ffb.achName, ".." ) != 0 &&
                 g_fpDirCount < FP_MAXDIRS )
            {
                strncpy( g_fpDirs[g_fpDirCount], ffb.achName, CCHMAXPATH - 1 );
                g_fpDirs[g_fpDirCount][CCHMAXPATH - 1] = '\0';
                g_fpDirCount++;
            }
            count = 1;
        } while ( DosFindNext( hdir, &ffb, sizeof( ffb ), &count ) == 0 );
        DosFindClose( hdir );
    }

    if ( g_fpDirCount > 1 )
        qsort( g_fpDirs, (size_t)g_fpDirCount, CCHMAXPATH, FpCompare );

    for ( i = 0; i < g_fpDirCount; i++ )
    {
        sprintf( item, "[%s]", g_fpDirs[i] );
        WinSendMsg( hwndLB, LM_INSERTITEM, MPFROMLONG( LIT_END ),
                    MPFROMP( item ) );
    }
}

/* The selected item's text, or "" when nothing is highlighted. */
static void FolderPickSelText( HWND hDlg, char *buf, int size )
{
    HWND hwndLB = WinWindowFromID( hDlg, IDC_FP_LIST );
    LONG sel;

    buf[0] = '\0';
    sel = (LONG)WinSendMsg( hwndLB, LM_QUERYSELECTION,
                            MPFROMLONG( LIT_FIRST ), MPVOID );
    if ( sel == LIT_NONE )
        return;

    WinSendMsg( hwndLB, LM_QUERYITEMTEXT,
                MPFROM2SHORT( (SHORT)sel, (SHORT)size ), MPFROMP( buf ) );
}

MRESULT EXPENTRY FolderPickProc( HWND hDlg, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg )
    {
    case WM_INITDLG:
        GetCwd( g_fpSaveCwd, sizeof( g_fpSaveCwd ) );
        if ( g_folderPick[0] )
            SetCwd( g_folderPick );               /* start in the suggestion */
        FolderPickRefresh( hDlg );
        return (MRESULT)FALSE;

    case WM_CONTROL:
        if ( SHORT1FROMMP( mp1 ) == IDC_FP_LIST &&
             SHORT2FROMMP( mp1 ) == LN_ENTER )    /* double-click / Enter */
        {
            char sel[CCHMAXPATH];
            FolderPickSelText( hDlg, sel, sizeof( sel ) );
            FolderPickEnter( sel );
            FolderPickRefresh( hDlg );
            return (MRESULT)FALSE;
        }
        break;

    case WM_COMMAND:
        switch ( SHORT1FROMMP( mp1 ) )
        {
        case DID_OK:
        {
            char sel[CCHMAXPATH];
            /* a highlighted folder wins; otherwise take the current directory */
            FolderPickSelText( hDlg, sel, sizeof( sel ) );
            if ( sel[0] )
                FolderPickEnter( sel );
            GetCwd( g_folderPick, sizeof( g_folderPick ) );
            SetCwd( g_fpSaveCwd );                /* restore the process CWD */
            WinDismissDlg( hDlg, DID_OK );
            return (MRESULT)FALSE;
        }
        case DID_CANCEL:
            SetCwd( g_fpSaveCwd );
            WinDismissDlg( hDlg, DID_CANCEL );
            return (MRESULT)FALSE;
        }
        break;
    }
    return WinDefDlgProc( hDlg, msg, mp1, mp2 );
}

/* Pick a destination folder.  'dir' holds the suggestion on entry and the
 * chosen folder on return.  FALSE if cancelled. */
static BOOL BrowseForFolder( HWND owner, char *dir, int dirSize )
{
    strncpy( g_folderPick, dir ? dir : "", sizeof( g_folderPick ) - 1 );
    g_folderPick[sizeof( g_folderPick ) - 1] = '\0';

    if ( WinDlgBox( HWND_DESKTOP, owner, FolderPickProc, NULLHANDLE,
                    IDD_FOLDERPICK, NULL ) != DID_OK )
        return FALSE;

    strncpy( dir, g_folderPick, (size_t)dirSize - 1 );
    dir[dirSize - 1] = '\0';
    return TRUE;
}

/* Extract dialog: a path field with Browse / New Folder and OK / Cancel.  The
 * chosen path is left in g_extractPath. */
MRESULT EXPENTRY ExtractDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg )
    {
    case WM_INITDLG:
        WinSetWindowText( hwnd, (PCSZ)g_extractTitle );
        WinSendDlgItemMsg( hwnd, IDC_EXT_PATH, EM_SETTEXTLIMIT,
                           MPFROMSHORT( (SHORT)( sizeof( g_extractPath ) - 1 ) ),
                           MPVOID );
        WinSetDlgItemText( hwnd, IDC_EXT_PATH, (PCSZ)g_extractPath );
        WinSetFocus( HWND_DESKTOP, WinWindowFromID( hwnd, IDC_EXT_PATH ) );
        return (MRESULT)TRUE;                     /* we set the focus */

    case WM_COMMAND:
        switch ( SHORT1FROMMP( mp1 ) )
        {
        case IDC_EXT_BROWSE:
        {
            char buf[CCHMAXPATH];
            WinQueryDlgItemText( hwnd, IDC_EXT_PATH,
                                 (LONG)sizeof( buf ), (PSZ)buf );
            if ( BrowseForFolder( hwnd, buf, sizeof( buf ) ) )
                WinSetDlgItemText( hwnd, IDC_EXT_PATH, (PCSZ)buf );
            return (MRESULT)FALSE;
        }
        case IDC_EXT_NEWDIR:
        {
            char cur[CCHMAXPATH], made[CCHMAXPATH];
            WinQueryDlgItemText( hwnd, IDC_EXT_PATH,
                                 (LONG)sizeof( cur ), (PSZ)cur );
            if ( NewFolderDlg( hwnd, cur, made, sizeof( made ) ) )
                WinSetDlgItemText( hwnd, IDC_EXT_PATH, (PCSZ)made );
            return (MRESULT)FALSE;
        }
        case DID_OK:
            WinQueryDlgItemText( hwnd, IDC_EXT_PATH,
                                 (LONG)sizeof( g_extractPath ),
                                 (PSZ)g_extractPath );
            if ( !g_extractPath[0] )
            {
                Say( hwnd, "Please enter or choose a destination folder.",
                     MB_OK | MB_INFORMATION );
                return (MRESULT)FALSE;
            }
            WinDismissDlg( hwnd, DID_OK );
            return (MRESULT)FALSE;
        case DID_CANCEL:
            WinDismissDlg( hwnd, DID_CANCEL );
            return (MRESULT)FALSE;
        }
        break;
    }
    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}

/* The single extract command: pick a destination (typed, browsed or newly
 * made), create it if needed, then extract the selected entries if any are
 * selected, or the whole archive if not.  Nothing selected is the state a
 * freshly opened archive is left in, so Extract without a selection means
 * "extract everything" - which the dialog's title spells out, since which of
 * the two is about to happen is otherwise easy to get wrong. */
static void DoExtractTo( HWND hwnd )
{
    int *sel = NULL;
    int  count;

    if ( !g_arc )
    {
        Say( hwnd, "Open an archive first.", MB_OK | MB_INFORMATION );
        return;
    }

    count = CnrSelectedCount( g_hwndCnr );
    if ( count > 0 )
    {
        sel = (int *)malloc( (size_t)count * sizeof( int ) );
        if ( !sel )
        {
            Say( hwnd, "Not enough memory for the selection.",
                 MB_OK | MB_ICONEXCLAMATION );
            return;
        }
        count = CnrSelectedIndices( g_hwndCnr, sel, count );
    }

    if ( count > 0 )
        sprintf( g_extractTitle, "Extract %d Selected %s", count,
                 ( count == 1 ) ? "Entry" : "Entries" );
    else
        sprintf( g_extractTitle, "Extract All %d Entries",
                 ArcNumEntries( g_arc ) );

    DeriveDestDir( g_extractPath, sizeof( g_extractPath ) );   /* suggestion */
    if ( WinDlgBox( HWND_DESKTOP, hwnd, ExtractDlgProc, NULLHANDLE,
                    IDD_EXTRACT, NULL ) != DID_OK )
    {
        if ( sel ) free( sel );
        return;
    }

    if ( !g_extractPath[0] ) { if ( sel ) free( sel ); return; }

    if ( !CreateDirTree( g_extractPath ) )
    {
        Say( hwnd, "Could not create the destination folder.",
             MB_OK | MB_ICONEXCLAMATION );
        if ( sel ) free( sel );
        return;
    }

    /* RunExtraction takes ownership of sel -- the extraction outlives this
       function now, so the selection cannot be freed on the way out. */
    if ( count > 0 ) RunExtraction( hwnd, sel, count, g_extractPath );
    else             RunExtraction( hwnd, NULL, 0, g_extractPath );
}

/*===========================================================================
 * Archive information
 *===========================================================================*/

/* Format a byte count as a friendly size string. */
static void FormatSize( double bytes, char *buf )
{
    if ( bytes < 1024.0 )
        sprintf( buf, "%.0f bytes", bytes );
    else if ( bytes < 1024.0 * 1024.0 )
        sprintf( buf, "%.1f KB", bytes / 1024.0 );
    else if ( bytes < 1024.0 * 1024.0 * 1024.0 )
        sprintf( buf, "%.1f MB", bytes / ( 1024.0 * 1024.0 ) );
    else
        sprintf( buf, "%.2f GB", bytes / ( 1024.0 * 1024.0 * 1024.0 ) );
}

/* Append a method name to a comma-separated list if not already present. */
static void AddDistinctMethod( char *list, int listSize, const char *m )
{
    int len;
    if ( !m || !m[0] ) return;
    if ( strstr( list, m ) ) return;                     /* already listed */
    len = (int)strlen( list );
    if ( len + (int)strlen( m ) + 3 >= listSize ) return;
    if ( len ) strcat( list, ", " );
    strcat( list, m );
}

/* Info dialog: shows the pre-built g_infoText block in a read-only MLE.
 *
 * The report is aligned with spaces, so the MLE is given a monospaced
 * presentation font - PM's default is proportional. */
MRESULT EXPENTRY InfoDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg )
    {
    case WM_INITDLG:
    {
        HWND        hwndMle = WinWindowFromID( hwnd, IDC_INFO_TEXT );
        const char *font    = "10.System Monospaced";
        ULONG       len     = (ULONG)strlen( g_infoText );
        IPT         ipt     = 0;

        WinSetPresParam( hwndMle, PP_FONTNAMESIZE,
                         (ULONG)strlen( font ) + 1, (PVOID)font );

        WinSendMsg( hwndMle, MLM_SETTEXTLIMIT, MPFROMLONG( len + 1 ), MPVOID );
        WinSendMsg( hwndMle, MLM_DISABLEREFRESH, MPVOID, MPVOID );
        WinSendMsg( hwndMle, MLM_SETIMPORTEXPORT,
                    MPFROMP( g_infoText ), MPFROMSHORT( (SHORT)len ) );
        WinSendMsg( hwndMle, MLM_IMPORT, MPFROMP( &ipt ), MPFROMLONG( len ) );
        WinSendMsg( hwndMle, MLM_ENABLEREFRESH, MPVOID, MPVOID );
        return (MRESULT)FALSE;
    }

    case WM_COMMAND:
        if ( SHORT1FROMMP( mp1 ) == DID_OK || SHORT1FROMMP( mp1 ) == DID_CANCEL )
        {
            WinDismissDlg( hwnd, DID_OK );
            return (MRESULT)FALSE;
        }
        break;
    }
    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}

/* Summarise the open archive and show it in the Info dialog. */
static void DoInfo( HWND hwnd )
{
    int    n, i, fileCount = 0, packedKnown = 0;
    double totalSize = 0.0, totalPacked = 0.0;
    char   methods[128];
    char   szSize[32], szPacked[32], ratioBuf[32], savedBuf[16];

    if ( !g_arc )
    {
        Say( hwnd, "Open an archive first.", MB_OK | MB_INFORMATION );
        return;
    }

    methods[0] = '\0';
    n = ArcNumEntries( g_arc );
    for ( i = 0; i < n; i++ )
    {
        UInt32 packed;
        if ( ArcEntryIsDir( g_arc, i ) ) continue;
        fileCount++;
        totalSize += (double)ArcEntrySize( g_arc, i );
        packed = ArcEntryPacked( g_arc, i );
        if ( packed != 0xFFFFFFFFUL )
        { totalPacked += (double)packed; packedKnown++; }
        AddDistinctMethod( methods, sizeof( methods ),
                           ArcEntryMethod( g_arc, i ) );
    }

    FormatSize( totalSize, szSize );
    if ( packedKnown > 0 ) FormatSize( totalPacked, szPacked );
    else                   strcpy( szPacked, "n/a (per-folder)" );

    if ( packedKnown > 0 && totalPacked > 0.0 )
    {
        int saved = ( totalSize > 0.0 )
            ? (int)( ( 1.0 - totalPacked / totalSize ) * 100.0 + 0.5 ) : 0;
        sprintf( ratioBuf, "%.2f : 1", totalSize / totalPacked );
        sprintf( savedBuf, "%d%%", saved );
    }
    else
    {
        strcpy( ratioBuf, "n/a" );
        strcpy( savedBuf, "n/a" );
    }
    if ( !methods[0] ) strcpy( methods, "(none)" );

    sprintf( g_infoText,
        "Archive:       %s\r\n"
        "Format:        %s\r\n"
        "Files:         %d\r\n"
        "\r\n"
        "Total size:    %s\r\n"
        "Packed size:   %s\r\n"
        "Compression:   %s\r\n"
        "\r\n"
        "Ratio:         %s\r\n"
        "Space saved:   %s\r\n",
        FileNamePart( g_arcPath ), ArcFormatName( g_arc ),
        fileCount, szSize, szPacked, methods, ratioBuf, savedBuf );

    WinDlgBox( HWND_DESKTOP, hwnd, InfoDlgProc, NULLHANDLE, IDD_INFO, NULL );
}

/*===========================================================================
 * Integrity test
 *===========================================================================*/

/* Decode and CRC-check every entry, writing nothing to disk.
 *
 * ASYNCHRONOUS: this starts the test and returns.  ArcOnDone reports it. */
static void DoTest( HWND hwnd )
{
    if ( !g_arc )
    {
        Say( hwnd, "Open an archive first.", MB_OK | MB_INFORMATION );
        return;
    }

    g_job.sel = NULL;
    ArcStartJob( hwnd, ARCJOB_TEST, "Testing", "Testing:", TRUE );
}

/*===========================================================================
 * Toolbar
 *
 * There is no toolbar control on OS/2, so the strip is plain WC_BUTTONs whose
 * IDs are the menu command IDs - which is why WM_COMMAND needs no extra
 * handling for them.  BS_NOPOINTERFOCUS keeps a click from pulling the focus
 * off the container.
 *===========================================================================*/
static void CreateToolbar( HWND parent )
{
    HPS         hps;
    FONTMETRICS fm;
    int         i;

    /* Size the buttons from the system font so the strip scales with the
     * display, rather than freezing pixel counts from a 1024x768 desktop. */
    hps = WinGetPS( parent );
    if ( hps != NULLHANDLE )
    {
        memset( &fm, 0, sizeof( fm ) );
        if ( GpiQueryFontMetrics( hps, (LONG)sizeof( fm ), &fm ) &&
             fm.lAveCharWidth > 0 && fm.lMaxBaselineExt > 0 )
        {
            g_btnW = fm.lAveCharWidth * 10;
            g_btnH = fm.lMaxBaselineExt * 2;
        }
        WinReleasePS( hps );
    }
    g_tbH = g_btnH + 8;

    for ( i = 0; i < TB_COUNT; i++ )
        g_hwndBtn[i] = WinCreateWindow( parent, (PCSZ)WC_BUTTON,
                            (PCSZ)g_tbText[i],
                            WS_VISIBLE | BS_PUSHBUTTON | BS_NOPOINTERFOCUS,
                            0, 0, g_btnW, g_btnH,
                            parent, HWND_TOP, g_tbId[i], NULL, NULL );
}

/* Enable the archive-dependent buttons only while an archive is open. */
static void UpdateToolbarState( void )
{
    BOOL have = ( g_arc != NULL );
    int  i;

    for ( i = 0; i < TB_COUNT; i++ )
    {
        if ( g_hwndBtn[i] == NULLHANDLE ) continue;
        if ( g_tbId[i] == IDM_ARCHIVE_INFO ||
             g_tbId[i] == IDM_ARCHIVE_TEST ||
             g_tbId[i] == IDM_ARCHIVE_EXTRACT )
            WinEnableWindow( g_hwndBtn[i], have );
    }
}

/* Lay the toolbar strip across the top and give the container the rest.  PM's
 * y grows upward, so "the top" is yTop - g_tbH. */
static void LayoutClient( HWND hwnd )
{
    RECTL rcl;
    LONG  cx, cy, x, y;
    int   i;

    WinQueryWindowRect( hwnd, &rcl );
    cx = rcl.xRight - rcl.xLeft;
    cy = rcl.yTop   - rcl.yBottom;

    y = cy - g_tbH + ( g_tbH - g_btnH ) / 2;
    x = 4;
    for ( i = 0; i < TB_COUNT; i++ )
    {
        if ( g_hwndBtn[i] == NULLHANDLE ) continue;
        WinSetWindowPos( g_hwndBtn[i], HWND_TOP, x, y, g_btnW, g_btnH,
                         SWP_MOVE | SWP_SIZE | SWP_SHOW );
        x += g_btnW + 4;
    }

    if ( g_hwndCnr != NULLHANDLE )
    {
        LONG cyCnr = cy - g_tbH;         /* the window can be shrunk below the
                                          * toolbar strip; never ask for a
                                          * negative size */
        if ( cyCnr < 0 ) cyCnr = 0;
        WinSetWindowPos( g_hwndCnr, HWND_TOP, 0, 0, cx, cyCnr,
                         SWP_MOVE | SWP_SIZE | SWP_SHOW );
    }
}

/*===========================================================================
 * Drag and drop
 *
 * A dropped archive arrives through direct manipulation: DM_DRAGOVER to say
 * whether we would take it, DM_DROP to receive it.  The container covers most
 * of the client, and it
 * relays what lands on it to its owner as CN_DRAGOVER / CN_DROP, so both
 * routes end up in the two helpers below.
 *===========================================================================*/

/* Accept exactly one droppable OS/2 file. */
static MRESULT DragOver( PDRAGINFO pdi )
{
    USHORT usOp = DOR_NODROPOP, usInd = DO_COPY;

    if ( pdi == NULL || !DrgAccessDraginfo( pdi ) )
        return MRFROM2SHORT( DOR_NEVERDROP, 0 );

    if ( pdi->cditem == 1 )
    {
        PDRAGITEM pdit = DrgQueryDragitemPtr( pdi, 0 );
        if ( pdit != NULL &&
             DrgVerifyRMF( pdit, (PCSZ)"DRM_OS2FILE", NULL ) &&
             ( pdit->fsSupportedOps & DO_COPYABLE ) )
            usOp = DOR_DROP;
    }

    DrgFreeDraginfo( pdi );
    return MRFROM2SHORT( usOp, usInd );
}

/* Take the dropped file's full path, then post it to ourselves: the drag
 * transaction must be finished before we spend seconds parsing an archive. */
static void DragDrop( HWND hwnd, PDRAGINFO pdi )
{
    if ( pdi == NULL || !DrgAccessDraginfo( pdi ) )
        return;

    g_dropPath[0] = '\0';
    if ( pdi->cditem >= 1 )
    {
        PDRAGITEM pdit = DrgQueryDragitemPtr( pdi, 0 );
        if ( pdit != NULL )
        {
            char dir[CCHMAXPATH], name[CCHMAXPATH];

            dir[0] = name[0] = '\0';
            DrgQueryStrName( pdit->hstrContainerName, sizeof( dir ), (PCSZ)dir );
            DrgQueryStrName( pdit->hstrSourceName,  sizeof( name ), (PCSZ)name );

            /* the container name already carries its trailing backslash */
            if ( strlen( dir ) + strlen( name ) < sizeof( g_dropPath ) )
            {
                strcpy( g_dropPath, dir );
                strcat( g_dropPath, name );
            }
        }
    }

    DrgDeleteDraginfoStrHandles( pdi );
    DrgFreeDraginfo( pdi );

    if ( g_dropPath[0] )
        WinPostMsg( hwnd, WM_XA_OPENDROP, MPVOID, MPVOID );
}

/*===========================================================================
 * Menu item state
 *===========================================================================*/
static void MenuEnable( HWND hwndMenu, USHORT id, BOOL bEnable )
{
    WinSendMsg( hwndMenu, MM_SETITEMATTR,
                MPFROM2SHORT( (SHORT)id, TRUE ),
                MPFROM2SHORT( MIA_DISABLED,
                              (SHORT)( bEnable ? 0 : MIA_DISABLED ) ) );
}

/* Tick / untick a menu item.  PM's resource grammar has no CHECKED keyword on
 * MENUITEM (unlike Windows' RC), so a checkable item's initial state has to be
 * pushed from WM_INITMENU rather than declared in the .RC. */
static void MenuCheck( HWND hwndMenu, USHORT id, BOOL bCheck )
{
    WinSendMsg( hwndMenu, MM_SETITEMATTR,
                MPFROM2SHORT( (SHORT)id, TRUE ),
                MPFROM2SHORT( MIA_CHECKED,
                              (SHORT)( bCheck ? MIA_CHECKED : 0 ) ) );
}

/*===========================================================================
 * Client window procedure
 *===========================================================================*/
MRESULT EXPENTRY ClientWndProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg )
    {
    /*----------------------------------------------------------------------
     * WinCreateStdWindow hands the client its final size, so a WM_SIZE need
     * never arrive - the first layout has to happen here.  (WM_CREATE
     * returns TRUE to ABORT creation on PM, inverted from Windows.)
     *--------------------------------------------------------------------*/
    case WM_CREATE:
        CreateToolbar( hwnd );

        g_hwndCnr = WinCreateWindow( hwnd, (PCSZ)WC_CONTAINER, (PCSZ)"",
                        WS_VISIBLE | CCS_EXTENDSEL | CCS_READONLY |
                        CCS_MINIRECORDCORE,
                        0, 0, 0, 0, hwnd, HWND_BOTTOM, IDC_LIST, NULL, NULL );
        if ( g_hwndCnr == NULLHANDLE )
            return (MRESULT)TRUE;                 /* abort window creation */

        /* Take over the container's messages for the selection rules in
         * CnrSubProc.  If PM will not hand the old procedure over there is
         * nothing to chain to, so the container is simply left as it was. */
        g_pfnCnrOld = WinSubclassWindow( g_hwndCnr, CnrSubProc );

        CnrSetupColumns( g_hwndCnr );
        LayoutClient( hwnd );
        UpdateToolbarState();
        return (MRESULT)FALSE;

    case WM_SIZE:
        LayoutClient( hwnd );
        return (MRESULT)FALSE;

    /* The container fills everything but the toolbar strip, which is all this
     * window ever has to paint. */
    case WM_PAINT:
    {
        RECTL rcl;
        HPS   hps = WinBeginPaint( hwnd, NULLHANDLE, NULL );
        WinQueryWindowRect( hwnd, &rcl );
        if ( rcl.yTop - g_tbH > rcl.yBottom )
            rcl.yBottom = rcl.yTop - g_tbH;
        WinFillRect( hps, &rcl, SYSCLR_BUTTONMIDDLE );
        WinEndPaint( hps );
        return (MRESULT)FALSE;
    }

    case WM_SETFOCUS:
        if ( SHORT1FROMMP( mp2 ) && g_hwndCnr != NULLHANDLE )
            WinSetFocus( HWND_DESKTOP, g_hwndCnr );
        return (MRESULT)FALSE;

    /*----------------------------------------------------------------------
     * Grey the archive-dependent items as the pulldown opens.  PM names the
     * submenu in mp1, so there is no positional index to keep in step with
     * the resource script.
     *--------------------------------------------------------------------*/
    case WM_INITMENU:
    {
        HWND hwndMenu = HWNDFROMMP( mp2 );
        BOOL have     = ( g_arc != NULL );

        if ( SHORT1FROMMP( mp1 ) == IDM_FILE )
            MenuEnable( hwndMenu, IDM_FILE_CLOSE, have );
        else if ( SHORT1FROMMP( mp1 ) == IDM_ARCHIVE )
        {
            MenuEnable( hwndMenu, IDM_ARCHIVE_EXTRACT, have );
            MenuEnable( hwndMenu, IDM_ARCHIVE_TEST,    have );
            MenuEnable( hwndMenu, IDM_ARCHIVE_INFO,    have );
            /* Ticked = folder names KEPT, so the tick is the inverse of the
             * backend's flatten flag.  Pushed here every time the pulldown
             * opens, which also supplies the initial state. */
            MenuCheck( hwndMenu, IDM_ARCHIVE_PATHS, (BOOL)!ArcFlattenPaths() );
        }
        return (MRESULT)FALSE;
    }

    /* Menu items, accelerators and the toolbar buttons all arrive here. */
    case WM_COMMAND:
        switch ( SHORT1FROMMP( mp1 ) )
        {
        case IDM_FILE_OPEN:
            DoOpen( hwnd );
            break;
        case IDM_FILE_CLOSE:
            CloseArchive();
            UpdateTitle();
            UpdateToolbarState();
            break;
        case IDM_ARCHIVE_EXTRACT:
            DoExtractTo( hwnd );
            break;
        case IDM_ARCHIVE_INFO:
            DoInfo( hwnd );
            break;
        case IDM_ARCHIVE_TEST:
            DoTest( hwnd );
            break;
        case IDM_ARCHIVE_PATHS:
            /* Session only; WM_INITMENU redraws the tick next time the
             * Archive pulldown opens, so nothing to update here. */
            ArcSetFlattenPaths( !ArcFlattenPaths() );
            ArcPrefSave();               /* remembered for next time */
            break;
        case IDM_FILE_EXIT:
            WinPostMsg( hwnd, WM_CLOSE, MPVOID, MPVOID );
            break;
        case IDM_HELP_ABOUT:
            WinDlgBox( HWND_DESKTOP, hwnd, AboutDlgProc, NULLHANDLE,
                       IDD_ABOUT, NULL );
            break;
        }
        return (MRESULT)FALSE;

    /* Container notifications: Enter / double-click extracts, and the
     * container relays drags that land on it. */
    case WM_CONTROL:
        if ( SHORT1FROMMP( mp1 ) == IDC_LIST )
        {
            switch ( SHORT2FROMMP( mp1 ) )
            {
            case CN_ENTER:
                DoExtractTo( hwnd );
                return (MRESULT)FALSE;
            case CN_DRAGOVER:
            {
                PCNRDRAGINFO pcdi = (PCNRDRAGINFO)mp2;
                return DragOver( pcdi ? pcdi->pDragInfo : NULL );
            }
            case CN_DROP:
            {
                PCNRDRAGINFO pcdi = (PCNRDRAGINFO)mp2;
                DragDrop( hwnd, pcdi ? pcdi->pDragInfo : NULL );
                return (MRESULT)FALSE;
            }
            }
        }
        break;

    case DM_DRAGOVER:
        return DragOver( (PDRAGINFO)mp1 );

    case DM_DROP:
        DragDrop( hwnd, (PDRAGINFO)mp1 );
        return (MRESULT)FALSE;

    case WM_XA_OPENDROP:
        if ( g_dropPath[0] )
            OpenArchiveFile( hwnd, g_dropPath );
        return (MRESULT)FALSE;

    /*----------------------------------------------------------------------
     * Worker plumbing.  Extraction, testing and the archive-header parse all
     * run on a worker thread; these two messages are the whole of the UI side.
     *--------------------------------------------------------------------*/

    /* The completion poll lives in the timer, and that is the copy that
       matters: the worker posts WMU_WORKER_DONE from a thread that never
       called WinInitialize, and a dropped post would leave the progress
       dialog up over an extraction that had already finished. */
    case WM_TIMER:
        if ( SHORT1FROMMP( mp1 ) == TID_ARCPROGRESS )
        {
            BOOL  cancelled = FALSE;
            ULONG question  = 0;

            ProgressTick();

            /* Ask before done: a worker blocked in an ask cannot finish, and
               a worker that just finished has no ask outstanding. */
            if ( PmWorkerPollAsk( &g_worker, &question ) )
                ArcOnAsk( hwnd, question );

            if ( PmWorkerPollDone( &g_worker, NULL, &cancelled ) )
                ArcOnDone( hwnd, cancelled );

            return (MRESULT)FALSE;
        }
        break;

    /* A prompt only -- it saves up to one tick, and does nothing if the timer
       reaped the job first.  mp2 is ignored: the latch carries the same flag
       and answers exactly once. */
    case WMU_WORKER_DONE: {
        BOOL cancelled = FALSE;

        if ( PmWorkerPollDone( &g_worker, NULL, &cancelled ) )
            ArcOnDone( hwnd, cancelled );
        return (MRESULT)FALSE;
    }

    /* Same prompt-only role for a worker question (the overwrite ask). */
    case WMU_WORKER_ASK: {
        ULONG question = 0;

        if ( PmWorkerPollAsk( &g_worker, &question ) )
            ArcOnAsk( hwnd, question );
        return (MRESULT)FALSE;
    }

    case WM_CLOSE:
        /* The worker holds g_arc and posts to this window, so it has to have
           stopped before CloseArchive frees the archive under it. */
        if ( ArcBusy() && !ArcShutdown() )
        {
            Say( hwnd, "An operation has not stopped yet. "
                       "Try again in a moment.", MB_OK | MB_WARNING );
            return (MRESULT)FALSE;
        }

        WinStopTimer( g_hab, g_hwndClient, TID_ARCPROGRESS );
        ProgressEnd( hwnd );

        CloseArchive();
        WinPostMsg( hwnd, WM_QUIT, MPVOID, MPVOID );
        return (MRESULT)FALSE;
    }

    return WinDefWindowProc( hwnd, msg, mp1, mp2 );
}

/*===========================================================================
 * About dialog
 *===========================================================================*/
MRESULT EXPENTRY AboutDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    if ( msg == WM_COMMAND &&
         ( SHORT1FROMMP( mp1 ) == DID_OK || SHORT1FROMMP( mp1 ) == DID_CANCEL ) )
    {
        WinDismissDlg( hwnd, DID_OK );
        return (MRESULT)FALSE;
    }
    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}

/*===========================================================================
 * CreateStdFrame - WinCreateStdWindow, with the resource-backed frame flags
 *                  isolated and the PM error reported
 *
 *   FCF_MENU, FCF_ICON and FCF_ACCELTABLE each send the frame off to load a
 *   resource by the frame's window id, and a load that fails takes the whole
 *   create down with it.  All WinCreateStdWindow then says is NULLHANDLE -
 *   which is how a program that is otherwise perfectly healthy ends up doing
 *   nothing but putting up "Window creation failed".
 *
 *   So: try the lot; if that fails, try again without each resource flag in
 *   turn, and finally without any of them.  The program runs - one icon or
 *   menu short - and szDiag (256 bytes, the caller's) names the resource PM
 *   would not load and carries the PM error code.  szDiag comes back empty
 *   when the first attempt worked, which is the normal case.
 *===========================================================================*/

#define FCF_RESOURCE_FLAGS  ( FCF_MENU | FCF_ICON | FCF_ACCELTABLE )

static HWND CreateStdFrame( HAB habApp, PCSZ pszClass, PCSZ pszTitle,
                            ULONG flWanted, ULONG idRes, HWND *phwndClient,
                            char *szDiag )
{
    static const ULONG  aflRes[3]  = { FCF_MENU, FCF_ICON, FCF_ACCELTABLE };
    static const char  *apszRes[3] = { "FCF_MENU", "FCF_ICON",
                                       "FCF_ACCELTABLE" };
    ULONG flCreate, flPresent, err;
    HWND  hwnd;
    int   i;

    szDiag[0] = '\0';

    flCreate = flWanted;
    hwnd = WinCreateStdWindow( HWND_DESKTOP, 0L, &flCreate, pszClass, pszTitle,
                               0L, NULLHANDLE, idRes, phwndClient );
    if ( hwnd != NULLHANDLE )
        return hwnd;

    /* WinGetLastError both reports and clears, so take it once, here. */
    err       = (ULONG)WinGetLastError( habApp ) & 0xFFFFL;
    flPresent = flWanted & FCF_RESOURCE_FLAGS;

    for ( i = 0; i < 3; i++ ) {
        if ( !( flPresent & aflRes[i] ) ) continue;

        flCreate = flWanted & ~aflRes[i];
        hwnd = WinCreateStdWindow( HWND_DESKTOP, 0L, &flCreate, pszClass,
                                   pszTitle, 0L, NULLHANDLE, idRes,
                                   phwndClient );
        if ( hwnd != NULLHANDLE ) {
            sprintf( szDiag,
                     "The window could only be created with %s dropped, so PM "
                     "will not load that resource (id %lu) out of the .EXE.\n\n"
                     "PM error 0x%04lX.",
                     apszRes[i], (unsigned long)idRes, (unsigned long)err );
            return hwnd;
        }
    }

    /* No single flag to blame; if more than one is in play, try with all of
       them off before giving up.                                            */
    if ( flPresent != 0 && flPresent != aflRes[0] &&
         flPresent != aflRes[1] && flPresent != aflRes[2] ) {
        flCreate = flWanted & ~FCF_RESOURCE_FLAGS;
        hwnd = WinCreateStdWindow( HWND_DESKTOP, 0L, &flCreate, pszClass,
                                   pszTitle, 0L, NULLHANDLE, idRes,
                                   phwndClient );
        if ( hwnd != NULLHANDLE ) {
            sprintf( szDiag,
                     "The window could only be created with the menu, icon and "
                     "accelerator flags all dropped: none of the .EXE's "
                     "resources for id %lu will load.\n\nPM error 0x%04lX.",
                     (unsigned long)idRes, (unsigned long)err );
            return hwnd;
        }
    }

    sprintf( szDiag, "WinCreateStdWindow failed with PM error 0x%04lX.",
             (unsigned long)err );
    return NULLHANDLE;
}

/*===========================================================================
 * Entry point
 *===========================================================================*/
int main( int argc, char *argv[] )
{
    HMQ   hmq;
    QMSG  qmsg;
    ULONG flFrameFlags;
    char  szDiag[256];

    g_hab = WinInitialize( 0 );
    if ( g_hab == NULLHANDLE )
        return 1;

    hmq = WinCreateMsgQueue( g_hab, 0 );
    if ( hmq == NULLHANDLE ) { WinTerminate( g_hab ); return 1; }

    /* Probing a not-ready drive in the folder browser, or a bad path on the
     * command line, must not raise a hard-error popup. */
    DosError( FERR_DISABLEHARDERR );

    if ( !WinRegisterClass( g_hab, (PCSZ)g_szClass, ClientWndProc,
                            CS_SIZEREDRAW, 0 ) ||
         !WinRegisterClass( g_hab, (PCSZ)g_szBarClass, BarWndProc,
                            CS_SIZEREDRAW, 0 ) )
    {
        WinDestroyMsgQueue( hmq );
        WinTerminate( g_hab );
        return 1;
    }

    flFrameFlags = FCF_TITLEBAR | FCF_SYSMENU  | FCF_SIZEBORDER
                 | FCF_MINMAX   | FCF_TASKLIST | FCF_SHELLPOSITION
                 | FCF_MENU     | FCF_ICON     | FCF_ACCELTABLE;

    g_hwndFrame = CreateStdFrame( g_hab, (PCSZ)g_szClass, (PCSZ)g_szTitle,
                                  flFrameFlags, ID_XARCHIVE,
                                  &g_hwndClient, szDiag );
    if ( g_hwndFrame == NULLHANDLE )
    {
        WinMessageBox( HWND_DESKTOP, HWND_DESKTOP, (PCSZ)szDiag,
                       (PCSZ)g_szTitle, 0, MB_OK | MB_ERROR );
        WinDestroyMsgQueue( hmq );
        WinTerminate( g_hab );
        return 1;
    }

    if ( szDiag[0] )
        WinMessageBox( HWND_DESKTOP, HWND_DESKTOP, (PCSZ)szDiag,
                       (PCSZ)g_szTitle, 0, MB_OK | MB_WARNING );

    WinShowWindow( g_hwndFrame, TRUE );
    UpdateTitle();

    /* Extraction asks before writing over an existing file (ArcWantWrite in
     * the shared backend); the hook runs on the worker and blocks until the
     * UI thread's dialog answers - see OverwriteHook / ArcOnAsk. */
    ArcSetOverwritePrompt( OverwriteHook, NULL );

    /* Preferences beside the executable, in the same XARCHIVE.INI the DOS and
     * Win32s builds use.  The Archive pulldown's tick comes from
     * WM_INITMENU, so loading here is all that is needed. */
    ArcPrefSetFileFromExe( argv[0] );
    ArcPrefLoad();

    /* An archive named on the command line (or by a WPS association). */
    if ( argc > 1 && argv[1][0] )
        OpenArchiveFile( g_hwndClient, argv[1] );

    while ( WinGetMsg( g_hab, &qmsg, NULLHANDLE, 0, 0 ) )
        WinDispatchMsg( g_hab, &qmsg );

    CloseArchive();
    WinDestroyWindow( g_hwndFrame );
    WinDestroyMsgQueue( hmq );
    WinTerminate( g_hab );
    return 0;
}
