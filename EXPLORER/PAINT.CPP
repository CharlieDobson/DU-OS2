/*===========================================================================
 * PAINT.CPP  -  Drawing, hit-testing, scroll: drives/files views, all modes
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 *   LoadBitmap / GetObject       ->  GpiLoadBitmap / GpiQueryBitmapParameters
 *   BitBlt from a memory DC      ->  WinDrawBitmap
 *   DrawIcon                     ->  WinDrawPointer
 *   FillRect(CreateSolidBrush)   ->  WinFillRect (takes a colour, not a brush)
 *   TextOut / ExtTextOut         ->  WinDrawText
 *   DrawText(DT_CENTER|WORDBREAK)->  WinDrawText in a loop (see DrawLabel)
 *   DrawFocusRect                ->  GpiBox with LINETYPE_ALTERNATE, FM_INVERT
 *   GetSysColor(COLOR_*)         ->  WinQuerySysColor(SYSCLR_*)
 *   SetScrollRange / SetScrollPos->  SBM_SETSCROLLBAR / SBM_SETTHUMBSIZE
 *
 * ViewGeom computes the cell arithmetic ONCE and everything else -- content
 * sizing, column counts, drawing, hit-testing -- asks it.  Keep it that way:
 * separate copies drift, and a click starts landing on the wrong item.
 *===========================================================================*/
#define INCL_WIN
#define INCL_GPI
#define INCL_DOSERRORS

#include <os2.h>
#include <string.h>
#include <stdio.h>

#include "res/resource.h"
#include "explorer.h"
#include "toolbar.h"
#include "statbar.h"
#include "drives.h"
#include "dirview.h"
#include "paint.h"

#define VIEW_BMP_COUNT  6

/* Details-view column widths - shared by DrawFileDetails and CalcContentSize */
#define DTL_NAME_W  200
#define DTL_DATE_W  100
#define DTL_SIZE_W   80
#define DTL_ATTR_W   70
#define DTL_NAME_PAD 12     /* gap between the widest name and the Date column */

/* The content area is inset by these before any cell is placed */
#define CONTENT_PAD  4

/* Character widths of the font being painted with, from GpiQueryWidthTable.
   Labels and the Details Name column are measured from this rather than
   with a GpiQueryTextBox per character. */
static LONG alCharW[256];

static void LoadCharWidths(HPS hps, LONG *alW)
{
    int i;

    if (!GpiQueryWidthTable(hps, 0L, 256L, alW)) {
        for (i = 0; i < 256; i++)
            alW[i] = 7;                 /* a guess beats a zero width */
    }
}

static LONG TextWidth(const LONG *alW, const char *s, int n)
{
    LONG w = 0;

    while (n-- > 0)
        w += alW[(unsigned char)*s++];
    return w;
}

/*------------------------------------------------------------------
  DetailWidestName - pixel width of the longest name in the list,
    measured once each time the directory is re-read

    Measured on a desktop PS: it carries the same system default font as the
    memory PS PaintContent draws into, and it is available outside a paint,
    which CalcContentSize needs.
------------------------------------------------------------------*/
static BOOL  bDtlMeasured = FALSE;
static ULONG ulDtlGen     = 0;
static LONG  lDtlWidest   = 0;

static LONG DetailWidestName(void)
{
    LONG alW[256];
    HPS  hps;
    int  i, n;

    if (bDtlMeasured && ulDtlGen == GetDirListGen())
        return lDtlWidest;

    hps = WinGetPS(HWND_DESKTOP);
    if (hps == NULLHANDLE)
        return 0;
    LoadCharWidths(hps, alW);
    WinReleasePS(hps);

    lDtlWidest = 0;
    n = GetFileCount();
    for (i = 0; i < n; i++) {
        FILEITEM *pFile = GetFileItem(i);
        LONG      w;

        if (pFile == NULL)
            continue;
        w = TextWidth(alW, pFile->szName, (int)strlen(pFile->szName));
        if (w > lDtlWidest)
            lDtlWidest = w;
    }

    ulDtlGen     = GetDirListGen();
    bDtlMeasured = TRUE;
    return lDtlWidest;
}

HWND hwndVScroll = NULLHANDLE;
HWND hwndHScroll = NULLHANDLE;

/* 0=FILE 1=FOLDER 2=DRVLOCAL 3=DRVNET 4=DRVFLOP 5=DRVCDROM */
static HBITMAP ahViewBmps[VIEW_BMP_COUNT];

static LONG anBmpSm[VIEW_BMP_COUNT] = {
    IDB_SM_FILE, IDB_SM_FOLDER, IDB_SM_DRVLOCAL,
    IDB_SM_DRVNET, IDB_SM_DRVFLOP, IDB_SM_DRVCDROM
};
static LONG anBmpMd[VIEW_BMP_COUNT] = {
    IDB_MD_FILE, IDB_MD_FOLDER, IDB_MD_DRVLOCAL,
    IDB_MD_DRVNET, IDB_MD_DRVFLOP, IDB_MD_DRVCDROM
};
static LONG anBmpLg[VIEW_BMP_COUNT] = {
    IDB_LG_FILE, IDB_LG_FOLDER, IDB_LG_DRVLOCAL,
    IDB_LG_DRVNET, IDB_LG_DRVFLOP, IDB_LG_DRVCDROM
};

#define SYSCLR(c)  WinQuerySysColor(HWND_DESKTOP, (c), 0)

/*==================================================================
 * Bitmaps
 *==================================================================*/

void FreeViewBitmaps(void)
{
    int i;

    for (i = 0; i < VIEW_BMP_COUNT; i++) {
        if (ahViewBmps[i] != NULLHANDLE) {
            GpiDeleteBitmap(ahViewBmps[i]);
            ahViewBmps[i] = NULLHANDLE;
        }
    }
}

