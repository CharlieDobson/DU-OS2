/*===========================================================================
 * DRAW.CPP  -  DiskStat drawing: 3-D pie chart, legend, tree view, status bar
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * Two things to keep in mind while reading this file:
 *
 *  1. COORDINATES.  PM's origin is bottom-left and y grows upwards: panes are
 *     measured down from yTop, and the pie's 3-D depth layers stack towards
 *     -y.  GpiCharStringAt takes a BASELINE origin, so text y is offset by
 *     lMaxDescender.
 *
 *  2. TEXT IS MEASURED, NOT COUNTED.  Folder names are truncated by measuring
 *     the string against the real column width with GpiQueryTextBox, so a long
 *     HPFS name is fitted and ellipsised rather than chopped at a fixed
 *     character count.
 *
 * Nothing here reads the model unless data_valid is set: while a scan runs,
 * the worker thread owns tree_nodes[] and there is no lock.  The early-out on
 * !data_valid in each entry point is load-bearing, not cosmetic.
 *===========================================================================*/
#define INCL_WIN
#define INCL_GPI

#include <os2.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "diskstat.h"

#define FOLDER_ICON_W   14
#define FOLDER_ICON_H   10
#define ICON_COL_W      18          /* icon width + gap before text           */
#define SCROLLBAR_PAD   4
#define COL_PCT_W       56          /* "% Used" / "100.0%"                    */
#define COL_SIZE_W      96          /* "1234.5 MB"                            */

// --------------------------------------------------------------------------
// File-local prototypes
// --------------------------------------------------------------------------

static void  Metrics       ( HPS hps, PFONTMETRICS pfm );
static FIXED FixedDeg      ( double deg );
static void  FillBox       ( HPS hps, LONG l, LONG b, LONG r, LONG t,
                             LONG rgb );
static void  Line          ( HPS hps, LONG x1, LONG y1, LONG x2, LONG y2,
                             LONG rgb );
static void  BoxOutline    ( HPS hps, LONG l, LONG b, LONG r, LONG t,
                             LONG rgb );
static void  TextAt        ( HPS hps, LONG x, LONG yBase, LONG rgb,
                             const char *s );
static void  TextClipped   ( HPS hps, LONG x, LONG yBase, LONG maxw,
                             LONG rgb, const char *s );
static void  PieSlice      ( HPS hps, LONG cx, LONG cy, LONG rx, LONG ry,
                             double startDeg, double sweepDeg, LONG rgb );
static void  PieSliceEdge  ( HPS hps, LONG cx, LONG cy, LONG rx, LONG ry,
                             double startDeg, double sweepDeg, LONG rgb );
static void  PieDisc       ( HPS hps, LONG cx, LONG cy, LONG rx, LONG ry,
                             LONG rgb, BOOL outline );
static void  DrawFolderIcon( HPS hps, LONG x, LONG yBottom, LONG row_h );
static void  DrawExpandBox ( HPS hps, LONG bx, LONG bcy, BOOL expanded );
static void  DrawTreeRow   ( HPS hps, PRECTL prcl, int i, LONG yBottom,
                             LONG row_h, LONG col2, LONG col3,
                             PFONTMETRICS pfm );
static void  DrawLegendEntry( HPS hps, LONG left, LONG yBottom, LONG row_h,
                              LONG rgb, const char *label,
                              unsigned long size, unsigned long total,
                              PFONTMETRICS pfm );
static void  TreeColumns   ( PRECTL prcl, LONG *col2, LONG *col3 );

// ==========================================================================
// Small GPI helpers
// ==========================================================================

// --------------------------------------------------------------------------
// DsSetRgbMode - put the presentation space into direct-RGB colour mode
//
//   A PS starts in index mode against the default colour table, where
//   GpiSetColor takes a CLR_* index.  Switching to LCOLF_RGB lets the whole
//   file speak in 0x00RRGGBB.  CACHED PSs ARE RESET BETWEEN PAINTS, so every
//   WM_PAINT must call this before any colour constant means anything.
// --------------------------------------------------------------------------

void DsSetRgbMode( HPS hps )
{
    GpiCreateLogColorTable( hps, 0, LCOLF_RGB, 0, 0, (PLONG)NULL );
    GpiSetBackMix( hps, BM_LEAVEALONE );        /* transparent text           */
}

