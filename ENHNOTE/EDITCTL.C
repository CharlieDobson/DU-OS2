/*====================================================================
 * EDITCTL.C -- Custom multiline edit control for Enhanced Notepad
 *              (OS/2 2.x / Warp Presentation Manager)
 *
 * Registers the "EnhEdit" window class and drives display over the
 * GAPBUF text engine: fixed-pitch layout, Notepad-style word wrap,
 * selection, scrolling, cursor, and the clipboard/undo operations the
 * host menu invokes, plus full keyboard/mouse input.
 *
 * Layout model
 * ------------
 * The viewport top is tracked as (topLine, topSub): a logical line
 * plus a wrapped sub-row within it.  Word-wrap breaks are computed on
 * demand per line (NextRowStart); there is no global prefix-sum table.
 * The vertical scrollbar slider maps to logical lines, while line/page
 * scrolling steps by display rows.  That is unchanged from the Windows
 * builds; what changed is everything around it.
 *
 * PM notes
 * --------
 * 1. COORDINATES.  PM's origin is the BOTTOM-left and y grows upwards.
 *    Display row i (0 = top) therefore occupies
 *        yBottom = cyWin - (i+1) * cyLine
 *    and a mouse y maps back through (cyWin - y) / cyLine.  RowY() and
 *    YToRow() are the only two places that know this, so painting and
 *    hit-testing cannot drift apart.
 *
 * 2. SCROLLBARS.  There is no WS_VSCROLL/WS_HSCROLL style to hang on
 *    an arbitrary PM window.  The control creates two WC_SCROLLBAR
 *    children of its own and insets the text area around them, so the
 *    host still deals with a single window.  Range and slider are set
 *    with SBM_SETSCROLLBAR / SBM_SETTHUMBSIZE; scroll notifications
 *    arrive as WM_VSCROLL / WM_HSCROLL with the command in
 *    SHORT2FROMMP(mp2) and the slider position in SHORT1FROMMP(mp2).
 *
 * 3. THE CURSOR is WinCreateCursor (CURSOR_SOLID to make one,
 *    CURSOR_SETPOS to move it), WinShowCursor and WinDestroyCursor.
 *
 * 4. THE FONT.  The layout is fixed-pitch, so the default (proportional)
 *    system font will not do.  PickFixedFont enumerates "System VIO"
 *    with GpiQueryFonts and builds a FATTRS from the matching
 *    FONTMETRICS -- an image font will not match on facename alone, it
 *    needs lMatch and a concrete size.
 *
 * 5. KEYBOARD.  PM delivers ONE WM_CHAR carrying both the virtual key
 *    and the character, distinguished by the KC_VIRTUALKEY and
 *    KC_CHAR flags, so OnKey handles the virtual keys first and only
 *    falls through to typing when no virtual key claimed the event.
 *
 * Compiler: Open Watcom 1.9  (wcc386 -bt=os2)
 *====================================================================*/

#define INCL_WIN
#define INCL_GPI
#define INCL_DOSMEMMGR
#define INCL_DOSERRORS

#include <os2.h>
#include <string.h>
#include <stdlib.h>
#include "gapbuf.h"
#include "editctl.h"

#define TABSIZE       8
#define MAXVIS        512       /* max visible columns drawn per row  */
#define SB_MAXRANGE   30000     /* scrollbar range ceiling            */
#define LCID_FIXED    1L        /* logical font id inside our PS      */
#define FIXED_FACE    "System VIO"
#define IDSB_VERT     0x7F01
#define IDSB_HORZ     0x7F02

/*--------------------------------------------------------------------
 * Per-control instance data (a pointer to it lives in the window's
 * one window-data slot).
 *------------------------------------------------------------------*/
typedef struct
{
    HWND    hwnd;
    HWND    hwndVSB, hwndHSB;   /* the control's own scrollbars       */
    TEXTBUF tb;

    FATTRS  fat;                /* the fixed-pitch logical font       */
    BOOL    bHaveFont;          /* FALSE = fall back to the default   */
    LONG    cxChar, cyLine, lDescender;

    LONG    cxWin, cyWin;       /* whole control window               */
    LONG    yOrg;               /* bottom of the text area            */
    LONG    cxClient, cyClient; /* text area size                     */
    int     nCols, nRows;       /* fully visible columns / rows       */

    BOOL    bWrap;
    LONG    topLine;            /* first visible logical line         */
    int     topSub;             /* wrapped sub-row within topLine     */
    int     leftCol;            /* horizontal scroll (wrap off)       */

    ULONG   caret;              /* caret document position            */
    ULONG   anchor;             /* selection anchor document position */
    int     prefCol;            /* preferred column for up/down       */

    ULONG   totalRows;          /* total display rows (for V bar)     */
    ULONG   maxCol;             /* widest line in columns (for H bar) */

    BOOL    bFocus;
    BOOL    bTracking;          /* mouse selection drag in progress   */
} CONTROL;

#define GETCTL(h)   ((CONTROL *)WinQueryWindowPtr((h), 0))
#define SETCTL(h,p) WinSetWindowPtr((h), 0, (PVOID)(p))

static HAB habEdit = NULLHANDLE;    /* kept by EditRegister           */

/*--------------------------------------------------------------------
 * Forward declarations
 *------------------------------------------------------------------*/
static void  ComputeMetrics(CONTROL *);
static ULONG NextRowStart(CONTROL *, LONG, ULONG, ULONG);
static ULONG CountLineRows(CONTROL *, LONG);
static int   ColOfPos(CONTROL *, ULONG, ULONG);
static void  FullRelayout(CONTROL *);
static void  UpdateScrollbars(CONTROL *);
static void  ClampTop(CONTROL *);
static void  EnsureCaretVisible(CONTROL *);
static void  PlaceCaret(CONTROL *);
static void  Refresh(CONTROL *);
static void  Notify(CONTROL *, USHORT);
static void  LayoutBars(CONTROL *);

/*--------------------------------------------------------------------
 * Selection bounds
 *------------------------------------------------------------------*/
static ULONG SelMin(CONTROL *c){ return c->anchor < c->caret ? c->anchor : c->caret; }
static ULONG SelMax(CONTROL *c){ return c->anchor > c->caret ? c->anchor : c->caret; }

/*--------------------------------------------------------------------
 * The two functions that own the top-down / bottom-up flip
 *------------------------------------------------------------------*/

/* Window y of the BOTTOM of display row 'row' (0 = topmost). */
static LONG RowY(CONTROL *ctl, int row)
{
    return ctl->cyWin - (LONG)(row + 1) * ctl->cyLine;
}

/* Display row containing window y, clamped to the visible rows. */
static int YToRow(CONTROL *ctl, LONG y)
{
    LONG dy = ctl->cyWin - y;
    int  row;

    if (dy < 0) dy = 0;
    row = (int)(dy / ctl->cyLine);
    if (row > ctl->nRows - 1) row = ctl->nRows - 1;
    if (row < 0) row = 0;
    return row;
}

/*====================================================================
 * Column arithmetic (fixed pitch, tab expansion)
 *====================================================================*/

static int CharCols(char c, int col)
{
    if (c == '\t') return TABSIZE - (col % TABSIZE);
    if (c == '\r') return 0;                /* stray CR: zero width  */
    return 1;
}

/* Document offset where the display row beginning at startPos ends
   (== start of the next row, or lineEnd).  Word wrap: prefer to break
   after the last space; hard-break an over-wide single token. */