/*------------------------------------------------------------------
  LoadViewBitmaps - load the bitmap set for the given view mode

    GpiLoadBitmap needs a presentation space to know what device format to
    build for; a cached desktop PS answers that and is released again at
    once.  The bitmaps themselves belong to the process, not to that PS.
------------------------------------------------------------------*/
void LoadViewBitmaps(int nView)
{
    HPS   hps;
    LONG *pTable;
    int   i;

    FreeViewBitmaps();

    if (nView == IDM_VIEW_SMALL || nView == IDM_VIEW_DETAILS)
        pTable = anBmpSm;
    else if (nView == IDM_VIEW_LARGE)
        pTable = anBmpLg;
    else
        pTable = anBmpMd;               /* IDM_VIEW_MEDIUM (default) */

    hps = WinGetPS(HWND_DESKTOP);
    if (hps == NULLHANDLE)
        return;

    for (i = 0; i < VIEW_BMP_COUNT; i++)
        ahViewBmps[i] = GpiLoadBitmap(hps, NULLHANDLE,
                                      (ULONG)pTable[i], 0L, 0L);

    WinReleasePS(hps);
}

/*------------------------------------------------------------------
  BitmapSize - dimensions of a loaded view bitmap
------------------------------------------------------------------*/
static BOOL BitmapSize(HBITMAP hbm, LONG *pcx, LONG *pcy)
{
    BITMAPINFOHEADER bmp;

    *pcx = 0;
    *pcy = 0;

    if (hbm == NULLHANDLE)
        return FALSE;

    memset(&bmp, 0, sizeof(bmp));
    bmp.cbFix = sizeof(bmp);

    if (!GpiQueryBitmapParameters(hbm, &bmp))
        return FALSE;

    *pcx = (LONG)bmp.cx;
    *pcy = (LONG)bmp.cy;
    return TRUE;
}

/*==================================================================
 * Geometry - one definition of the cell arithmetic
 *==================================================================*/

typedef struct _VIEWGEOM {
    LONG bmW, bmH;      /* reference bitmap size                        */
    LONG cellW, cellH;  /* icon-grid cell                               */
    LONG rowH;          /* details row height                           */
    LONG hdrH;          /* details header height (0 in the drives view) */
    LONG nameW;         /* details Name column width (files view)       */
    int  cols;          /* columns in the icon grid, 1 in details       */
    int  count;         /* number of items to lay out                   */
} VIEWGEOM;

/*------------------------------------------------------------------
  ViewGeom - fill in the layout for the current view and location

    cxVisible is the width of the content area.  Returns FALSE when there is
    nothing to lay out (no reference bitmap loaded yet, say), in which case
    every caller simply does nothing.
------------------------------------------------------------------*/
static BOOL ViewGeom(LONG cxVisible, VIEWGEOM *pg)
{
    BOOL    bDrives = (BOOL)(szCurrentPath[0] == '\0');
    HBITMAP hRef;

    memset(pg, 0, sizeof(*pg));

    /* Drives view sizes on the local-drive bitmap, files view on the folder */
    hRef = bDrives ? ahViewBmps[2] : ahViewBmps[1];

    if (!BitmapSize(hRef, &pg->bmW, &pg->bmH))
        return FALSE;

    pg->count = bDrives ? GetDriveCount() : GetFileCount();

    if (nCurrentView == IDM_VIEW_DETAILS) {
        pg->rowH  = pg->bmH + 4;
        pg->hdrH  = bDrives ? 0 : pg->rowH;   /* drives details has no header */
        pg->cellW = cxVisible;
        pg->cellH = pg->rowH;
        pg->cols  = 1;

        /* The Name column is wide enough for the longest name, but grows
           only into room the pane already has, so a long name never pushes
           the other columns off the side; never narrower than DTL_NAME_W.
           A name that still does not fit ends in "..." when drawn. */
        if (!bDrives) {
            LONG lRoom = cxVisible - (CONTENT_PAD + pg->bmW + 4 + DTL_DATE_W
                                      + DTL_SIZE_W + DTL_ATTR_W + CONTENT_PAD);

            pg->nameW = DetailWidestName() + DTL_NAME_PAD;
            if (pg->nameW > lRoom)      pg->nameW = lRoom;
            if (pg->nameW < DTL_NAME_W) pg->nameW = DTL_NAME_W;
        }
    }
    else {
        pg->cellW = pg->bmW + 34;
        if (bDrives) {
            if (pg->cellW < 82) pg->cellW = 82;
        }
        else {
            if (pg->cellW < 90) pg->cellW = 90;  /* room for a whole name */
        }
        pg->cellH = pg->bmH + 30;                /* bitmap + 2-line label */
        pg->cols  = (int)(cxVisible / pg->cellW);
        if (pg->cols < 1) pg->cols = 1;
    }

    return TRUE;
}

/*==================================================================
 * The coordinate flip
 *==================================================================*/

/*------------------------------------------------------------------
  ContentRect - the client area less toolbar, status bar and scrollbars
------------------------------------------------------------------*/
void ContentRect(HWND hwndCli, PRECTL prcl)
{
    LONG sbw = WinQuerySysValue(HWND_DESKTOP, SV_CXVSCROLL);
    LONG sbh = WinQuerySysValue(HWND_DESKTOP, SV_CYHSCROLL);

    if (sbw <= 0) sbw = 16;
    if (sbh <= 0) sbh = 16;

    WinQueryWindowRect(hwndCli, prcl);

    prcl->yTop    -= TB_HEIGHT;
    prcl->yBottom += SB_HEIGHT + sbh;
    prcl->xRight  -= sbw;

    if (prcl->yTop   < prcl->yBottom) prcl->yTop   = prcl->yBottom;
    if (prcl->xRight < prcl->xLeft)   prcl->xRight = prcl->xLeft;
}