static void Metrics( HPS hps, PFONTMETRICS pfm )
{
    memset( pfm, 0, sizeof( FONTMETRICS ) );
    GpiQueryFontMetrics( hps, sizeof( FONTMETRICS ), pfm );
}

// --------------------------------------------------------------------------
// FixedDeg - degrees to the 16.16 FIXED the Gpi arc calls want
// --------------------------------------------------------------------------

static FIXED FixedDeg( double deg )
{
    if ( deg < 0.0 )     deg = 0.0;
    if ( deg > 360.0 )   deg = 360.0;
    return (FIXED)(LONG)( deg * 65536.0 );
}

// --------------------------------------------------------------------------
// FillBox / Line / BoxOutline - rectangle fill, line, rectangle outline
//
//   FillBox takes a half-open rect (right / top exclusive) to match RECTL and
//   WinFillRect; BoxOutline is inclusive, because GpiBox is.
// --------------------------------------------------------------------------

static void FillBox( HPS hps, LONG l, LONG b, LONG r, LONG t, LONG rgb )
{
    RECTL rcl;

    if ( r <= l || t <= b ) return;

    rcl.xLeft   = l;
    rcl.yBottom = b;
    rcl.xRight  = r;
    rcl.yTop    = t;
    WinFillRect( hps, &rcl, rgb );
}

static void Line( HPS hps, LONG x1, LONG y1, LONG x2, LONG y2, LONG rgb )
{
    POINTL ptl;

    GpiSetColor( hps, rgb );
    ptl.x = x1; ptl.y = y1;
    GpiMove( hps, &ptl );
    ptl.x = x2; ptl.y = y2;
    GpiLine( hps, &ptl );
}

static void BoxOutline( HPS hps, LONG l, LONG b, LONG r, LONG t, LONG rgb )
{
    POINTL ptl;

    GpiSetColor( hps, rgb );
    ptl.x = l; ptl.y = b;
    GpiMove( hps, &ptl );
    ptl.x = r; ptl.y = t;
    GpiBox( hps, DRO_OUTLINE, &ptl, 0, 0 );
}

// --------------------------------------------------------------------------
// TextAt - draw a string with its baseline at (x, yBase)
// --------------------------------------------------------------------------

static void TextAt( HPS hps, LONG x, LONG yBase, LONG rgb, const char *s )
{
    POINTL ptl;
    LONG   len = (LONG)strlen( s );

    if ( len <= 0 ) return;

    GpiSetColor( hps, rgb );
    ptl.x = x;
    ptl.y = yBase;
    GpiCharStringAt( hps, &ptl, len, (PCH)s );
}

// --------------------------------------------------------------------------
// TextClipped - like TextAt, but fitted to maxw pixels with an ellipsis
//
//   Binary search on the character count, so a 250-character HPFS name costs
//   ~8 GpiQueryTextBox calls rather than 250.
// --------------------------------------------------------------------------

static void TextClipped( HPS hps, LONG x, LONG yBase, LONG maxw,
                         LONG rgb, const char *s )
{
    char   buf[MAX_NAME_LEN + 8];
    POINTL aptl[TXTBOX_COUNT];
    POINTL ptl;
    LONG   full, lo, hi, best;

    full = (LONG)strlen( s );
    if ( full <= 0 || maxw <= 0 ) return;
    if ( full > MAX_NAME_LEN ) full = MAX_NAME_LEN;

    memcpy( buf, s, full );
    buf[full] = '\0';

    GpiQueryTextBox( hps, full, (PCH)buf, TXTBOX_COUNT, aptl );
    if ( aptl[TXTBOX_BOTTOMRIGHT].x - aptl[TXTBOX_BOTTOMLEFT].x <= maxw ) {
        GpiSetColor( hps, rgb );
        ptl.x = x; ptl.y = yBase;
        GpiCharStringAt( hps, &ptl, full, (PCH)buf );
        return;
    }

    // largest prefix whose "prefix..." still fits
    lo = 0; hi = full; best = 0;
    while ( lo <= hi ) {
        LONG mid = ( lo + hi ) / 2;

        memcpy( buf, s, mid );
        strcpy( buf + mid, "..." );

        GpiQueryTextBox( hps, mid + 3, (PCH)buf, TXTBOX_COUNT, aptl );
        if ( aptl[TXTBOX_BOTTOMRIGHT].x - aptl[TXTBOX_BOTTOMLEFT].x <= maxw ) {
            best = mid;
            lo = mid + 1;
        }
        else {
            hi = mid - 1;
        }
    }

    memcpy( buf, s, best );
    strcpy( buf + best, "..." );

    GpiSetColor( hps, rgb );
    ptl.x = x; ptl.y = yBase;
    GpiCharStringAt( hps, &ptl, best + 3, (PCH)buf );
}

