/*===========================================================================
 * STATBAR.CPP  -  Status bar: selection pane plus right-hand disk space panes
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 *   _dos_getdiskfree / diskfree_t  ->  DosQueryFSInfo(FSIL_ALLOC) / FSALLOCATE
 *   SetErrorMode(SEM_FAILCRIT...)  ->  DosError(FERR_DISABLEHARDERR), set once
 *                                      at start-up rather than around each call
 *   CreatePen / MoveTo / LineTo    ->  GpiSetColor / GpiMove / GpiLine
 *   DrawText(DT_VCENTER)           ->  WinDrawText(DT_VCENTER)
 *
 * The volume totals are computed in 64 BITS before being reduced for display:
 * an HPFS volume holds more bytes than 32 can count, and silently wrapping
 * would be worse than clamping.
 *===========================================================================*/
#define INCL_WIN
#define INCL_GPI
#define INCL_DOSFILEMGR
#define INCL_DOSERRORS

#include <os2.h>
#include <string.h>
#include <stdio.h>

#include "explorer.h"
#include "dirview.h"
#include "statbar.h"

HWND hwndStatusBar = NULLHANDLE;

/* Width of each right-hand disk pane; the selection pane takes the rest */
#define SB_DISK_W   110

static char szPaneSel[80] = "";
static char szPane0[80]   = "";
static char szPane1[80]   = "";

#define SYSCLR(c)  WinQuerySysColor(HWND_DESKTOP, (c), 0)
#define EXP_WHITE  0x00FFFFFFL

/*------------------------------------------------------------------
  FormatSize - a byte count as "nMB" or "nKB"
------------------------------------------------------------------*/
static void FormatSize(unsigned __int64 cb, char *pszOut)
{
    if (cb >= 1048576ui64)
        sprintf(pszOut, "%luMB", (unsigned long)(cb / 1048576ui64));
    else
        sprintf(pszOut, "%luKB", (unsigned long)((cb + 512ui64) / 1024ui64));
}

/*------------------------------------------------------------------
  Line - one GPI line in the given colour
------------------------------------------------------------------*/
static void Line(HPS hps, LONG x1, LONG y1, LONG x2, LONG y2, LONG clr)
{
    POINTL ptl;

    GpiSetColor(hps, clr);
    ptl.x = x1; ptl.y = y1;
    GpiMove(hps, &ptl);
    ptl.x = x2; ptl.y = y2;
    GpiLine(hps, &ptl);
}

/*------------------------------------------------------------------
  DrawSunkenRect - the 1-pixel inset border of a status pane

    Windows put the shadow along the top and left.  PM's top edge is the
    HIGH y, so the two pairs of lines change places rather than colours.
------------------------------------------------------------------*/
static void DrawSunkenRect(HPS hps, const RECTL *prcl)
{
    LONG clrDark  = SYSCLR(SYSCLR_BUTTONDARK);

    Line(hps, prcl->xLeft, prcl->yBottom + 1,
              prcl->xLeft, prcl->yTop    - 1, clrDark);
    Line(hps, prcl->xLeft, prcl->yTop    - 1,
              prcl->xRight - 1, prcl->yTop - 1, clrDark);

    Line(hps, prcl->xLeft,     prcl->yBottom,
              prcl->xRight - 1, prcl->yBottom, EXP_WHITE);
    Line(hps, prcl->xRight - 1, prcl->yBottom,
              prcl->xRight - 1, prcl->yTop - 1, EXP_WHITE);
}

/*------------------------------------------------------------------
  DrawPane - sunken border plus left-aligned, vertically centred text
------------------------------------------------------------------*/
static void DrawPane(HPS hps, RECTL *prcl, const char *psz)
{
    RECTL rcl;

    DrawSunkenRect(hps, prcl);

    rcl           = *prcl;
    rcl.xLeft    += 3;
    rcl.xRight   -= 3;
    rcl.yBottom  += 1;
    rcl.yTop     -= 1;

    if (psz[0] != '\0' && rcl.xRight > rcl.xLeft)
        WinDrawText(hps, (LONG)strlen(psz), (PCH)psz, &rcl,
                    SYSCLR(SYSCLR_WINDOWTEXT), 0L,
                    DT_LEFT | DT_VCENTER);
}