/*------------------------------------------------------------------
  CellRect - logical box (top-down, unscrolled) -> PM rectangle
------------------------------------------------------------------*/
void CellRect(const RECTL *pcont, LONG lx, LONG ly, LONG cw, LONG ch,
              PRECTL prclOut)
{
    prclOut->xLeft   = pcont->xLeft + lx - nScrollX;
    prclOut->xRight  = prclOut->xLeft + cw;
    prclOut->yTop    = pcont->yTop - (ly - nScrollY);
    prclOut->yBottom = prclOut->yTop - ch;
}

/*------------------------------------------------------------------
  PointToLogical - PM point -> content-logical point
------------------------------------------------------------------*/
void PointToLogical(const RECTL *pcont, LONG x, LONG y, LONG *plx, LONG *ply)
{
    *plx = x - pcont->xLeft + nScrollX;
    *ply = (pcont->yTop - y) + nScrollY;
}

/*==================================================================
 * Sizes, columns, scrolling
 *==================================================================*/

LONG GetViewCellHeight(void)
{
    LONG bmW, bmH;

    if (!BitmapSize(ahViewBmps[1], &bmW, &bmH))
        return 40;                      /* safe fallback */

    if (nCurrentView == IDM_VIEW_DETAILS)
        return bmH + 4;

    return bmH + 30;
}

int GetViewColumns(HWND hwndCli)
{
    RECTL    rcl;
    VIEWGEOM g;

    ContentRect(hwndCli, &rcl);

    if (!ViewGeom(rcl.xRight - rcl.xLeft, &g))
        return 1;

    return g.cols;
}

/*------------------------------------------------------------------
  CalcContentSize - total logical size of the current content
------------------------------------------------------------------*/
void CalcContentSize(LONG cxVisible, LONG *pcx, LONG *pcy)
{
    VIEWGEOM g;
    LONG     rows;

    *pcx = cxVisible;
    *pcy = 0;

    if (!ViewGeom(cxVisible, &g) || g.count == 0)
        return;

    if (nCurrentView == IDM_VIEW_DETAILS) {
        *pcy = g.hdrH + (LONG)g.count * g.rowH + 8;

        if (szCurrentPath[0] != '\0') {
            *pcx = CONTENT_PAD + g.bmW + 4 + g.nameW + DTL_DATE_W
                 + DTL_SIZE_W + DTL_ATTR_W + CONTENT_PAD;
        }
    }
    else {
        rows = ((LONG)g.count + g.cols - 1) / g.cols;
        *pcy = rows * g.cellH + 8;
    }

    if (*pcx < cxVisible)
        *pcx = cxVisible;
}

/*------------------------------------------------------------------
  Scroll scale factors.

    !! PM LIMIT !!  A PM scrollbar carries its position, range and thumb size
    as SHORTs, so it cannot describe a content area taller than 32767 pixels.
    With MAX_FILES at 2048 a details list is already 40000 pixels tall and a
    one-column large-icon view is far worse, so the range is SCALED.

    So the scrollbar counts in units of these scale factors rather than in
    pixels.  Everything else -- the scroll offsets, the layout, the hit
    tester -- stays in pixels; only the two conversions at the scrollbar
    boundary know about the scaling, and they are these two functions.
------------------------------------------------------------------*/
#define SB_UNITS_MAX  30000L

static LONG sbScaleY = 1;
static LONG sbScaleX = 1;

LONG ScrollScaleY(void) { return sbScaleY; }
LONG ScrollScaleX(void) { return sbScaleX; }

static LONG ScaleFor(LONG nRange)
{
    return (nRange <= SB_UNITS_MAX) ? 1L : ((nRange / SB_UNITS_MAX) + 1L);
}

/*------------------------------------------------------------------
  UpdateScrollBars - recompute the two scrollbars' ranges and thumbs

    A PM scrollbar with a zero range DISABLES itself rather than hiding, which
    keeps the content area a fixed size instead of reflowing every time a
    directory happens to fit.
------------------------------------------------------------------*/
void UpdateScrollBars(HWND hwndCli)
{
    RECTL rcl;
    LONG  cxVisible, cyVisible, cxContent, cyContent, nMaxX, nMaxY;

    ContentRect(hwndCli, &rcl);

    cxVisible = rcl.xRight - rcl.xLeft;
    cyVisible = rcl.yTop   - rcl.yBottom;
    if (cxVisible < 1) cxVisible = 1;
    if (cyVisible < 1) cyVisible = 1;

    CalcContentSize(cxVisible, &cxContent, &cyContent);

    nMaxY = cyContent - cyVisible;
    nMaxX = cxContent - cxVisible;
    if (nMaxY < 0) nMaxY = 0;
    if (nMaxX < 0) nMaxX = 0;

    if (nScrollY > nMaxY) nScrollY = nMaxY;
    if (nScrollX > nMaxX) nScrollX = nMaxX;

    sbScaleY = ScaleFor(cyContent);
    sbScaleX = ScaleFor(cxContent);

    if (hwndVScroll != NULLHANDLE) {
        WinSendMsg(hwndVScroll, SBM_SETSCROLLBAR,
                   MPFROMSHORT((SHORT)(nScrollY / sbScaleY)),
                   MPFROM2SHORT(0, (SHORT)(nMaxY / sbScaleY)));
        WinSendMsg(hwndVScroll, SBM_SETTHUMBSIZE,
                   MPFROM2SHORT((SHORT)(cyVisible / sbScaleY),
                                (SHORT)((cyContent > 0 ? cyContent : 1)
                                        / sbScaleY)),
                   MPVOID);
    }

    if (hwndHScroll != NULLHANDLE) {
        WinSendMsg(hwndHScroll, SBM_SETSCROLLBAR,
                   MPFROMSHORT((SHORT)(nScrollX / sbScaleX)),
                   MPFROM2SHORT(0, (SHORT)(nMaxX / sbScaleX)));
        WinSendMsg(hwndHScroll, SBM_SETTHUMBSIZE,
                   MPFROM2SHORT((SHORT)(cxVisible / sbScaleX),
                                (SHORT)((cxContent > 0 ? cxContent : 1)
                                        / sbScaleX)),
                   MPVOID);
    }
}