static ULONG NextRowStart(CONTROL *ctl, LONG line, ULONG startPos, ULONG lineEnd)
{
    ULONG p = startPos, lastSpace = startPos;
    int   col = 0, nc = ctl->nCols;
    BOOL  haveSpace = FALSE;

    (void)line;
    if (!ctl->bWrap) return lineEnd;
    if (nc < 1) nc = 1;

    while (p < lineEnd)
    {
        char c   = TbCharAt(&ctl->tb, p);
        int  adv = CharCols(c, col);

        if (col + adv > nc)
        {
            if (haveSpace && lastSpace > startPos) return lastSpace;
            if (p > startPos)                      return p;
            return startPos + 1;                /* force progress    */
        }
        col += adv; p++;
        if (c == ' ' || c == '\t') { lastSpace = p; haveSpace = TRUE; }
    }
    return lineEnd;
}

static ULONG CountLineRows(CONTROL *ctl, LONG line)
{
    ULONG start = TbLineStart(&ctl->tb, line);
    ULONG end   = TbLineEnd(&ctl->tb, line);
    ULONG p, rows = 0;

    if (!ctl->bWrap) return 1;
    p = start;
    do { p = NextRowStart(ctl, line, p, end); rows++; } while (p < end);
    return rows ? rows : 1;
}

static ULONG SumRows(CONTROL *ctl, LONG a, LONG b)
{
    ULONG t = 0; LONG i;
    if (a < 0) a = 0;
    if (b > ctl->tb.nLines - 1) b = ctl->tb.nLines - 1;
    for (i = a; i <= b; i++) t += CountLineRows(ctl, i);
    return t;
}

/* Column of document position 'pos' measured from row start. */
static int ColOfPos(CONTROL *ctl, ULONG rowStart, ULONG pos)
{
    ULONG p = rowStart; int col = 0;
    while (p < pos) { col += CharCols(TbCharAt(&ctl->tb, p), col); p++; }
    return col;
}

/* Start offset of sub-row 'sub' within a logical line. */
static ULONG RowStartOfSub(CONTROL *ctl, LONG line, int sub)
{
    ULONG start = TbLineStart(&ctl->tb, line);
    ULONG end   = TbLineEnd(&ctl->tb, line);
    ULONG rs = start; int i;
    if (!ctl->bWrap) return start;
    for (i = 0; i < sub; i++)
    {
        ULONG e = NextRowStart(ctl, line, rs, end);
        if (e >= end) break;
        rs = e;
    }
    return rs;
}

/* Sub-row containing 'pos'; also returns the row start. */
static int SubRowOfPos(CONTROL *ctl, LONG line, ULONG pos, ULONG *pRowStart)
{
    ULONG end = TbLineEnd(&ctl->tb, line);
    ULONG rs  = TbLineStart(&ctl->tb, line), re;
    int   sub = 0;

    if (!ctl->bWrap) { if (pRowStart) *pRowStart = rs; return 0; }
    for (;;)
    {
        re = NextRowStart(ctl, line, rs, end);
        if (pos < re || re >= end) { if (pRowStart) *pRowStart = rs; return sub; }
        rs = re; sub++;
    }
}

/*====================================================================
 * Viewport stepping
 *====================================================================*/

static BOOL StepDownRow(CONTROL *ctl)
{
    ULONG end = TbLineEnd(&ctl->tb, ctl->topLine);
    ULONG rs  = RowStartOfSub(ctl, ctl->topLine, ctl->topSub);
    ULONG re  = NextRowStart(ctl, ctl->topLine, rs, end);

    if (re >= end)
    {
        if (ctl->topLine >= ctl->tb.nLines - 1) return FALSE;
        ctl->topLine++; ctl->topSub = 0;
    }
    else ctl->topSub++;
    return TRUE;
}

static BOOL StepUpRow(CONTROL *ctl)
{
    if (ctl->topSub > 0) { ctl->topSub--; return TRUE; }
    if (ctl->topLine <= 0) return FALSE;
    ctl->topLine--;
    ctl->topSub = (int)CountLineRows(ctl, ctl->topLine) - 1;
    return TRUE;
}

/* Screen row (0..nRows-1) of caret's (line,sub) relative to the top,
   or -1 if above the top or beyond the visible rows. */
static int RowsFromTop(CONTROL *ctl, LONG cLine, int cSub)
{
    LONG line = ctl->topLine; int sub = ctl->topSub, i;
    ULONG lineEnd, rs;

    if (cLine < ctl->topLine ||
        (cLine == ctl->topLine && cSub < ctl->topSub))
        return -1;

    lineEnd = TbLineEnd(&ctl->tb, line);
    rs      = RowStartOfSub(ctl, line, sub);

    for (i = 0; i <= ctl->nRows; i++)
    {
        ULONG re;
        if (line == cLine && sub == cSub) return (i < ctl->nRows) ? i : -1;
        re = NextRowStart(ctl, line, rs, lineEnd);
        if (re >= lineEnd)
        {
            if (line >= ctl->tb.nLines - 1) return -1;
            line++; sub = 0;
            lineEnd = TbLineEnd(&ctl->tb, line);
            rs = TbLineStart(&ctl->tb, line);
        }
        else { sub++; rs = re; }
    }
    return -1;
}

/*====================================================================
 * The fixed-pitch font
 *
 * A bitmap ("image") font will not match on facename alone -- PM hands
 * back the default outline font instead.  So enumerate the family,
 * pick a size, and carry lMatch plus the concrete height/width into
 * the FATTRS.  If the family is missing we fall back to whatever the
 * PS already has; the layout then assumes a pitch that is only the
 * average, but the control still works.
 *====================================================================*/

static void PickFixedFont(CONTROL *ctl, HPS hps)
{
    LONG         cFonts, cRemaining, lTemp = 0;
    FONTMETRICS *pfm;
    LONG         i, best = -1, bestDist = 0;

    ctl->bHaveFont = FALSE;
    memset(&ctl->fat, 0, sizeof(FATTRS));

    cFonts = GpiQueryFonts(hps, QF_PUBLIC, (PCSZ)FIXED_FACE, &lTemp,
                           (LONG)sizeof(FONTMETRICS), (PFONTMETRICS)NULL);
    if (cFonts <= 0) return;

    pfm = (FONTMETRICS *)malloc((ULONG)cFonts * sizeof(FONTMETRICS));
    if (!pfm) return;

    cRemaining = cFonts;
    GpiQueryFonts(hps, QF_PUBLIC, (PCSZ)FIXED_FACE, &cRemaining,
                  (LONG)sizeof(FONTMETRICS), pfm);

    /* Prefer a fixed-pitch face about 16 pixels tall -- the familiar
       8x16 VIO cell.  Anything fixed will do if that is missing. */
    for (i = 0; i < cFonts; i++)
    {
        LONG dist;
        if (!(pfm[i].fsType & FM_TYPE_FIXED)) continue;
        if (pfm[i].lAveCharWidth <= 0 || pfm[i].lMaxBaselineExt <= 0) continue;

        dist = pfm[i].lMaxBaselineExt - 16;
        if (dist < 0) dist = -dist;
        if (best < 0 || dist < bestDist) { best = i; bestDist = dist; }
    }
    if (best < 0)
    {
        free(pfm);
        return;
    }

    ctl->fat.usRecordLength  = sizeof(FATTRS);
    ctl->fat.fsSelection     = 0;
    ctl->fat.lMatch          = pfm[best].lMatch;
    strcpy(ctl->fat.szFacename, pfm[best].szFacename);
    ctl->fat.idRegistry      = pfm[best].idRegistry;
    ctl->fat.usCodePage      = 0;           /* the process code page  */
    ctl->fat.lMaxBaselineExt = pfm[best].lMaxBaselineExt;
    ctl->fat.lAveCharWidth   = pfm[best].lAveCharWidth;
    ctl->fat.fsType          = 0;
    ctl->fat.fsFontUse       = FATTR_FONTUSE_NOMIX;

    ctl->bHaveFont = TRUE;
    free(pfm);
}

/* Select the control's font into a presentation space. */
static void SetFontOnPS(CONTROL *ctl, HPS hps)
{
    if (!ctl->bHaveFont) return;
    if (GpiCreateLogFont(hps, NULL, LCID_FIXED, &ctl->fat) != GPI_ERROR)
        GpiSetCharSet(hps, LCID_FIXED);
}

