/*===========================================================================
 * TOOLBAR.CPP  -  Toolbar: owner-drawn buttons, path combo, etched separators
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * The toolbar is its own window with eight bitmap buttons and a drive combo.
 * The buttons are drawn by hand so they can carry a bitmap:
 *
 *      BS_OWNERDRAW + WM_DRAWITEM  ->  BS_USERBUTTON + WM_CONTROL / BN_PAINT
 *      DRAWITEMSTRUCT.itemState    ->  USERBUTTON.fsState (BDS_HILITED etc.)
 *      LoadBitmap / BitBlt         ->  GpiLoadBitmap / WinDrawBitmap
 *      forwarding WM_COMMAND up    ->  same, but the click arrives as
 *                                      WM_CONTROL / BN_CLICKED first
 *
 * The one structural change is the combo box.  A PM combobox's window
 * rectangle covers its drop-down list as well as its entry field, so a combo
 * tall enough to drop anything would be clipped away by a 32-pixel toolbar.
 * The combo is therefore a child of the CLIENT, sitting over the toolbar
 * strip, and the toolbar class is registered CS_CLIPSIBLINGS so it does not
 * paint across it.  Being owned by the client also means CBN_LBSELECT goes
 * straight there instead of being relayed.
 *===========================================================================*/
#define INCL_WIN
#define INCL_GPI

#include <os2.h>
#include <string.h>

#include "res/resource.h"
#include "explorer.h"
#include "toolbar.h"

HWND hwndToolbar   = NULLHANDLE;
HWND hwndPathCombo = NULLHANDLE;

/* Button layout constants */
#define BTN_Y       3       /* bottom edge of the buttons in the toolbar */
#define BTN_IC      26      /* icon button dimensions (square)           */
#define COMBO_W     220     /* path combo width                          */
#define COMBO_H     140     /* combo window height: entry + drop-down     */
#define GAP         4       /* gap between controls within a group        */
#define SEP_W       10      /* width reserved for each visual separator   */

typedef struct _TBBTN {
    ULONG   idCmd;
    ULONG   idBmp;
    HWND    hwnd;
    HBITMAP hbm;
} TBBTN;

static TBBTN aBtns[] = {
    { IDM_FILE_BACK,   IDB_TB_BACK,   NULLHANDLE, NULLHANDLE },
    { IDM_EDIT_CUT,    IDB_TB_CUT,    NULLHANDLE, NULLHANDLE },
    { IDM_EDIT_COPY,   IDB_TB_COPY,   NULLHANDLE, NULLHANDLE },
    { IDM_EDIT_PASTE,  IDB_TB_PASTE,  NULLHANDLE, NULLHANDLE },
    { IDM_FILE_RENAME, IDB_TB_RENAME, NULLHANDLE, NULLHANDLE },
    { IDM_FILE_DELETE, IDB_TB_DELETE, NULLHANDLE, NULLHANDLE },
    { IDM_FILE_PROPS,  IDB_TB_PROPS,  NULLHANDLE, NULLHANDLE },
    { IDM_FILE_SEARCH, IDB_TB_FIND,   NULLHANDLE, NULLHANDLE }
};

#define NUM_BTNS  ((int)(sizeof(aBtns) / sizeof(aBtns[0])))

/* Separator x-centres, recorded during WM_CREATE and drawn in WM_PAINT */
#define MAX_SEPS    4
static LONG anSepX[MAX_SEPS];
static int  nSepCount = 0;

#define SYSCLR(c)  WinQuerySysColor(HWND_DESKTOP, (c), 0)
#define EXP_WHITE  0x00FFFFFFL