/*------------------------------------------------------------------
  EnsureItemVisible - scroll vertically so item nIdx is fully shown
------------------------------------------------------------------*/
void EnsureItemVisible(HWND hwndCli, int nIdx)
{
    RECTL    rcl;
    VIEWGEOM g;
    LONG     cyVisible, yTop, yBot, cxContent, cyContent;

    if (nIdx < 0)
        return;

    ContentRect(hwndCli, &rcl);
    cyVisible = rcl.yTop - rcl.yBottom;
    if (cyVisible < 1) cyVisible = 1;

    if (!ViewGeom(rcl.xRight - rcl.xLeft, &g))
        return;

    if (nCurrentView == IDM_VIEW_DETAILS) {
        yTop = g.hdrH + (LONG)nIdx * g.rowH;
        yBot = yTop + g.rowH;
    }
    else {
        yTop = CONTENT_PAD + (LONG)(nIdx / g.cols) * g.cellH;
        yBot = yTop + g.cellH;
    }

    if (yTop < nScrollY)
        nScrollY = yTop;
    else if (yBot > nScrollY + cyVisible)
        nScrollY = yBot - cyVisible;

    CalcContentSize(rcl.xRight - rcl.xLeft, &cxContent, &cyContent);
    if (nScrollY > cyContent - cyVisible)
        nScrollY = cyContent - cyVisible;
    if (nScrollY < 0)
        nScrollY = 0;

    if (hwndVScroll != NULLHANDLE)
        WinSendMsg(hwndVScroll, SBM_SETPOS,
                   MPFROMSHORT((SHORT)(nScrollY / sbScaleY)), MPVOID);
}

/*==================================================================
 * Small drawing helpers
 *==================================================================*/

/*------------------------------------------------------------------
  DrawFocusFrame - the PM equivalent of DrawFocusRect

    An alternating (dotted) line drawn in invert mix, so a second call over
    the same rectangle rubs it out again.  GpiBox takes INCLUSIVE corners,
    unlike RECTL, hence the -1s.
------------------------------------------------------------------*/
static void DrawFocusFrame(HPS hps, const RECTL *prcl)
{
    POINTL ptl;

    GpiSetMix(hps, FM_INVERT);
    GpiSetLineType(hps, LINETYPE_ALTERNATE);

    ptl.x = prcl->xLeft;
    ptl.y = prcl->yBottom;
    GpiMove(hps, &ptl);

    ptl.x = prcl->xRight - 1;
    ptl.y = prcl->yTop   - 1;
    GpiBox(hps, DRO_OUTLINE, &ptl, 0L, 0L);

    GpiSetLineType(hps, LINETYPE_DEFAULT);
    GpiSetMix(hps, FM_DEFAULT);
}

/*------------------------------------------------------------------
  Icon labels.

    A long HPFS name will not fit the two text lines under an icon, and
    WinDrawText's DT_WORDBREAK only ever breaks at spaces, so labels are
    laid out here: broken at a space where there is one and mid-word where
    there is not, as many lines as fit, the last ending in "..." when there
    is more.  The focused item shows its WHOLE name, drawn over the row
    beneath after everything else is painted - PaintContent redraws the
    whole pane into a memory bitmap every time, so nothing is left behind
    when the focus moves on.

    Widths come from alCharW, loaded once per paint.
------------------------------------------------------------------*/
#define LBL_MAX_LINES   128
#define LBL_DOTS        "..."
#define LBL_MARGIN      4       /* label width is the cell less this */

typedef struct _LBLLINE {
    int nStart;
    int nLen;
} LBLLINE;

static LONG LabelLineHeight(HPS hps)
{
    FONTMETRICS fm;
    LONG        lineH;

    memset(&fm, 0, sizeof(fm));
    GpiQueryFontMetrics(hps, sizeof(fm), &fm);

    lineH = fm.lMaxBaselineExt + fm.lExternalLeading;
    return (lineH < 1) ? 12 : lineH;
}

/* LayoutLabel - break psz into at most nMaxLines lines no wider than
   cxMax.  Returns the line count; *pbMore says whether text was left. */
static int LayoutLabel(const char *psz, LONG cxMax, int nMaxLines,
                       LBLLINE *aLines, BOOL *pbMore)
{
    int nLen   = (int)strlen(psz);
    int p      = 0;
    int nLines = 0;

    while (p < nLen && nLines < nMaxLines) {
        int  j;
        LONG w      = 0;
        int  nSpace = -1;

        /* A wrapped line does not start with the space it wrapped at */
        while (nLines > 0 && p < nLen && psz[p] == ' ')
            p++;
        if (p >= nLen)
            break;

        for (j = p; j < nLen; j++) {
            LONG cw = alCharW[(unsigned char)psz[j]];

            if (w + cw > cxMax && j > p)
                break;
            if (psz[j] == ' ')
                nSpace = j;
            w += cw;
        }

        aLines[nLines].nStart = p;
        if (j < nLen && nSpace > p) {
            aLines[nLines].nLen = nSpace - p;      /* break at the space */
            p = nSpace + 1;
        }
        else {
            aLines[nLines].nLen = j - p;           /* all of it, or mid-word */
            p = j;
        }
        nLines++;
    }

    while (p < nLen && psz[p] == ' ')
        p++;
    *pbMore = (BOOL)(p < nLen);
    return nLines;
}

/* EllipsizeLast - refill the last line from the rest of the name, as much
   as fits with "..." after it */