// ==========================================================================
// Shared geometry - PMMAIN.CPP hit-tests with exactly these numbers
// ==========================================================================

LONG DsRowHeight( HPS hps )
{
    FONTMETRICS fm;
    LONG rh;

    Metrics( hps, &fm );
    rh = fm.lMaxBaselineExt + fm.lExternalLeading;
    if ( rh < 12 ) rh = 12;
    return rh;
}

LONG DsHeaderHeight( HPS hps )
{
    return DsRowHeight( hps ) + 6;
}

LONG DsIndentX( PRECTL prcl, int depth )
{
    return prcl->xLeft + 4 + (LONG)depth * INDENT_WIDTH;
}

// --------------------------------------------------------------------------
// TreeColumns - anchor Size and % Used at fixed widths from the right edge,
//   so they never collide on a narrow pane; Folder takes what is left.
// --------------------------------------------------------------------------

static void TreeColumns( PRECTL prcl, LONG *col2, LONG *col3 )
{
    LONG right_edge = prcl->xRight - SCROLLBAR_PAD;
    LONG min_folder = prcl->xLeft + 4 + ICON_COL_W + 40;

    *col3 = right_edge - COL_PCT_W;
    *col2 = *col3 - COL_SIZE_W;

    if ( *col2 < min_folder ) {
        *col2 = min_folder;
        *col3 = *col2 + COL_SIZE_W;
    }
}

// ==========================================================================
// FormatSize
// ==========================================================================

void FormatSize( unsigned long bytes, char *buffer )
{
    unsigned long whole, frac;

    if ( bytes >= 1048576L ) {
        whole = bytes / 1048576L;
        frac  = ( ( bytes % 1048576L ) * 10L ) / 1048576L;
        sprintf( buffer, "%lu.%lu MB", whole, frac );
    }
    else if ( bytes >= 1024L ) {
        sprintf( buffer, "%lu KB", bytes / 1024L );
    }
    else {
        sprintf( buffer, "%lu bytes", bytes );
    }
}

// ==========================================================================
// Pie chart
// ==========================================================================

// --------------------------------------------------------------------------
// PieSlice - filled wedge from the ellipse centre
//
//   GpiPartialArc draws a line from the current point to the start of the arc
//   and then the arc itself; starting at the centre and closing the area gives
//   exactly the wedge GDI's Pie() produced.
// --------------------------------------------------------------------------

static void PieSlice( HPS hps, LONG cx, LONG cy, LONG rx, LONG ry,
                      double startDeg, double sweepDeg, LONG rgb )
{
    ARCPARAMS ap;
    POINTL    ptl;

    if ( sweepDeg <= 0.0 || rx <= 0 || ry <= 0 ) return;

    ap.lP = rx; ap.lQ = ry; ap.lR = 0; ap.lS = 0;
    GpiSetArcParams( hps, &ap );

    ptl.x = cx; ptl.y = cy;

    GpiSetColor( hps, rgb );
    GpiBeginArea( hps, BA_ALTERNATE );
    GpiMove( hps, &ptl );
    GpiPartialArc( hps, &ptl, MAKEFIXED( 1, 0 ),
                   FixedDeg( startDeg ), FixedDeg( sweepDeg ) );
    GpiEndArea( hps );
}

// --------------------------------------------------------------------------
// PieSliceEdge - the same wedge, outlined only
// --------------------------------------------------------------------------

