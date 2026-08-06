/*===========================================================================
 * PMMAIN.CPP  -  Explorer: main, client window, navigation, drag-and-drop
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * A client window that paints a grid of drives or files between a toolbar
 * and a status bar.
 *
 *   WinMain / RegisterClass        ->  main / WinRegisterClass
 *   CreateWindow(OVERLAPPEDWINDOW) ->  WinCreateStdWindow(FCF_*)
 *   GetMessage / DispatchMessage   ->  WinGetMsg / WinDispatchMsg
 *   WS_VSCROLL | WS_HSCROLL        ->  two WC_SCROLLBAR children of our own
 *   WM_KEYDOWN                     ->  WM_CHAR with KC_VIRTUALKEY
 *   WM_LBUTTON* / WM_RBUTTONUP     ->  WM_BUTTON1* / WM_BUTTON2UP
 *   SetCapture / ReleaseCapture    ->  WinSetCapture(HWND_DESKTOP, ...)
 *   CreatePopupMenu / AppendMenu   ->  WinLoadMenu + MM_SETITEMATTR
 *   TrackPopupMenu                 ->  WinPopupMenu
 *   Check/EnableMenuItem           ->  MM_SETITEMATTR, MIA_CHECKED/_DISABLED
 *   GetPrivateProfile* (EXPLORER.INI) -> Prf* on OS2.INI
 *   WinExec                        ->  WinStartApp (see FILEOPS.CPP)
 *   CTL3D.DLL                      ->  dropped; PM controls are already 3-D
 *
 * TWO THINGS WORTH KNOWING BEFORE CHANGING ANYTHING HERE.
 *
 * 1. THE OFF-SCREEN BUFFER.  Scrolling an icon grid flickers badly if drawn
 *    straight to the screen, so PaintContent draws into a memory DC
 *    (GpiCreateBitmap) and GpiBitBlts it across.  The buffer's rectangle is
 *    (0,0,cx,cy), and PAINT.CPP's CellRect works from whatever surface
 *    rectangle it is handed, so the drawing code neither knows nor cares
 *    which of the two it is painting into.
 *
 * 2. DRAG-AND-DROP BETWEEN INSTANCES uses NAMED SHARED MEMORY, because two
 *    OS/2 processes do not share an address space: the source fills
 *    \SHAREMEM\EXPLORER\DRAGDROP, sends the message, and the target opens
 *    the block by name.  The message id is a system atom.
 *
 *    OS/2's native drag protocol (DM_DRAGOVER / DM_DROP) would additionally
 *    interoperate with Workplace Shell folders.  That is a larger, different
 *    feature and is deliberately not what this does.
 *===========================================================================*/
#define INCL_WIN
#define INCL_GPI
#define INCL_WINWORKPLACE
#define INCL_WINSHELLDATA
#define INCL_DEV
#define INCL_DOSFILEMGR
#define INCL_DOSMEMMGR
#define INCL_DOSMISC
#define INCL_DOSERRORS

#include <os2.h>
#include <string.h>
#include <stdio.h>

#include "res/resource.h"
#include "explorer.h"
#include "drives.h"
#include "dirview.h"
#include "paint.h"
#include "toolbar.h"
#include "statbar.h"
#include "search.h"
#include "fileops.h"
#include "props.h"
#include "network.h"
#include "pmworker.h"

/*==================================================================
 * Globals
 *==================================================================*/

HAB   hab        = NULLHANDLE;
HWND  hwndFrame  = NULLHANDLE;
HWND  hwndClient = NULLHANDLE;

char  szAppName[] = "Explorer";
char  szTitle[]   = "Explorer";
char  szExePath[CCHMAXPATH] = "";

char  szCurrentPath[CCHMAXPATH] = "";
int   nCurrentView   = IDM_VIEW_MEDIUM;
int   nSelectedDrive = -1;
int   nSelectedFile  = -1;
BOOL  abSelectedFiles[MAX_FILES];
LONG  nScrollX       = 0;
LONG  nScrollY       = 0;
ULONG uClipFormat    = 0;
ULONG uDragDropMsg   = 0;
BOOL  bShowHidden    = FALSE;
int   nDragHoverIdx  = -1;
BOOL  bComboSyncing  = FALSE;   /* see EXPLORER.H - LM_SELECTITEM notifies */

/* Index of the drive or file last clicked with button 2 (-1 = none) */
static int nContextDrive = -1;
static int nContextFile  = -1;

/* The popup menu currently up, so the next one can dispose of it */
static HWND hwndCtxMenu = NULLHANDLE;

/*==================================================================
 * Small shared helpers
 *==================================================================*/

void EnsureTrailingSlash(char *pszPath)
{
    int n = (int)strlen(pszPath);

    if (n > 0 && pszPath[n - 1] != '\\' && n < CCHMAXPATH - 2) {
        pszPath[n]     = '\\';
        pszPath[n + 1] = '\0';
    }
}

ULONG ExpMessageBox(HWND hwnd, const char *pszText, ULONG flStyle)
{
    return WinMessageBox(HWND_DESKTOP, hwnd, (PCSZ)pszText,
                         (PCSZ)szTitle, 0, flStyle);
}

/*------------------------------------------------------------------
  IsDriveReady - TRUE if the drive holds a readable volume

    The probe is picked by drive type: DosQueryFSInfo is the cheap answer for
    a local volume, but a redirector or a CD may not answer it meaningfully,
    and a directory listing is what those do implement.

    DosError(FERR_DISABLEHARDERR) is set once in main, so probing an empty
    drive returns an error rather than raising a hard-error popup.
------------------------------------------------------------------*/
BOOL IsDriveReady(char cDrive)
{
    DRIVEINFO   *pDrv;
    FSALLOCATE   fsa;
    HDIR         hdir = HDIR_CREATE;
    FILEFINDBUF3 ff;
    ULONG        cnt  = 1;
    char         szPat[16];
    int          nType;

    if (cDrive >= 'a' && cDrive <= 'z')
        cDrive = (char)(cDrive - 'a' + 'A');

    pDrv  = FindDriveInfo(cDrive);
    nType = (pDrv != NULL) ? pDrv->nType : DRV_FIXED;

    if (nType == DRV_REMOTE || nType == DRV_CDROM) {

        sprintf(szPat, "%c:\\*", cDrive);

        if (DosFindFirst((PCSZ)szPat, &hdir,
                         FILE_NORMAL | FILE_READONLY | FILE_HIDDEN |
                         FILE_SYSTEM | FILE_ARCHIVED | FILE_DIRECTORY,
                         (PVOID)&ff, sizeof(ff), &cnt,
                         FIL_STANDARD) == NO_ERROR) {
            DosFindClose(hdir);
            return TRUE;
        }

        /* An empty but perfectly accessible root reports no more files */
        return (BOOL)(DosQueryFSInfo((ULONG)(cDrive - 'A' + 1), FSIL_ALLOC,
                                     (PVOID)&fsa, sizeof(fsa)) == NO_ERROR);
    }

    memset(&fsa, 0, sizeof(fsa));

    return (BOOL)(DosQueryFSInfo((ULONG)(cDrive - 'A' + 1), FSIL_ALLOC,
                                 (PVOID)&fsa, sizeof(fsa)) == NO_ERROR);
}

/*==================================================================
 * Settings, in OS2.INI rather than a private EXPLORER.INI
 *==================================================================*/

#define INI_APP         "Explorer"
#define INI_KEY_VIEW    "View"
#define INI_KEY_HIDDEN  "ShowHidden"

static void SaveViewSetting(int nView)
{
    char szVal[16];

    sprintf(szVal, "%d", nView);
    PrfWriteProfileString(HINI_USERPROFILE, (PCSZ)INI_APP,
                          (PCSZ)INI_KEY_VIEW, (PCSZ)szVal);
}

static int LoadViewSetting(void)
{
    return (int)PrfQueryProfileInt(HINI_USERPROFILE, (PCSZ)INI_APP,
                                   (PCSZ)INI_KEY_VIEW,
                                   (LONG)IDM_VIEW_MEDIUM);
}

static void SaveHiddenSetting(BOOL bHidden)
{
    PrfWriteProfileString(HINI_USERPROFILE, (PCSZ)INI_APP,
                          (PCSZ)INI_KEY_HIDDEN,
                          (PCSZ)(bHidden ? "1" : "0"));
}

