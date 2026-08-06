/*===========================================================================
 * PAINT.H  -  Drawing, hit-testing, and scroll helper declarations
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * THE COORDINATE MODEL
 *
 *   PM's origin is the bottom-left and y grows upwards; a grid of icons is a
 *   stack of rows counted downwards from the top.  Rather than flip the
 *   layout arithmetic -- which is shared with the hit-tester and the scroll
 *   maths and would then have to be flipped in three places -- the layout
 *   stays in CONTENT-LOGICAL
 *   coordinates: origin at the top-left of the content area, y DOWN,
 *   unscrolled.  Two functions do the turning over, and painting and
 *   hit-testing both go through them, so they cannot drift apart:
 *
 *       CellRect()      logical box  ->  PM rectangle on the target surface
 *       PointToLogical() PM point     ->  logical point
 *
 *   The "target surface" is described by a RECTL: the window's content area
 *   when painting straight to the screen, or (0,0,cx,cy) when painting into
 *   the off-screen buffer.  The scroll offset is folded in by CellRect, so
 *   the same drawing code serves both.
 *===========================================================================*/

#ifndef PAINT_H
#define PAINT_H

/* The content area's own scrollbars.  PM has no WS_VSCROLL style to hang on
   an ordinary window, so the client creates these and insets around them. */
extern HWND hwndVScroll;
extern HWND hwndHScroll;

void  LoadViewBitmaps(int nView);
void  FreeViewBitmaps(void);

/* The client rectangle less the toolbar, the status bar and the scrollbars,
   in client (PM, bottom-left) coordinates. */
void  ContentRect(HWND hwndCli, PRECTL prcl);

/* Logical box -> PM rectangle on the surface described by pcont */
void  CellRect(const RECTL *pcont, LONG lx, LONG ly, LONG cw, LONG ch,
               PRECTL prclOut);

/* PM point on pcont -> content-logical point */
void  PointToLogical(const RECTL *pcont, LONG x, LONG y, LONG *plx, LONG *ply);

LONG  GetViewCellHeight(void);
int   GetViewColumns(HWND hwndCli);
void  CalcContentSize(LONG cxVisible, LONG *pcx, LONG *pcy);
void  UpdateScrollBars(HWND hwndCli);

/* Pixels per scrollbar unit.  A PM scrollbar's position is a SHORT, which
   cannot describe a content area taller than 32767 pixels; these convert
   between the two at the scrollbar boundary and nowhere else. */
LONG  ScrollScaleY(void);
LONG  ScrollScaleX(void);
void  EnsureItemVisible(HWND hwndCli, int nIdx);

void  DrawDrives(HPS hps, const RECTL *pcont);
void  DrawFiles (HPS hps, const RECTL *pcont);

int   HitTestDrive(HWND hwndCli, LONG x, LONG y);
int   HitTestFile (HWND hwndCli, LONG x, LONG y);

#endif /* PAINT_H */