static void PieSliceEdge( HPS hps, LONG cx, LONG cy, LONG rx, LONG ry,
                          double startDeg, double sweepDeg, LONG rgb )
{
    ARCPARAMS ap;
    POINTL    ptl;

    if ( sweepDeg <= 0.0 || rx <= 0 || ry <= 0 ) return;

    ap.lP = rx; ap.lQ = ry; ap.lR = 0; ap.lS = 0;
    GpiSetArcParams( hps, &ap );

    ptl.x = cx; ptl.y = cy;

    GpiSetColor( hps, rgb );
    GpiMove( hps, &ptl );
    GpiPartialArc( hps, &ptl, MAKEFIXED( 1, 0 ),
                   FixedDeg( startDeg ), FixedDeg( sweepDeg ) );
    GpiLine( hps, &ptl );                       /* close back to the centre  */
}

// --------------------------------------------------------------------------
// PieDisc - the whole ellipse (the "entirely used" / "entirely free" case)
// --------------------------------------------------------------------------

static void PieDisc( HPS hps, LONG cx, LONG cy, LONG rx, LONG ry,
                     LONG rgb, BOOL outline )
{
    ARCPARAMS ap;
    POINTL    ptl;

    if ( rx <= 0 || ry <= 0 ) return;

    ap.lP = rx; ap.lQ = ry; ap.lR = 0; ap.lS = 0;
    GpiSetArcParams( hps, &ap );

    ptl.x = cx; ptl.y = cy;
    GpiSetColor( hps, rgb );
    GpiMove( hps, &ptl );
    GpiFullArc( hps, outline ? DRO_OUTLINEFILL : DRO_FILL, MAKEFIXED( 1, 0 ) );
}

// --------------------------------------------------------------------------
// DrawPieChart - 3-D pie of disk usage.
//   With a folder selected: 3 slices - other-used (blue), the selection
//   (yellow), free (green).  Otherwise 2.
// --------------------------------------------------------------------------

void DrawPieChart( HPS hps, PRECTL prcl )
{
    LONG   cx, cy, rx, ry, chart_height, d;
    double sel_size, other_used;
    double sel_deg, other_deg, used_deg, free_deg;
    double min_sel_deg;
    BOOL   have_sel;

    if ( !data_valid || total_disk_space == 0L ) return;

    chart_height = ( prcl->yTop - prcl->yBottom ) * 2 / 3;
    cx = ( prcl->xLeft + prcl->xRight ) / 2;
    rx = ( prcl->xRight - prcl->xLeft ) / 2 - PIE_MARGIN;
    ry = chart_height / 3 - PIE_MARGIN;         /* squished for the 3-D look  */

    if ( rx < 20 || ry < 14 ) return;

    // top face sits below the pane top, so the depth layers have room under it
    cy = prcl->yTop - PIE_MARGIN - ry;

    have_sel = (BOOL)( selected_node >= 0 && selected_node < tree_count );
    sel_size = have_sel
        ? (double)tree_nodes[selected_node].total_size : 0.0;

    other_used = (double)used_disk_space - sel_size;
    if ( other_used < 0.0 ) other_used = 0.0;

    used_deg  = ( (double)used_disk_space / (double)total_disk_space ) * 360.0;
    free_deg  = 360.0 - used_deg;
    sel_deg   = ( sel_size   / (double)total_disk_space ) * 360.0;
    other_deg = ( other_used / (double)total_disk_space ) * 360.0;

    // keep a tiny selection visible: floor it at 1.5% of the pie and take the
    // difference out of the "other used" slice
    min_sel_deg = 360.0 * 0.015;
    if ( have_sel && sel_size > 0.0 && sel_deg < min_sel_deg ) {
        sel_deg   = min_sel_deg;
        other_deg = ( other_deg > 0.0 ) ? ( 360.0 - sel_deg - free_deg ) : 0.0;
        if ( other_deg < 0.0 ) other_deg = 0.0;
        used_deg  = sel_deg + other_deg;
    }

    if ( used_disk_space > 0L && free_disk_space > 0L ) {

        // ---- depth layers, bottom up (towards -y in PM) ----
        for ( d = PIE_DEPTH; d > 0; d-- ) {
            LONG dy = cy - d;

            if ( other_used > 0.0 )
                PieSlice( hps, cx, dy, rx, ry, 0.0, other_deg, DS_USED_DK );

            if ( have_sel && sel_size > 0.0 )
                PieSlice( hps, cx, dy, rx, ry, other_deg, sel_deg, DS_SEL_DK );

            if ( free_disk_space > 0L )
                PieSlice( hps, cx, dy, rx, ry, used_deg, free_deg, DS_FREE_DK );
        }

        // ---- top face ----
        if ( other_used > 0.0 ) {
            PieSlice    ( hps, cx, cy, rx, ry, 0.0, other_deg, DS_USED );
            PieSliceEdge( hps, cx, cy, rx, ry, 0.0, other_deg, DS_BLACK );
        }

        if ( have_sel && sel_size > 0.0 ) {
            PieSlice    ( hps, cx, cy, rx, ry, other_deg, sel_deg, DS_SEL );
            PieSliceEdge( hps, cx, cy, rx, ry, other_deg, sel_deg, DS_BLACK );
        }

        if ( free_disk_space > 0L ) {
            PieSlice    ( hps, cx, cy, rx, ry, used_deg, free_deg, DS_FREE );
            PieSliceEdge( hps, cx, cy, rx, ry, used_deg, free_deg, DS_BLACK );
        }
    }
    else {
        // entirely used or entirely free
        LONG dark  = ( free_disk_space == 0L ) ? DS_USED_DK : DS_FREE_DK;
        LONG light = ( free_disk_space == 0L ) ? DS_USED    : DS_FREE;

        for ( d = PIE_DEPTH; d > 0; d-- )
            PieDisc( hps, cx, cy - d, rx, ry, dark, FALSE );

        PieDisc( hps, cx, cy, rx, ry, light, FALSE );
        GpiSetColor( hps, DS_BLACK );
        PieDisc( hps, cx, cy, rx, ry, light, TRUE );
    }
}

