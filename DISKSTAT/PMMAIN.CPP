/*===========================================================================
 * PMMAIN.CPP  -  DiskStat main window, toolbar, drive combo, and About box
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * A client window painting a toolbar, a pie pane and a status bar, plus a
 * child window for the scrolling folder tree.  F5 / F3 come from the frame's
 * accelerator table (RES\DISKSTAT.RC).
 *
 * This is the UI half of the scan: SCAN.CPP runs it on a worker thread and
 * makes no PM calls, so the "Scanning" dialog, the progress timer and the
 * WMU_WORKER_DONE handler all live here.
 *===========================================================================*/
#define INCL_WIN
#define INCL_GPI
#define INCL_DOSMISC
#define INCL_DOSERRORS

#include <os2.h>
#include <string.h>
#include <stdio.h>

#include "diskstat.h"
#include "pmworker.h"
#include "res/resource.h"

// --------------------------------------------------------------------------
// Global data
// --------------------------------------------------------------------------

HAB             hab               = NULLHANDLE;
HWND            hwndFrame         = NULLHANDLE;
HWND            hwndClient        = NULLHANDLE;
HWND            hDriveCombo       = NULLHANDLE;
HWND            hRightPane        = NULLHANDLE;
HWND            hTreeScroll       = NULLHANDLE;

int             current_drive     = 3;          /* 1=A, 2=B, 3=C, ...         */
unsigned long   total_disk_space  = 0L;
unsigned long   free_disk_space   = 0L;
unsigned long   used_disk_space   = 0L;

tree_node_t     tree_nodes[MAX_TREE_NODES];
int             tree_count        = 0;
int             selected_node     = -1;

BOOL            scan_in_progress  = FALSE;
BOOL            data_valid        = FALSE;
BOOL            disk_size_clamped = FALSE;

int             folder_scroll_pos = 0;

static LONG     splitter_x        = LEFT_PANE_MAX;

// --------------------------------------------------------------------------
// Scan progress UI
//
//   Progress is PULLED: a timer copies whatever directory the worker has most
//   recently recorded into the dialog.  Pushing a message per directory would
//   flood the queue on a large volume.
// --------------------------------------------------------------------------

#define TID_SCANPROGRESS    1
#define SCAN_TIMER_MS       200

static HWND     g_hScanDlg        = NULLHANDLE;

// --------------------------------------------------------------------------
// Forward declarations
// --------------------------------------------------------------------------

