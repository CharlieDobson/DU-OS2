/*===========================================================================
 * PMMAIN.CPP  -  Reveal for OS/2: main window, menu, About box
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * The OS/2 port of the Dobson Utilities REVEAL system-information viewer.
 * The window is a category list on the left and a detail pane on the right,
 * with a summary of the machine along the bottom - the same shape as the
 * Windows builds.  F5, F9 and F3 come from the frame's accelerator table
 * (RES\REVEAL.RC).
 *
 * The one thing that runs long - the benchmark - runs on a worker thread
 * (PMDATA.CPP), and this file is its UI half: the confirmation, the timer
 * that notices it finishing, and the wait for it on the way out.
 *===========================================================================*/
#define INCL_WIN
#define INCL_GPI
#define INCL_DOSERRORS

#include <os2.h>
#include <stdio.h>
#include <string.h>

#include "reveal.h"
#include "pmworker.h"
#include "res/resource.h"

/*---------------------------------------------------------------------------
 * Globals
 *-------------------------------------------------------------------------*/

HAB     hab        = NULLHANDLE;
int     g_category = CAT_SYSTEM;

static HWND hwndFrame  = NULLHANDLE;
static HWND hwndClient = NULLHANDLE;
static HWND hCatList   = NULLHANDLE;
static HWND hPane      = NULLHANDLE;

/* LM_SELECTITEM notifies, unlike Windows' LB_SETCURSEL: selecting the list's
   item from code comes back as an LN_SELECT exactly as if the user had
   clicked it.  This is raised around every such call so the handler can
   tell the two apart. */
static BOOL g_list_syncing = FALSE;

#define TID_BENCHMARK       1
#define BENCH_TIMER_MS      250

#define WMU_START           ( WM_USER + 1 )     /* first page, after creation */
#define WMU_FOCUSLIST       ( WM_USER + 2 )     /* focus to the list, later   */