static BOOL LoadHiddenSetting(void)
{
    return (BOOL)(PrfQueryProfileInt(HINI_USERPROFILE, (PCSZ)INI_APP,
                                     (PCSZ)INI_KEY_HIDDEN, 0L) != 0);
}

/*==================================================================
 * Menu state
 *==================================================================*/

static HWND MenuBar(void)
{
    return WinWindowFromID(hwndFrame, FID_MENU);
}

static void MenuEnable(HWND hwndMenu, ULONG idItem, BOOL bEnable)
{
    if (hwndMenu == NULLHANDLE)
        return;

    WinSendMsg(hwndMenu, MM_SETITEMATTR,
               MPFROM2SHORT((SHORT)idItem, TRUE),
               MPFROM2SHORT(MIA_DISABLED,
                            (SHORT)(bEnable ? 0 : MIA_DISABLED)));
}

static void MenuCheck(HWND hwndMenu, ULONG idItem, BOOL bCheck)
{
    if (hwndMenu == NULLHANDLE)
        return;

    WinSendMsg(hwndMenu, MM_SETITEMATTR,
               MPFROM2SHORT((SHORT)idItem, TRUE),
               MPFROM2SHORT(MIA_CHECKED,
                            (SHORT)(bCheck ? MIA_CHECKED : 0)));
}

/*==================================================================
 * Layout
 *==================================================================*/

static void LayoutChildren(HWND hwnd)
{
    RECTL rcl;
    LONG  cx, cy, sbw, sbh, contentTop, contentBot;

    WinQueryWindowRect(hwnd, &rcl);
    cx = rcl.xRight - rcl.xLeft;
    cy = rcl.yTop   - rcl.yBottom;

    sbw = WinQuerySysValue(HWND_DESKTOP, SV_CXVSCROLL);
    sbh = WinQuerySysValue(HWND_DESKTOP, SV_CYHSCROLL);
    if (sbw <= 0) sbw = 16;
    if (sbh <= 0) sbh = 16;

    LayoutToolbar(hwnd);
    LayoutStatusBar(hwnd);

    contentTop = cy - TB_HEIGHT;
    contentBot = SB_HEIGHT + sbh;

    if (contentTop < contentBot)
        contentTop = contentBot;

    if (hwndVScroll != NULLHANDLE)
        WinSetWindowPos(hwndVScroll, HWND_TOP,
                        cx - sbw, contentBot, sbw, contentTop - contentBot,
                        SWP_MOVE | SWP_SIZE | SWP_SHOW);

    if (hwndHScroll != NULLHANDLE)
        WinSetWindowPos(hwndHScroll, HWND_TOP,
                        0, SB_HEIGHT, cx - sbw, sbh,
                        SWP_MOVE | SWP_SIZE | SWP_SHOW);

    UpdateScrollBars(hwnd);
}

/*==================================================================
 * Navigation
 *==================================================================*/

void NavigateTo(HWND hwnd, const char *pszPath)
{
    char szCap[CCHMAXPATH + 32];
    HWND hwndMenu = MenuBar();
    int  i;

    WinSetPointer(HWND_DESKTOP,
                  WinQuerySysPointer(HWND_DESKTOP, SPTR_WAIT, FALSE));

    if (pszPath == NULL || pszPath[0] == '\0') {
        szCurrentPath[0] = '\0';
        FreeFileIcons();
    }
    else {
        strncpy(szCurrentPath, pszPath, sizeof(szCurrentPath) - 2);
        szCurrentPath[sizeof(szCurrentPath) - 2] = '\0';
        EnsureTrailingSlash(szCurrentPath);
        EnumDirectory(szCurrentPath, bShowHidden);
    }

    /* Sync the toolbar combo to the current drive letter */
    if (hwndPathCombo != NULLHANDLE && szCurrentPath[0] != '\0') {

        char cDrive = szCurrentPath[0];
        int  nItems = (int)(LONG)WinSendMsg(hwndPathCombo,
                                            LM_QUERYITEMCOUNT,
                                            MPVOID, MPVOID);
        int  j;

        if (cDrive >= 'a' && cDrive <= 'z')
            cDrive = (char)(cDrive - 'a' + 'A');

        for (j = 0; j < nItems; j++) {

            char szItem[16];

            szItem[0] = '\0';
            WinSendMsg(hwndPathCombo, LM_QUERYITEMTEXT,
                       MPFROM2SHORT((SHORT)j, (SHORT)sizeof(szItem)),
                       MPFROMP(szItem));

            if (szItem[0] == cDrive) {
                /* our own selection, not the user's - see EXPLORER.H */
                bComboSyncing = TRUE;
                WinSendMsg(hwndPathCombo, LM_SELECTITEM,
                           MPFROMLONG((LONG)j), MPFROMSHORT(TRUE));
                bComboSyncing = FALSE;
                break;
            }
        }
    }

    /* The frame owns the title bar, so the text goes to the frame */
    if (szCurrentPath[0] == '\0') {
        WinSetWindowText(hwndFrame, (PCSZ)szTitle);
    }
    else {
        sprintf(szCap, "Explorer - %s", szCurrentPath);
        WinSetWindowText(hwndFrame, (PCSZ)szCap);
    }

    /* New Dir / Rename / Delete are only meaningful inside a directory */
    {
        BOOL bInDir = (BOOL)(szCurrentPath[0] != '\0');

        MenuEnable(hwndMenu, IDM_FILE_NEW_DIR, bInDir);
        MenuEnable(hwndMenu, IDM_FILE_RENAME,  bInDir);
        MenuEnable(hwndMenu, IDM_FILE_DELETE,  bInDir);
    }

    /* Reset selection before the status bar is touched, so its selection
       pane never briefly shows the previous directory's count. */
    for (i = 0; i < MAX_FILES; i++)
        abSelectedFiles[i] = FALSE;

    nSelectedDrive = -1;
    nSelectedFile  = -1;
    nContextDrive  = -1;
    nContextFile   = -1;
    UpdateStatusSelection();

    /* In the drives view, clear the disk panes rather than probing a drive
       nobody asked about - that would spin up a floppy. */
    if (szCurrentPath[0] != '\0')
        UpdateStatusBar(szCurrentPath[0]);
    else
        ClearStatusBar();

    nScrollX = 0;
    nScrollY = 0;
    UpdateScrollBars(hwnd);

    WinInvalidateRect(hwnd, NULL, TRUE);
    WinUpdateWindow(hwnd);

    WinSetPointer(HWND_DESKTOP,
                  WinQuerySysPointer(HWND_DESKTOP, SPTR_ARROW, FALSE));
}

/*------------------------------------------------------------------
  ChangeView - switch view mode, reload the bitmaps, repaint
------------------------------------------------------------------*/
static void ChangeView(HWND hwnd, int nNewView)
{
    HWND hwndMenu = MenuBar();

    MenuCheck(hwndMenu, (ULONG)nCurrentView, FALSE);
    nCurrentView = nNewView;
    LoadViewBitmaps(nCurrentView);
    MenuCheck(hwndMenu, (ULONG)nCurrentView, TRUE);
    SaveViewSetting(nCurrentView);

    nScrollX = 0;
    nScrollY = 0;
    UpdateScrollBars(hwnd);

    WinInvalidateRect(hwnd, NULL, TRUE);
    WinUpdateWindow(hwnd);
}

/*------------------------------------------------------------------
  GoToParent - the toolbar's Back button
------------------------------------------------------------------*/
static void GoToParent(HWND hwnd)
{
    char  szParent[CCHMAXPATH];
    char *p;
    int   n;

    if (szCurrentPath[0] == '\0')
        return;

    strcpy(szParent, szCurrentPath);
    n = (int)strlen(szParent);

    if (n > 0 && szParent[n - 1] == '\\') {
        szParent[n - 1] = '\0';
        n--;
    }

    if (n <= 2) {                       /* at a root: back to the drives */
        NavigateTo(hwnd, NULL);
        return;
    }

    p = szParent + n - 1;
    while (p > szParent && *p != '\\')
        p--;

    if (*p == '\\')
        *(p + 1) = '\0';

    NavigateTo(hwnd, szParent);
}