// --------------------------------------------------------------------------
// DrawLegendEntry - one legend row: swatch + "label pct% (size)"
// --------------------------------------------------------------------------

static void DrawLegendEntry( HPS hps, LONG left, LONG yBottom, LONG row_h,
                             LONG rgb, const char *label,
                             unsigned long size, unsigned long total,
                             PFONTMETRICS pfm )
{
    char          text[MAX_NAME_LEN + 64];
    char          size_buf[24];
    unsigned long pct10;
    LONG          sw_b, yBase;

    // 10x10 swatch, vertically centred in the row
    sw_b = yBottom + ( row_h - 10 ) / 2;
    FillBox   ( hps, left, sw_b, left + 10, sw_b + 10, rgb );
    BoxOutline( hps, left, sw_b, left + 9,  sw_b + 9,  DS_BLACK );

    pct10 = ( total > 0L )
          ? (unsigned long)( (double)size / (double)total * 1000.0 ) : 0L;
    FormatSize( size, size_buf );
    sprintf( text, "%s  %lu.%lu%%  (%s)",
             label, pct10 / 10L, pct10 % 10L, size_buf );

    yBase = yBottom + pfm->lMaxDescender;
    TextAt( hps, left + 16, yBase, DS_BLACK, text );
}

// --------------------------------------------------------------------------
// DrawPieLegend - colour-coded legend below the pie.
//   Two entries normally, three when a folder is selected.
// --------------------------------------------------------------------------