static void ClearFontOnPS(CONTROL *ctl, HPS hps)
{
    if (!ctl->bHaveFont) return;
    GpiSetCharSet(hps, LCID_DEFAULT);
    GpiDeleteSetId(hps, LCID_FIXED);
}

/*====================================================================
 * Metrics, layout, scrollbars
 *====================================================================*/

static void ComputeMetrics(CONTROL *ctl)
{
    HPS         hps;
    FONTMETRICS fm;

    hps = WinGetPS(ctl->hwnd);
    SetFontOnPS(ctl, hps);
    memset(&fm, 0, sizeof(fm));
    GpiQueryFontMetrics(hps, (LONG)sizeof(fm), &fm);
    ClearFontOnPS(ctl, hps);
    WinReleasePS(hps);

    ctl->cxChar     = fm.lAveCharWidth;
    ctl->cyLine     = fm.lMaxBaselineExt + fm.lExternalLeading;
    ctl->lDescender = fm.lMaxDescender;
    if (ctl->cxChar < 1) ctl->cxChar = 1;
    if (ctl->cyLine < 1) ctl->cyLine = 1;

    ctl->nCols = (int)(ctl->cxClient / ctl->cxChar);
    ctl->nRows = (int)(ctl->cyClient / ctl->cyLine);
    if (ctl->nCols < 1) ctl->nCols = 1;
    if (ctl->nRows < 1) ctl->nRows = 1;
}

/* Place the two scrollbars inside the control and work out what is
   left for text.  With word wrap on there is nothing to scroll
   sideways, so the horizontal bar is hidden and the text area grows
   down to y == 0. */
static void LayoutBars(CONTROL *ctl)
{
    LONG sbw = WinQuerySysValue(HWND_DESKTOP, SV_CXVSCROLL);
    LONG sbh = WinQuerySysValue(HWND_DESKTOP, SV_CYHSCROLL);
    LONG textW, textH;

    if (sbw <= 0) sbw = 16;
    if (sbh <= 0) sbh = 16;

    ctl->yOrg = ctl->bWrap ? 0 : sbh;

    textW = ctl->cxWin - sbw;
    textH = ctl->cyWin - ctl->yOrg;
    if (textW < 1) textW = 1;
    if (textH < 1) textH = 1;

    ctl->cxClient = textW;
    ctl->cyClient = textH;

    if (ctl->hwndVSB != NULLHANDLE)
        WinSetWindowPos(ctl->hwndVSB, HWND_TOP,
                        ctl->cxWin - sbw, ctl->yOrg, sbw, textH,
                        SWP_MOVE | SWP_SIZE | SWP_SHOW);

    if (ctl->hwndHSB != NULLHANDLE)
    {
        if (ctl->bWrap)
            WinShowWindow(ctl->hwndHSB, FALSE);
        else
            WinSetWindowPos(ctl->hwndHSB, HWND_TOP,
                            0, 0, textW, sbh,
                            SWP_MOVE | SWP_SIZE | SWP_SHOW);
    }
}

static void ComputeMaxCol(CONTROL *ctl)
{
    LONG i; ULONG mx = 0;
    for (i = 0; i < ctl->tb.nLines; i++)
    {
        ULONG w = (ULONG)ColOfPos(ctl, TbLineStart(&ctl->tb, i),
                                  TbLineEnd(&ctl->tb, i));
        if (w > mx) mx = w;
    }
    ctl->maxCol = mx;
}

static void RecountTotal(CONTROL *ctl)
{
    if (!ctl->bWrap) { ctl->totalRows = (ULONG)ctl->tb.nLines; return; }
    ctl->totalRows = SumRows(ctl, 0, ctl->tb.nLines - 1);
}

static void FullRelayout(CONTROL *ctl)
{
    ComputeMaxCol(ctl);
    RecountTotal(ctl);
    ClampTop(ctl);
}

static void ClampTop(CONTROL *ctl)
{
    int rows;
    if (ctl->topLine < 0) ctl->topLine = 0;
    if (ctl->topLine > ctl->tb.nLines - 1) ctl->topLine = ctl->tb.nLines - 1;
    rows = (int)CountLineRows(ctl, ctl->topLine);
    if (ctl->topSub < 0) ctl->topSub = 0;
    if (ctl->topSub > rows - 1) ctl->topSub = rows - 1;
    if (ctl->leftCol < 0) ctl->leftCol = 0;
}

static void UpdateScrollbars(CONTROL *ctl)
{
    LONG nLines = ctl->tb.nLines;
    LONG vMax, vPos, hMax;

    /* Vertical: the slider maps to logical lines. */
    vMax = (nLines - 1 > (LONG)SB_MAXRANGE) ? SB_MAXRANGE
         : (nLines - 1 < 0 ? 0 : nLines - 1);
    vPos = (vMax > 0 && nLines > 1)
         ? (LONG)(((ULONG)ctl->topLine * (ULONG)vMax) / (ULONG)(nLines - 1))
         : 0;

    if (ctl->hwndVSB != NULLHANDLE)
    {
        WinSendMsg(ctl->hwndVSB, SBM_SETSCROLLBAR,
                   MPFROMSHORT((SHORT)vPos),
                   MPFROM2SHORT(0, (SHORT)vMax));
        WinSendMsg(ctl->hwndVSB, SBM_SETTHUMBSIZE,
                   MPFROM2SHORT((SHORT)ctl->nRows,
                                (SHORT)(nLines > 0 ? nLines : 1)),
                   MPVOID);
    }

    /* Horizontal: only meaningful when word wrap is off. */
    if (ctl->hwndHSB != NULLHANDLE && !ctl->bWrap)
    {
        hMax = (LONG)(ctl->maxCol > (ULONG)SB_MAXRANGE
                      ? (ULONG)SB_MAXRANGE : ctl->maxCol);
        if (ctl->leftCol > hMax) ctl->leftCol = (int)hMax;

        WinSendMsg(ctl->hwndHSB, SBM_SETSCROLLBAR,
                   MPFROMSHORT((SHORT)ctl->leftCol),
                   MPFROM2SHORT(0, (SHORT)hMax));
        WinSendMsg(ctl->hwndHSB, SBM_SETTHUMBSIZE,
                   MPFROM2SHORT((SHORT)ctl->nCols,
                                (SHORT)(hMax > 0 ? hMax + ctl->nCols : 1)),
                   MPVOID);
    }
    else if (ctl->bWrap)
        ctl->leftCol = 0;
}

/*====================================================================
 * Cursor (PM's name for the caret) and refresh
 *====================================================================*/

static void EnsureCaretVisible(CONTROL *ctl)
{
    LONG  cLine = TbLineFromPos(&ctl->tb, ctl->caret);
    ULONG rs = 0;
    int   cSub = SubRowOfPos(ctl, cLine, ctl->caret, &rs);

    if (cLine < ctl->topLine ||
        (cLine == ctl->topLine && cSub < ctl->topSub))
    {
        ctl->topLine = cLine; ctl->topSub = cSub;
    }
    else
    {
        int guard = ctl->nRows + 1;
        int sr = RowsFromTop(ctl, cLine, cSub);
        while (sr < 0 && guard-- > 0 && StepDownRow(ctl))
            sr = RowsFromTop(ctl, cLine, cSub);
        if (sr < 0) { ctl->topLine = cLine; ctl->topSub = cSub; }
    }

    if (!ctl->bWrap)                        /* horizontal follow     */
    {
        int col = ColOfPos(ctl, rs, ctl->caret);
        if (col < ctl->leftCol) ctl->leftCol = col;
        else if (col >= ctl->leftCol + ctl->nCols)
            ctl->leftCol = col - ctl->nCols + 1;
        if (ctl->leftCol < 0) ctl->leftCol = 0;
    }
    ClampTop(ctl);
}