/*==================================================================
 * Drag and drop
 *==================================================================*/

#define DRAG_THRESHOLD  4

static int          nDragState     = DRAG_NONE;
static LONG         nDragStartX    = 0;
static LONG         nDragStartY    = 0;
static HWND         hwndDragTarget = NULLHANDLE;
static EXPPATHLIST *pDragPaths     = NULL;

static void FreeDragPaths(void)
{
    if (pDragPaths != NULL) {
        DosFreeMem(pDragPaths);
        pDragPaths = NULL;
    }
}

static void DoCancelDrag(HWND hwnd)
{
    if (nDragState == DRAG_ACTIVE)
        WinSetCapture(HWND_DESKTOP, NULLHANDLE);

    FreeDragPaths();

    nDragState     = DRAG_NONE;
    nDragHoverIdx  = -1;
    hwndDragTarget = NULLHANDLE;

    WinInvalidateRect(hwnd, NULL, FALSE);
    WinUpdateWindow(hwnd);
}

/*------------------------------------------------------------------
  ExplorerClientUnder - another Explorer's client window at a point

    The point arrives in our own client coordinates and is mapped to the
    desktop; the immediate desktop child there is a frame, whose FID_CLIENT
    tells us whether it is one of us.
------------------------------------------------------------------*/
static HWND ExplorerClientUnder(HWND hwnd, LONG x, LONG y)
{
    POINTL ptl;
    HWND   hwndOther, hwndOtherClient;
    char   szClass[64];

    ptl.x = x;
    ptl.y = y;
    WinMapWindowPoints(hwnd, HWND_DESKTOP, &ptl, 1L);

    hwndOther = WinWindowFromPoint(HWND_DESKTOP, &ptl, FALSE);
    if (hwndOther == NULLHANDLE || hwndOther == hwndFrame)
        return NULLHANDLE;

    hwndOtherClient = WinWindowFromID(hwndOther, FID_CLIENT);
    if (hwndOtherClient == NULLHANDLE || hwndOtherClient == hwnd)
        return NULLHANDLE;

    szClass[0] = '\0';
    WinQueryClassName(hwndOtherClient, (LONG)sizeof(szClass), (PCH)szClass);

    if (strcmp(szClass, EXP_CLIENT_CLASS) != 0)
        return NULLHANDLE;

    return hwndOtherClient;
}

/* Our own reference to the published drag block, held only for the
   duration of the WinSendMsg that tells the other instance to read it. */
static PVOID pDragShared = NULL;

/*------------------------------------------------------------------
  PublishDragBlock - copy a path list into the named shared block

    Returns TRUE if the other process will be able to open it.  The caller
    must call RetractDragBlock once the far side has been told, or the name
    stays taken and the NEXT drag's DosAllocSharedMem fails: a named block
    lives until every reference to it is freed, and the receiver only ever
    drops the one it opened.
------------------------------------------------------------------*/
static void RetractDragBlock(void)
{
    if (pDragShared != NULL) {
        DosFreeMem(pDragShared);
        pDragShared = NULL;
    }
}

static BOOL PublishDragBlock(const EXPPATHLIST *pList, BOOL bIsMove)
{
    PVOID pStale = NULL;

    RetractDragBlock();

    /* A block left over from a drag that died mid-flight - ours or a
       crashed instance's - would take the name, so clear it first. */
    if (DosGetNamedSharedMem(&pStale, (PCSZ)EXP_DRAGMEM_NAME,
                             PAG_READ | PAG_WRITE) == NO_ERROR)
        DosFreeMem(pStale);

    if (DosAllocSharedMem(&pDragShared, (PCSZ)EXP_DRAGMEM_NAME,
                          pList->cbTotal,
                          PAG_COMMIT | PAG_READ | PAG_WRITE) != NO_ERROR) {
        pDragShared = NULL;
        return FALSE;
    }

    memcpy(pDragShared, pList, pList->cbTotal);
    ((EXPPATHLIST *)pDragShared)->ulOp = bIsMove ? CLIP_OP_CUT
                                                 : CLIP_OP_COPY;
    return TRUE;
}

/*------------------------------------------------------------------
  ReceiveDragBlock - the other side of PublishDragBlock

    Copies the shared block into private memory and hands the caller a
    pointer it must DosFreeMem.
------------------------------------------------------------------*/
static EXPPATHLIST *ReceiveDragBlock(void)
{
    PVOID        pShared = NULL;
    EXPPATHLIST *pLocal  = NULL;
    EXPPATHLIST *pSrc;

    if (DosGetNamedSharedMem(&pShared, (PCSZ)EXP_DRAGMEM_NAME,
                             PAG_READ | PAG_WRITE) != NO_ERROR)
        return NULL;

    pSrc = (EXPPATHLIST *)pShared;

    if (pSrc->cbTotal >= EXP_PATHLIST_HDR &&
        pSrc->cbTotal <= EXP_PATHLIST_MAX_BYTES) {

        if (DosAllocMem((PPVOID)&pLocal, pSrc->cbTotal,
                        PAG_COMMIT | PAG_READ | PAG_WRITE) == NO_ERROR)
            memcpy(pLocal, pSrc, pSrc->cbTotal);
    }

    DosFreeMem(pShared);
    return pLocal;
}

/*==================================================================
 * Painting
 *==================================================================*/

/*------------------------------------------------------------------
  PaintContent - draw the grid off-screen, then blit it into place
------------------------------------------------------------------*/
static void PaintContent(HPS hpsWin, const RECTL *pcont)
{
    DEVOPENSTRUC      dop;
    BITMAPINFOHEADER2 bmp2;
    SIZEL             sizl;
    RECTL             rclBuf;
    POINTL            aptl[3];
    HDC               hdcMem;
    HPS               hpsMem;
    HBITMAP           hbm, hbmOld;
    LONG              alData[2];
    LONG              cx, cy;

    cx = pcont->xRight - pcont->xLeft;
    cy = pcont->yTop   - pcont->yBottom;

    if (cx < 1 || cy < 1)
        return;

    memset(&dop, 0, sizeof(dop));

    hdcMem = DevOpenDC(hab, OD_MEMORY, (PCSZ)"*", 5L,
                       (PDEVOPENDATA)&dop, NULLHANDLE);
    if (hdcMem == NULLHANDLE)
        return;

    sizl.cx = cx;
    sizl.cy = cy;

    hpsMem = GpiCreatePS(hab, hdcMem, &sizl,
                         PU_PELS | GPIF_DEFAULT | GPIT_MICRO | GPIA_ASSOC);
    if (hpsMem == NULLHANDLE) {
        DevCloseDC(hdcMem);
        return;
    }

    /* The buffer has to match the screen's format or GpiBitBlt cannot
       copy it there without conversion. */
    alData[0] = 1;
    alData[1] = 24;
    DevQueryCaps(GpiQueryDevice(hpsWin), CAPS_COLOR_PLANES,   1L, &alData[0]);
    DevQueryCaps(GpiQueryDevice(hpsWin), CAPS_COLOR_BITCOUNT, 1L, &alData[1]);

    memset(&bmp2, 0, sizeof(bmp2));
    bmp2.cbFix     = sizeof(bmp2);
    bmp2.cx        = cx;
    bmp2.cy        = cy;
    bmp2.cPlanes   = (USHORT)alData[0];
    bmp2.cBitCount = (USHORT)alData[1];

    hbm = GpiCreateBitmap(hpsMem, (PBITMAPINFOHEADER2)&bmp2,
                          0L, NULL, NULL);
    if (hbm == NULLHANDLE) {
        GpiDestroyPS(hpsMem);
        DevCloseDC(hdcMem);
        return;
    }

    hbmOld = GpiSetBitmap(hpsMem, hbm);

    /* A fresh PS comes up in index colour mode; RGB values need saying so */
    GpiCreateLogColorTable(hpsMem, 0L, LCOLF_RGB, 0L, 0L, (PLONG)NULL);

    rclBuf.xLeft   = 0;
    rclBuf.yBottom = 0;
    rclBuf.xRight  = cx;
    rclBuf.yTop    = cy;

    if (szCurrentPath[0] == '\0')
        DrawDrives(hpsMem, &rclBuf);
    else
        DrawFiles(hpsMem, &rclBuf);

    /* target lower-left, target upper-right, source lower-left */
    aptl[0].x = pcont->xLeft;   aptl[0].y = pcont->yBottom;
    aptl[1].x = pcont->xRight;  aptl[1].y = pcont->yTop;
    aptl[2].x = 0;              aptl[2].y = 0;

    GpiBitBlt(hpsWin, hpsMem, 3L, aptl, ROP_SRCCOPY, BBO_IGNORE);

    GpiSetBitmap(hpsMem, hbmOld);
    GpiDeleteBitmap(hbm);
    GpiDestroyPS(hpsMem);
    DevCloseDC(hdcMem);
}

