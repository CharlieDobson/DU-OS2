/*===========================================================================
 * EXPLORER.H  -  Application globals, navigation, and shared declarations
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *===========================================================================*/

#ifndef EXPLORER_H
#define EXPLORER_H

/*---------------------------------------------------------------------------
 * Window classes registered by this program
 *-------------------------------------------------------------------------*/
#define EXP_CLIENT_CLASS    "ExplorerClient"
#define EXP_TOOLBAR_CLASS   "ExplorerToolbar"
#define EXP_STATBAR_CLASS   "ExplorerStatusBar"
#define EXP_SEARCH_CLASS    "ExplorerSearch"

/*---------------------------------------------------------------------------
 * Application globals
 *-------------------------------------------------------------------------*/
extern HAB   hab;
extern HWND  hwndFrame;
extern HWND  hwndClient;
extern char  szAppName[];
extern char  szTitle[];

/* Full path of this executable, kept so File / New Window can start another
   copy of us.  Taken from argv[0] in main. */
extern char  szExePath[];

/* Navigation state - current directory path; empty = drives view.
   Always ends with a backslash when non-empty. */
extern char  szCurrentPath[];

/* View mode - one of IDM_VIEW_SMALL / MEDIUM / LARGE / DETAILS */
extern int   nCurrentView;

/* Selection indices (-1 = none) */
extern int   nSelectedDrive;
extern int   nSelectedFile;

/* Multi-selection flags - one per file slot (TRUE = selected) */
extern BOOL  abSelectedFiles[];

/* Scroll position, in content-logical pixels (x right, y DOWN) */
extern LONG  nScrollX;
extern LONG  nScrollY;

/* Clipboard format, from the system atom table */
extern ULONG uClipFormat;

/* Message id for cross-instance drag-and-drop, also a system atom.
   mp1 = BOOL bIsMove, mp2 = unused; the payload travels through the named
   shared memory block below. */
extern ULONG uDragDropMsg;

/* Show hidden/system files in the directory view */
extern BOOL  bShowHidden;

/* Index of the folder highlighted as the current drop target (-1 = none) */
extern int   nDragHoverIdx;

/* Drag state constants */
#define DRAG_NONE     0
#define DRAG_PENDING  1
#define DRAG_ACTIVE   2

/*---------------------------------------------------------------------------
 * Cross-instance drag payload.
 *
 * PM processes cannot share a private allocation, so the block is
 * named shared memory: the source fills it and posts uDragDropMsg,
 * the target opens it by name with DosGetNamedSharedMem and copies it out.
 * Only one drag can be in flight at a time, which is true of a mouse.
 *-------------------------------------------------------------------------*/
#define EXP_DRAGMEM_NAME  "\\SHAREMEM\\EXPLORER\\DRAGDROP"

/*---------------------------------------------------------------------------
 * One path-list block serves both the clipboard and the drag transfer.
 *
 * A drag is a copy or a move, which is what ulOp already says, so the
 * clipboard and the drag transfer use one format and one reader.
 *
 * The paths follow the header back to back, each null-terminated, and
 * cbTotal covers the whole block so a reader can copy it out of the
 * clipboard's shared memory without walking it first.
 *-------------------------------------------------------------------------*/
#define CLIP_OP_COPY  0
#define CLIP_OP_CUT   1

typedef struct _EXPPATHLIST {
    ULONG cbTotal;      /* size of this block, header included */
    ULONG ulOp;         /* CLIP_OP_COPY or CLIP_OP_CUT         */
    ULONG cPaths;
    CHAR  achPaths[1];  /* path0\0 path1\0 ...                 */
} EXPPATHLIST;

#define EXP_PATHLIST_HDR        ((ULONG)(sizeof(EXPPATHLIST) - sizeof(CHAR)))

/* Sanity bound on a block arriving from the clipboard or another process */
#define EXP_PATHLIST_MAX_BYTES  0x100000UL

/*---------------------------------------------------------------------------
 * Navigation.  Pass NULL or "" to return to the top-level drives view.
 *-------------------------------------------------------------------------*/
void NavigateTo(HWND hwnd, const char *pszPath);

/* TRUE if the drive holds a readable volume. */
BOOL IsDriveReady(char cDrive);

/*---------------------------------------------------------------------------
 * TRUE while the drive combo is being set from code rather than by the user.
 *
 * !! PM TRAP !!  LM_SELECTITEM DOES send CBN_LBSELECT, so the combo cannot be
 * seeded silently.  Without this flag, filling the
 * combo at startup looks exactly like the user choosing the first drive --
 * which probes it and puts up a "not ready" box before the window is even
 * usable -- and the sync at the end of NavigateTo re-enters NavigateTo.
 *-------------------------------------------------------------------------*/
extern BOOL bComboSyncing;

/* Append a backslash to pszPath unless it already ends with one. */
void EnsureTrailingSlash(char *pszPath);

/* Put up a WinMessageBox owned by hwnd with the application title. */
ULONG ExpMessageBox(HWND hwnd, const char *pszText, ULONG flStyle);

/* Window procedures */
MRESULT EXPENTRY ClientWndProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2);
MRESULT EXPENTRY AboutDlgProc (HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2);

#endif /* EXPLORER_H */