void DrawPieLegend( HPS hps, PRECTL prcl )
{
    FONTMETRICS   fm;
    LONG          legend_top, legend_left, row_h, y, ry, chart_height;
    unsigned long used_val;
    BOOL          have_sel;

    if ( !data_valid || total_disk_space == 0L ) return;

    Metrics( hps, &fm );
    row_h = DsRowHeight( hps ) + 4;

    chart_height = ( prcl->yTop - prcl->yBottom ) * 2 / 3;
    ry = chart_height / 3 - PIE_MARGIN;

    // just under the deepest depth layer of the pie
    legend_top  = prcl->yTop - PIE_MARGIN - 2 * ry - PIE_DEPTH - 8;
    legend_left = prcl->xLeft + PIE_MARGIN;

    have_sel = (BOOL)( selected_node >= 0 && selected_node < tree_count );

    y = legend_top - row_h;

    // "Used" - the selection is broken out into its own entry, so subtract it
    // here.  A subtree can read larger than the used total when the volume was
    // clamped, so guard the subtraction rather than wrapping it.
    used_val = used_disk_space;
    if ( have_sel ) {
        unsigned long sel = tree_nodes[selected_node].total_size;
        used_val = ( sel < used_disk_space ) ? ( used_disk_space - sel ) : 0L;
    }

    if ( y >= prcl->yBottom ) {
        DrawLegendEntry( hps, legend_left, y, row_h, DS_USED,
                         "Used", used_val, total_disk_space, &fm );
    }

    // the selected folder
    if ( have_sel ) {
        y -= row_h;
        if ( y >= prcl->yBottom ) {
            DrawLegendEntry( hps, legend_left, y, row_h, DS_SEL,
                             tree_nodes[selected_node].name,
                             tree_nodes[selected_node].total_size,
                             total_disk_space, &fm );
        }
    }

    // "Free"
    y -= row_h;
    if ( y >= prcl->yBottom ) {
        DrawLegendEntry( hps, legend_left, y, row_h, DS_FREE,
                         "Free", free_disk_space, total_disk_space, &fm );
    }
}

// ==========================================================================
// Tree view
// ==========================================================================

// --------------------------------------------------------------------------
// DrawFolderIcon - small folder glyph, vertically centred in the row
// --------------------------------------------------------------------------

static void DrawFolderIcon( HPS hps, LONG x, LONG yBottom, LONG row_h )
{
    LONG iy = yBottom + ( row_h - FOLDER_ICON_H ) / 2;

    // tab, on top of the body (PM: larger y is higher up)
    FillBox   ( hps, x, iy + FOLDER_ICON_H - 4, x + 6, iy + FOLDER_ICON_H,
                DS_FOLDER_TAB );
    BoxOutline( hps, x, iy + FOLDER_ICON_H - 4, x + 5, iy + FOLDER_ICON_H - 1,
                DS_FOLDER_EDGE );

    // body
    FillBox   ( hps, x, iy, x + FOLDER_ICON_W, iy + FOLDER_ICON_H - 2,
                DS_FOLDER_BODY );
    BoxOutline( hps, x, iy, x + FOLDER_ICON_W - 1, iy + FOLDER_ICON_H - 3,
                DS_FOLDER_EDGE );
}

// --------------------------------------------------------------------------
// DrawExpandBox - the [+] / [-] glyph, centred on bcy
// --------------------------------------------------------------------------

static void DrawExpandBox( HPS hps, LONG bx, LONG bcy, BOOL expanded )
{
    FillBox   ( hps, bx, bcy - 4, bx + EXPAND_BOX_SIZE,
                bcy + EXPAND_BOX_SIZE - 4, DS_WHITE );
    BoxOutline( hps, bx, bcy - 4, bx + EXPAND_BOX_SIZE - 1,
                bcy + EXPAND_BOX_SIZE - 5, DS_SHADOW );

    Line( hps, bx + 2, bcy, bx + 6, bcy, DS_BLACK );
    if ( !expanded )
        Line( hps, bx + 4, bcy - 2, bx + 4, bcy + 2, DS_BLACK );
}

// --------------------------------------------------------------------------
// DrawTreeRow - one row of the tree view
// --------------------------------------------------------------------------