static void PlaceCaret(CONTROL *ctl)
{
    LONG  cLine;
    ULONG rs = 0;
    int   cSub, screenRow, col;
    LONG  x, y;

    if (!ctl->bFocus) return;

    cLine = TbLineFromPos(&ctl->tb, ctl->caret);
    cSub  = SubRowOfPos(ctl, cLine, ctl->caret, &rs);
    screenRow = RowsFromTop(ctl, cLine, cSub);

    if (screenRow < 0)                      /* park it off-window    */
    {
        WinCreateCursor(ctl->hwnd, -ctl->cxChar, -ctl->cyLine,
                        0, 0, CURSOR_SETPOS, NULL);
        return;
    }
    col = ColOfPos(ctl, rs, ctl->caret);
    x   = (LONG)(col - ctl->leftCol) * ctl->cxChar;
    y   = RowY(ctl, screenRow);
    WinCreateCursor(ctl->hwnd, x, y, 0, 0, CURSOR_SETPOS, NULL);
}

static void Refresh(CONTROL *ctl)
{
    UpdateScrollbars(ctl);
    WinInvalidateRect(ctl->hwnd, NULL, FALSE);
    PlaceCaret(ctl);
}

static void Notify(CONTROL *ctl, USHORT code)
{
    HWND owner = WinQueryWindow(ctl->hwnd, QW_OWNER);
    if (owner != NULLHANDLE)
        WinSendMsg(owner, WM_CONTROL,
                   MPFROM2SHORT((SHORT)WinQueryWindowUShort(ctl->hwnd, QWS_ID),
                                (SHORT)code),
                   MPFROMHWND(ctl->hwnd));
}

/*====================================================================
 * Painting
 *
 * A row is drawn as up to three fixed-pitch runs -- before, inside and
 * after the selection -- so nothing needs clipping: with a fixed cell
 * width each run's x is just its start column times cxChar.  GPI has no
 * single opaque-and-clipped text call to lean on instead.
 *====================================================================*/

static void DrawRun(CONTROL *ctl, HPS hps, char *buf, int from, int to,
                    LONG yBase, LONG fg)
{
    POINTL ptl;
    if (to <= from) return;
    GpiSetColor(hps, fg);
    ptl.x = (LONG)from * ctl->cxChar;
    ptl.y = yBase;
    GpiCharStringAt(hps, &ptl, (LONG)(to - from), (PCH)(buf + from));
}

static void DrawRow(CONTROL *ctl, HPS hps, int row, ULONG rowStart, ULONG rowEnd,
                    LONG clrBack, LONG clrText, LONG clrSelBack, LONG clrSelText)
{
    char  buf[MAXVIS];
    int   visCols = ctl->nCols, visStart, visEnd, col, i, drawLen;
    int   s0 = 0, s1 = 0;
    ULONG p, selMin, selMax;
    RECTL rc;
    LONG  yBot = RowY(ctl, row), yBase;

    if (visCols > MAXVIS) visCols = MAXVIS;
    visStart = ctl->leftCol;
    visEnd   = visStart + visCols;
    for (i = 0; i < visCols; i++) buf[i] = ' ';

    col = 0; p = rowStart;
    while (p < rowEnd && col < visEnd)
    {
        char c = TbCharAt(&ctl->tb, p);
        if (c == '\t')
        {
            int adv = TABSIZE - (col % TABSIZE), k;
            for (k = 0; k < adv; k++)
            {
                int gc = col + k;
                if (gc >= visStart && gc < visEnd) buf[gc - visStart] = ' ';
            }
            col += adv;
        }
        else if (c == '\r') { /* zero width */ }
        else
        {
            if (col >= visStart && col < visEnd) buf[col - visStart] = c;
            col++;
        }
        p++;
    }
    drawLen = (col > visStart) ? (col < visEnd ? col - visStart : visCols) : 0;

    /* ---- backgrounds ---- */
    rc.xLeft   = 0;
    rc.xRight  = ctl->cxClient;
    rc.yBottom = (yBot < ctl->yOrg) ? ctl->yOrg : yBot;
    rc.yTop    = yBot + ctl->cyLine;
    if (rc.yTop > rc.yBottom)
        WinFillRect(hps, &rc, clrBack);

    selMin = SelMin(ctl); selMax = SelMax(ctl);
    if (selMax > selMin)
    {
        ULONG rMin = selMin > rowStart ? selMin : rowStart;
        ULONG rMax = selMax < rowEnd   ? selMax : rowEnd;
        if (rMin < rMax)
        {
            int c0 = ColOfPos(ctl, rowStart, rMin);
            int c1 = ColOfPos(ctl, rowStart, rMax);
            if (c0 < visStart) c0 = visStart;
            if (c1 > visEnd)   c1 = visEnd;
            if (c1 > c0)
            {
                s0 = c0 - visStart;
                s1 = c1 - visStart;
                rc.xLeft  = (LONG)s0 * ctl->cxChar;
                rc.xRight = (LONG)s1 * ctl->cxChar;
                if (rc.yTop > rc.yBottom)
                    WinFillRect(hps, &rc, clrSelBack);
            }
        }
    }

    /* ---- text, in up to three runs ---- */
    if (drawLen <= 0) return;
    if (s0 > drawLen) s0 = drawLen;
    if (s1 > drawLen) s1 = drawLen;

    yBase = yBot + ctl->lDescender;         /* GPI draws from baseline */
    GpiSetBackMix(hps, BM_LEAVEALONE);      /* backgrounds already laid */

    DrawRun(ctl, hps, buf, 0,  s0,      yBase, clrText);
    DrawRun(ctl, hps, buf, s0, s1,      yBase, clrSelText);
    DrawRun(ctl, hps, buf, s1, drawLen, yBase, clrText);
}

static void OnPaint(CONTROL *ctl)
{
    HPS   hps;
    RECTL rclPaint, rc;
    LONG  line, nLines = ctl->tb.nLines;
    ULONG rowStart, rowEnd, lineEnd;
    LONG  clrBack, clrText, clrSelBack, clrSelText;
    int   row, i;

    hps = WinBeginPaint(ctl->hwnd, NULLHANDLE, &rclPaint);

    /* A cached PS comes back in index colour mode every time. */
    GpiCreateLogColorTable(hps, 0, LCOLF_RGB, 0, 0, NULL);
    SetFontOnPS(ctl, hps);

    clrBack    = WinQuerySysColor(HWND_DESKTOP, SYSCLR_WINDOW,           0);
    clrText    = WinQuerySysColor(HWND_DESKTOP, SYSCLR_WINDOWTEXT,       0);
    clrSelBack = WinQuerySysColor(HWND_DESKTOP, SYSCLR_HILITEBACKGROUND, 0);
    clrSelText = WinQuerySysColor(HWND_DESKTOP, SYSCLR_HILITEFOREGROUND, 0);

    if (ctl->bFocus) WinShowCursor(ctl->hwnd, FALSE);

    line     = ctl->topLine;
    lineEnd  = TbLineEnd(&ctl->tb, line);
    rowStart = TbLineStart(&ctl->tb, line);
    for (i = 0; i < ctl->topSub; i++)
        rowStart = NextRowStart(ctl, line, rowStart, lineEnd);

    row = 0;
    while (RowY(ctl, row) + ctl->cyLine > ctl->yOrg && line < nLines)
    {
        rowEnd = NextRowStart(ctl, line, rowStart, lineEnd);
        DrawRow(ctl, hps, row, rowStart, rowEnd,
                clrBack, clrText, clrSelBack, clrSelText);
        row++;

        if (rowEnd >= lineEnd)
        {
            line++;
            if (line >= nLines) break;
            lineEnd  = TbLineEnd(&ctl->tb, line);
            rowStart = TbLineStart(&ctl->tb, line);
        }
        else rowStart = rowEnd;
    }

    /* blank area past EOF -- everything below the last row drawn */
    rc.xLeft   = 0;
    rc.xRight  = ctl->cxClient;
    rc.yBottom = ctl->yOrg;
    rc.yTop    = RowY(ctl, row - 1);
    if (row == 0) rc.yTop = ctl->cyWin;
    if (rc.yTop > rc.yBottom)
        WinFillRect(hps, &rc, clrBack);

    if (ctl->bFocus) WinShowCursor(ctl->hwnd, TRUE);

    ClearFontOnPS(ctl, hps);
    WinEndPaint(hps);
}