MRESULT EXPENTRY ClientWndProc  ( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY TreePaneWndProc( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY AboutDlgProc   ( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY ScanDlgProc    ( HWND, ULONG, MPARAM, MPARAM );

static LONG ComputeSplitterX  ( LONG client_width );
static LONG ScrollBarWidth    ( void );
static void LayoutChildren    ( HWND hwnd );
static void UpdateTreeScroll  ( void );
static int  VisibleTreeRows   ( void );
static void PaneMetrics       ( HWND hwndPane, PRECTL prcl,
                                LONG *rh, LONG *hdr );
static void BeginScanUI       ( HWND hwndCli, const char *what );
static void EndScanUI         ( HWND hwndCli );
static void FinishScan        ( HWND hwndCli, BOOL fCancelled );

// --------------------------------------------------------------------------
// ComputeSplitterX - dynamic left-pane width from the client width
//
//   The right pane (folder tree) gets priority: on a narrow window the left
//   pane shrinks to LEFT_PANE_MIN so the tree keeps RIGHT_PANE_MIN; on a wide
//   window the left pane is capped at LEFT_PANE_MAX so the extra room feeds
//   the tree columns rather than enlarging the pie.
// --------------------------------------------------------------------------

static LONG ComputeSplitterX( LONG client_width )
{
    LONG sx = client_width * 2 / 5;             /* ~40% to the left pane      */

    if ( sx > LEFT_PANE_MAX ) sx = LEFT_PANE_MAX;
    if ( sx < LEFT_PANE_MIN ) sx = LEFT_PANE_MIN;

    if ( client_width - sx < RIGHT_PANE_MIN )
        sx = client_width - RIGHT_PANE_MIN;

    if ( sx < LEFT_PANE_MIN ) sx = LEFT_PANE_MIN;
    return sx;
}

static LONG ScrollBarWidth( void )
{
    LONG w = WinQuerySysValue( HWND_DESKTOP, SV_CXVSCROLL );
    return ( w > 0 ) ? w : 16;
}

// --------------------------------------------------------------------------
// PaneMetrics - the tree pane's client rect plus the row / header heights
//
//   Painting and hit-testing both go through this, so they cannot drift
//   apart.  Keep it that way: separate copies of the row arithmetic are how
//   a click starts landing on the wrong row.
// --------------------------------------------------------------------------

static void PaneMetrics( HWND hwndPane, PRECTL prcl, LONG *rh, LONG *hdr )
{
    HPS hps;

    WinQueryWindowRect( hwndPane, prcl );

    hps = WinGetPS( hwndPane );
    *rh  = DsRowHeight( hps );
    *hdr = DsHeaderHeight( hps );
    WinReleasePS( hps );

    if ( *rh < 1 ) *rh = 12;
}

static int VisibleTreeRows( void )
{
    RECTL rcl;
    LONG  rh, hdr;
    int   vis;

    if ( hRightPane == NULLHANDLE ) return 1;

    PaneMetrics( hRightPane, &rcl, &rh, &hdr );

    vis = (int)( ( ( rcl.yTop - rcl.yBottom ) - hdr ) / rh );
    return ( vis < 1 ) ? 1 : vis;
}

// --------------------------------------------------------------------------
// UpdateTreeScroll - clamp folder_scroll_pos and resync the scrollbar
// --------------------------------------------------------------------------

static void UpdateTreeScroll( void )
{
    int vis, maxpos;

    if ( hTreeScroll == NULLHANDLE ) return;

    vis    = VisibleTreeRows();
    maxpos = ( tree_count > vis ) ? ( tree_count - vis ) : 0;

    if ( folder_scroll_pos > maxpos ) folder_scroll_pos = maxpos;
    if ( folder_scroll_pos < 0 )      folder_scroll_pos = 0;

    WinSendMsg( hTreeScroll, SBM_SETSCROLLBAR,
                MPFROMSHORT( (SHORT)folder_scroll_pos ),
                MPFROM2SHORT( 0, (SHORT)maxpos ) );

    WinSendMsg( hTreeScroll, SBM_SETTHUMBSIZE,
                MPFROM2SHORT( (SHORT)vis,
                              (SHORT)( tree_count > 0 ? tree_count : 1 ) ),
                MPVOID );
}

// --------------------------------------------------------------------------
// LayoutChildren - place the toolbar controls, the tree pane and its scrollbar
//
//   PM client coordinates: (0,0) is the BOTTOM-left corner, so the toolbar is
//   measured down from yTop and the status bar sits at yBottom.
// --------------------------------------------------------------------------

static void LayoutChildren( HWND hwnd )
{
    RECTL rcl;
    LONG  cx, cy, content_top, content_bot, sbw;
    LONG  pane_left, pane_right, pane_h;

    WinQueryWindowRect( hwnd, &rcl );
    cx = rcl.xRight - rcl.xLeft;
    cy = rcl.yTop   - rcl.yBottom;

    splitter_x  = ComputeSplitterX( cx );
    content_top = cy - TOOLBAR_HEIGHT;
    content_bot = STATUSBAR_HEIGHT;
    sbw         = ScrollBarWidth();

    pane_left  = splitter_x + 2;
    pane_right = cx - sbw;
    pane_h     = content_top - content_bot;

    if ( pane_right < pane_left ) pane_right = pane_left;
    if ( pane_h < 1 )             pane_h = 1;

    // toolbar: "Drive:" label is painted, the combo and button are windows.
    // The combobox window holds the entry field at its top and drops its list
    // downwards inside the same rectangle, so it is positioned by its TOP.
    if ( hDriveCombo != NULLHANDLE ) {
        WinSetWindowPos( hDriveCombo, HWND_TOP,
                         44, cy - 6 - 110, 60, 110,
                         SWP_MOVE | SWP_SIZE | SWP_SHOW );
    }

    if ( WinWindowFromID( hwnd, IDC_REFRESHBTN ) != NULLHANDLE ) {
        WinSetWindowPos( WinWindowFromID( hwnd, IDC_REFRESHBTN ), HWND_TOP,
                         112, cy - TOOLBAR_HEIGHT + 5, 72, 22,
                         SWP_MOVE | SWP_SIZE | SWP_SHOW );
    }

    if ( hRightPane != NULLHANDLE ) {
        WinSetWindowPos( hRightPane, HWND_TOP,
                         pane_left, content_bot,
                         pane_right - pane_left, pane_h,
                         SWP_MOVE | SWP_SIZE | SWP_SHOW );
    }

    if ( hTreeScroll != NULLHANDLE ) {
        WinSetWindowPos( hTreeScroll, HWND_TOP,
                         pane_right, content_bot, sbw, pane_h,
                         SWP_MOVE | SWP_SIZE | SWP_SHOW );
    }

    UpdateTreeScroll();
}

// --------------------------------------------------------------------------
// BeginScanUI / EndScanUI - the visible half of a scan
//
//   BeginScanUI is called straight after ScanDrive / ExpandNode.  Those are
//   asynchronous now, so it first checks whether a worker actually started --
//   if the thread could not be created the model was left alone and there is
//   nothing to put a dialog over.
//
//   The test is scan_in_progress, NOT ScanBusy().  ScanBusy() drops the
//   instant the worker returns, which on a small volume can be before this
//   line is reached; the timer would then never start, and the completion the
//   timer was supposed to notice would sit unreaped forever.
//   scan_in_progress stays raised until the UI itself takes the result.
// --------------------------------------------------------------------------

static void BeginScanUI( HWND hwndCli, const char *what )
{
    if ( !scan_in_progress ) return;

    g_hScanDlg = WinLoadDlg( HWND_DESKTOP, hwndCli, ScanDlgProc,
                             NULLHANDLE, IDD_SCANNING, NULL );

    if ( g_hScanDlg != NULLHANDLE ) {
        WinSetDlgItemText( g_hScanDlg, IDC_SCANTEXT, (PCSZ)what );
        WinSetDlgItemText( g_hScanDlg, IDC_SCANPATH, (PCSZ)"" );
        WinShowWindow( g_hScanDlg, TRUE );
    }

    WinStartTimer( hab, hwndCli, TID_SCANPROGRESS, SCAN_TIMER_MS );

    WinSetPointer( HWND_DESKTOP,
                   WinQuerySysPointer( HWND_DESKTOP, SPTR_WAIT, FALSE ) );

    // data_valid is FALSE now, so both panes repaint as "scanning"
    WinInvalidateRect( hwndCli, NULL, TRUE );
    if ( hRightPane != NULLHANDLE )
        WinInvalidateRect( hRightPane, NULL, TRUE );
}

static void EndScanUI( HWND hwndCli )
{
    WinStopTimer( hab, hwndCli, TID_SCANPROGRESS );

    if ( g_hScanDlg != NULLHANDLE ) {
        WinDestroyWindow( g_hScanDlg );
        g_hScanDlg = NULLHANDLE;
    }

    WinSetPointer( HWND_DESKTOP,
                   WinQuerySysPointer( HWND_DESKTOP, SPTR_ARROW, FALSE ) );
}

// --------------------------------------------------------------------------
// FinishScan - take a completed job and put the window back in order
//
//   Reached from two places -- WMU_WORKER_DONE and the progress timer -- but
//   only ever after ScanPollDone has answered TRUE, and that answers TRUE
//   once per job.  The latch, not the caller, is what makes this run exactly
//   once, so neither path needs to know about the other.
// --------------------------------------------------------------------------

static void FinishScan( HWND hwndCli, BOOL fCancelled )
{
    char title[64];

    // Hands tree_nodes[] back to the UI thread and re-raises data_valid;
    // nothing below may read the model before this call.
    ScanJobFinished( fCancelled );
    EndScanUI( hwndCli );

    sprintf( title, "Disk Statistcs - Drive %c:", 'A' + current_drive - 1 );
    WinSetWindowText( hwndFrame, (PCSZ)title );

    UpdateTreeScroll();
    WinInvalidateRect( hwndCli, NULL, TRUE );
    if ( hRightPane != NULLHANDLE )
        WinInvalidateRect( hRightPane, NULL, TRUE );
}

// ==========================================================================
// CreateStdFrame - WinCreateStdWindow, with the resource-backed frame flags
//                  isolated and the PM error reported
//
//   FCF_MENU, FCF_ICON and FCF_ACCELTABLE each send the frame off to load a
//   resource by the frame's window id, and a load that fails takes the whole
//   create down with it.  All WinCreateStdWindow then says is NULLHANDLE -
//   which is how a program that is otherwise perfectly healthy ends up doing
//   nothing but putting up "Window creation failed".
//
//   So: try the lot; if that fails, try again without each resource flag in
//   turn, and finally without any of them.  The program runs - one icon or
//   menu short - and szDiag (256 bytes, the caller's) names the resource PM
//   would not load and carries the PM error code.  szDiag comes back empty
//   when the first attempt worked, which is the normal case.
// ==========================================================================

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

    // WinGetLastError both reports and clears, so take it once, here.
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

    // No single flag to blame; if more than one is in play, try with all of
    // them off before giving up.
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

// ==========================================================================
// main
// ==========================================================================

int main( int argc, char *argv[] )
{
    HMQ   hmq;
    QMSG  qmsg;
    ULONG flFrameFlags;
    LONG  scr_w, scr_h, w, h, x, y;
    char  szDiag[256];

    (void)argc; (void)argv;

    hab = WinInitialize( 0 );
    if ( hab == NULLHANDLE ) return 1;

    hmq = WinCreateMsgQueue( hab, 0 );
    if ( hmq == NULLHANDLE ) {
        WinTerminate( hab );
        return 1;
    }

    // Probing drives must not raise "Drive not ready" hard-error popups.
    DosError( FERR_DISABLEHARDERR );

    if ( !WinRegisterClass( hab, (PCSZ)APP_NAME, ClientWndProc,
                            CS_SIZEREDRAW, 0 )
      || !WinRegisterClass( hab, (PCSZ)RIGHTPANE_CLASS, TreePaneWndProc,
                            CS_SIZEREDRAW, 0 ) ) {
        WinDestroyMsgQueue( hmq );
        WinTerminate( hab );
        return 1;
    }

    flFrameFlags = FCF_TITLEBAR  | FCF_SYSMENU | FCF_SIZEBORDER
                 | FCF_MINMAX    | FCF_TASKLIST
                 | FCF_MENU      | FCF_ICON    | FCF_ACCELTABLE;

    hwndFrame = CreateStdFrame( hab, (PCSZ)APP_NAME,
                                (PCSZ)"Disk Statistics",
                                flFrameFlags, ID_DISKSTAT,
                                &hwndClient, szDiag );

    if ( hwndFrame == NULLHANDLE ) {
        WinMessageBox( HWND_DESKTOP, HWND_DESKTOP, (PCSZ)szDiag,
                       (PCSZ)APP_NAME, 0, MB_OK | MB_ERROR );
        WinDestroyMsgQueue( hmq );
        WinTerminate( hab );
        return 1;
    }

    if ( szDiag[0] )
        WinMessageBox( HWND_DESKTOP, HWND_DESKTOP, (PCSZ)szDiag,
                       (PCSZ)APP_NAME, 0, MB_OK | MB_WARNING );

    // three quarters of the desktop, centred
    scr_w = WinQuerySysValue( HWND_DESKTOP, SV_CXSCREEN );
    scr_h = WinQuerySysValue( HWND_DESKTOP, SV_CYSCREEN );
    w = scr_w * 3 / 4;
    h = scr_h * 3 / 4;
    x = ( scr_w - w ) / 2;
    y = ( scr_h - h ) / 2;

    WinSetWindowPos( hwndFrame, HWND_TOP, x, y, w, h,
                     SWP_MOVE | SWP_SIZE | SWP_SHOW | SWP_ACTIVATE );

    while ( WinGetMsg( hab, &qmsg, NULLHANDLE, 0, 0 ) )
        WinDispatchMsg( hab, &qmsg );

    WinDestroyWindow( hwndFrame );
    WinDestroyMsgQueue( hmq );
    WinTerminate( hab );
    return 0;
}

// ==========================================================================
// ClientWndProc
// ==========================================================================

MRESULT EXPENTRY ClientWndProc( HWND hwnd, ULONG msg,
                                MPARAM mp1, MPARAM mp2 )
{
    switch ( msg ) {

    case WM_CREATE: {
        hDriveCombo = WinCreateWindow( hwnd, WC_COMBOBOX, (PCSZ)"",
                          WS_VISIBLE | CBS_DROPDOWNLIST,
                          0, 0, 0, 0,
                          hwnd, HWND_TOP, IDC_DRVCOMBO, NULL, NULL );

        WinCreateWindow( hwnd, WC_BUTTON, (PCSZ)"~Refresh",
                          WS_VISIBLE | BS_PUSHBUTTON,
                          0, 0, 0, 0,
                          hwnd, HWND_TOP, IDC_REFRESHBTN, NULL, NULL );

        hRightPane = WinCreateWindow( hwnd, (PCSZ)RIGHTPANE_CLASS, (PCSZ)"",
                          WS_VISIBLE,
                          0, 0, 0, 0,
                          hwnd, HWND_TOP, IDC_TREEPANE, NULL, NULL );

        hTreeScroll = WinCreateWindow( hwnd, WC_SCROLLBAR, (PCSZ)"",
                          WS_VISIBLE | SBS_VERT,
                          0, 0, 0, 0,
                          hwnd, HWND_TOP, IDC_TREESCROLL, NULL, NULL );

        LayoutChildren( hwnd );
        PopulateDriveCombo( hwnd );

        // first scan happens once the frame is up and the loop is pumping
        WinPostMsg( hwnd, WM_COMMAND,
                    MPFROMSHORT( IDM_FILE_REFRESH ),
                    MPFROM2SHORT( CMDSRC_OTHER, FALSE ) );
        return (MRESULT)FALSE;
    }

    case WM_SIZE:
        LayoutChildren( hwnd );
        WinInvalidateRect( hwnd, NULL, TRUE );
        return (MRESULT)FALSE;

    // the whole client is painted below; skipping the erase kills the flash
    case WM_ERASEBACKGROUND:
        return (MRESULT)FALSE;

    case WM_PAINT: {
        HPS   hps;
        RECTL rclPaint, rcl, left_rc, status_rc, toolbar_rc;
        LONG  cx, cy, content_top, content_bot;

        hps = WinBeginPaint( hwnd, NULLHANDLE, &rclPaint );
        DsSetRgbMode( hps );

        WinQueryWindowRect( hwnd, &rcl );
        cx = rcl.xRight;
        cy = rcl.yTop;

        content_top = cy - TOOLBAR_HEIGHT;
        content_bot = STATUSBAR_HEIGHT;

        // ---- toolbar ----
        toolbar_rc.xLeft   = 0;
        toolbar_rc.yBottom = content_top;
        toolbar_rc.xRight  = cx;
        toolbar_rc.yTop    = cy;
        WinFillRect( hps, &toolbar_rc, DS_FACE );

        {
            FONTMETRICS fm;
            POINTL      ptl;

            memset( &fm, 0, sizeof( fm ) );
            GpiQueryFontMetrics( hps, sizeof( fm ), &fm );

            GpiSetColor( hps, DS_BLACK );
            ptl.x = 6;
            ptl.y = content_top + ( TOOLBAR_HEIGHT - fm.lMaxBaselineExt ) / 2
                                + fm.lMaxDescender;
            GpiCharStringAt( hps, &ptl, 6, (PCH)"Drive:" );
        }

        // toolbar separator
        GpiSetColor( hps, DS_SHADOW );
        {
            POINTL ptl;
            ptl.x = 0;  ptl.y = content_top;
            GpiMove( hps, &ptl );
            ptl.x = cx; ptl.y = content_top;
            GpiLine( hps, &ptl );
        }

        // ---- left pane: white backdrop, pie, legend ----
        left_rc.xLeft   = 0;
        left_rc.yBottom = content_bot;
        left_rc.xRight  = splitter_x;
        left_rc.yTop    = content_top;
        WinFillRect( hps, &left_rc, DS_WHITE );

        // vertical divider between the panes
        {
            RECTL div_rc;
            div_rc.xLeft   = splitter_x;
            div_rc.yBottom = content_bot;
            div_rc.xRight  = splitter_x + 2;
            div_rc.yTop    = content_top;
            WinFillRect( hps, &div_rc, DS_SHADOW );
        }

        DrawPieChart ( hps, &left_rc );
        DrawPieLegend( hps, &left_rc );

        // ---- status bar ----
        status_rc.xLeft   = 0;
        status_rc.yBottom = 0;
        status_rc.xRight  = cx;
        status_rc.yTop    = content_bot;
        DrawStatusBar( hps, &status_rc );

        WinEndPaint( hps );
        return (MRESULT)FALSE;
    }

    // ---- drive combobox ----
    case WM_CONTROL: {
        if ( SHORT1FROMMP( mp1 ) == IDC_DRVCOMBO
             && SHORT2FROMMP( mp1 ) == CBN_LBSELECT ) {
            LONG sel;

            // A scan already owns the model; changing drives mid-scan would
            // have the worker sizing one volume into a tree labelled with
            // another.  The combo is left showing the new letter, which the
            // WMU_WORKER_DONE handler resyncs.
            if ( ScanBusy() ) return (MRESULT)FALSE;

            sel = (LONG)WinSendMsg( hDriveCombo, LM_QUERYSELECTION,
                                    MPFROMSHORT( LIT_FIRST ), MPVOID );
            if ( sel != LIT_NONE ) {
                LONG drv = (LONG)WinSendMsg( hDriveCombo, LM_QUERYITEMHANDLE,
                                             MPFROMSHORT( (SHORT)sel ),
                                             MPVOID );
                if ( drv >= 1 && drv <= 26 ) {
                    char what[64];

                    current_drive = (int)drv;
                    ScanDrive( hwnd );

                    sprintf( what, "Scanning drive %c: ...",
                             'A' + current_drive - 1 );
                    BeginScanUI( hwnd, what );
                    UpdateTreeScroll();
                }
            }
            return (MRESULT)FALSE;
        }
        break;
    }

    // ---- menu items, accelerators and the Refresh push button ----
    case WM_COMMAND: {
        switch ( SHORT1FROMMP( mp1 ) ) {

        case IDC_REFRESHBTN:
        case IDM_FILE_REFRESH: {
            char what[64];

            if ( ScanBusy() ) return (MRESULT)FALSE;

            ScanDrive( hwnd );

            sprintf( what, "Scanning drive %c: ...", 'A' + current_drive - 1 );
            BeginScanUI( hwnd, what );
            UpdateTreeScroll();
            return (MRESULT)FALSE;
        }

        case IDM_FILE_EXIT:
            WinPostMsg( hwnd, WM_CLOSE, MPVOID, MPVOID );
            return (MRESULT)FALSE;

        case IDM_HELP_ABOUT:
            WinDlgBox( HWND_DESKTOP, hwnd, AboutDlgProc,
                       NULLHANDLE, IDD_ABOUT, NULL );
            return (MRESULT)FALSE;
        }
        break;
    }

    // ---- progress tick: show where the worker is, and notice it stopping ---
    //
    //   The completion check lives here as well as in WMU_WORKER_DONE, and
    //   this is the copy that is actually load-bearing.  The worker posts that
    //   message from a thread that never called WinInitialize; if PM declines
    //   to deliver it, this tick is the only thing that ever finds out the
    //   scan ended, and without it the dialog stays up over a finished scan.
    case WM_TIMER:
        if ( SHORT1FROMMP( mp1 ) == TID_SCANPROGRESS ) {
            BOOL fCancelled = FALSE;

            if ( g_hScanDlg != NULLHANDLE )
                WinSetDlgItemText( g_hScanDlg, IDC_SCANPATH,
                                   (PCSZ)ScanProgressPath() );

            if ( ScanPollDone( &fCancelled ) )
                FinishScan( hwnd, fCancelled );

            return (MRESULT)FALSE;
        }
        break;

    // ---- the worker finished (or gave up after a Cancel) ----
    //
    //   Only a prompt: it saves up to one timer period, and does nothing at
    //   all if the tick got there first.
    case WMU_WORKER_DONE: {
        BOOL fCancelled = FALSE;

        if ( ScanPollDone( &fCancelled ) )
            FinishScan( hwnd, fCancelled );
        return (MRESULT)FALSE;
    }

    // ---- shutdown: the worker must be stopped before its window goes ----
    case WM_CLOSE:
        if ( ScanBusy() ) {
            if ( !ScanShutdown() ) {
                // Still running after five seconds -- almost certainly stuck
                // in a Dos* call on a dead redirector.  Refusing to close is
                // the honest outcome: tearing the frame down now would leave
                // a thread writing into a half-destroyed process.
                WinMessageBox( HWND_DESKTOP, hwnd,
                    (PCSZ)"The drive scan has not stopped yet -- it is "
                          "probably waiting on a drive that is not "
                          "responding.\n\nTry again in a moment.",
                    (PCSZ)APP_NAME, 0, MB_OK | MB_WARNING );
                return (MRESULT)FALSE;
            }
            ScanPollDone( NULL );       /* drain the latch; we are leaving  */
            ScanJobFinished( TRUE );
            EndScanUI( hwnd );
        }
        break;

    // ---- the tree pane's scrollbar ----
    case WM_VSCROLL: {
        int pos, page, maxpos, vis;

        if ( SHORT1FROMMP( mp1 ) != IDC_TREESCROLL ) break;

        vis    = VisibleTreeRows();
        page   = vis;
        maxpos = ( tree_count > vis ) ? ( tree_count - vis ) : 0;
        pos    = folder_scroll_pos;

        switch ( SHORT2FROMMP( mp2 ) ) {
        case SB_LINEUP:          pos--;            break;
        case SB_LINEDOWN:        pos++;            break;
        case SB_PAGEUP:          pos -= page;      break;
        case SB_PAGEDOWN:        pos += page;      break;
        case SB_SLIDERTRACK:
        case SB_SLIDERPOSITION:  pos = (SHORT)SHORT1FROMMP( mp2 ); break;
        default:                                   break;
        }

        if ( pos < 0 )      pos = 0;
        if ( pos > maxpos ) pos = maxpos;

        if ( pos != folder_scroll_pos ) {
            folder_scroll_pos = pos;
            WinSendMsg( hTreeScroll, SBM_SETPOS,
                        MPFROMSHORT( (SHORT)pos ), MPVOID );
            WinInvalidateRect( hRightPane, NULL, TRUE );
        }
        return (MRESULT)FALSE;
    }

    case WM_MOUSEMOVE:
        if ( scan_in_progress ) {
            WinSetPointer( HWND_DESKTOP,
                WinQuerySysPointer( HWND_DESKTOP, SPTR_WAIT, FALSE ) );
            return (MRESULT)TRUE;
        }
        break;
    }

    return WinDefWindowProc( hwnd, msg, mp1, mp2 );
}

// ==========================================================================
// TreePaneWndProc - the scrolling folder tree
// ==========================================================================

MRESULT EXPENTRY TreePaneWndProc( HWND hwnd, ULONG msg,
                                  MPARAM mp1, MPARAM mp2 )
{
    switch ( msg ) {

    // WM_PAINT whitens the whole pane before drawing rows, so letting PM erase
    // first would only add a flash
    case WM_ERASEBACKGROUND:
        return (MRESULT)FALSE;

    case WM_PAINT: {
        HPS   hps;
        RECTL rclPaint, rcl;

        hps = WinBeginPaint( hwnd, NULLHANDLE, &rclPaint );
        DsSetRgbMode( hps );

        WinQueryWindowRect( hwnd, &rcl );
        WinFillRect( hps, &rcl, DS_WHITE );

        DrawTreeView( hps, &rcl );

        WinEndPaint( hps );
        return (MRESULT)FALSE;
    }

    case WM_SIZE:
        UpdateTreeScroll();
        return (MRESULT)FALSE;

    case WM_BUTTON1DOWN:
    case WM_BUTTON1DBLCLK: {
        RECTL rcl;
        LONG  rh, hdr, mx, my, off, indent;
        int   row, node_idx;

        // The worker owns tree_nodes[] while a scan runs, so hit-testing it
        // would be reading an array that is being rewritten underneath.
        if ( ScanBusy() ) return (MRESULT)TRUE;

        mx = (LONG)(SHORT)SHORT1FROMMP( mp1 );
        my = (LONG)(SHORT)SHORT2FROMMP( mp1 );

        PaneMetrics( hwnd, &rcl, &rh, &hdr );

        // rows start immediately below the header and march downwards
        off = ( rcl.yTop - hdr ) - my;
        if ( off <= 0 ) break;                  /* clicked the header         */

        row = (int)( ( off - 1 ) / rh );
        node_idx = folder_scroll_pos + row;
        if ( node_idx < 0 || node_idx >= tree_count ) break;

        indent = DsIndentX( &rcl, (int)tree_nodes[node_idx].depth );

        if ( ( tree_nodes[node_idx].flags & NODE_HAS_CHILDREN )
             && mx >= indent && mx < indent + EXPAND_BOX_SIZE ) {
            if ( tree_nodes[node_idx].flags & NODE_EXPANDED ) {
                CollapseNode( node_idx );
                UpdateTreeScroll();
            }
            else {
                // Asynchronous: this starts a worker and returns.  The dialog
                // and the timer belong to the client window, which is also
                // what the worker posts WMU_WORKER_DONE to.
                ExpandNode( node_idx, hwnd );
                BeginScanUI( hwndClient, "Scanning folder ..." );
                return (MRESULT)TRUE;
            }
        }
        else {
            selected_node = ( selected_node == node_idx ) ? -1 : node_idx;
        }

        WinInvalidateRect( hwnd, NULL, TRUE );
        // the pie and the legend follow the selection
        WinInvalidateRect( hwndClient, NULL, TRUE );
        return (MRESULT)TRUE;
    }

    case WM_MOUSEMOVE:
        if ( scan_in_progress ) {
            WinSetPointer( HWND_DESKTOP,
                WinQuerySysPointer( HWND_DESKTOP, SPTR_WAIT, FALSE ) );
            return (MRESULT)TRUE;
        }
        break;
    }

    return WinDefWindowProc( hwnd, msg, mp1, mp2 );
}

// ==========================================================================
// ScanDlgProc - the modeless "Scanning" notice
//
//   Cancel only raises the worker's cancel flag; it does not dismiss the
//   dialog.  The worker decides when it has actually stopped, and the
//   WMU_WORKER_DONE handler is the single place the dialog is destroyed --
//   tearing it down from here would leave a worker posting to a dead window.
//
//   The button is disabled on the way out so a second click cannot queue a
//   redundant cancel, and the caption says what is happening.
// ==========================================================================

MRESULT EXPENTRY ScanDlgProc( HWND hwndDlg, ULONG msg,
                              MPARAM mp1, MPARAM mp2 )
{
    switch ( msg ) {

    case WM_COMMAND:
        if ( SHORT1FROMMP( mp1 ) == DID_CANCEL ) {
            ScanCancel();

            WinEnableWindow( WinWindowFromID( hwndDlg, DID_CANCEL ), FALSE );
            WinSetDlgItemText( hwndDlg, IDC_SCANTEXT,
                               (PCSZ)"Stopping ..." );
            return (MRESULT)FALSE;
        }
        break;

    // A modeless dialog would otherwise dismiss itself here, out from under
    // the worker that is still running.
    case WM_CLOSE:
        ScanCancel();
        return (MRESULT)FALSE;
    }

    return WinDefDlgProc( hwndDlg, msg, mp1, mp2 );
}

// ==========================================================================
// AboutDlgProc
// ==========================================================================

MRESULT EXPENTRY AboutDlgProc( HWND hwndDlg, ULONG msg,
                               MPARAM mp1, MPARAM mp2 )
{
    switch ( msg ) {

    case WM_COMMAND:
        if ( SHORT1FROMMP( mp1 ) == DID_OK
          || SHORT1FROMMP( mp1 ) == DID_CANCEL ) {
            WinDismissDlg( hwndDlg, TRUE );
            return (MRESULT)FALSE;
        }
        break;
    }

    return WinDefDlgProc( hwndDlg, msg, mp1, mp2 );
}