static void DrawTreeRow( HPS hps, PRECTL prcl, int i, LONG yBottom,
                         LONG row_h, LONG col2, LONG col3,
                         PFONTMETRICS pfm )
{
    LONG          indent, icon_x, name_x, yBase, text_rgb;
    int           has_kids;
    unsigned long pct10;
    char          text[64];
    char          size_buf[24];

    indent   = DsIndentX( prcl, (int)tree_nodes[i].depth );
    has_kids = ( tree_nodes[i].flags & NODE_HAS_CHILDREN );

    // row background
    if ( i == selected_node ) {
        FillBox( hps, prcl->xLeft, yBottom, prcl->xRight, yBottom + row_h,
                 DS_SELROW );
        text_rgb = DS_WHITE;
    }
    else {
        if ( i % 2 == 0 ) {
            FillBox( hps, prcl->xLeft, yBottom, prcl->xRight, yBottom + row_h,
                     DS_ALTROW );
        }
        text_rgb = DS_BLACK;
    }

    if ( has_kids )
        DrawExpandBox( hps, indent, yBottom + row_h / 2,
                       (BOOL)( tree_nodes[i].flags & NODE_EXPANDED ) );

    // always reserve the expand-box width so the columns line up
    icon_x = indent + EXPAND_BOX_SIZE + 3;
    DrawFolderIcon( hps, icon_x, yBottom, row_h );

    yBase  = yBottom + pfm->lMaxDescender;
    name_x = icon_x + ICON_COL_W;

    TextClipped( hps, name_x, yBase, col2 - name_x - 6, text_rgb,
                 tree_nodes[i].name );

    FormatSize( tree_nodes[i].total_size, size_buf );
    TextAt( hps, col2, yBase, text_rgb, size_buf );

    pct10 = ( total_disk_space > 0L )
          ? (unsigned long)( (double)tree_nodes[i].total_size
                             / (double)total_disk_space * 1000.0 )
          : 0L;
    sprintf( text, "%lu.%lu%%", pct10 / 10L, pct10 % 10L );
    TextAt( hps, col3, yBase, text_rgb, text );
}

// --------------------------------------------------------------------------
// DrawTreeView - the right-pane folder tree
// --------------------------------------------------------------------------

void DrawTreeView( HPS hps, PRECTL prcl )
{
    FONTMETRICS fm;
    LONG        row_h, hdr_h, col2, col3, y, hdr_base;
    int         i;

    if ( !data_valid ) return;

    Metrics( hps, &fm );
    row_h = DsRowHeight( hps );
    hdr_h = DsHeaderHeight( hps );

    TreeColumns( prcl, &col2, &col3 );

    // column header, across the top of the pane
    FillBox( hps, prcl->xLeft, prcl->yTop - hdr_h, prcl->xRight, prcl->yTop,
             DS_FACE );

    hdr_base = prcl->yTop - hdr_h + 4 + fm.lMaxDescender;
    TextAt( hps, prcl->xLeft + 4 + ICON_COL_W, hdr_base, DS_BLACK, "Folder" );
    TextAt( hps, col2, hdr_base, DS_BLACK, "Size" );
    TextAt( hps, col3, hdr_base, DS_BLACK, "% Used" );

    Line( hps, prcl->xLeft, prcl->yTop - hdr_h,
          prcl->xRight, prcl->yTop - hdr_h, DS_SHADOW );

    // rows, marching downwards from just under the header
    y = prcl->yTop - hdr_h - row_h;

    for ( i = folder_scroll_pos; i < tree_count; i++ ) {
        if ( y < prcl->yBottom ) break;
        DrawTreeRow( hps, prcl, i, y, row_h, col2, col3, &fm );
        y -= row_h;
    }
}

// ==========================================================================
// Status bar
// ==========================================================================

void DrawStatusBar( HPS hps, PRECTL prcl )
{
    FONTMETRICS fm;
    char        text[160];
    char        total_buf[24];
    char        free_buf[24];

    Metrics( hps, &fm );

    // background, then a 2-pixel raised edge along the top
    FillBox( hps, prcl->xLeft, prcl->yBottom, prcl->xRight, prcl->yTop - 2,
             DS_FACE );
    Line( hps, prcl->xLeft, prcl->yTop - 1, prcl->xRight, prcl->yTop - 1,
          DS_WHITE );
    Line( hps, prcl->xLeft, prcl->yTop - 2, prcl->xRight, prcl->yTop - 2,
          DS_SHADOW );

    if ( data_valid ) {
        FormatSize( total_disk_space, total_buf );
        FormatSize( free_disk_space, free_buf );
        sprintf( text, "  Drive %c:    Total: %s%s    Free: %s",
                 'A' + current_drive - 1, total_buf,
                 disk_size_clamped ? "+" : "", free_buf );
    }
    else if ( scan_in_progress ) {
        sprintf( text, "  Scanning drive %c: ...", 'A' + current_drive - 1 );
    }
    else {
        strcpy( text, "  Select a drive to begin" );
    }

    TextAt( hps, prcl->xLeft + 4,
            prcl->yBottom + ( STATUSBAR_HEIGHT - 2 - fm.lMaxBaselineExt ) / 2
                          + fm.lMaxDescender,
            DS_BLACK, text );
}