/*====================================================================
 * Editing primitive: replace the selection with 'src'
 *====================================================================*/

/* Recompute the preferred column from the caret (called after any
   horizontal caret movement). */
static void UpdatePrefCol(CONTROL *ctl)
{
    LONG  line = TbLineFromPos(&ctl->tb, ctl->caret);
    ULONG rs = 0;
    SubRowOfPos(ctl, line, ctl->caret, &rs);
    ctl->prefCol = ColOfPos(ctl, rs, ctl->caret);
}

/* Grow maxCol to cover lines [a,b] (used by wrap-off edits). */
static void UpdateMaxColSpan(CONTROL *ctl, LONG a, LONG b)
{
    LONG i;
    if (a < 0) a = 0;
    if (b > ctl->tb.nLines - 1) b = ctl->tb.nLines - 1;
    for (i = a; i <= b; i++)
    {
        ULONG w = (ULONG)ColOfPos(ctl, TbLineStart(&ctl->tb, i),
                                  TbLineEnd(&ctl->tb, i));
        if (w > ctl->maxCol) ctl->maxCol = w;
    }
}

/* Replace the current selection with 'src'.  Relayout is incremental:
   only the touched logical-line span is re-measured, so ordinary
   typing stays O(1) in the file size. */
static void ReplaceSel(CONTROL *ctl, const char *src, ULONG len,
                       BOOL bBreakUndo)
{
    ULONG selMin = SelMin(ctl), selMax = SelMax(ctl);
    LONG  oldFirst = TbLineFromPos(&ctl->tb, selMin);
    LONG  oldLast  = TbLineFromPos(&ctl->tb, selMax);
    ULONG oldRows  = ctl->bWrap ? SumRows(ctl, oldFirst, oldLast) : 0;
    LONG  newLast;

    if (selMax > selMin)
        TbDelete(&ctl->tb, selMin, selMax - selMin, ctl->caret, selMin);
    if (len > 0)
        TbInsert(&ctl->tb, selMin, src, len, selMin, selMin + len);

    ctl->caret = ctl->anchor = selMin + len;
    if (bBreakUndo) TbBreakUndo(&ctl->tb);

    newLast = TbLineFromPos(&ctl->tb, selMin + len);
    if (ctl->bWrap)
        ctl->totalRows += SumRows(ctl, oldFirst, newLast) - oldRows;
    else
    {
        ctl->totalRows = (ULONG)ctl->tb.nLines;
        UpdateMaxColSpan(ctl, oldFirst, newLast);
    }

    UpdatePrefCol(ctl);
    EnsureCaretVisible(ctl);
    UpdateScrollbars(ctl);
    WinInvalidateRect(ctl->hwnd, NULL, FALSE);
    PlaceCaret(ctl);
    Notify(ctl, ENHEN_CHANGE);
}

/* Relayout + caret after an edit whose location we don't track
   (undo/redo/load). */
static void AfterBulkEdit(CONTROL *ctl)
{
    FullRelayout(ctl);
    EnsureCaretVisible(ctl);
    Refresh(ctl);
    Notify(ctl, ENHEN_CHANGE);
}

/*====================================================================
 * Interactive input: caret movement, editing keys, mouse selection
 *====================================================================*/

static BOOL IsWordCh(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '_' || (unsigned char)c >= 128;
}

/* Document position at (or just before) 'targetCol' within a row. */
static ULONG PosAtColInRow(CONTROL *ctl, ULONG rowStart, ULONG rowEnd,
                           int targetCol)
{
    ULONG p = rowStart; int col = 0;
    while (p < rowEnd)
    {
        int adv = CharCols(TbCharAt(&ctl->tb, p), col);
        if (col + adv > targetCol) break;
        col += adv; p++;
    }
    return p;
}

/* Start/end document positions of the display row holding the caret. */
static void CaretRowBounds(CONTROL *ctl, ULONG *prs, ULONG *pre)
{
    LONG  line = TbLineFromPos(&ctl->tb, ctl->caret);
    ULONG rs = 0;
    SubRowOfPos(ctl, line, ctl->caret, &rs);
    *prs = rs;
    *pre = NextRowStart(ctl, line, rs, TbLineEnd(&ctl->tb, line));
}

static ULONG WordRight(CONTROL *ctl, ULONG p)
{
    ULONG len = TbLength(&ctl->tb);
    while (p < len &&  IsWordCh(TbCharAt(&ctl->tb, p))) p++;
    while (p < len && !IsWordCh(TbCharAt(&ctl->tb, p))) p++;
    return p;
}

static ULONG WordLeft(CONTROL *ctl, ULONG p)
{
    while (p > 0 && !IsWordCh(TbCharAt(&ctl->tb, p - 1))) p--;
    while (p > 0 &&  IsWordCh(TbCharAt(&ctl->tb, p - 1))) p--;
    return p;
}

/* Map a window point to a document position. */
static ULONG XYToPos(CONTROL *ctl, LONG x, LONG y)
{
    LONG  line = ctl->topLine, nLines = ctl->tb.nLines;
    int   sub = ctl->topSub, row, i, targetCol;
    ULONG lineEnd  = TbLineEnd(&ctl->tb, line);
    ULONG rowStart = RowStartOfSub(ctl, line, sub), rowEnd;

    row = YToRow(ctl, y);
    for (i = 0; i < row; i++)
    {
        rowEnd = NextRowStart(ctl, line, rowStart, lineEnd);
        if (rowEnd >= lineEnd)
        {
            if (line >= nLines - 1) break;
            line++;
            lineEnd  = TbLineEnd(&ctl->tb, line);
            rowStart = TbLineStart(&ctl->tb, line);
        }
        else rowStart = rowEnd;
    }
    rowEnd    = NextRowStart(ctl, line, rowStart, lineEnd);
    targetCol = ctl->leftCol + (int)((x < 0) ? 0 : x / ctl->cxChar);
    return PosAtColInRow(ctl, rowStart, rowEnd, targetCol);
}

/* Move the caret; extend keeps the anchor (shift-select), setPref
   refreshes the preferred column for subsequent vertical moves. */
static void MoveCaretTo(CONTROL *ctl, ULONG pos, BOOL extend, BOOL setPref)
{
    BOOL hadSel = (ctl->anchor != ctl->caret);
    LONG oTop = ctl->topLine;
    int  oSub = ctl->topSub, oLeft = ctl->leftCol;

    ctl->caret = pos;
    if (!extend) ctl->anchor = pos;
    TbBreakUndo(&ctl->tb);
    if (setPref) UpdatePrefCol(ctl);

    EnsureCaretVisible(ctl);
    if (hadSel || extend ||
        oTop != ctl->topLine || oSub != ctl->topSub || oLeft != ctl->leftCol)
    {
        UpdateScrollbars(ctl);
        WinInvalidateRect(ctl->hwnd, NULL, FALSE);
    }
    PlaceCaret(ctl);
}