/*------------------------------------------------------------------
  Line - one GPI line between two points in the current colour
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
  DrawUserButton - paint one toolbar button

    PM hands us the button's own presentation space and its state; the
    rectangle is the button's window rect, so (0,0) is its bottom-left.
------------------------------------------------------------------*/
static void DrawUserButton(PUSERBUTTON pub)
{
    RECTL            rcl;
    BITMAPINFOHEADER bmp;
    POINTL           ptl;
    HPS              hps   = pub->hps;
    HBITMAP          hbm   = NULLHANDLE;
    BOOL             bDown;
    LONG             clrHi, clrSh, w, h, xOff, yOff;
    int              i;

    for (i = 0; i < NUM_BTNS; i++) {
        if (aBtns[i].hwnd == pub->hwnd) {
            hbm = aBtns[i].hbm;
            break;
        }
    }

    WinQueryWindowRect(pub->hwnd, &rcl);
    w = rcl.xRight - rcl.xLeft;
    h = rcl.yTop   - rcl.yBottom;

    bDown = (BOOL)((pub->fsState & BDS_HILITED) ? TRUE : FALSE);

    GpiCreateLogColorTable(hps, 0L, LCOLF_RGB, 0L, 0L, (PLONG)NULL);

    WinFillRect(hps, &rcl, SYSCLR(SYSCLR_BUTTONMIDDLE));

    /* 3-D border.  In PM the top edge is the HIGH y, so the usual
       "top/left highlight, bottom/right shadow" is turned over. */
    clrHi = bDown ? SYSCLR(SYSCLR_BUTTONDARK) : EXP_WHITE;
    clrSh = bDown ? EXP_WHITE : SYSCLR(SYSCLR_BUTTONDARK);

    Line(hps, 0,     1,     0,     h - 1, clrHi);       /* left  */
    Line(hps, 0,     h - 1, w - 1, h - 1, clrHi);       /* top   */
    Line(hps, 0,     0,     w - 1, 0,     clrSh);       /* bottom*/
    Line(hps, w - 1, 0,     w - 1, h - 1, clrSh);       /* right */

    if (hbm != NULLHANDLE) {

        memset(&bmp, 0, sizeof(bmp));
        bmp.cbFix = sizeof(bmp);

        if (GpiQueryBitmapParameters(hbm, &bmp)) {

            xOff = (w - (LONG)bmp.cx) / 2 + (bDown ? 1 : 0);
            yOff = (h - (LONG)bmp.cy) / 2 - (bDown ? 1 : 0);

            ptl.x = xOff;
            ptl.y = yOff;
            WinDrawBitmap(hps, hbm, NULL, &ptl, 0L, 0L, DBM_NORMAL);
        }
    }
}

/*------------------------------------------------------------------
  MakeButton - one BS_USERBUTTON, owned by the toolbar so it draws it
------------------------------------------------------------------*/
static HWND MakeButton(HWND hwnd, ULONG idCmd, LONG x)
{
    return WinCreateWindow(hwnd, WC_BUTTON, (PCSZ)"",
                           WS_VISIBLE | BS_USERBUTTON | BS_NOPOINTERFOCUS,
                           x, BTN_Y, BTN_IC, BTN_IC,
                           hwnd, HWND_TOP, idCmd, NULL, NULL);
}