static void EllipsizeLast(const char *psz, LBLLINE *pLine, LONG cxMax)
{
    LONG wDots  = TextWidth(alCharW, LBL_DOTS, 3);
    int  nAvail = (int)strlen(psz) - pLine->nStart;
    int  n      = 0;
    LONG w      = 0;

    while (n < nAvail &&
           w + alCharW[(unsigned char)psz[pLine->nStart + n]] + wDots <= cxMax) {
        w += alCharW[(unsigned char)psz[pLine->nStart + n]];
        n++;
    }
    while (n > 0 && psz[pLine->nStart + n - 1] == ' ')
        n--;
    pLine->nLen = n;
}

/* DrawLabelLines - each line centred in the box's width, from yTop down,
   the last with "..." if bDots */
static void DrawLabelLines(HPS hps, const RECTL *prcl, const char *psz,
                           const LBLLINE *aLines, int nLines, BOOL bDots,
                           LONG lineH, LONG clrFore)
{
    char  szLine[CCHMAXPATHCOMP + sizeof(LBL_DOTS)];
    RECTL rcl = *prcl;
    int   k, n;

    for (k = 0; k < nLines; k++) {
        n = aLines[k].nLen;
        if (n > CCHMAXPATHCOMP - 1)
            n = CCHMAXPATHCOMP - 1;
        memcpy(szLine, psz + aLines[k].nStart, n);
        szLine[n] = '\0';
        if (bDots && k == nLines - 1) {
            strcat(szLine, LBL_DOTS);
            n += 3;
        }

        rcl.yTop    = prcl->yTop - (LONG)k * lineH;
        rcl.yBottom = rcl.yTop - lineH;
        WinDrawText(hps, (LONG)n, (PCH)szLine, &rcl, clrFore, 0L,
                    DT_CENTER | DT_TOP);
    }
}

/*------------------------------------------------------------------
  DrawLabel - centred item caption: as many lines as fit the box, up to
    nMaxLines, the last ending in "..." if the name goes on
------------------------------------------------------------------*/
static void DrawLabel(HPS hps, const RECTL *prcl, const char *psz,
                      LONG clrFore, int nMaxLines)
{
    LBLLINE aLines[LBL_MAX_LINES];
    LONG    lineH = LabelLineHeight(hps);
    LONG    cxMax = (prcl->xRight - prcl->xLeft) - LBL_MARGIN;
    int     nFit  = (int)((prcl->yTop - prcl->yBottom) / lineH);
    int     nLines;
    BOOL    bMore;

    if (nFit > nMaxLines) nFit = nMaxLines;
    if (nFit < 1)         nFit = 1;
    if (cxMax < 1)        cxMax = 1;

    nLines = LayoutLabel(psz, cxMax, nFit, aLines, &bMore);
    if (bMore && nLines > 0)
        EllipsizeLast(psz, &aLines[nLines - 1], cxMax);

    DrawLabelLines(hps, prcl, psz, aLines, nLines, bMore, lineH, clrFore);
}

/*------------------------------------------------------------------
  DrawFullLabel - the focused item's whole name, starting at the top of
    its label box and running down past the cell as far as it needs, on
    a highlight band; returns the band so a focus frame can take it in
------------------------------------------------------------------*/
static void DrawFullLabel(HPS hps, const RECTL *prcl, const char *psz,
                          LONG clrFore, LONG clrBack, PRECTL prclBand)
{
    LBLLINE aLines[LBL_MAX_LINES];
    LONG    lineH = LabelLineHeight(hps);
    LONG    cxMax = (prcl->xRight - prcl->xLeft) - LBL_MARGIN;
    int     nLines;
    BOOL    bMore;

    if (cxMax < 1)
        cxMax = 1;

    nLines = LayoutLabel(psz, cxMax, LBL_MAX_LINES, aLines, &bMore);
    if (bMore && nLines > 0)
        EllipsizeLast(psz, &aLines[nLines - 1], cxMax);

    *prclBand = *prcl;
    if (prclBand->yTop - (LONG)nLines * lineH - 2 < prclBand->yBottom)
        prclBand->yBottom = prclBand->yTop - (LONG)nLines * lineH - 2;

    WinFillRect(hps, prclBand, clrBack);
    DrawLabelLines(hps, prcl, psz, aLines, nLines, bMore, lineH, clrFore);
}

/*------------------------------------------------------------------
  DrawCellText - single line, vertically centred, clipped to the cell

    Replaces ExtTextOut(ETO_CLIPPED): WinDrawText clips to the rectangle it
    is given, so the explicit clip rectangle is not needed.
------------------------------------------------------------------*/
static void DrawCellText(HPS hps, const RECTL *prcl, const char *psz,
                         LONG clrFore)
{
    RECTL rcl = *prcl;

    if (psz[0] == '\0')
        return;

    WinDrawText(hps, (LONG)strlen(psz), (PCH)psz, &rcl,
                clrFore, 0L, DT_LEFT | DT_VCENTER);
}

/*------------------------------------------------------------------
  BlitBitmap - put a view bitmap at the top-left of a logical box
------------------------------------------------------------------*/
static void BlitBitmap(HPS hps, HBITMAP hbm, LONG x, LONG yTop,
                       LONG bmW, LONG bmH)
{
    POINTL ptl;

    if (hbm == NULLHANDLE)
        return;

    (void)bmW;

    ptl.x = x;
    ptl.y = yTop - bmH;         /* WinDrawBitmap places the BOTTOM-left */

    WinDrawBitmap(hps, hbm, NULL, &ptl, 0L, 0L, DBM_NORMAL);
}

/*------------------------------------------------------------------
  DriveTypeToBmpIndex - map a DRV_* kind to an ahViewBmps[] slot
------------------------------------------------------------------*/
static int DriveTypeToBmpIndex(int nType)
{
    switch (nType) {
    case DRV_REMOVABLE: return 4;   /* DRVFLOP  */
    case DRV_REMOTE:    return 3;   /* DRVNET   */
    case DRV_CDROM:     return 5;   /* DRVCDROM */
    default:            return 2;   /* DRVLOCAL */
    }
}