static void VertMove(CONTROL *ctl, int dir, BOOL extend)
{
    LONG  line = TbLineFromPos(&ctl->tb, ctl->caret), tline = line;
    ULONG rs = 0;
    int   sub = SubRowOfPos(ctl, line, ctl->caret, &rs), tsub = 0, rows;
    ULONG trs, tre, np;

    if (dir < 0)
    {
        if (sub > 0) { tline = line; tsub = sub - 1; }
        else if (line > 0) { tline = line - 1; tsub = (int)CountLineRows(ctl, tline) - 1; }
        else return;
    }
    else
    {
        rows = (int)CountLineRows(ctl, line);
        if (sub < rows - 1) { tline = line; tsub = sub + 1; }
        else if (line < ctl->tb.nLines - 1) { tline = line + 1; tsub = 0; }
        else return;
    }
    trs = RowStartOfSub(ctl, tline, tsub);
    tre = NextRowStart(ctl, tline, trs, TbLineEnd(&ctl->tb, tline));
    np  = PosAtColInRow(ctl, trs, tre, ctl->prefCol);
    MoveCaretTo(ctl, np, extend, FALSE);
}

static void PageMove(CONTROL *ctl, int dir, BOOL extend)
{
    LONG  line = TbLineFromPos(&ctl->tb, ctl->caret), tline;
    ULONG rs = 0;
    int   sub = SubRowOfPos(ctl, line, ctl->caret, &rs), tsub;
    int   count = ctl->nRows - 1, i;
    ULONG trs, tre, np;

    if (count < 1) count = 1;
    tline = line; tsub = sub;
    for (i = 0; i < count; i++)
    {
        if (dir < 0)
        {
            if (tsub > 0) tsub--;
            else if (tline > 0) { tline--; tsub = (int)CountLineRows(ctl, tline) - 1; }
            else break;
        }
        else
        {
            int rows = (int)CountLineRows(ctl, tline);
            if (tsub < rows - 1) tsub++;
            else if (tline < ctl->tb.nLines - 1) { tline++; tsub = 0; }
            else break;
        }
    }
    trs = RowStartOfSub(ctl, tline, tsub);
    tre = NextRowStart(ctl, tline, trs, TbLineEnd(&ctl->tb, tline));
    np  = PosAtColInRow(ctl, trs, tre, ctl->prefCol);
    MoveCaretTo(ctl, np, extend, FALSE);
}

static void InsertText(CONTROL *ctl, const char *s, ULONG n, BOOL bBreak)
{
    ReplaceSel(ctl, s, n, bBreak);
}

/* Virtual keys.  Returns FALSE if the key was not ours, so WM_CHAR can
   go on to consider the character half of the same event. */
static BOOL OnVirtualKey(CONTROL *ctl, USHORT vk, BOOL shift, BOOL ctrl)
{
    ULONG len   = TbLength(&ctl->tb);
    ULONG caret = ctl->caret;
    ULONG rs, re;

    switch (vk)
    {
    case VK_LEFT:
        if (ctrl) MoveCaretTo(ctl, WordLeft(ctl, caret), shift, TRUE);
        else if (!shift && ctl->anchor != ctl->caret)
            MoveCaretTo(ctl, SelMin(ctl), FALSE, TRUE);
        else MoveCaretTo(ctl, caret > 0 ? caret - 1 : 0, shift, TRUE);
        break;
    case VK_RIGHT:
        if (ctrl) MoveCaretTo(ctl, WordRight(ctl, caret), shift, TRUE);
        else if (!shift && ctl->anchor != ctl->caret)
            MoveCaretTo(ctl, SelMax(ctl), FALSE, TRUE);
        else MoveCaretTo(ctl, caret < len ? caret + 1 : len, shift, TRUE);
        break;
    case VK_UP:       VertMove(ctl, -1, shift); break;
    case VK_DOWN:     VertMove(ctl, +1, shift); break;
    case VK_PAGEUP:   PageMove(ctl, -1, shift); break;
    case VK_PAGEDOWN: PageMove(ctl, +1, shift); break;
    case VK_HOME:
        if (ctrl) MoveCaretTo(ctl, 0, shift, TRUE);
        else { CaretRowBounds(ctl, &rs, &re); MoveCaretTo(ctl, rs, shift, TRUE); }
        break;
    case VK_END:
        if (ctrl) MoveCaretTo(ctl, len, shift, TRUE);
        else { CaretRowBounds(ctl, &rs, &re); MoveCaretTo(ctl, re, shift, TRUE); }
        break;
    case VK_DELETE:
        if (ctl->anchor != ctl->caret)
            ReplaceSel(ctl, "", 0, TRUE);
        else if (caret < len)
        {
            ctl->anchor = caret; ctl->caret = caret + 1;
            ReplaceSel(ctl, "", 0, TRUE);
        }
        break;
    case VK_BACKSPACE:
        if (ctl->anchor != ctl->caret)
            ReplaceSel(ctl, "", 0, TRUE);
        else if (caret > 0)
        {
            ctl->anchor = caret - 1; ctl->caret = caret;
            ReplaceSel(ctl, "", 0, TRUE);
        }
        break;
    case VK_NEWLINE:
    case VK_ENTER:
        InsertText(ctl, "\n", 1, TRUE);
        break;
    case VK_TAB:
        InsertText(ctl, "\t", 1, FALSE);
        break;
    default:
        return FALSE;
    }
    return TRUE;
}

static void OnChar(CONTROL *ctl, USHORT c)
{
    char ch;
    if (c < 32 || c > 255) return;      /* control chars handled above */
    ch = (char)c;
    ReplaceSel(ctl, &ch, 1, FALSE);
}

/* One WM_CHAR carries the virtual key, the character, or both. */
static BOOL OnKey(CONTROL *ctl, MPARAM mp1, MPARAM mp2)
{
    USHORT fs   = SHORT1FROMMP(mp1);
    USHORT usch = SHORT1FROMMP(mp2);
    USHORT usvk = SHORT2FROMMP(mp2);
    BOOL   shift = (fs & KC_SHIFT) != 0;
    BOOL   ctrl  = (fs & KC_CTRL)  != 0;

    if (fs & KC_KEYUP)        return TRUE;      /* swallow the release */
    if (fs & KC_INVALIDCOMP)  return TRUE;

    if ((fs & KC_VIRTUALKEY) && OnVirtualKey(ctl, usvk, shift, ctrl))
        return TRUE;

    /* Ctrl+key and Alt+key belong to the accelerator table / menu. */
    if ((fs & KC_CHAR) && !(fs & KC_CTRL) && !(fs & KC_ALT))
    {
        OnChar(ctl, usch);
        return TRUE;
    }
    return FALSE;
}

static void OnButton1Down(CONTROL *ctl, LONG x, LONG y, BOOL shift)
{
    WinSetFocus(HWND_DESKTOP, ctl->hwnd);
    MoveCaretTo(ctl, XYToPos(ctl, x, y), shift, TRUE);
    WinSetCapture(HWND_DESKTOP, ctl->hwnd);
    ctl->bTracking = TRUE;
}

static void OnMouseMove(CONTROL *ctl, LONG x, LONG y)
{
    if (!ctl->bTracking) return;
    if (y > ctl->cyWin)        StepUpRow(ctl);   /* dragged above     */
    else if (y < ctl->yOrg)    StepDownRow(ctl); /* dragged below     */
    ctl->caret = XYToPos(ctl, x, y);
    ClampTop(ctl);
    EnsureCaretVisible(ctl);
    UpdateScrollbars(ctl);
    WinInvalidateRect(ctl->hwnd, NULL, FALSE);
    PlaceCaret(ctl);
}

static void OnButton1Up(CONTROL *ctl)
{
    if (ctl->bTracking)
    {
        WinSetCapture(HWND_DESKTOP, NULLHANDLE);
        ctl->bTracking = FALSE;
    }
}