/*------------------------------------------------------------------
  StatusBarWndProc
------------------------------------------------------------------*/
static MRESULT EXPENTRY StatusBarWndProc(HWND hwnd, ULONG msg,
                                         MPARAM mp1, MPARAM mp2)
{
    switch (msg) {

    case WM_ERASEBACKGROUND:
        return (MRESULT)FALSE;

    case WM_PAINT: {
        HPS   hps;
        RECTL rclPaint, rcl, rcPane;
        LONG  xTotal, xFree;

        hps = WinBeginPaint(hwnd, NULLHANDLE, &rclPaint);
        GpiCreateLogColorTable(hps, 0L, LCOLF_RGB, 0L, 0L, (PLONG)NULL);

        WinQueryWindowRect(hwnd, &rcl);
        WinFillRect(hps, &rcl, SYSCLR(SYSCLR_BUTTONMIDDLE));

        /* Raised bar edge along the top of the bar - the HIGH y in PM. */
        Line(hps, rcl.xLeft, rcl.yTop - 1, rcl.xRight, rcl.yTop - 1,
             EXP_WHITE);

        /* The two disk panes are a fixed width and hug the right edge;
           the selection pane fills whatever is left over. */
        xFree  = rcl.xRight - 2 - SB_DISK_W;
        xTotal = xFree - 2 - SB_DISK_W;

        if (xTotal - 2 > rcl.xLeft + 2) {
            rcPane.xLeft   = rcl.xLeft + 2;
            rcPane.yBottom = rcl.yBottom + 2;
            rcPane.xRight  = xTotal - 2;
            rcPane.yTop    = rcl.yTop - 2;
            DrawPane(hps, &rcPane, szPaneSel);
        }

        rcPane.xLeft   = xTotal;
        rcPane.yBottom = rcl.yBottom + 2;
        rcPane.xRight  = xFree - 2;
        rcPane.yTop    = rcl.yTop - 2;
        DrawPane(hps, &rcPane, szPane0);

        rcPane.xLeft   = xFree;
        rcPane.yBottom = rcl.yBottom + 2;
        rcPane.xRight  = rcl.xRight - 2;
        rcPane.yTop    = rcl.yTop - 2;
        DrawPane(hps, &rcPane, szPane1);

        WinEndPaint(hps);
        return (MRESULT)FALSE;
    }
    }

    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

/*------------------------------------------------------------------
  RegisterStatusBarClass
------------------------------------------------------------------*/
BOOL RegisterStatusBarClass(HAB habAnchor)
{
    return WinRegisterClass(habAnchor, (PCSZ)EXP_STATBAR_CLASS,
                            StatusBarWndProc,
                            CS_SIZEREDRAW | CS_CLIPSIBLINGS, 0);
}

/*------------------------------------------------------------------
  CreateStatusBar
------------------------------------------------------------------*/
HWND CreateStatusBar(HWND hwndParent)
{
    hwndStatusBar = WinCreateWindow(hwndParent, (PCSZ)EXP_STATBAR_CLASS,
                                    (PCSZ)"", WS_VISIBLE,
                                    0, 0, 0, 0,
                                    hwndParent, HWND_TOP, 0, NULL, NULL);

    return hwndStatusBar;
}

/*------------------------------------------------------------------
  LayoutStatusBar - along the bottom of the client
------------------------------------------------------------------*/
void LayoutStatusBar(HWND hwndParent)
{
    RECTL rcl;

    if (hwndStatusBar == NULLHANDLE)
        return;

    WinQueryWindowRect(hwndParent, &rcl);

    WinSetWindowPos(hwndStatusBar, HWND_TOP,
                    0, 0, rcl.xRight - rcl.xLeft, SB_HEIGHT,
                    SWP_MOVE | SWP_SIZE | SWP_SHOW);
}

/*------------------------------------------------------------------
  Repaint - invalidate and update, if the bar exists
------------------------------------------------------------------*/
static void Repaint(void)
{
    if (hwndStatusBar != NULLHANDLE) {
        WinInvalidateRect(hwndStatusBar, NULL, TRUE);
        WinUpdateWindow(hwndStatusBar);
    }
}

/*------------------------------------------------------------------
  UpdateStatusBar - query free space for cDriveLetter and repaint

    Fails cleanly when no volume is present.  DosError(FERR_DISABLEHARDERR)
    is set once in main, so an empty floppy returns an error here instead of
    raising a hard-error popup.
------------------------------------------------------------------*/
void UpdateStatusBar(char cDriveLetter)
{
    FSALLOCATE       fsa;
    unsigned __int64 bpu, cbTotal, cbFree;
    char             szSize[40];
    ULONG            ulDrive;

    if (cDriveLetter >= 'a' && cDriveLetter <= 'z')
        cDriveLetter = (char)(cDriveLetter - 'a' + 'A');

    ulDrive = (ULONG)(cDriveLetter - 'A' + 1);      /* 1 = A, 2 = B, ... */

    memset(&fsa, 0, sizeof(fsa));

    if (DosQueryFSInfo(ulDrive, FSIL_ALLOC,
                       (PVOID)&fsa, sizeof(fsa)) == NO_ERROR
        && fsa.cUnit != 0) {

        bpu     = (unsigned __int64)fsa.cSectorUnit *
                  (unsigned __int64)fsa.cbSector;
        cbTotal = (unsigned __int64)fsa.cUnit      * bpu;
        cbFree  = (unsigned __int64)fsa.cUnitAvail * bpu;

        FormatSize(cbTotal, szSize);
        sprintf(szPane0, "Total: %s", szSize);

        FormatSize(cbFree, szSize);
        sprintf(szPane1, "Free: %s", szSize);
    }
    else {
        strcpy(szPane0, "Total: N/A");
        strcpy(szPane1, "Free: N/A");
    }

    Repaint();
}

/*------------------------------------------------------------------
  ClearStatusBar - blank the disk panes without probing anything
------------------------------------------------------------------*/
void ClearStatusBar(void)
{
    szPane0[0] = '\0';
    szPane1[0] = '\0';
    Repaint();
}

/*------------------------------------------------------------------
  UpdateStatusSelection - item count and combined size of the selection

    Repaints only when the text actually changes, so it stays cheap to call
    liberally.
------------------------------------------------------------------*/
void UpdateStatusSelection(void)
{
    FILEITEM        *pFile;
    char             szNew[80];
    char             szSize[40];
    unsigned __int64 cbTotal = 0ui64;
    int              nCount, nSel = 0, i;

    szNew[0] = '\0';

    if (szCurrentPath[0] != '\0') {

        nCount = GetFileCount();

        for (i = 0; i < nCount; i++) {

            if (!abSelectedFiles[i])
                continue;

            nSel++;

            pFile = GetFileItem(i);
            if (pFile != NULL && !pFile->bIsDir)
                cbTotal += (unsigned __int64)pFile->cbFile;
        }

        if (nSel > 0) {
            FormatSize(cbTotal, szSize);
            if (nSel == 1)
                sprintf(szNew, "1 item selected (%s)", szSize);
            else
                sprintf(szNew, "%d items selected (%s)", nSel, szSize);
        }
    }

    if (strcmp(szNew, szPaneSel) == 0)
        return;                         /* nothing changed - no repaint */

    strcpy(szPaneSel, szNew);
    Repaint();
}