/*------------------------------------------------------------------
  FormatAttr - a DOS/OS2 attribute word as the letters "RHSA"
------------------------------------------------------------------*/
static void FormatAttr(ULONG attr, char *pszOut)
{
    pszOut[0] = (char)((attr & FILE_READONLY) ? 'R' : '-');
    pszOut[1] = (char)((attr & FILE_HIDDEN)   ? 'H' : '-');
    pszOut[2] = (char)((attr & FILE_SYSTEM)   ? 'S' : '-');
    pszOut[3] = (char)((attr & FILE_ARCHIVED) ? 'A' : '-');
    pszOut[4] = '\0';
}

/*------------------------------------------------------------------
  FirstVisibleRow - first grid row that can appear on screen
------------------------------------------------------------------*/
static LONG FirstVisibleRow(LONG cellH)
{
    LONG row;

    if (cellH < 1)
        return 0;

    row = (nScrollY - CONTENT_PAD) / cellH;
    return (row < 0) ? 0 : row;
}

/*==================================================================
 * Hit-testing
 *==================================================================*/

/*------------------------------------------------------------------
  HitTestGrid - shared body of HitTestDrive and HitTestFile

    Both views lay out identically once ViewGeom has spoken, so there is one
    hit-tester for both.
------------------------------------------------------------------*/
static int HitTestGrid(HWND hwndCli, LONG x, LONG y)
{
    RECTL    rcl;
    VIEWGEOM g;
    LONG     lx, ly, col, row;
    int      idx;

    ContentRect(hwndCli, &rcl);

    /* Reject anything outside the visible content area */
    if (x < rcl.xLeft || x >= rcl.xRight ||
        y < rcl.yBottom || y >= rcl.yTop)
        return -1;

    if (!ViewGeom(rcl.xRight - rcl.xLeft, &g) || g.count == 0)
        return -1;

    PointToLogical(&rcl, x, y, &lx, &ly);

    if (nCurrentView == IDM_VIEW_DETAILS) {
        if (ly < g.hdrH)
            return -1;                  /* the header row */
        idx = (int)((ly - g.hdrH) / g.rowH);
        return (idx >= 0 && idx < g.count) ? idx : -1;
    }

    lx -= CONTENT_PAD;
    ly -= CONTENT_PAD;
    if (lx < 0 || ly < 0)
        return -1;

    col = lx / g.cellW;
    row = ly / g.cellH;
    if (col >= g.cols)
        return -1;

    idx = (int)(row * g.cols + col);
    return (idx >= 0 && idx < g.count) ? idx : -1;
}

int HitTestDrive(HWND hwndCli, LONG x, LONG y)
{
    if (szCurrentPath[0] != '\0')
        return -1;

    return HitTestGrid(hwndCli, x, y);
}

int HitTestFile(HWND hwndCli, LONG x, LONG y)
{
    if (szCurrentPath[0] == '\0')
        return -1;

    return HitTestGrid(hwndCli, x, y);
}

/*==================================================================
 * Drawing
 *==================================================================*/

/*------------------------------------------------------------------
  DrawDrives - paint the drive icons
------------------------------------------------------------------*/
void DrawDrives(HPS hps, const RECTL *pcont)
{
    VIEWGEOM   g;
    DRIVEINFO *pDrv;
    RECTL      rcCell, rcLabel;
    LONG       cxVisible, lx, ly, firstRow;
    LONG       clrText;
    int        i, col;
    BOOL       bSel;

    cxVisible = pcont->xRight - pcont->xLeft;

    WinFillRect(hps, (PRECTL)pcont, SYSCLR(SYSCLR_WINDOW));

    if (!ViewGeom(cxVisible, &g) || g.count == 0)
        return;

    LoadCharWidths(hps, alCharW);

    col = 0;

    if (nCurrentView == IDM_VIEW_DETAILS) {
        /* At most 26 rows, and no header in this view: start at the top and
           let the y test below drop whatever has scrolled off. */
        i  = 0;
        lx = 0;
        ly = 0;
    }
    else {
        firstRow = FirstVisibleRow(g.cellH);
        i        = (int)(firstRow * g.cols);
        lx       = CONTENT_PAD;
        ly       = CONTENT_PAD + firstRow * g.cellH;
    }

    for (; i < g.count; i++) {

        pDrv = GetDriveInfo(i);
        if (pDrv == NULL)
            continue;

        bSel    = (BOOL)(i == nSelectedDrive);
        clrText = bSel ? SYSCLR(SYSCLR_HILITEFOREGROUND)
                       : SYSCLR(SYSCLR_WINDOWTEXT);

        CellRect(pcont, lx, ly, g.cellW, g.cellH, &rcCell);

        if (rcCell.yTop < pcont->yBottom)
            break;                      /* scrolled past the bottom */

        if (nCurrentView == IDM_VIEW_DETAILS) {

            if (bSel)
                WinFillRect(hps, &rcCell, SYSCLR(SYSCLR_HILITEBACKGROUND));

            BlitBitmap(hps, ahViewBmps[DriveTypeToBmpIndex(pDrv->nType)],
                       rcCell.xLeft,
                       rcCell.yTop - (g.cellH - g.bmH) / 2,
                       g.bmW, g.bmH);

            rcLabel          = rcCell;
            rcLabel.xLeft   += g.bmW + 6;
            DrawCellText(hps, &rcLabel, pDrv->szLabel, clrText);
        }
        else {
            BlitBitmap(hps, ahViewBmps[DriveTypeToBmpIndex(pDrv->nType)],
                       rcCell.xLeft + (g.cellW - g.bmW) / 2,
                       rcCell.yTop, g.bmW, g.bmH);

            /* label area beneath the bitmap */
            rcLabel         = rcCell;
            rcLabel.yTop    = rcCell.yTop - g.bmH - 2;

            if (bSel)
                WinFillRect(hps, &rcLabel, SYSCLR(SYSCLR_HILITEBACKGROUND));

            DrawLabel(hps, &rcLabel, pDrv->szLabel, clrText, 2);

            if (bSel)
                DrawFocusFrame(hps, &rcCell);
        }

        col++;
        if (col >= g.cols) {
            col = 0;
            lx  = (nCurrentView == IDM_VIEW_DETAILS) ? 0 : CONTENT_PAD;
            ly += g.cellH;
        }
        else {
            lx += g.cellW;
        }
    }
}