/*==================================================================
 * Context menus
 *==================================================================*/

static void ShowContextMenu(HWND hwnd, ULONG idMenu, LONG x, LONG y,
                            BOOL bWantOpen, BOOL bWantDisconnect)
{
    HWND hwndMenu;

    if (hwndCtxMenu != NULLHANDLE) {
        WinDestroyWindow(hwndCtxMenu);
        hwndCtxMenu = NULLHANDLE;
    }

    hwndMenu = WinLoadMenu(hwnd, NULLHANDLE, idMenu);
    if (hwndMenu == NULLHANDLE)
        return;

    /* Load the template and delete what does not apply, so the wording and
       the accelerators stay in the resource script with the rest. */
    if (!bWantOpen)
        WinSendMsg(hwndMenu, MM_DELETEITEM,
                   MPFROM2SHORT(IDM_CTX_OPEN, TRUE), MPVOID);

    if (idMenu == IDM_CTXMENU_DRIVE && !bWantDisconnect)
        WinSendMsg(hwndMenu, MM_DELETEITEM,
                   MPFROM2SHORT(IDM_CTX_DISCONNECT, TRUE), MPVOID);

    hwndCtxMenu = hwndMenu;

    WinPopupMenu(hwnd, hwnd, hwndMenu, x, y, 0L,
                 PU_HCONSTRAIN | PU_VCONSTRAIN |
                 PU_MOUSEBUTTON1 | PU_KEYBOARD);
}

/*==================================================================
 * Keyboard navigation
 *==================================================================*/

static void MoveSelection(HWND hwnd, USHORT usVK)
{
    int cols = GetViewColumns(hwnd);
    int i;

    if (szCurrentPath[0] == '\0') {

        int        nCount = GetDriveCount();
        int        n      = nSelectedDrive;
        DRIVEINFO *pDrv;

        if (nCount == 0)
            return;

        if (n < 0) {
            n = 0;                      /* the first press picks item 0 */
        }
        else {
            switch (usVK) {
            case VK_LEFT:  if (n > 0)             n--;        break;
            case VK_RIGHT: if (n < nCount - 1)    n++;        break;
            case VK_UP:    if (n - cols >= 0)     n -= cols;  break;
            case VK_DOWN:  if (n + cols < nCount) n += cols;  break;
            case VK_HOME:  n = 0;                             break;
            case VK_END:   n = nCount - 1;                    break;
            }
        }

        if (n == nSelectedDrive)
            return;

        nSelectedDrive = n;
        nContextDrive  = -1;

        pDrv = GetDriveInfo(n);
        if (pDrv != NULL) {
            /* As on a single click: do not spin up a floppy */
            if (pDrv->nType == DRV_REMOVABLE)
                ClearStatusBar();
            else
                UpdateStatusBar(pDrv->szLabel[0]);
        }
    }
    else {

        int nCount = GetFileCount();
        int n      = nSelectedFile;

        if (nCount == 0)
            return;

        if (n < 0 || n >= nCount) {
            n = 0;
        }
        else {
            switch (usVK) {
            case VK_LEFT:  if (n > 0)             n--;        break;
            case VK_RIGHT: if (n < nCount - 1)    n++;        break;
            case VK_UP:    if (n - cols >= 0)     n -= cols;  break;
            case VK_DOWN:  if (n + cols < nCount) n += cols;  break;
            case VK_HOME:  n = 0;                             break;
            case VK_END:   n = nCount - 1;                    break;
            }
        }

        /* A keyboard move replaces the selection with a single item */
        for (i = 0; i < MAX_FILES; i++)
            abSelectedFiles[i] = FALSE;

        nSelectedFile      = n;
        abSelectedFiles[n] = TRUE;
        nContextFile       = -1;

        UpdateStatusSelection();
    }

    EnsureItemVisible(hwnd, (szCurrentPath[0] == '\0')
                             ? nSelectedDrive : nSelectedFile);
    WinInvalidateRect(hwnd, NULL, FALSE);
    WinUpdateWindow(hwnd);
}

/*------------------------------------------------------------------
  OpenDrive - navigate into a drive, complaining if it is not ready
------------------------------------------------------------------*/
static void OpenDrive(HWND hwnd, int nDriveIdx)
{
    DRIVEINFO *pDrv = GetDriveInfo(nDriveIdx);
    char       szMsg[80];

    if (pDrv == NULL)
        return;

    if (!IsDriveReady(pDrv->szLabel[0])) {
        sprintf(szMsg, "Drive %c: is not ready.", pDrv->szLabel[0]);
        ExpMessageBox(hwnd, szMsg, MB_OK | MB_WARNING);
        return;
    }

    NavigateTo(hwnd, pDrv->szLabel);
}

/*------------------------------------------------------------------
  SingleSelectedFile - the one selected item, or -1 if not exactly one
------------------------------------------------------------------*/
static int SingleSelectedFile(void)
{
    int nCount = GetFileCount();
    int nSel   = 0;
    int nIdx   = -1;
    int i;

    for (i = 0; i < nCount; i++) {
        if (abSelectedFiles[i]) {
            nSel++;
            nIdx = i;
        }
    }

    return (nSel == 1) ? nIdx : -1;
}

/*==================================================================
 * Command handling
 *==================================================================*/

