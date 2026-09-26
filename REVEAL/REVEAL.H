/*===========================================================================
 * REVEAL.H  -  Reveal for OS/2: shared declarations
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * The OS/2 port of the Dobson Utilities REVEAL system-information viewer.
 * Same window, same pages, same report model as the Windows builds in
 * S:\CPP\DUSOURCE\REVEAL (Win16) and S:\CPP\DU32\REVEAL (Win32s); what
 * changed is where the answers come from.  A PM program cannot issue a BIOS
 * call or read the first megabyte, so the figures come from:
 *
 *   DosQuerySysInfo       version, boot drive, memory, system limits
 *   DosDevConfig          the BIOS equipment list: diskettes, serial and
 *                         parallel ports, coprocessor, model byte
 *   CONFIG.SYS            swap file, device drivers, mouse, keyboard, LIBPATH
 *   TESTCFG$              a copy of the adapter/BIOS ROM area C0000h-FFFFFh
 *                         (LOWMEM.CPP) for the BIOS names and the map
 *   the Window List       what is running
 *
 * Include <os2.h> before this header.
 *===========================================================================*/

#ifndef REVEAL_H
#define REVEAL_H 1

/*---------------------------------------------------------------------------
 * Window classes / names
 *-------------------------------------------------------------------------*/

#define APP_NAME            "Reveal"
#define APP_TITLE           "Reveal - System Information"
#define MAIN_CLASS          "RevealMainWnd2"
#define PANE_CLASS          "RevealDetailPane2"

#define LIST_PANE_WIDTH     136     /* category list, pixels                */
#define LIST_PANE_MIN       96
#define STATUSBAR_HEIGHT    22

/* The presentation parameter every control of ours is given, so the list,
   the pane button and the pane's own text all agree. */
#define UI_FONT             "8.Helv"

/*---------------------------------------------------------------------------
 * Categories.  The order here is the order of the list box and of the
 * File menu; keep IDM_VIEW_FIRST + n in RES\REVEAL.RC in step.
 *-------------------------------------------------------------------------*/

enum reveal_category_t {
    CAT_SYSTEM = 0,
    CAT_PROCESSOR,
    CAT_MEMORY,
    CAT_FIRSTMEG,
    CAT_DISPLAY,
    CAT_DRIVES,
    CAT_NETWORK,
    CAT_PORTS,
    CAT_OS2,
    CAT_ENVIRONMENT,
    CAT_BENCHMARK,
    CAT_COUNT
};

/*---------------------------------------------------------------------------
 * The report model.
 *
 * Every page is built as a flat list of rows; the detail pane knows how to
 * paint each row kind and nothing else.  Adding a page therefore means
 * writing one Build...() function in PMDATA.CPP - no painting code.
 *-------------------------------------------------------------------------*/

enum report_kind_t {
    RK_END = 0,     /* terminator                                          */
    RK_HEADING,     /* bold section title with a rule beside it            */
    RK_ITEM,        /* label / value                                       */
    RK_SUBITEM,     /* indented label / value                              */
    RK_BLANK,       /* vertical space                                      */
    RK_COLHDR,      /* tab-separated column headings                       */
    RK_COLROW,      /* tab-separated data row (fixed pitch)                */
    RK_GAUGE,       /* label, value text and a 0..100 bar                  */
    RK_NOTE,        /* small note spanning the pane                        */
    RK_MEMMAP,      /* the first-megabyte map block (tall, self-drawing)   */
    RK_BUTTON       /* a real push button, captioned from 'label'          */
};

#define RPT_LABEL_MAX   36
#define RPT_VALUE_MAX   72
#define RPT_MAX_ROWS    500

typedef struct report_row_t {
    int  kind;
    int  pct;                       /* RK_GAUGE only, 0..100               */
    char label[RPT_LABEL_MAX];
    char value[RPT_VALUE_MAX];
} report_row_t;

/*---------------------------------------------------------------------------
 * First-megabyte map: 256 blocks of 4 KB.
 *-------------------------------------------------------------------------*/

#define MEMMAP_BLOCKS       256
#define MEMMAP_BLOCK_BYTES  4096L

enum memmap_class_t {
    MM_UNUSED = 0,      /* nothing there / not reported                    */
    MM_SYSTEM,          /* interrupt vectors, BIOS data area               */
    MM_PROGRAM,         /* OS/2, its drivers and programs                  */
    MM_FREE,            /* (never used by this build - see the legend)     */
    MM_VIDEO,           /* video adapter RAM                               */
    MM_ADAPTER_ROM,     /* option ROM found by its 55 AA signature         */
    MM_UPPER,           /* the rest of the adapter area                    */
    MM_SYSTEM_ROM,      /* the system BIOS itself                          */
    MM_CLASS_COUNT
};

extern BYTE g_memmap[ MEMMAP_BLOCKS ];

/*---------------------------------------------------------------------------
 * PMDATA.CPP - data gathering and report building
 *-------------------------------------------------------------------------*/

BOOL        RvDataInit( void );
void        RvDataTerm( void );
void        RvDataRefresh( void );          /* re-read the volatile figures */

int         RvBuildReport( int category, report_row_t* rows, int max_rows );
const char* RvCategoryName( int category );
int         RvCategoryBitmap( int category );   /* resource id, or 0       */
void        RvStatusText( char* buffer, int size );

/* The benchmark runs on a worker thread (see PMDATA.CPP): six seconds of
   the UI thread not reading its queue would freeze the whole desktop, PM
   having a single input queue for every program on the screen.

   RvBenchmarkStart   starts it; FALSE if it is already running or the
                      thread could not be created.  hwndNotify gets
                      WMU_WORKER_DONE (PMWORKER.H) when it ends.
   RvBenchmarkPoll    TRUE exactly once, when a finished run's results have
                      been taken; call from WMU_WORKER_DONE AND a timer.
   RvBenchmarkStop    cancels and waits up to ms; TRUE once it has stopped. */
BOOL        RvBenchmarkStart( HWND hwndNotify );
BOOL        RvBenchmarkRunning( void );
BOOL        RvBenchmarkPoll( void );
BOOL        RvBenchmarkStop( ULONG ms );
BOOL        RvBenchmarkDone( void );

/*---------------------------------------------------------------------------
 * PMPANE.CPP - the detail pane
 *-------------------------------------------------------------------------*/

BOOL        PaneRegisterClass( HAB hab );
BOOL        PaneTextFont( FATTRS* fat );    /* the pane's body font, so the
                                               status bar can match it      */
void        PaneSetCategory( HWND hPane, int category );
void        PaneRefresh( HWND hPane );
MRESULT EXPENTRY PaneWndProc( HWND, ULONG, MPARAM, MPARAM );

/*---------------------------------------------------------------------------
 * PMMAIN.CPP - shared with the pane and the data module
 *-------------------------------------------------------------------------*/

extern HAB  hab;
extern int  g_category;

#endif /* REVEAL_H */