/*------------------------------------------------------------------
  DrawFileDetails - directory contents as a list with columns
------------------------------------------------------------------*/
static void DrawFileDetails(HPS hps, const RECTL *pcont, VIEWGEOM *pg)
{
    FILEITEM *pFile;
    RECTL     rcRow, rcCell, rcHdr;
    POINTL    ptl;
    char      szDate[32];
    char      szSize[32];
    char      szAttr[8];
    char      szName[CCHMAXPATHCOMP + sizeof(LBL_DOTS)];
    LONG      xIcon, xName, xDate, xSize, xAttr, ly, clrText, xOrigin;
    LONG      cxName, wDots;
    int       i, nName;
    BOOL      bSel;

    wDots = TextWidth(alCharW, LBL_DOTS, 3);

    xIcon = CONTENT_PAD;
    xName = xIcon + pg->bmW + 4;
    xDate = xName + pg->nameW;
    xSize = xDate + DTL_DATE_W;
    xAttr = xSize + DTL_SIZE_W;

    /* ---- header row ----

       CellRect gives the header its y, which scrolls with the content as it
       did on Windows.  Its x is forced to the pane instead of being taken
       from the same call, so that scrolling right does not drag the grey
       band off the left edge and leave a gap on the right; only the column
       captions inside it move. */
    CellRect(pcont, 0, 0, pcont->xRight - pcont->xLeft, pg->hdrH, &rcHdr);
    xOrigin      = rcHdr.xLeft;         /* scrolled: the captions use this */
    rcHdr.xLeft  = pcont->xLeft;        /* unscrolled: the band uses this  */
    rcHdr.xRight = pcont->xRight;

    WinFillRect(hps, &rcHdr, SYSCLR(SYSCLR_BUTTONMIDDLE));

    rcCell = rcHdr; rcCell.xLeft = xOrigin + xName;
    DrawCellText(hps, &rcCell, "Name", SYSCLR(SYSCLR_WINDOWTEXT));
    rcCell = rcHdr; rcCell.xLeft = xOrigin + xDate;
    DrawCellText(hps, &rcCell, "Date", SYSCLR(SYSCLR_WINDOWTEXT));
    rcCell = rcHdr; rcCell.xLeft = xOrigin + xSize;
    DrawCellText(hps, &rcCell, "Size", SYSCLR(SYSCLR_WINDOWTEXT));
    rcCell = rcHdr; rcCell.xLeft = xOrigin + xAttr;
    DrawCellText(hps, &rcCell, "Attributes", SYSCLR(SYSCLR_WINDOWTEXT));

    /* divider under the header */
    GpiSetColor(hps, SYSCLR(SYSCLR_BUTTONDARK));
    ptl.x = rcHdr.xLeft;  ptl.y = rcHdr.yBottom;
    GpiMove(hps, &ptl);
    ptl.x = rcHdr.xRight; ptl.y = rcHdr.yBottom;
    GpiLine(hps, &ptl);

    /* ---- data rows ---- */
    ly = pg->hdrH;

    for (i = 0; i < pg->count; i++) {

        pFile = GetFileItem(i);
        if (pFile == NULL)
            continue;

        CellRect(pcont, 0, ly, pcont->xRight - pcont->xLeft, pg->rowH,
                 &rcRow);
        ly += pg->rowH;

        /* The row band spans the pane; only its contents scroll sideways */
        xOrigin      = rcRow.xLeft;
        rcRow.xLeft  = pcont->xLeft;
        rcRow.xRight = pcont->xRight;

        if (rcRow.yBottom >= pcont->yTop)
            continue;                   /* still above the viewport */
        if (rcRow.yTop <= pcont->yBottom)
            break;                      /* below it - and so is the rest */

        bSel = abSelectedFiles[i];

        if (bSel) {
            WinFillRect(hps, &rcRow, SYSCLR(SYSCLR_HILITEBACKGROUND));
            clrText = SYSCLR(SYSCLR_HILITEFOREGROUND);
        }
        else {
            clrText = SYSCLR(SYSCLR_WINDOWTEXT);
        }

        BlitBitmap(hps,
                   pFile->bIsDir ? ahViewBmps[1] : ahViewBmps[0],
                   xOrigin + xIcon,
                   rcRow.yTop - (pg->rowH - pg->bmH) / 2,
                   pg->bmW, pg->bmH);

        /* name, ending in "..." if it is wider than its column */
        strcpy(szName, pFile->szName);
        nName  = (int)strlen(szName);
        cxName = pg->nameW - DTL_NAME_PAD / 2;
        if (TextWidth(alCharW, szName, nName) > cxName) {
            LONG w = wDots;

            nName = 0;
            while (szName[nName] != '\0' &&
                   w + alCharW[(unsigned char)szName[nName]] <= cxName) {
                w += alCharW[(unsigned char)szName[nName]];
                nName++;
            }
            while (nName > 0 && szName[nName - 1] == ' ')
                nName--;
            strcpy(szName + nName, LBL_DOTS);
        }
        rcCell = rcRow;
        rcCell.xLeft  = xOrigin + xName;
        rcCell.xRight = xOrigin + xDate - 2;
        DrawCellText(hps, &rcCell, szName, clrText);

        /* date */
        if (pFile->fdate.year != 0 || pFile->fdate.month != 0) {
            sprintf(szDate, "%02u/%02u/%04u",
                    (unsigned)pFile->fdate.month,
                    (unsigned)pFile->fdate.day,
                    (unsigned)pFile->fdate.year + 1980);
        }
        else {
            szDate[0] = '\0';
        }
        rcCell.xLeft  = xOrigin + xDate;
        rcCell.xRight = xOrigin + xSize - 2;
        DrawCellText(hps, &rcCell, szDate, clrText);

        /* size */
        if (pFile->bIsDir)
            strcpy(szSize, "<DIR>");
        else if (pFile->cbFile >= 1048576UL)
            sprintf(szSize, "%luMB", pFile->cbFile / 1048576UL);
        else if (pFile->cbFile >= 1024UL)
            sprintf(szSize, "%luKB", pFile->cbFile / 1024UL);
        else
            sprintf(szSize, "%lu", pFile->cbFile);

        rcCell.xLeft  = xOrigin + xSize;
        rcCell.xRight = xOrigin + xAttr - 2;
        DrawCellText(hps, &rcCell, szSize, clrText);

        /* attributes */
        FormatAttr(pFile->attrFile, szAttr);
        rcCell.xLeft  = xOrigin + xAttr;
        rcCell.xRight = rcRow.xRight - 2;
        DrawCellText(hps, &rcCell, szAttr, clrText);

        if (i == nDragHoverIdx)
            DrawFocusFrame(hps, &rcRow);
    }
}