static void OnCommand(HWND hwnd, USHORT idCmd)
{
    HWND hwndMenu = MenuBar();

    switch (idCmd) {

    /* ---- File ---- */
    case IDM_FILE_NEW_WINDOW:
        if (szExePath[0] != '\0')
            LaunchProgram(hwnd, szExePath, NULL);
        break;

    case IDM_FILE_NEW_DIR:
        DoNewDir(hwnd);
        break;

    case IDM_FILE_RENAME: {
        int nIdx = (nContextFile >= 0) ? nContextFile : SingleSelectedFile();
        if (nIdx >= 0)
            DoRename(hwnd, nIdx);
        break;
    }

    case IDM_FILE_DELETE:
        if (nContextFile >= 0)
            DoDelete(hwnd, nContextFile);
        else
            DoDeleteSelected(hwnd);
        break;

    case IDM_FILE_SEARCH:
        OpenSearchWindow(hwnd);
        break;

    case IDM_FILE_REFRESH:
        if (szCurrentPath[0] != '\0')
            NavigateTo(hwnd, szCurrentPath);
        else {
            EnumDrives();
            PopulatePathCombo(hwndPathCombo);
            NavigateTo(hwnd, NULL);
        }
        break;

    case IDM_FILE_EXIT:
        WinPostMsg(hwnd, WM_CLOSE, MPVOID, MPVOID);
        break;

    case IDM_FILE_BACK:
        GoToParent(hwnd);
        break;

    case IDM_FILE_PROPS:
        if (szCurrentPath[0] != '\0') {
            int nIdx = (nContextFile >= 0) ? nContextFile
                                           : SingleSelectedFile();
            if (nIdx >= 0)
                DoProperties(hwnd, nIdx);
        }
        else if (nSelectedDrive >= 0) {
            DoDriveProperties(hwnd, nSelectedDrive);
        }
        break;

    /* ---- Edit ---- */
    case IDM_EDIT_CUT:
        if (nContextFile >= 0)
            DoClipboardOp(hwnd, nContextFile, CLIP_OP_CUT);
        else
            DoClipboardOpSelected(hwnd, CLIP_OP_CUT);
        break;

    case IDM_EDIT_COPY:
        if (nContextFile >= 0)
            DoClipboardOp(hwnd, nContextFile, CLIP_OP_COPY);
        else
            DoClipboardOpSelected(hwnd, CLIP_OP_COPY);
        break;

    case IDM_EDIT_PASTE:
        DoPaste(hwnd);
        break;

    /* ---- View ---- */
    case IDM_VIEW_SMALL:
    case IDM_VIEW_MEDIUM:
    case IDM_VIEW_LARGE:
    case IDM_VIEW_DETAILS:
        ChangeView(hwnd, (int)idCmd);
        break;

    case IDM_VIEW_SHOW_HIDDEN:
        bShowHidden = (BOOL)(!bShowHidden);
        MenuCheck(hwndMenu, IDM_VIEW_SHOW_HIDDEN, bShowHidden);
        SaveHiddenSetting(bShowHidden);
        if (szCurrentPath[0] != '\0')
            NavigateTo(hwnd, szCurrentPath);
        break;

    /* ---- Network ---- */
    case IDM_NETWORK_CONNECT:
    case IDM_NETWORK_DISCONNECT:
        if (idCmd == IDM_NETWORK_CONNECT)
            OpenConnectNetDrive(hwnd);
        else
            OpenDisconnectNetDrive(hwnd);

        EnumDrives();
        PopulatePathCombo(hwndPathCombo);
        if (szCurrentPath[0] == '\0') {
            UpdateScrollBars(hwnd);
            WinInvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case IDM_CTX_DISCONNECT:
        if (nContextDrive >= 0) {

            DRIVEINFO *pDrv = GetDriveInfo(nContextDrive);

            if (pDrv != NULL && pDrv->nType == DRV_REMOTE) {
                DisconnectNetDriveDirect(hwnd, pDrv->szLabel[0]);
                EnumDrives();
                PopulatePathCombo(hwndPathCombo);
                if (szCurrentPath[0] == '\0') {
                    UpdateScrollBars(hwnd);
                    WinInvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
        break;

    /* ---- Context menu ---- */
    case IDM_CTX_OPEN:
        if (nContextDrive >= 0)
            OpenDrive(hwnd, nContextDrive);
        else if (nContextFile >= 0)
            OpenFileItem(hwnd, nContextFile);
        break;

    case IDM_CTX_PROPS:
        if (nContextDrive >= 0 && szCurrentPath[0] == '\0')
            DoDriveProperties(hwnd, nContextDrive);
        else if (nContextFile >= 0 && szCurrentPath[0] != '\0')
            DoProperties(hwnd, nContextFile);
        break;

    /* ---- Help ---- */
    case IDM_HELP_ABOUT:
        WinDlgBox(HWND_DESKTOP, hwnd, AboutDlgProc,
                  NULLHANDLE, IDD_ABOUT, NULL);
        break;
    }
}

/*==================================================================
 * ClientWndProc
 *==================================================================*/

MRESULT EXPENTRY ClientWndProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    /* The cross-instance drop message is a system atom, so it cannot be a
       case label; it is tested before the switch. */
    if (uDragDropMsg != 0 && msg == uDragDropMsg) {

        EXPPATHLIST *pList = ReceiveDragBlock();

        if (pList != NULL) {

            if (szCurrentPath[0] != '\0') {

                char szDst[CCHMAXPATH];

                strcpy(szDst, szCurrentPath);
                EnsureTrailingSlash(szDst);

                DoPasteFromList(hwnd, pList, (BOOL)LONGFROMMP(mp1), szDst);
                NavigateTo(hwnd, szCurrentPath);
            }

            DosFreeMem(pList);
        }

        return (MRESULT)FALSE;
    }

    switch (msg) {

    case WM_CREATE: {
        HWND hwndMenu;

        uClipFormat  = (ULONG)WinAddAtom(WinQuerySystemAtomTable(),
                                         (PCSZ)"ExplorerFileDrop");
        uDragDropMsg = (ULONG)WinAddAtom(WinQuerySystemAtomTable(),
                                         (PCSZ)"ExplorerDragDrop");

        CreateToolbar(hwnd);
        CreateStatusBar(hwnd);

        hwndVScroll = WinCreateWindow(hwnd, WC_SCROLLBAR, (PCSZ)"",
                                      WS_VISIBLE | SBS_VERT,
                                      0, 0, 0, 0,
                                      hwnd, HWND_TOP, IDC_VSCROLL,
                                      NULL, NULL);

        hwndHScroll = WinCreateWindow(hwnd, WC_SCROLLBAR, (PCSZ)"",
                                      WS_VISIBLE | SBS_HORZ,
                                      0, 0, 0, 0,
                                      hwnd, HWND_TOP, IDC_HSCROLL,
                                      NULL, NULL);

        EnumDrives();
        ReconnectNetDrives(hwnd);

        nCurrentView = LoadViewSetting();
        bShowHidden  = LoadHiddenSetting();

        /* OS2.INI is shared with every other program on the machine, so a
           value read out of it gets checked rather than trusted. */
        if (nCurrentView != IDM_VIEW_SMALL  &&
            nCurrentView != IDM_VIEW_MEDIUM &&
            nCurrentView != IDM_VIEW_LARGE  &&
            nCurrentView != IDM_VIEW_DETAILS)
            nCurrentView = IDM_VIEW_MEDIUM;

        LoadViewBitmaps(nCurrentView);
        PopulatePathCombo(hwndPathCombo);

        hwndMenu = MenuBar();
        MenuCheck (hwndMenu, (ULONG)nCurrentView, TRUE);
        MenuCheck (hwndMenu, IDM_VIEW_SHOW_HIDDEN, bShowHidden);
        MenuEnable(hwndMenu, IDM_FILE_NEW_DIR, FALSE);
        MenuEnable(hwndMenu, IDM_FILE_RENAME,  FALSE);
        MenuEnable(hwndMenu, IDM_FILE_DELETE,  FALSE);

        /* No redirector, no Network menu.  PM names the submenu by id, not
           by position. */
        if (!IsNetworkPresent())
            MenuEnable(hwndMenu, IDM_MENU_NETWORK, FALSE);

        ClearStatusBar();

        /* WinCreateStdWindow sizes the client itself, so laying the
           children out only on WM_SIZE may never happen at all. */
        LayoutChildren(hwnd);
        return (MRESULT)FALSE;          /* TRUE would abort the create */
    }

    case WM_SIZE:
        LayoutChildren(hwnd);
        WinInvalidateRect(hwnd, NULL, TRUE);
        return (MRESULT)FALSE;

    case WM_ERASEBACKGROUND:
        /* WM_PAINT covers the whole client; erasing first only flashes */
        return (MRESULT)FALSE;

    case WM_PAINT: {
        HPS   hps;
        RECTL rclPaint, rclContent, rclGap;
        LONG  sbw, sbh;

        hps = WinBeginPaint(hwnd, NULLHANDLE, &rclPaint);
        GpiCreateLogColorTable(hps, 0L, LCOLF_RGB, 0L, 0L, (PLONG)NULL);

        ContentRect(hwnd, &rclContent);
        PaintContent(hps, &rclContent);

        /* The square where the two scrollbars meet belongs to nobody */
        sbw = WinQuerySysValue(HWND_DESKTOP, SV_CXVSCROLL);
        sbh = WinQuerySysValue(HWND_DESKTOP, SV_CYHSCROLL);
        if (sbw <= 0) sbw = 16;
        if (sbh <= 0) sbh = 16;

        WinQueryWindowRect(hwnd, &rclGap);
        rclGap.xLeft   = rclGap.xRight - sbw;
        rclGap.yBottom = SB_HEIGHT;
        rclGap.yTop    = SB_HEIGHT + sbh;
        WinFillRect(hps, &rclGap,
                    WinQuerySysColor(HWND_DESKTOP, SYSCLR_BUTTONMIDDLE, 0));

        WinEndPaint(hps);
        return (MRESULT)FALSE;
    }

    /* ---- the drive combo, owned by us so its notification lands here ---- */
    case WM_CONTROL:
        if (SHORT1FROMMP(mp1) == IDC_TB_PATHCOMBO &&
            SHORT2FROMMP(mp1) == CBN_LBSELECT) {

            LONG nSel;

            /* Our own LM_SELECTITEM, not the user - see EXPLORER.H */
            if (bComboSyncing)
                return (MRESULT)FALSE;

            nSel = (LONG)WinSendMsg(hwndPathCombo, LM_QUERYSELECTION,
                                    MPFROMLONG(LIT_FIRST), MPVOID);

            if (nSel != LIT_NONE) {

                char szBuf[16];
                char szMsg[80];

                szBuf[0] = '\0';
                WinSendMsg(hwndPathCombo, LM_QUERYITEMTEXT,
                           MPFROM2SHORT((SHORT)nSel, (SHORT)sizeof(szBuf)),
                           MPFROMP(szBuf));

                if (!IsDriveReady(szBuf[0])) {
                    sprintf(szMsg, "Drive %c: is not ready.", szBuf[0]);
                    ExpMessageBox(hwnd, szMsg, MB_OK | MB_WARNING);
                    UpdateStatusBar(szBuf[0]);
                }
                else {
                    NavigateTo(hwnd, szBuf);
                }
            }

            return (MRESULT)FALSE;
        }
        break;

    case WM_COMMAND:
        OnCommand(hwnd, SHORT1FROMMP(mp1));
        return (MRESULT)FALSE;

    /* ---- keyboard ---- */
    case WM_CHAR: {
        USHORT fsFlags = SHORT1FROMMP(mp1);
        USHORT usVK    = SHORT2FROMMP(mp2);

        if (fsFlags & KC_KEYUP)
            return (MRESULT)TRUE;

        if (!(fsFlags & KC_VIRTUALKEY))
            break;

        switch (usVK) {

        case VK_DELETE:
            if (nContextFile >= 0)
                DoDelete(hwnd, nContextFile);
            else
                DoDeleteSelected(hwnd);
            return (MRESULT)TRUE;

        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_HOME:
        case VK_END:
            MoveSelection(hwnd, usVK);
            return (MRESULT)TRUE;

        case VK_NEWLINE:
        case VK_ENTER:
            if (szCurrentPath[0] == '\0') {
                if (nSelectedDrive >= 0)
                    OpenDrive(hwnd, nSelectedDrive);
            }
            else if (nSelectedFile >= 0) {
                OpenFileItem(hwnd, nSelectedFile);
            }
            return (MRESULT)TRUE;
        }
        break;
    }

    /* ---- mouse ---- */
    case WM_BUTTON1DOWN: {
        LONG x = (LONG)(SHORT)SHORT1FROMMP(mp1);
        LONG y = (LONG)(SHORT)SHORT2FROMMP(mp1);
        BOOL bCtrl;
        BOOL bChanged = FALSE;
        int  nHit;

        WinSetFocus(HWND_DESKTOP, hwnd);

        if (nDragState != DRAG_NONE)
            DoCancelDrag(hwnd);

        bCtrl = (BOOL)((WinGetKeyState(HWND_DESKTOP, VK_CTRL) & 0x8000)
                       ? TRUE : FALSE);

        if (szCurrentPath[0] == '\0') {

            /* Drives view: one at a time, Ctrl means nothing */
            nHit = HitTestDrive(hwnd, x, y);

            if (nHit != nSelectedDrive) {

                DRIVEINFO *pDrv;

                nSelectedDrive = nHit;
                bChanged       = TRUE;

                pDrv = GetDriveInfo(nHit);
                if (pDrv != NULL) {
                    if (pDrv->nType == DRV_REMOVABLE)
                        ClearStatusBar();
                    else
                        UpdateStatusBar(pDrv->szLabel[0]);
                }
            }
        }
        else {

            nHit = HitTestFile(hwnd, x, y);

            if (bCtrl) {
                if (nHit >= 0) {
                    abSelectedFiles[nHit] = (BOOL)(!abSelectedFiles[nHit]);
                    nSelectedFile = abSelectedFiles[nHit] ? nHit : -1;
                    bChanged = TRUE;
                }
            }
            else if (nHit != nSelectedFile ||
                     (nHit >= 0 && !abSelectedFiles[nHit])) {

                int i;

                for (i = 0; i < MAX_FILES; i++)
                    abSelectedFiles[i] = FALSE;

                nSelectedFile = nHit;
                if (nHit >= 0)
                    abSelectedFiles[nHit] = TRUE;

                bChanged = TRUE;
            }
        }

        if (bChanged) {
            UpdateStatusSelection();
            WinInvalidateRect(hwnd, NULL, FALSE);
            WinUpdateWindow(hwnd);
        }

        /* Arm a possible drag if an item was hit */
        if (szCurrentPath[0] != '\0' && nHit >= 0) {
            nDragState  = DRAG_PENDING;
            nDragStartX = x;
            nDragStartY = y;
        }

        return (MRESULT)TRUE;
    }

    case WM_BUTTON1DBLCLK: {
        LONG x = (LONG)(SHORT)SHORT1FROMMP(mp1);
        LONG y = (LONG)(SHORT)SHORT2FROMMP(mp1);
        int  nHit;

        if (szCurrentPath[0] == '\0') {
            nHit = HitTestDrive(hwnd, x, y);
            if (nHit >= 0)
                OpenDrive(hwnd, nHit);
        }
        else {
            nHit = HitTestFile(hwnd, x, y);
            if (nHit >= 0)
                OpenFileItem(hwnd, nHit);
        }

        return (MRESULT)TRUE;
    }

    case WM_MOUSEMOVE: {
        LONG x = (LONG)(SHORT)SHORT1FROMMP(mp1);
        LONG y = (LONG)(SHORT)SHORT2FROMMP(mp1);
        LONG dx, dy;
        int  nNewHover;

        if (nDragState == DRAG_NONE)
            break;

        if (!(WinGetKeyState(HWND_DESKTOP, VK_BUTTON1) & 0x8000)) {
            DoCancelDrag(hwnd);
            break;
        }

        if (nDragState == DRAG_PENDING) {

            dx = x - nDragStartX; if (dx < 0) dx = -dx;
            dy = y - nDragStartY; if (dy < 0) dy = -dy;

            if (dx > DRAG_THRESHOLD || dy > DRAG_THRESHOLD) {

                FreeDragPaths();
                pDragPaths = BuildPathList(-1, CLIP_OP_CUT);

                if (pDragPaths != NULL) {
                    nDragState = DRAG_ACTIVE;
                    WinSetCapture(HWND_DESKTOP, hwnd);
                }
                else {
                    nDragState = DRAG_NONE;
                    break;
                }
            }
        }

        if (nDragState == DRAG_ACTIVE) {

            nNewHover      = -1;
            hwndDragTarget = NULLHANDLE;

            /* A folder in this window, so long as it is not itself dragged */
            {
                int nHit = HitTestFile(hwnd, x, y);

                if (nHit >= 0 && !abSelectedFiles[nHit]) {
                    FILEITEM *pHit = GetFileItem(nHit);
                    if (pHit != NULL && pHit->bIsDir)
                        nNewHover = nHit;
                }
            }

            if (nNewHover < 0)
                hwndDragTarget = ExplorerClientUnder(hwnd, x, y);

            /* Cross while a drop is possible, arrow while it is not */
            WinSetPointer(HWND_DESKTOP,
                          WinQuerySysPointer(HWND_DESKTOP,
                              (nNewHover >= 0 ||
                               hwndDragTarget != NULLHANDLE)
                                  ? SPTR_MOVE : SPTR_ILLEGAL,
                              FALSE));

            if (nNewHover != nDragHoverIdx) {
                nDragHoverIdx = nNewHover;
                WinInvalidateRect(hwnd, NULL, FALSE);
                WinUpdateWindow(hwnd);
            }

            return (MRESULT)TRUE;
        }
        break;
    }

    case WM_BUTTON1UP:
        if (nDragState == DRAG_ACTIVE) {

            EXPPATHLIST *pList;
            HWND         hwndTgt;
            BOOL         bIsMove;
            int          nHover;

            /* Shift copies, plain drags move */
            bIsMove = (BOOL)((WinGetKeyState(HWND_DESKTOP, VK_SHIFT) & 0x8000)
                             ? FALSE : TRUE);

            /* Take the state before DoCancelDrag clears it */
            nHover     = nDragHoverIdx;
            hwndTgt    = hwndDragTarget;
            pList      = pDragPaths;
            pDragPaths = NULL;          /* so DoCancelDrag leaves it alone */

            DoCancelDrag(hwnd);

            if (nHover >= 0 && pList != NULL) {

                FILEITEM *pTarget = GetFileItem(nHover);

                if (pTarget != NULL && pTarget->bIsDir) {

                    char szDstDir[CCHMAXPATH];

                    strcpy(szDstDir, szCurrentPath);
                    EnsureTrailingSlash(szDstDir);
                    strcat(szDstDir, pTarget->szName);
                    EnsureTrailingSlash(szDstDir);

                    DoPasteFromList(hwnd, pList, bIsMove, szDstDir);
                    NavigateTo(hwnd, szCurrentPath);
                }
            }
            else if (hwndTgt != NULLHANDLE && pList != NULL) {

                /* Another instance takes it.  WinSendMsg is synchronous
                   across processes, so the shared block is still there
                   when the far side reads it. */
                if (PublishDragBlock(pList, bIsMove)) {

                    WinSendMsg(hwndTgt, uDragDropMsg,
                               MPFROMLONG((LONG)bIsMove), MPVOID);

                    /* The far side has read and released it by now, so
                       dropping our reference retires the name too. */
                    RetractDragBlock();

                    if (bIsMove)
                        NavigateTo(hwnd, szCurrentPath);
                }
            }

            if (pList != NULL)
                DosFreeMem(pList);

            return (MRESULT)TRUE;
        }
        else if (nDragState == DRAG_PENDING) {
            nDragState = DRAG_NONE;
        }
        break;

    case WM_BUTTON2UP: {
        LONG x = (LONG)(SHORT)SHORT1FROMMP(mp1);
        LONG y = (LONG)(SHORT)SHORT2FROMMP(mp1);
        int  nHit;

        nContextDrive = -1;
        nContextFile  = -1;

        if (szCurrentPath[0] == '\0') {

            nHit = HitTestDrive(hwnd, x, y);

            if (nHit >= 0) {

                DRIVEINFO *pDrv = GetDriveInfo(nHit);

                nContextDrive  = nHit;
                nSelectedDrive = nHit;
                WinInvalidateRect(hwnd, NULL, FALSE);
                WinUpdateWindow(hwnd);

                ShowContextMenu(hwnd, IDM_CTXMENU_DRIVE, x, y, TRUE,
                                (BOOL)(pDrv != NULL &&
                                       pDrv->nType == DRV_REMOTE));
            }
        }
        else {

            nHit = HitTestFile(hwnd, x, y);

            if (nHit >= 0) {

                FILEITEM *pFile = GetFileItem(nHit);
                BOOL      bOpen = FALSE;

                nContextFile  = nHit;
                nSelectedFile = nHit;
                WinInvalidateRect(hwnd, NULL, FALSE);
                WinUpdateWindow(hwnd);

                if (pFile != NULL) {
                    if (pFile->bIsDir || IsExecFile(pFile->szName)) {
                        bOpen = TRUE;
                    }
                    else {
                        char szFull[CCHMAXPATH];

                        strcpy(szFull, szCurrentPath);
                        EnsureTrailingSlash(szFull);
                        strcat(szFull, pFile->szName);
                        bOpen = HasAssociation(szFull);
                    }
                }

                ShowContextMenu(hwnd, IDM_CTXMENU_FILE, x, y, bOpen, FALSE);
            }
        }

        return (MRESULT)TRUE;
    }

    /* ---- scrollbars ---- */
    case WM_VSCROLL: {
        RECTL rcl;
        LONG  cyVisible, cxContent, cyContent, nNewY = nScrollY;

        if (SHORT1FROMMP(mp1) != IDC_VSCROLL)
            break;

        ContentRect(hwnd, &rcl);
        cyVisible = rcl.yTop - rcl.yBottom;
        if (cyVisible < 1) cyVisible = 1;

        CalcContentSize(rcl.xRight - rcl.xLeft, &cxContent, &cyContent);

        switch (SHORT2FROMMP(mp2)) {
        case SB_LINEUP:         nNewY -= GetViewCellHeight();       break;
        case SB_LINEDOWN:       nNewY += GetViewCellHeight();       break;
        case SB_PAGEUP:         nNewY -= cyVisible;                 break;
        case SB_PAGEDOWN:       nNewY += cyVisible;                 break;
        case SB_SLIDERTRACK:
        case SB_SLIDERPOSITION:
            nNewY = (LONG)(SHORT)SHORT1FROMMP(mp2) * ScrollScaleY();
            break;
        default:                                                    break;
        }

        if (nNewY > cyContent - cyVisible) nNewY = cyContent - cyVisible;
        if (nNewY < 0)                     nNewY = 0;

        if (nNewY != nScrollY) {
            nScrollY = nNewY;
            WinSendMsg(hwndVScroll, SBM_SETPOS,
                       MPFROMSHORT((SHORT)(nScrollY / ScrollScaleY())),
                       MPVOID);
            WinInvalidateRect(hwnd, NULL, FALSE);
            WinUpdateWindow(hwnd);
        }

        return (MRESULT)FALSE;
    }

    case WM_HSCROLL: {
        RECTL rcl;
        LONG  cxVisible, cxContent, cyContent, nNewX = nScrollX;

        if (SHORT1FROMMP(mp1) != IDC_HSCROLL)
            break;

        ContentRect(hwnd, &rcl);
        cxVisible = rcl.xRight - rcl.xLeft;
        if (cxVisible < 1) cxVisible = 1;

        CalcContentSize(cxVisible, &cxContent, &cyContent);

        switch (SHORT2FROMMP(mp2)) {
        case SB_LINELEFT:       nNewX -= 16;                        break;
        case SB_LINERIGHT:      nNewX += 16;                        break;
        case SB_PAGELEFT:       nNewX -= cxVisible;                 break;
        case SB_PAGERIGHT:      nNewX += cxVisible;                 break;
        case SB_SLIDERTRACK:
        case SB_SLIDERPOSITION:
            nNewX = (LONG)(SHORT)SHORT1FROMMP(mp2) * ScrollScaleX();
            break;
        default:                                                    break;
        }

        if (nNewX > cxContent - cxVisible) nNewX = cxContent - cxVisible;
        if (nNewX < 0)                     nNewX = 0;

        if (nNewX != nScrollX) {
            nScrollX = nNewX;
            WinSendMsg(hwndHScroll, SBM_SETPOS,
                       MPFROMSHORT((SHORT)(nScrollX / ScrollScaleX())),
                       MPVOID);
            WinInvalidateRect(hwnd, NULL, FALSE);
            WinUpdateWindow(hwnd);
        }

        return (MRESULT)FALSE;
    }

    /*==============================================================
     * File-operation worker (FILEOPS.CPP)
     *
     *   Copy, move and delete all run on a worker thread now.  These three
     *   messages are the whole of the UI side of it: a timer that refreshes
     *   the progress dialog, the overwrite question the worker cannot ask
     *   for itself, and the completion notice.
     *==============================================================*/

    case WM_TIMER:
        if (SHORT1FROMMP(mp1) == TID_FILEPROGRESS) {
            FileOpsOnTimer(hwnd);
            return (MRESULT)FALSE;
        }
        break;

    /* Both of these go to FileOpsOnTimer rather than straight to the handler
       they name.  It polls the worker's latches and does whichever of the two
       is actually outstanding, so the timer alone is enough if PM never
       delivers the message -- see FILEOPS.H.  mp1/mp2 are ignored on purpose:
       the latch carries the same information and cannot arrive twice. */
    case WMU_WORKER_ASK:
    case WMU_WORKER_DONE:
        FileOpsOnTimer(hwnd);
        return (MRESULT)FALSE;

    case WM_CLOSE:
        /* Both workers write globals and post to windows this teardown is
           about to destroy, so both have to have stopped first.  The search
           window's own WM_CLOSE covers the case where it is closed alone;
           this covers quitting with it still open. */
        if (FileOpsBusy() && !FileOpsShutdown()) {
            ExpMessageBox(hwnd,
                "A file operation has not stopped yet -- it is probably "
                "waiting on a drive that is not responding.\n\n"
                "Try again in a moment.",
                MB_OK | MB_WARNING);
            return (MRESULT)FALSE;
        }

        if (SearchBusy() && !SearchShutdown()) {
            ExpMessageBox(hwnd,
                "The search has not stopped yet. Try again in a moment.",
                MB_OK | MB_WARNING);
            return (MRESULT)FALSE;
        }
        break;

    case WM_DESTROY:
        if (hwndCtxMenu != NULLHANDLE) {
            WinDestroyWindow(hwndCtxMenu);
            hwndCtxMenu = NULLHANDLE;
        }
        FreeDragPaths();
        RetractDragBlock();
        FreeFileIcons();
        FreeViewBitmaps();
        break;
    }

    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

/*==================================================================
 * About box
 *==================================================================*/

MRESULT EXPENTRY AboutDlgProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {

    case WM_COMMAND:
        if (SHORT1FROMMP(mp1) == DID_OK ||
            SHORT1FROMMP(mp1) == DID_CANCEL) {
            WinDismissDlg(hwnd, TRUE);
            return (MRESULT)FALSE;
        }
        break;
    }

    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

/*==================================================================
 * CreateStdFrame - WinCreateStdWindow, with the resource-backed frame
 *                  flags isolated and the PM error reported
 *
 *   FCF_MENU, FCF_ICON and FCF_ACCELTABLE each send the frame off to
 *   load a resource by the frame's window id, and a load that fails
 *   takes the whole create down with it.  All WinCreateStdWindow then
 *   says is NULLHANDLE - which is how a program that is otherwise
 *   perfectly healthy ends up doing nothing but putting up "Window
 *   creation failed".
 *
 *   So: try the lot; if that fails, try again without each resource
 *   flag in turn, and finally without any of them.  The program runs -
 *   one icon or menu short - and szDiag (256 bytes, the caller's) names
 *   the resource PM would not load and carries the PM error code.
 *   szDiag comes back empty when the first attempt worked.
 *==================================================================*/

#define FCF_RESOURCE_FLAGS  (FCF_MENU | FCF_ICON | FCF_ACCELTABLE)

static HWND CreateStdFrame(HAB habApp, PCSZ pszClass, PCSZ pszTitle,
                           ULONG flWanted, ULONG idRes, HWND *phwndClient,
                           char *szDiag)
{
    static const ULONG  aflRes[3]  = { FCF_MENU, FCF_ICON, FCF_ACCELTABLE };
    static const char  *apszRes[3] = { "FCF_MENU", "FCF_ICON",
                                       "FCF_ACCELTABLE" };
    ULONG flCreate, flPresent, err;
    HWND  hwnd;
    int   i;

    szDiag[0] = '\0';

    flCreate = flWanted;
    hwnd = WinCreateStdWindow(HWND_DESKTOP, 0L, &flCreate, pszClass, pszTitle,
                              0L, NULLHANDLE, idRes, phwndClient);
    if (hwnd != NULLHANDLE)
        return hwnd;

    /* WinGetLastError both reports and clears, so take it once, here. */
    err       = (ULONG)WinGetLastError(habApp) & 0xFFFFL;
    flPresent = flWanted & FCF_RESOURCE_FLAGS;

    for (i = 0; i < 3; i++)
    {
        if (!(flPresent & aflRes[i])) continue;

        flCreate = flWanted & ~aflRes[i];
        hwnd = WinCreateStdWindow(HWND_DESKTOP, 0L, &flCreate, pszClass,
                                  pszTitle, 0L, NULLHANDLE, idRes,
                                  phwndClient);
        if (hwnd != NULLHANDLE)
        {
            sprintf(szDiag,
                    "The window could only be created with %s dropped, so PM "
                    "will not load that resource (id %lu) out of the .EXE.\n\n"
                    "PM error 0x%04lX.",
                    apszRes[i], (unsigned long)idRes, (unsigned long)err);
            return hwnd;
        }
    }

    /* No single flag to blame; if more than one is in play, try with all
       of them off before giving up.                                     */
    if (flPresent != 0 && flPresent != aflRes[0] &&
        flPresent != aflRes[1] && flPresent != aflRes[2])
    {
        flCreate = flWanted & ~FCF_RESOURCE_FLAGS;
        hwnd = WinCreateStdWindow(HWND_DESKTOP, 0L, &flCreate, pszClass,
                                  pszTitle, 0L, NULLHANDLE, idRes,
                                  phwndClient);
        if (hwnd != NULLHANDLE)
        {
            sprintf(szDiag,
                    "The window could only be created with the menu, icon and "
                    "accelerator flags all dropped: none of the .EXE's "
                    "resources for id %lu will load.\n\nPM error 0x%04lX.",
                    (unsigned long)idRes, (unsigned long)err);
            return hwnd;
        }
    }

    sprintf(szDiag, "WinCreateStdWindow failed with PM error 0x%04lX.",
            (unsigned long)err);
    return NULLHANDLE;
}

/*==================================================================
 * main
 *==================================================================*/

int main(int argc, char *argv[])
{
    HMQ   hmq;
    QMSG  qmsg;
    ULONG flFrameFlags;
    LONG  scrW, scrH, w, h, x, y;
    char  szDiag[256];

    if (argc > 0 && argv[0] != NULL) {
        strncpy(szExePath, argv[0], sizeof(szExePath) - 1);
        szExePath[sizeof(szExePath) - 1] = '\0';
    }

    hab = WinInitialize(0);
    if (hab == NULLHANDLE)
        return 1;

    hmq = WinCreateMsgQueue(hab, 0);
    if (hmq == NULLHANDLE) {
        WinTerminate(hab);
        return 1;
    }

    /* Probing drives must not raise "drive not ready" hard-error popups */
    DosError(FERR_DISABLEHARDERR);

    /* NOT CS_CLIPCHILDREN.  A PM combobox's window rectangle covers its
       drop-down list as well as its entry field, so the drive combo is
       COMBO_H (140) tall while only its top ~24 pixels are ever visible.
       It is a child of the client, so CS_CLIPCHILDREN would cut that whole
       220x140 rectangle out of every WM_PAINT and leave a block of the
       content area below the combo permanently unpainted.  Clipping is not
       needed anyway: the client only ever draws inside ContentRect (plus the
       square between the scrollbars), and every other child - toolbar,
       status bar, both scrollbars - lies outside it.  */
    if (!WinRegisterClass(hab, (PCSZ)EXP_CLIENT_CLASS, ClientWndProc,
                          CS_SIZEREDRAW, 0)
     || !RegisterToolbarClass(hab)
     || !RegisterStatusBarClass(hab)
     || !RegisterSearchClass(hab)) {
        WinDestroyMsgQueue(hmq);
        WinTerminate(hab);
        return 1;
    }

    flFrameFlags = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER
                 | FCF_MINMAX   | FCF_TASKLIST
                 | FCF_MENU     | FCF_ICON    | FCF_ACCELTABLE;

    hwndFrame = CreateStdFrame(hab, (PCSZ)EXP_CLIENT_CLASS, (PCSZ)szTitle,
                               flFrameFlags, ID_EXPLORER,
                               &hwndClient, szDiag);

    if (hwndFrame == NULLHANDLE) {
        WinMessageBox(HWND_DESKTOP, HWND_DESKTOP, (PCSZ)szDiag,
                      (PCSZ)szTitle, 0, MB_OK | MB_ERROR);
        WinDestroyMsgQueue(hmq);
        WinTerminate(hab);
        return 1;
    }

    if (szDiag[0])
        WinMessageBox(HWND_DESKTOP, HWND_DESKTOP, (PCSZ)szDiag,
                      (PCSZ)szTitle, 0, MB_OK | MB_WARNING);

    /* Three quarters of the desktop, centred */
    scrW = WinQuerySysValue(HWND_DESKTOP, SV_CXSCREEN);
    scrH = WinQuerySysValue(HWND_DESKTOP, SV_CYSCREEN);
    w = scrW * 3 / 4;
    h = scrH * 3 / 4;
    x = (scrW - w) / 2;
    y = (scrH - h) / 2;

    WinSetWindowPos(hwndFrame, HWND_TOP, x, y, w, h,
                    SWP_MOVE | SWP_SIZE | SWP_SHOW | SWP_ACTIVATE);

    /* Start in the drives view */
    NavigateTo(hwndClient, NULL);

    while (WinGetMsg(hab, &qmsg, NULLHANDLE, 0, 0))
        WinDispatchMsg(hab, &qmsg);

    WinDestroyWindow(hwndFrame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