/*------------------------------------------------------------------
  ToolbarWndProc
------------------------------------------------------------------*/
static MRESULT EXPENTRY ToolbarWndProc(HWND hwnd, ULONG msg,
                                       MPARAM mp1, MPARAM mp2)
{
    switch (msg) {

    case WM_CREATE: {
        HPS  hps;
        LONG x = GAP;
        int  i;

        nSepCount = 0;

        hps = WinGetPS(HWND_DESKTOP);
        if (hps != NULLHANDLE) {
            for (i = 0; i < NUM_BTNS; i++)
                aBtns[i].hbm = GpiLoadBitmap(hps, NULLHANDLE,
                                             aBtns[i].idBmp, 0L, 0L);
            WinReleasePS(hps);
        }

        /* The combo lives in the client (see the file header); its width is
           still reserved here so the buttons line up where they always did. */
        x += COMBO_W + GAP;

        aBtns[0].hwnd = MakeButton(hwnd, IDM_FILE_BACK, x);
        x += BTN_IC + GAP;

        if (nSepCount < MAX_SEPS) anSepX[nSepCount++] = x + SEP_W / 2;
        x += SEP_W;

        aBtns[1].hwnd = MakeButton(hwnd, IDM_EDIT_CUT, x);
        x += BTN_IC + GAP;
        aBtns[2].hwnd = MakeButton(hwnd, IDM_EDIT_COPY, x);
        x += BTN_IC + GAP;
        aBtns[3].hwnd = MakeButton(hwnd, IDM_EDIT_PASTE, x);
        x += BTN_IC + GAP;

        if (nSepCount < MAX_SEPS) anSepX[nSepCount++] = x + SEP_W / 2;
        x += SEP_W;

        aBtns[4].hwnd = MakeButton(hwnd, IDM_FILE_RENAME, x);
        x += BTN_IC + GAP;
        aBtns[5].hwnd = MakeButton(hwnd, IDM_FILE_DELETE, x);
        x += BTN_IC + GAP;

        if (nSepCount < MAX_SEPS) anSepX[nSepCount++] = x + SEP_W / 2;
        x += SEP_W;

        aBtns[6].hwnd = MakeButton(hwnd, IDM_FILE_PROPS, x);
        x += BTN_IC + GAP;

        if (nSepCount < MAX_SEPS) anSepX[nSepCount++] = x + SEP_W / 2;
        x += SEP_W;

        aBtns[7].hwnd = MakeButton(hwnd, IDM_FILE_SEARCH, x);

        return (MRESULT)FALSE;          /* FALSE proceeds; TRUE would abort */
    }

    case WM_CONTROL: {
        USHORT usNotify = SHORT2FROMMP(mp1);

        if (usNotify == BN_PAINT) {
            DrawUserButton((PUSERBUTTON)PVOIDFROMMP(mp2));
            return (MRESULT)FALSE;
        }

        if (usNotify == BN_CLICKED) {
            /* Relay to the main window */
            WinPostMsg(WinQueryWindow(hwnd, QW_PARENT), WM_COMMAND,
                       MPFROMSHORT(SHORT1FROMMP(mp1)),
                       MPFROM2SHORT(CMDSRC_PUSHBUTTON, TRUE));
            return (MRESULT)FALSE;
        }
        break;
    }

    case WM_ERASEBACKGROUND:
        return (MRESULT)FALSE;          /* WM_PAINT fills the whole strip */

    case WM_PAINT: {
        HPS   hps;
        RECTL rclPaint, rcl;
        LONG  yTop, yBot;
        int   i;

        hps = WinBeginPaint(hwnd, NULLHANDLE, &rclPaint);
        GpiCreateLogColorTable(hps, 0L, LCOLF_RGB, 0L, 0L, (PLONG)NULL);

        WinQueryWindowRect(hwnd, &rcl);
        WinFillRect(hps, &rcl, SYSCLR(SYSCLR_BUTTONMIDDLE));

        yBot = BTN_Y;
        yTop = rcl.yTop - BTN_Y;

        for (i = 0; i < nSepCount; i++) {
            Line(hps, anSepX[i],     yBot, anSepX[i],     yTop,
                 SYSCLR(SYSCLR_BUTTONDARK));
            Line(hps, anSepX[i] + 1, yBot, anSepX[i] + 1, yTop,
                 EXP_WHITE);
        }

        WinEndPaint(hps);
        return (MRESULT)FALSE;
    }

    case WM_DESTROY: {
        int i;

        for (i = 0; i < NUM_BTNS; i++) {
            if (aBtns[i].hbm != NULLHANDLE) {
                GpiDeleteBitmap(aBtns[i].hbm);
                aBtns[i].hbm = NULLHANDLE;
            }
            aBtns[i].hwnd = NULLHANDLE;
        }
        break;
    }
    }

    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

/*------------------------------------------------------------------
  RegisterToolbarClass
------------------------------------------------------------------*/
BOOL RegisterToolbarClass(HAB habAnchor)
{
    return WinRegisterClass(habAnchor, (PCSZ)EXP_TOOLBAR_CLASS,
                            ToolbarWndProc,
                            CS_SIZEREDRAW | CS_CLIPSIBLINGS, 0);
}

/*------------------------------------------------------------------
  CreateToolbar - the toolbar strip plus the drive combo above it
------------------------------------------------------------------*/
HWND CreateToolbar(HWND hwndParent)
{
    hwndToolbar = WinCreateWindow(hwndParent, (PCSZ)EXP_TOOLBAR_CLASS,
                                  (PCSZ)"", WS_VISIBLE | WS_CLIPSIBLINGS,
                                  0, 0, 0, 0,
                                  hwndParent, HWND_BOTTOM, 0, NULL, NULL);

    /* Child AND owner is the client, so CBN_LBSELECT needs no relaying.
       HWND_TOP puts it above the toolbar, which clips itself around it. */
    hwndPathCombo = WinCreateWindow(hwndParent, WC_COMBOBOX, (PCSZ)"",
                                    WS_VISIBLE | CBS_DROPDOWNLIST,
                                    0, 0, 0, 0,
                                    hwndParent, HWND_TOP,
                                    IDC_TB_PATHCOMBO, NULL, NULL);

    return hwndToolbar;
}

/*------------------------------------------------------------------
  LayoutToolbar - place the strip and the combo after a resize
------------------------------------------------------------------*/
void LayoutToolbar(HWND hwndParent)
{
    RECTL rcl;
    LONG  cx, cy;

    WinQueryWindowRect(hwndParent, &rcl);
    cx = rcl.xRight - rcl.xLeft;
    cy = rcl.yTop   - rcl.yBottom;

    if (hwndToolbar != NULLHANDLE)
        WinSetWindowPos(hwndToolbar, HWND_BOTTOM,
                        0, cy - TB_HEIGHT, cx, TB_HEIGHT,
                        SWP_MOVE | SWP_SIZE | SWP_SHOW | SWP_ZORDER);

    /* Positioned by its TOP: the rectangle holds the drop-down list too */
    if (hwndPathCombo != NULLHANDLE)
        WinSetWindowPos(hwndPathCombo, HWND_TOP,
                        GAP, cy - (BTN_Y - 1) - COMBO_H, COMBO_W, COMBO_H,
                        SWP_MOVE | SWP_SIZE | SWP_SHOW | SWP_ZORDER);
}