MRESULT EXPENTRY ClientWndProc( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY AboutDlgProc ( HWND, ULONG, MPARAM, MPARAM );

/*---------------------------------------------------------------------------
 * StartingCategory - "REVEAL MEMORY" opens on the Memory page
 *
 * A page name on the command line, matched on as many letters as were
 * given, so REVEAL FIRST and REVEAL BENCH both work.  Anything unrecognised
 * simply opens the System page rather than complaining: a system-information
 * viewer refusing to start is never the more useful answer.
 *-------------------------------------------------------------------------*/

static int StartingCategory( const char* command_line )
{
    char    wanted[ 32 ];
    int     length = 0;
    int     i;

    if ( command_line == NULL ) return CAT_SYSTEM;

    while ( *command_line == ' ' || *command_line == '\t' ) ++command_line;
    while ( *command_line == '/' || *command_line == '-' ) ++command_line;

    while ( *command_line != '\0' && *command_line != ' ' &&
            length < (int)sizeof( wanted ) - 1 ) {
        wanted[ length++ ] = *command_line++;
    }
    wanted[ length ] = '\0';
    if ( length == 0 ) return CAT_SYSTEM;

    for ( i = 0; i < CAT_COUNT; ++i ) {
        const char* name = RvCategoryName( i );
        int         n;

        for ( n = 0; n < length; ++n ) {
            char a = wanted[ n ];
            char b = name[ n ];

            if ( a >= 'a' && a <= 'z' ) a = (char)( a - 'a' + 'A' );
            if ( b >= 'a' && b <= 'z' ) b = (char)( b - 'a' + 'A' );
            if ( a != b ) break;
        }
        if ( n == length ) return i;
    }

    return CAT_SYSTEM;
}

/* ==========================================================================
 * CreateStdFrame - WinCreateStdWindow, with the resource-backed frame flags
 *                  isolated and the PM error reported
 *
 *   FCF_MENU, FCF_ICON and FCF_ACCELTABLE each send the frame off to load a
 *   resource by the frame's window id, and a load that fails takes the whole
 *   create down with it - all WinCreateStdWindow then says is NULLHANDLE.
 *   So: try the lot; if that fails, try again without each resource flag in
 *   turn, and finally without any of them.  The program runs - one icon or
 *   menu short - and szDiag (256 bytes, the caller's) names the resource PM
 *   would not load.  szDiag comes back empty when the first attempt worked.
 *   (The same helper as the rest of the OS/2 suite.)
 * ==========================================================================*/

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

/* ==========================================================================
 * main
 * ==========================================================================*/

int main( int argc, char *argv[] )
{
    HMQ   hmq;
    QMSG  qmsg;
    ULONG flFrameFlags;
    LONG  scr_w, scr_h, w, h, x, y;
    char  szDiag[256];

    hab = WinInitialize( 0 );
    if ( hab == NULLHANDLE ) return 1;

    hmq = WinCreateMsgQueue( hab, 0 );
    if ( hmq == NULLHANDLE ) {
        WinTerminate( hab );
        return 1;
    }

    g_category = StartingCategory( ( argc > 1 ) ? argv[1] : NULL );

    // Identifying the processor and timing it takes a moment.
    WinSetPointer( HWND_DESKTOP,
                   WinQuerySysPointer( HWND_DESKTOP, SPTR_WAIT, FALSE ) );
    RvDataInit();
    WinSetPointer( HWND_DESKTOP,
                   WinQuerySysPointer( HWND_DESKTOP, SPTR_ARROW, FALSE ) );

    if ( !WinRegisterClass( hab, (PCSZ)MAIN_CLASS, ClientWndProc,
                            CS_SIZEREDRAW | CS_CLIPCHILDREN, 0 )
      || !PaneRegisterClass( hab ) ) {
        WinMessageBox( HWND_DESKTOP, HWND_DESKTOP,
                       (PCSZ)"Window registration failed!",
                       (PCSZ)APP_NAME, 0, MB_OK | MB_ERROR );
        RvDataTerm();
        WinDestroyMsgQueue( hmq );
        WinTerminate( hab );
        return 1;
    }

    flFrameFlags = FCF_TITLEBAR  | FCF_SYSMENU | FCF_SIZEBORDER
                 | FCF_MINMAX    | FCF_TASKLIST
                 | FCF_MENU      | FCF_ICON    | FCF_ACCELTABLE;

    hwndFrame = CreateStdFrame( hab, (PCSZ)MAIN_CLASS, (PCSZ)APP_TITLE,
                                flFrameFlags, ID_REVEAL,
                                &hwndClient, szDiag );

    if ( hwndFrame == NULLHANDLE ) {
        WinMessageBox( HWND_DESKTOP, HWND_DESKTOP, (PCSZ)szDiag,
                       (PCSZ)APP_NAME, 0, MB_OK | MB_ERROR );
        RvDataTerm();
        WinDestroyMsgQueue( hmq );
        WinTerminate( hab );
        return 1;
    }

    if ( szDiag[0] )
        WinMessageBox( HWND_DESKTOP, HWND_DESKTOP, (PCSZ)szDiag,
                       (PCSZ)APP_NAME, 0, MB_OK | MB_WARNING );

    // four fifths of the desktop, centred, and no bigger than the Windows
    // builds open at
    scr_w = WinQuerySysValue( HWND_DESKTOP, SV_CXSCREEN );
    scr_h = WinQuerySysValue( HWND_DESKTOP, SV_CYSCREEN );
    w = scr_w * 4 / 5;
    h = scr_h * 4 / 5;
    if ( w > 640 ) w = 640;
    if ( h > 460 ) h = 460;
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

/* ==========================================================================
 * Layout
 *
 *   PM client coordinates: (0,0) is the BOTTOM-left corner, so the status
 *   bar is the strip at y = 0 and the list and the pane stand on top of it.
 * ==========================================================================*/

static void LayoutChildren( HWND hwnd )
{
    RECTL rcl;
    LONG  list_width;
    LONG  content_h;

    WinQueryWindowRect( hwnd, &rcl );

    list_width = LIST_PANE_WIDTH;
    if ( list_width > rcl.xRight / 3 ) list_width = rcl.xRight / 3;
    if ( list_width < LIST_PANE_MIN )  list_width = LIST_PANE_MIN;

    content_h = rcl.yTop - STATUSBAR_HEIGHT;
    if ( content_h < 40 ) content_h = 40;

    if ( hCatList != NULLHANDLE ) {
        WinSetWindowPos( hCatList, HWND_TOP, 0, STATUSBAR_HEIGHT,
                         list_width, content_h, SWP_MOVE | SWP_SIZE | SWP_SHOW );
    }
    if ( hPane != NULLHANDLE ) {
        WinSetWindowPos( hPane, HWND_TOP, list_width, STATUSBAR_HEIGHT,
                         rcl.xRight - list_width, content_h,
                         SWP_MOVE | SWP_SIZE | SWP_SHOW );
    }
}

/* --------------------------------------------------------------------------
 * PaintStatusBar - the one line that is true no matter which page is showing
 * --------------------------------------------------------------------------*/

static void PaintStatusBar( HWND hwnd, HPS hps )
{
    RECTL   rcl, bar;
    POINTL  ptl;
    FATTRS  fat;
    BOOL    font = FALSE;
    char    text[ 200 ];
    LONG    shadow = WinQuerySysColor( HWND_DESKTOP, SYSCLR_BUTTONDARK, 0L );
    LONG    light  = WinQuerySysColor( HWND_DESKTOP, SYSCLR_BUTTONLIGHT, 0L );
    FONTMETRICS fm;

    GpiCreateLogColorTable( hps, 0, LCOLF_RGB, 0, 0, NULL );
    GpiSetBackMix( hps, BM_LEAVEALONE );

    WinQueryWindowRect( hwnd, &rcl );

    bar = rcl;
    bar.yTop = STATUSBAR_HEIGHT;
    WinFillRect( hps, &bar,
                 WinQuerySysColor( HWND_DESKTOP, SYSCLR_DIALOGBACKGROUND, 0L ) );

    // an etched rule across the top of the bar, as the Windows builds draw it
    GpiSetColor( hps, shadow );
    ptl.x = bar.xLeft + 2;   ptl.y = STATUSBAR_HEIGHT - 3;  GpiMove( hps, &ptl );
    ptl.x = bar.xRight - 3;                                 GpiLine( hps, &ptl );
    GpiSetColor( hps, light );
    ptl.x = bar.xLeft + 2;   ptl.y = STATUSBAR_HEIGHT - 4;  GpiMove( hps, &ptl );
    ptl.x = bar.xRight - 3;                                 GpiLine( hps, &ptl );

    text[ 0 ] = '\0';
    RvStatusText( text, sizeof( text ) );

    // the pane's body font, so the bar and the page read as one
    if ( PaneTextFont( &fat ) &&
         GpiCreateLogFont( hps, NULL, 1L, &fat ) != GPI_ERROR ) {
        GpiSetCharSet( hps, 1L );
        font = TRUE;
    }

    memset( &fm, 0, sizeof( fm ) );
    GpiQueryFontMetrics( hps, (LONG)sizeof( fm ), &fm );

    GpiSetColor( hps, WinQuerySysColor( HWND_DESKTOP, SYSCLR_WINDOWTEXT, 0L ) );
    ptl.x = 8;
    ptl.y = ( STATUSBAR_HEIGHT - 4 - fm.lMaxBaselineExt ) / 2 + fm.lMaxDescender;
    GpiCharStringAt( hps, &ptl, (LONG)strlen( text ), (PCH)text );

    if ( font ) {
        GpiSetCharSet( hps, LCID_DEFAULT );
        GpiDeleteSetId( hps, 1L );
    }
}

/* --------------------------------------------------------------------------
 * Menu state
 * --------------------------------------------------------------------------*/

static HWND FrameMenu( void )
{
    return WinWindowFromID( WinQueryWindow( hwndClient, QW_PARENT ), FID_MENU );
}

static void SetMenuAttr( USHORT id, USHORT attr, BOOL on )
{
    HWND hMenu = FrameMenu();

    if ( hMenu == NULLHANDLE ) return;
    WinSendMsg( hMenu, MM_SETITEMATTR, MPFROM2SHORT( id, TRUE ),
                MPFROM2SHORT( attr, on ? attr : 0 ) );
}

/* --------------------------------------------------------------------------
 * SelectCategory - list box, menu tick and detail pane kept in step
 * --------------------------------------------------------------------------*/

static void SelectCategory( int category )
{
    int i;

    if ( category < 0 || category >= CAT_COUNT ) return;

    g_category = category;

    if ( hCatList != NULLHANDLE ) {
        g_list_syncing = TRUE;
        WinSendMsg( hCatList, LM_SELECTITEM, MPFROMLONG( category ),
                    MPFROMSHORT( TRUE ) );
        g_list_syncing = FALSE;
    }

    for ( i = 0; i < CAT_COUNT; ++i ) {
        SetMenuAttr( (USHORT)( IDM_VIEW_FIRST + i ), MIA_CHECKED, i == category );
    }

    if ( hPane != NULLHANDLE ) PaneSetCategory( hPane, category );
}

/* --------------------------------------------------------------------------
 * The benchmark's UI half
 *
 *   Completion is noticed by whichever gets there first of WMU_WORKER_DONE
 *   and the timer; RvBenchmarkPoll answers TRUE once, so BenchmarkFinished
 *   runs once.  The timer is the load-bearing path - see PMWORKER.H on why
 *   a post from a worker thread is not relied on.
 * --------------------------------------------------------------------------*/

static void StartBenchmark( HWND hwnd )
{
    if ( !RvBenchmarkStart( hwnd ) ) {
        WinMessageBox( HWND_DESKTOP, hwnd,
                       (PCSZ)"The benchmark could not be started.",
                       (PCSZ)APP_NAME, 0, MB_OK | MB_WARNING );
        return;
    }

    WinStartTimer( hab, hwnd, TID_BENCHMARK, BENCH_TIMER_MS );
    SetMenuAttr( IDM_VIEW_BENCHMARK, MIA_DISABLED, TRUE );

    // the page now says it is running, and loses its button
    SelectCategory( CAT_BENCHMARK );
}

static void BenchmarkFinished( HWND hwnd )
{
    WinStopTimer( hab, hwnd, TID_BENCHMARK );
    SetMenuAttr( IDM_VIEW_BENCHMARK, MIA_DISABLED, FALSE );

    // The user may have gone to another page while it ran; the results are
    // there when they come back, and they are not dragged back to see them.
    if ( g_category == CAT_BENCHMARK && hPane != NULLHANDLE )
        PaneRefresh( hPane );
}

/* ==========================================================================
 * ClientWndProc
 * ==========================================================================*/

MRESULT EXPENTRY ClientWndProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg ) {

    case WM_CREATE: {
        int i;

        hwndClient = hwnd;

        hCatList = WinCreateWindow( hwnd, WC_LISTBOX, (PCSZ)"",
                                    WS_VISIBLE | LS_NOADJUSTPOS,
                                    0, 0, 0, 0,
                                    hwnd, HWND_TOP, IDC_CATLIST, NULL, NULL );
        if ( hCatList != NULLHANDLE ) {
            WinSetPresParam( hCatList, PP_FONTNAMESIZE,
                             (ULONG)strlen( UI_FONT ) + 1, (PVOID)UI_FONT );
            for ( i = 0; i < CAT_COUNT; ++i ) {
                WinSendMsg( hCatList, LM_INSERTITEM, MPFROMLONG( LIT_END ),
                            MPFROMP( RvCategoryName( i ) ) );
            }
        }

        hPane = WinCreateWindow( hwnd, (PCSZ)PANE_CLASS, (PCSZ)"",
                                 WS_VISIBLE,
                                 0, 0, 0, 0,
                                 hwnd, HWND_TOP, IDC_PANE, NULL, NULL );

        // WinCreateStdWindow sizes the client itself, so a WM_SIZE may never
        // come: lay out from what the window is now
        LayoutChildren( hwnd );

        // The first page is built once the frame exists - the menu the page
        // ticks is not there yet while the client is being created.
        WinPostMsg( hwnd, WMU_START, MPVOID, MPVOID );
        return (MRESULT)FALSE;
    }

    case WMU_START:
        SelectCategory( g_category );
        if ( hCatList != NULLHANDLE ) WinSetFocus( HWND_DESKTOP, hCatList );
        return (MRESULT)FALSE;

    case WM_SIZE:
        LayoutChildren( hwnd );
        WinInvalidateRect( hwnd, NULL, FALSE );
        return (MRESULT)FALSE;

    // Focus arriving at the client belongs to the list.  Posted rather than
    // done here: PM does not allow the focus to be moved again from inside
    // the WM_SETFOCUS that is delivering it.
    case WM_SETFOCUS:
        if ( SHORT1FROMMP( mp2 ) && hCatList != NULLHANDLE )
            WinPostMsg( hwnd, WMU_FOCUSLIST, MPVOID, MPVOID );
        break;

    case WMU_FOCUSLIST:
        if ( WinQueryFocus( HWND_DESKTOP ) == hwnd && hCatList != NULLHANDLE )
            WinSetFocus( HWND_DESKTOP, hCatList );
        return (MRESULT)FALSE;

    // only the status bar is the client's own; the children cover the rest
    case WM_ERASEBACKGROUND:
        return (MRESULT)FALSE;

    case WM_PAINT: {
        HPS   hps;
        RECTL rclPaint;

        hps = WinBeginPaint( hwnd, NULLHANDLE, &rclPaint );
        PaintStatusBar( hwnd, hps );
        WinEndPaint( hps );
        return (MRESULT)FALSE;
    }

    case WM_CONTROL:
        if ( SHORT1FROMMP( mp1 ) == IDC_CATLIST &&
             SHORT2FROMMP( mp1 ) == LN_SELECT ) {
            LONG sel;

            if ( g_list_syncing ) return (MRESULT)FALSE;

            // sign-extended from the SHORT the list returns, as pmwin.h's
            // WinQueryLboxSelectedItem does, or LIT_NONE reads as 65535
            sel = (LONG)(SHORT)SHORT1FROMMR( WinSendMsg( hCatList, LM_QUERYSELECTION,
                                                         MPFROMLONG( LIT_FIRST ),
                                                         MPVOID ) );
            if ( sel != LIT_NONE && sel != g_category )
                SelectCategory( (int)sel );
            return (MRESULT)FALSE;
        }
        break;

    case WM_COMMAND: {
        USHORT cmd = SHORT1FROMMP( mp1 );

        if ( cmd >= IDM_VIEW_FIRST && cmd < IDM_VIEW_FIRST + CAT_COUNT ) {
            SelectCategory( (int)( cmd - IDM_VIEW_FIRST ) );
            return (MRESULT)FALSE;
        }

        switch ( cmd ) {

        case IDM_VIEW_REFRESH:
            WinSetPointer( HWND_DESKTOP,
                           WinQuerySysPointer( HWND_DESKTOP, SPTR_WAIT, FALSE ) );
            RvDataRefresh();
            if ( hPane != NULLHANDLE ) PaneRefresh( hPane );
            WinInvalidateRect( hwnd, NULL, FALSE );
            WinSetPointer( HWND_DESKTOP,
                           WinQuerySysPointer( HWND_DESKTOP, SPTR_ARROW, FALSE ) );
            return (MRESULT)FALSE;

        case IDM_VIEW_BENCHMARK:
            if ( RvBenchmarkRunning() ) return (MRESULT)FALSE;

            // The pane's "Run Benchmark Now" button, on a page that already
            // says what running it costs, has asked the question once
            // already; the menu and F9 ask it here.
            if ( SHORT1FROMMP( mp2 ) != CMDSRC_PUSHBUTTON &&
                 WinMessageBox( HWND_DESKTOP, hwnd,
                     (PCSZ)"The benchmark runs for about six seconds.  Other "
                           "programs running at the same time will lower the "
                           "scores.\n\nRun it now?",
                     (PCSZ)APP_NAME, 0, MB_YESNO | MB_QUERY ) != MBID_YES )
                return (MRESULT)FALSE;

            StartBenchmark( hwnd );
            return (MRESULT)FALSE;

        case IDM_FILE_EXIT:
            WinPostMsg( hwnd, WM_CLOSE, MPVOID, MPVOID );
            return (MRESULT)FALSE;

        case IDM_HELP_ABOUT:
            WinDlgBox( HWND_DESKTOP, hwnd, AboutDlgProc,
                       NULLHANDLE, IDD_ABOUT, NULL );
            return (MRESULT)FALSE;

        case IDM_HELP_SUPPORT:      // OK-only, so About's procedure serves
            WinDlgBox( HWND_DESKTOP, hwnd, AboutDlgProc,
                       NULLHANDLE, IDD_SUPPORT, NULL );
            return (MRESULT)FALSE;
        }
        break;
    }

    case WM_TIMER:
        if ( SHORT1FROMMP( mp1 ) == TID_BENCHMARK ) {
            if ( RvBenchmarkPoll() ) BenchmarkFinished( hwnd );
            return (MRESULT)FALSE;
        }
        break;

    // only a prompt: saves up to one timer period, does nothing if the tick
    // got there first
    case WMU_WORKER_DONE:
        if ( RvBenchmarkPoll() ) BenchmarkFinished( hwnd );
        return (MRESULT)FALSE;

    // The worker must be stopped before the window it posts to goes away.
    case WM_CLOSE:
        if ( RvBenchmarkRunning() ) {
            if ( !RvBenchmarkStop( 5000 ) ) {
                WinMessageBox( HWND_DESKTOP, hwnd,
                    (PCSZ)"The benchmark has not stopped yet.\n\n"
                          "Try again in a moment.",
                    (PCSZ)APP_NAME, 0, MB_OK | MB_WARNING );
                return (MRESULT)FALSE;
            }
            RvBenchmarkPoll();          /* drain the latch; we are leaving  */
            WinStopTimer( hab, hwnd, TID_BENCHMARK );
        }
        break;

    case WM_DESTROY:
        RvDataTerm();
        break;
    }

    return WinDefWindowProc( hwnd, msg, mp1, mp2 );
}

/* ==========================================================================
 * AboutDlgProc - About and Support, both OK-only
 * ==========================================================================*/

MRESULT EXPENTRY AboutDlgProc( HWND hwndDlg, ULONG msg, MPARAM mp1, MPARAM mp2 )
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