static void OnDblClk(CONTROL *ctl, LONG x, LONG y)
{
    ULONG pos = XYToPos(ctl, x, y), len = TbLength(&ctl->tb), s, e;
    BOOL  onWord = (pos < len && IsWordCh(TbCharAt(&ctl->tb, pos))) ||
                   (pos > 0   && IsWordCh(TbCharAt(&ctl->tb, pos - 1)));
    if (onWord)
    {
        s = pos; e = pos;
        while (s > 0   && IsWordCh(TbCharAt(&ctl->tb, s - 1))) s--;
        while (e < len && IsWordCh(TbCharAt(&ctl->tb, e)))     e++;
        ctl->anchor = s; ctl->caret = e;
    }
    else ctl->anchor = ctl->caret = pos;

    UpdatePrefCol(ctl);
    WinInvalidateRect(ctl->hwnd, NULL, FALSE);
    PlaceCaret(ctl);
}

/*====================================================================
 * Scrolling
 *====================================================================*/

static void OnVScroll(CONTROL *ctl, USHORT cmd, SHORT pos)
{
    LONG nLines = ctl->tb.nLines;
    LONG vMax;
    int  i;

    switch (cmd)
    {
    case SB_LINEUP:    StepUpRow(ctl);   break;
    case SB_LINEDOWN:  StepDownRow(ctl); break;
    case SB_PAGEUP:    for (i = 0; i < ctl->nRows - 1; i++) if (!StepUpRow(ctl))   break; break;
    case SB_PAGEDOWN:  for (i = 0; i < ctl->nRows - 1; i++) if (!StepDownRow(ctl)) break; break;
    case SB_SLIDERTRACK:
    case SB_SLIDERPOSITION:
        vMax = (nLines - 1 > (LONG)SB_MAXRANGE) ? SB_MAXRANGE
             : (nLines - 1 < 0 ? 0 : nLines - 1);
        ctl->topLine = (vMax > 0 && nLines > 1)
                     ? (LONG)(((ULONG)pos * (ULONG)(nLines - 1)) / (ULONG)vMax)
                     : 0;
        ctl->topSub = 0;
        break;
    default: return;
    }
    ClampTop(ctl);
    UpdateScrollbars(ctl);
    WinInvalidateRect(ctl->hwnd, NULL, FALSE);
    PlaceCaret(ctl);
}

static void OnHScroll(CONTROL *ctl, USHORT cmd, SHORT pos)
{
    LONG hMax = (LONG)(ctl->maxCol > (ULONG)SB_MAXRANGE
                       ? (ULONG)SB_MAXRANGE : ctl->maxCol);

    if (ctl->bWrap) return;
    switch (cmd)
    {
    case SB_LINELEFT:   ctl->leftCol--; break;
    case SB_LINERIGHT:  ctl->leftCol++; break;
    case SB_PAGELEFT:   ctl->leftCol -= ctl->nCols; break;
    case SB_PAGERIGHT:  ctl->leftCol += ctl->nCols; break;
    case SB_SLIDERTRACK:
    case SB_SLIDERPOSITION: ctl->leftCol = pos; break;
    default: return;
    }
    if (ctl->leftCol > (int)hMax) ctl->leftCol = (int)hMax;
    if (ctl->leftCol < 0)         ctl->leftCol = 0;
    UpdateScrollbars(ctl);
    WinInvalidateRect(ctl->hwnd, NULL, FALSE);
    PlaceCaret(ctl);
}

/*====================================================================
 * Window procedure
 *====================================================================*/

MRESULT EXPENTRY EditWndProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    CONTROL *ctl = GETCTL(hwnd);

    switch (msg)
    {
    case WM_CREATE:
    {
        RECTL rcl;
        HPS   hps;

        ctl = (CONTROL *)malloc(sizeof(CONTROL));
        if (!ctl) return (MRESULT)TRUE;         /* TRUE aborts create */
        memset(ctl, 0, sizeof(CONTROL));
        ctl->hwnd = hwnd;
        if (!TbInit(&ctl->tb)) { free(ctl); return (MRESULT)TRUE; }
        SETCTL(hwnd, ctl);

        hps = WinGetPS(hwnd);
        PickFixedFont(ctl, hps);
        WinReleasePS(hps);

        ctl->hwndVSB = WinCreateWindow(hwnd, WC_SCROLLBAR, (PCSZ)"",
                            WS_VISIBLE | SBS_VERT, 0, 0, 0, 0,
                            hwnd, HWND_TOP, IDSB_VERT, NULL, NULL);
        ctl->hwndHSB = WinCreateWindow(hwnd, WC_SCROLLBAR, (PCSZ)"",
                            WS_VISIBLE | SBS_HORZ, 0, 0, 0, 0,
                            hwnd, HWND_TOP, IDSB_HORZ, NULL, NULL);

        WinQueryWindowRect(hwnd, &rcl);
        ctl->cxWin = rcl.xRight - rcl.xLeft;
        ctl->cyWin = rcl.yTop   - rcl.yBottom;

        LayoutBars(ctl);
        ComputeMetrics(ctl);
        FullRelayout(ctl);
        UpdateScrollbars(ctl);
        return (MRESULT)FALSE;
    }

    case WM_DESTROY:
        if (ctl) { TbFree(&ctl->tb); free(ctl); SETCTL(hwnd, NULL); }
        return (MRESULT)FALSE;

    case WM_SIZE:
        if (!ctl) break;
        ctl->cxWin = (LONG)SHORT1FROMMP(mp2);
        ctl->cyWin = (LONG)SHORT2FROMMP(mp2);
        LayoutBars(ctl);
        ComputeMetrics(ctl);
        if (ctl->bWrap) RecountTotal(ctl);  /* wrap depends on width */
        ClampTop(ctl);
        UpdateScrollbars(ctl);
        WinInvalidateRect(hwnd, NULL, FALSE);
        PlaceCaret(ctl);
        return (MRESULT)FALSE;

    case WM_ERASEBACKGROUND:
        return (MRESULT)FALSE;               /* painted in WM_PAINT  */

    case WM_PAINT:
        if (!ctl) break;
        OnPaint(ctl);
        return (MRESULT)FALSE;

    case WM_SETFOCUS:
        if (!ctl) break;
        if (SHORT1FROMMP(mp2))               /* gaining the focus     */
        {
            ctl->bFocus = TRUE;
            WinCreateCursor(hwnd, 0, 0, 2, ctl->cyLine, CURSOR_SOLID, NULL);
            PlaceCaret(ctl);
            WinShowCursor(hwnd, TRUE);
        }
        else
        {
            ctl->bFocus = FALSE;
            if (ctl->bTracking)
            {
                WinSetCapture(HWND_DESKTOP, NULLHANDLE);
                ctl->bTracking = FALSE;
            }
            WinDestroyCursor(hwnd);
        }
        return (MRESULT)FALSE;

    case WM_VSCROLL:
        if (!ctl) break;
        OnVScroll(ctl, SHORT2FROMMP(mp2), (SHORT)SHORT1FROMMP(mp2));
        return (MRESULT)FALSE;

    case WM_HSCROLL:
        if (!ctl) break;
        OnHScroll(ctl, SHORT2FROMMP(mp2), (SHORT)SHORT1FROMMP(mp2));
        return (MRESULT)FALSE;

    case WM_CHAR:
        if (!ctl) break;
        if (OnKey(ctl, mp1, mp2)) return (MRESULT)TRUE;
        break;

    case WM_BUTTON1DOWN:
        if (!ctl) break;
        OnButton1Down(ctl,
                      (LONG)(SHORT)SHORT1FROMMP(mp1),
                      (LONG)(SHORT)SHORT2FROMMP(mp1),
                      (WinGetKeyState(HWND_DESKTOP, VK_SHIFT) & 0x8000) != 0);
        return (MRESULT)TRUE;

    case WM_MOUSEMOVE:
        if (!ctl) break;
        OnMouseMove(ctl, (LONG)(SHORT)SHORT1FROMMP(mp1),
                         (LONG)(SHORT)SHORT2FROMMP(mp1));
        WinSetPointer(HWND_DESKTOP,
                      WinQuerySysPointer(HWND_DESKTOP, SPTR_TEXT, FALSE));
        return (MRESULT)TRUE;

    case WM_BUTTON1UP:
        if (!ctl) break;
        OnButton1Up(ctl);
        return (MRESULT)TRUE;

    case WM_BUTTON1DBLCLK:
        if (!ctl) break;
        OnDblClk(ctl, (LONG)(SHORT)SHORT1FROMMP(mp1),
                      (LONG)(SHORT)SHORT2FROMMP(mp1));
        return (MRESULT)TRUE;
    }

    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