/*------------------------------------------------------------------
  DrawFiles - paint the current directory's contents
------------------------------------------------------------------*/
void DrawFiles(HPS hps, const RECTL *pcont)
{
    VIEWGEOM  g;
    FILEITEM *pFile;
    RECTL     rcCell, rcLabel;
    LONG      cxVisible, lx, ly, firstRow, iconH, cxIcon, cyIcon, clrText;
    int       i, col;
    BOOL      bSel;
    int       nFocus = -1;          /* the focused item, drawn last */
    RECTL     rcFocusCell, rcFocusLabel;

    cxVisible = pcont->xRight - pcont->xLeft;

    WinFillRect(hps, (PRECTL)pcont, SYSCLR(SYSCLR_WINDOW));

    if (!ViewGeom(cxVisible, &g) || g.count == 0)
        return;

    LoadCharWidths(hps, alCharW);

    if (nCurrentView == IDM_VIEW_DETAILS) {
        DrawFileDetails(hps, pcont, &g);
        return;
    }

    cxIcon = WinQuerySysValue(HWND_DESKTOP, SV_CXICON);
    cyIcon = WinQuerySysValue(HWND_DESKTOP, SV_CYICON);
    if (cxIcon <= 0) cxIcon = 32;
    if (cyIcon <= 0) cyIcon = 32;

    firstRow = FirstVisibleRow(g.cellH);
    i        = (int)(firstRow * g.cols);
    if (i > g.count)
        return;

    col = 0;
    lx  = CONTENT_PAD;
    ly  = CONTENT_PAD + firstRow * g.cellH;

    for (; i < g.count; i++) {

        pFile = GetFileItem(i);
        if (pFile == NULL)
            continue;

        bSel    = abSelectedFiles[i];
        clrText = bSel ? SYSCLR(SYSCLR_HILITEFOREGROUND)
                       : SYSCLR(SYSCLR_WINDOWTEXT);

        CellRect(pcont, lx, ly, g.cellW, g.cellH, &rcCell);

        if (rcCell.yTop < pcont->yBottom)
            break;

        if (pFile->hIcon != NULLHANDLE) {
            iconH = cyIcon;
            WinDrawPointer(hps,
                           rcCell.xLeft + (g.cellW - cxIcon) / 2,
                           rcCell.yTop  - cyIcon,
                           pFile->hIcon, DP_NORMAL);
        }
        else {
            iconH = g.bmH;
            BlitBitmap(hps,
                       pFile->bIsDir ? ahViewBmps[1] : ahViewBmps[0],
                       rcCell.xLeft + (g.cellW - g.bmW) / 2,
                       rcCell.yTop, g.bmW, g.bmH);
        }

        rcLabel      = rcCell;
        rcLabel.yTop = rcCell.yTop - iconH - 2;

        if (bSel && i == nSelectedFile) {
            /* Its label and frame come after the loop, over the row below.
               The frames are inverted lines, so drawing one here as well
               would rub out the one drawn then where the two coincide. */
            nFocus       = i;
            rcFocusCell  = rcCell;
            rcFocusLabel = rcLabel;
        }
        else {
            if (bSel)
                WinFillRect(hps, &rcLabel, SYSCLR(SYSCLR_HILITEBACKGROUND));

            DrawLabel(hps, &rcLabel, pFile->szName, clrText, 2);

            if (bSel)
                DrawFocusFrame(hps, &rcCell);

            if (i == nDragHoverIdx)
                DrawFocusFrame(hps, &rcCell);
        }

        col++;
        if (col >= g.cols) {
            col = 0;
            lx  = CONTENT_PAD;
            ly += g.cellH;
        }
        else {
            lx += g.cellW;
        }
    }

    /* The focused item's whole name, over whatever lies beneath it, and one
       frame round the cell and the name together */
    if (nFocus >= 0) {
        RECTL rcBand, rcFrame;

        pFile = GetFileItem(nFocus);
        if (pFile != NULL) {
            DrawFullLabel(hps, &rcFocusLabel, pFile->szName,
                          SYSCLR(SYSCLR_HILITEFOREGROUND),
                          SYSCLR(SYSCLR_HILITEBACKGROUND), &rcBand);

            rcFrame = rcFocusCell;
            if (rcBand.yBottom < rcFrame.yBottom)
                rcFrame.yBottom = rcBand.yBottom;
            DrawFocusFrame(hps, &rcFrame);
        }
    }
}