/*====================================================================
 * Class registration
 *====================================================================*/

BOOL EditRegister(HAB hab)
{
    habEdit = hab;
    return WinRegisterClass(hab, (PCSZ)ENHEDIT_CLASS, EditWndProc,
                            CS_SIZEREDRAW | CS_CLIPCHILDREN,
                            sizeof(PVOID));
}

/*====================================================================
 * Public API
 *====================================================================*/

BOOL EditLoad(HWND hwnd, const char *path)
{
    CONTROL *ctl = GETCTL(hwnd);
    int err;
    if (!TbLoadFile(&ctl->tb, path, &err)) return FALSE;
    ctl->caret = ctl->anchor = 0;
    ctl->topLine = 0; ctl->topSub = 0; ctl->leftCol = 0;
    FullRelayout(ctl);
    Refresh(ctl);
    return TRUE;
}

BOOL EditSave(HWND hwnd, const char *path)
{
    CONTROL *ctl = GETCTL(hwnd);
    int err;
    return TbSaveFile(&ctl->tb, path, &err);
}

void EditNewFile(HWND hwnd)
{
    CONTROL *ctl = GETCTL(hwnd);
    TbClear(&ctl->tb);
    ctl->caret = ctl->anchor = 0;
    ctl->topLine = 0; ctl->topSub = 0; ctl->leftCol = 0;
    FullRelayout(ctl);
    Refresh(ctl);
}

BOOL EditIsModified (HWND hwnd) { return TbIsModified(&GETCTL(hwnd)->tb); }
void EditSetModified(HWND hwnd, BOOL b) { TbSetModified(&GETCTL(hwnd)->tb, b); }

void EditSetWordWrap(HWND hwnd, BOOL bWrap)
{
    CONTROL *ctl = GETCTL(hwnd);
    if ((ctl->bWrap != 0) == (bWrap != 0)) return;
    ctl->bWrap = bWrap;
    ctl->leftCol = 0;
    LayoutBars(ctl);            /* the H bar comes and goes with wrap */
    ComputeMetrics(ctl);        /* ... which changes the text height  */
    FullRelayout(ctl);
    EnsureCaretVisible(ctl);
    Refresh(ctl);
}

BOOL EditGetWordWrap(HWND hwnd) { return GETCTL(hwnd)->bWrap; }

BOOL EditCanUndo(HWND hwnd) { return TbCanUndo(&GETCTL(hwnd)->tb); }
BOOL EditCanRedo(HWND hwnd) { return TbCanRedo(&GETCTL(hwnd)->tb); }

void EditUndo(HWND hwnd)
{
    CONTROL *ctl = GETCTL(hwnd);
    ULONG c;
    if (TbUndo(&ctl->tb, &c)) { ctl->caret = ctl->anchor = c; AfterBulkEdit(ctl); }
}

void EditRedo(HWND hwnd)
{
    CONTROL *ctl = GETCTL(hwnd);
    ULONG c;
    if (TbRedo(&ctl->tb, &c)) { ctl->caret = ctl->anchor = c; AfterBulkEdit(ctl); }
}

BOOL EditHasSel(HWND hwnd) { CONTROL *c = GETCTL(hwnd); return c->anchor != c->caret; }

BOOL EditCanPaste(HWND hwnd)
{
    ULONG fl = 0;
    (void)hwnd;
    return WinQueryClipbrdFmtInfo(habEdit, CF_TEXT, &fl);
}

void EditSelectAll(HWND hwnd)
{
    CONTROL *ctl = GETCTL(hwnd);
    ctl->anchor = 0;
    ctl->caret  = TbLength(&ctl->tb);
    WinInvalidateRect(hwnd, NULL, FALSE);
    PlaceCaret(ctl);
}

/*--------------------------------------------------------------------
 * Clipboard.  PM wants CF_TEXT as a CFI_POINTER into shared, giveable
 * memory; once handed over the block belongs to the system, so it is
 * only freed on the path where WinOpenClipbrd fails.
 *------------------------------------------------------------------*/

void EditCopy(HWND hwnd)
{
    CONTROL *ctl = GETCTL(hwnd);
    ULONG selMin = SelMin(ctl), selMax = SelMax(ctl), len, nl, i, out;
    PVOID pmem = NULL;
    char *p;

    if (selMax <= selMin) return;
    len = selMax - selMin;

    /* Count newlines to size the LF->CRLF expansion. */
    nl = 0;
    for (i = 0; i < len; i++)
        if (TbCharAt(&ctl->tb, selMin + i) == '\n') nl++;

    if (DosAllocSharedMem(&pmem, NULL, len + nl + 1,
                          PAG_COMMIT | PAG_READ | PAG_WRITE | OBJ_GIVEABLE)
        != NO_ERROR)
        return;

    p = (char *)pmem;
    out = 0;
    for (i = 0; i < len; i++)
    {
        char c = TbCharAt(&ctl->tb, selMin + i);
        if (c == '\n') p[out++] = '\r';
        p[out++] = c;
    }
    p[out] = '\0';

    if (WinOpenClipbrd(habEdit))
    {
        WinEmptyClipbrd(habEdit);
        WinSetClipbrdData(habEdit, (ULONG)pmem, CF_TEXT, CFI_POINTER);
        WinCloseClipbrd(habEdit);
    }
    else DosFreeMem(pmem);
}

void EditPaste(HWND hwnd)
{
    CONTROL *ctl = GETCTL(hwnd);
    char *src, *dst;
    ULONG n, i, out;

    if (!WinOpenClipbrd(habEdit)) return;

    src = (char *)WinQueryClipbrdData(habEdit, CF_TEXT);
    if (!src) { WinCloseClipbrd(habEdit); return; }

    n = 0;
    while (src[n] != '\0') n++;             /* measure to the NUL     */

    dst = (char *)malloc(n ? n : 1);
    if (!dst) { WinCloseClipbrd(habEdit); return; }

    out = 0;
    for (i = 0; i < n; i++)
    {
        char c = src[i];
        if (c == '\r' && i + 1 < n && src[i + 1] == '\n') continue;
        dst[out++] = c;
    }
    WinCloseClipbrd(habEdit);               /* done with clipboard mem */

    ReplaceSel(ctl, dst, out, TRUE);
    free(dst);
}

void EditDeleteSel(HWND hwnd)
{
    CONTROL *ctl = GETCTL(hwnd);
    if (ctl->anchor != ctl->caret)
        ReplaceSel(ctl, "", 0, TRUE);
}

void EditCut(HWND hwnd)
{
    if (EditHasSel(hwnd)) { EditCopy(hwnd); EditDeleteSel(hwnd); }
}

BOOL EditFindNext(HWND hwnd, const char *needle, BOOL bMatchCase)
{
    CONTROL *ctl = GETCTL(hwnd);
    ULONG start = SelMax(ctl), found;

    if (TbFind(&ctl->tb, start, needle, bMatchCase, &found))
    {
        ctl->anchor = found;
        ctl->caret  = found + (ULONG)strlen(needle);
        EnsureCaretVisible(ctl);
        Refresh(ctl);
        return TRUE;
    }
    return FALSE;
}
