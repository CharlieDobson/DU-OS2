/*===========================================================================
 * PMPANE.CPP  -  Reveal for OS/2: the detail pane
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * One child window paints every page.  It knows about the row kinds declared
 * in REVEAL.H and nothing else: what a page contains is PMDATA.CPP's
 * business, which is what keeps a new page down to one Build...() function.
 *
 * The look is the Windows builds': an illustration and a title across the
 * top of the page, then label/value pairs under ruled section headings, and
 * bar gauges where a proportion says more than a number.
 *
 * COORDINATES.  PM puts the origin at the BOTTOM-left and counts upwards.
 * A report is a document that flows from the top down, and every measure in
 * it - row heights, the scroll position, where the button lands - is a
 * distance from the top, so the layout here is done top-down throughout and
 * converted to PM's frame in exactly one place: the small set of drawing
 * primitives below (FillTD, LineTD, TextTD...), which take a top-down y and
 * the pane's current height.  Nothing else in this file ever does the
 * subtraction, so nothing else can get it wrong.
 *===========================================================================*/
#define INCL_WIN
#define INCL_GPI
#define INCL_DOSERRORS

#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reveal.h"
#include "res/resource.h"

/*---------------------------------------------------------------------------
 * Pane state.  There is exactly one detail pane, so it lives here rather
 * than in window words.
 *-------------------------------------------------------------------------*/

static report_row_t*    report      = NULL;
static int              report_rows = 0;

static int      pane_category  = -1;
static LONG     scroll_pos     = 0;         /* pixels from the top           */
static LONG     content_height = 0;         /* pixels, body only             */
static HBITMAP  hPageBitmap    = NULLHANDLE;
static LONG     bitmap_width   = 0;
static LONG     bitmap_height  = 0;

static HWND     hScroll        = NULLHANDLE;
static HWND     hPaneButton    = NULLHANDLE;

static LONG     pane_h         = 0;         /* client height, for TD -> PM   */
static LONG     line_height    = 14;
static LONG     char_width     = 5;
static LONG     fixed_width    = 7;
static LONG     header_height  = 56;

/* PM scrollbar positions and ranges are SHORTs.  A page cannot reach 32767
   pixels today, but the bar counts in units of sb_unit pixels so that one
   that did would still scroll rather than wrap. */
static LONG     sb_unit        = 1;

#define PANE_MARGIN     12
#define GAUGE_WIDTH     180

#define PANE_BUTTON_W   160
#define PANE_BUTTON_H   24

/* Column positions for RK_COLHDR / RK_COLROW, in characters of the fixed
   pitch font.  Chosen so the widest table still ends inside the pane.
   TABLE_END is the stop after the last: where a right-justified cell in
   the last column ends. */
#define TABLE_STOPS     4
#define TABLE_END       50
static const int TableStops[ TABLE_STOPS ] = { 9, 20, 30, 40 };

/*---------------------------------------------------------------------------
 * Per-category captions.  Wording is a property of the user interface, so it
 * lives with the user interface and not with the data gatherer.
 *-------------------------------------------------------------------------*/

static const char* CategoryCaptions[ CAT_COUNT ] = {
    "This system",
    "Central Processing Unit",
    "Physical memory and the swap file",
    "Display mode and display drivers",
    "Disk, diskette and CD-ROM drives",
    "Network software and redirected drives",
    "Serial and parallel ports, input and multimedia",
    "Version, limits, programs and drivers",
    "Operating system and environment variables",
    "Processor, coprocessor and memory throughput"
};

#define PANE_BLACK       0x00000000L
#define PANE_WHITE       0x00FFFFFFL

/*===========================================================================
 * Fonts
 *
 * Four logical fonts - text, bold, title, fixed - chosen once at WM_CREATE
 * and re-created on every WM_PAINT, because a font's lcid belongs to a
 * presentation space and WinBeginPaint hands out a fresh cached one each
 * time.
 *
 * The faces are OS/2's bitmap ("image") fonts - Helv for text and Courier
 * for the tables - which every release from 2.0 to 4.5 ships, and which are
 * what the Windows builds' MS Sans Serif and fixed font look like on
 * OS/2.  An image font will NOT match on its facename alone (PM quietly
 * hands back the default outline font), so each is found by enumerating the
 * face and copying lMatch and the exact size into the FATTRS.  Bold is PM's
 * emboldening of the same face; if PM will not do it, bold is plain.
 *=========================================================================*/

enum pane_font_id_t { FONT_TEXT = 0, FONT_BOLD, FONT_TITLE, FONT_FIXED, FONT_COUNT };

#define LCID_PANE_BASE  1L

typedef struct pane_font_t {
    BOOL    ok;             /* FATTRS is usable; else LCID_DEFAULT         */
    FATTRS  fat;
    LONG    ascender;
    LONG    descender;
    LONG    height;         /* lMaxBaselineExt                              */
    LONG    leading;
    LONG    ave_width;
    BOOL    live;           /* created in the current PS                   */
} pane_font_t;

static pane_font_t fonts[ FONT_COUNT ];

static BOOL FindImageFont( HPS hps, const char* face, int points, FATTRS* fat )
{
    LONG         wanted = 0;
    LONG         count;
    FONTMETRICS* fm;
    LONG         res[ 2 ] = { 0, 0 };
    HDC          hdc;
    LONG         i;
    LONG         best = -1;
    LONG         best_dist = 0;

    count = GpiQueryFonts( hps, QF_PUBLIC, (PCSZ)face, &wanted,
                           (LONG)sizeof( FONTMETRICS ), NULL );
    if ( count <= 0 ) return FALSE;

    fm = (FONTMETRICS*)malloc( (size_t)count * sizeof( FONTMETRICS ) );
    if ( fm == NULL ) return FALSE;

    wanted = count;
    GpiQueryFonts( hps, QF_PUBLIC, (PCSZ)face, &wanted,
                   (LONG)sizeof( FONTMETRICS ), fm );

    hdc = GpiQueryDevice( hps );
    if ( hdc != NULLHANDLE && hdc != HDC_ERROR )
        DevQueryCaps( hdc, CAPS_HORIZONTAL_FONT_RES, 2, res );

    for ( i = 0; i < wanted; ++i ) {
        LONG dist;

        if ( fm[ i ].fsDefn & FM_DEFN_OUTLINE ) continue;
        if ( fm[ i ].lMaxBaselineExt <= 0 ) continue;

        /* an image font is drawn for one device resolution: the 96 and 120
           dpi sets both enumerate, and only the display's own looks right */
        if ( res[ 0 ] > 0 && ( fm[ i ].sXDeviceRes != res[ 0 ] ||
                               fm[ i ].sYDeviceRes != res[ 1 ] ) ) continue;

        dist = (LONG)fm[ i ].sNominalPointSize - (LONG)( points * 10 );
        if ( dist < 0 ) dist = -dist;
        if ( best < 0 || dist < best_dist ) { best = i; best_dist = dist; }
    }

    if ( best >= 0 ) {
        memset( fat, 0, sizeof( *fat ) );
        fat->usRecordLength  = sizeof( FATTRS );
        fat->lMatch          = fm[ best ].lMatch;
        strcpy( fat->szFacename, fm[ best ].szFacename );
        fat->idRegistry      = fm[ best ].idRegistry;
        fat->usCodePage      = 0;           /* the process code page        */
        fat->lMaxBaselineExt = fm[ best ].lMaxBaselineExt;
        fat->lAveCharWidth   = fm[ best ].lAveCharWidth;
        fat->fsFontUse       = FATTR_FONTUSE_NOMIX;
    }

    free( fm );
    return best >= 0;
}

/*---------------------------------------------------------------------------
 * MeasureFont - create it once in a scratch PS to learn its metrics, and to
 * find out whether PM accepts it at all
 *-------------------------------------------------------------------------*/

static void MeasureFont( HPS hps, pane_font_t* f )
{
    FONTMETRICS fm;
    LONG        match = GPI_ERROR;

    if ( f->ok ) {
        match = GpiCreateLogFont( hps, NULL, LCID_PANE_BASE, &f->fat );

        /* bold that PM will not synthesise: fall back to the plain face */
        if ( match != FONT_MATCH && ( f->fat.fsSelection & FATTR_SEL_BOLD ) ) {
            if ( match != GPI_ERROR ) GpiDeleteSetId( hps, LCID_PANE_BASE );
            f->fat.fsSelection &= ~FATTR_SEL_BOLD;
            match = GpiCreateLogFont( hps, NULL, LCID_PANE_BASE, &f->fat );
        }
        if ( match == GPI_ERROR ) f->ok = FALSE;
    }

    if ( f->ok ) GpiSetCharSet( hps, LCID_PANE_BASE );

    memset( &fm, 0, sizeof( fm ) );
    GpiQueryFontMetrics( hps, (LONG)sizeof( fm ), &fm );

    f->ascender  = fm.lMaxAscender;
    f->descender = fm.lMaxDescender;
    f->height    = fm.lMaxBaselineExt;
    f->leading   = fm.lExternalLeading;
    f->ave_width = fm.lAveCharWidth;

    if ( f->height <= 0 )    f->height = 12;
    if ( f->ascender <= 0 )  f->ascender = f->height - f->descender;
    if ( f->ave_width <= 0 ) f->ave_width = 6;

    GpiSetCharSet( hps, LCID_DEFAULT );
    if ( match != GPI_ERROR ) GpiDeleteSetId( hps, LCID_PANE_BASE );
}

static void ChooseFonts( HWND hwnd )
{
    HPS hps = WinGetPS( hwnd );
    int i;

    memset( fonts, 0, sizeof( fonts ) );
    if ( hps == NULLHANDLE ) return;

    fonts[ FONT_TEXT ].ok  = FindImageFont( hps, "Helv", 8,  &fonts[ FONT_TEXT ].fat );
    fonts[ FONT_BOLD ].ok  = FindImageFont( hps, "Helv", 8,  &fonts[ FONT_BOLD ].fat );
    fonts[ FONT_TITLE ].ok = FindImageFont( hps, "Helv", 12, &fonts[ FONT_TITLE ].fat );
    fonts[ FONT_FIXED ].ok = FindImageFont( hps, "Courier", 8, &fonts[ FONT_FIXED ].fat )
                          || FindImageFont( hps, "System Monospaced", 8,
                                            &fonts[ FONT_FIXED ].fat );

    fonts[ FONT_BOLD ].fat.fsSelection  |= FATTR_SEL_BOLD;
    fonts[ FONT_TITLE ].fat.fsSelection |= FATTR_SEL_BOLD;

    for ( i = 0; i < FONT_COUNT; ++i ) MeasureFont( hps, &fonts[ i ] );

    WinReleasePS( hps );

    line_height = fonts[ FONT_TEXT ].height + fonts[ FONT_TEXT ].leading + 2;
    char_width  = fonts[ FONT_TEXT ].ave_width;
    fixed_width = fonts[ FONT_FIXED ].ave_width;
    if ( char_width < 4 )  char_width = 4;
    if ( fixed_width < 4 ) fixed_width = 4;
}

static void CreatePaintFonts( HPS hps )
{
    int i;

    for ( i = 0; i < FONT_COUNT; ++i ) {
        fonts[ i ].live = fonts[ i ].ok &&
            GpiCreateLogFont( hps, NULL, LCID_PANE_BASE + i, &fonts[ i ].fat ) != GPI_ERROR;
    }
}

static void DeletePaintFonts( HPS hps )
{
    int i;

    GpiSetCharSet( hps, LCID_DEFAULT );
    for ( i = 0; i < FONT_COUNT; ++i ) {
        if ( fonts[ i ].live ) GpiDeleteSetId( hps, LCID_PANE_BASE + i );
        fonts[ i ].live = FALSE;
    }
}

BOOL PaneTextFont( FATTRS* fat )
{
    if ( !fonts[ FONT_TEXT ].ok ) return FALSE;
    *fat = fonts[ FONT_TEXT ].fat;
    return TRUE;
}

static void UseFont( HPS hps, int which )
{
    GpiSetCharSet( hps, fonts[ which ].live ? LCID_PANE_BASE + which : LCID_DEFAULT );
}

/*===========================================================================
 * Top-down drawing primitives - the only code that knows PM counts upwards
 *
 * A top-down y is a distance in pixels from the top of the pane's client
 * area; a top-down rectangle is [left, right) x [top, bottom).
 *=========================================================================*/

static void FillTD( HPS hps, LONG left, LONG top, LONG right, LONG bottom, LONG colour )
{
    RECTL rc;

    if ( right <= left || bottom <= top ) return;
    rc.xLeft   = left;
    rc.xRight  = right;
    rc.yBottom = pane_h - bottom;
    rc.yTop    = pane_h - top;
    WinFillRect( hps, &rc, colour );
}

/* a line between two pixels, both included */
static void LineTD( HPS hps, LONG x1, LONG y1, LONG x2, LONG y2, LONG colour )
{
    POINTL ptl;

    GpiSetColor( hps, colour );
    ptl.x = x1;  ptl.y = pane_h - 1 - y1;
    GpiMove( hps, &ptl );
    ptl.x = x2;  ptl.y = pane_h - 1 - y2;
    GpiLine( hps, &ptl );
}

/* text whose cell top is at 'top' - where Windows' TextOut puts it */
static void TextTD( HPS hps, int font, LONG x, LONG top, const char* text, LONG colour )
{
    POINTL ptl;
    LONG   len = (LONG)strlen( text );

    if ( len == 0 ) return;
    UseFont( hps, font );
    GpiSetColor( hps, colour );
    ptl.x = x;
    ptl.y = pane_h - top - fonts[ font ].ascender;
    GpiCharStringAt( hps, &ptl, len, (PCH)text );
}

/* the same, clipped at 'right' */
static void TextClipTD( HPS hps, int font, LONG x, LONG top, LONG right,
                        const char* text, LONG colour )
{
    POINTL ptl;
    RECTL  clip;
    LONG   len = (LONG)strlen( text );

    if ( len == 0 || right <= x ) return;
    UseFont( hps, font );
    GpiSetColor( hps, colour );
    ptl.x = x;
    ptl.y = pane_h - top - fonts[ font ].ascender;
    clip.xLeft   = x;
    clip.xRight  = right;
    clip.yTop    = pane_h - top;
    clip.yBottom = pane_h - top - fonts[ font ].height - fonts[ font ].leading;
    GpiCharStringPosAt( hps, &ptl, &clip, CHS_CLIP, len, (PCH)text, NULL );
}

static LONG TextWidth( HPS hps, int font, const char* text )
{
    POINTL box[ TXTBOX_COUNT ];
    LONG   len = (LONG)strlen( text );

    if ( len == 0 ) return 0;
    UseFont( hps, font );
    if ( !GpiQueryTextBox( hps, len, (PCH)text, TXTBOX_COUNT, box ) )
        return len * fonts[ font ].ave_width;
    return box[ TXTBOX_CONCAT ].x;
}

/* bitmap with its top-left corner at (x, top) */
static void BitmapTD( HPS hps, HBITMAP hbm, LONG x, LONG top, LONG height )
{
    POINTL ptl;

    ptl.x = x;
    ptl.y = pane_h - top - height;          /* WinDrawBitmap takes the BOTTOM-left */
    WinDrawBitmap( hps, hbm, NULL, &ptl, 0L, 0L, DBM_NORMAL );
}

/*===========================================================================
 * Colours and 3-D edges
 *
 * Read from the system colours on every paint: they are cheap to ask for,
 * and asking each time means a change of scheme needs nothing cached to be
 * thrown away.
 *=========================================================================*/

typedef struct pane_colours_t {
    LONG window, text, face, shadow, light, grey_text, bar;
} pane_colours_t;

static pane_colours_t clr;

static void ReadColours( void )
{
    clr.window    = WinQuerySysColor( HWND_DESKTOP, SYSCLR_WINDOW, 0L );
    clr.text      = WinQuerySysColor( HWND_DESKTOP, SYSCLR_WINDOWTEXT, 0L );
    clr.face      = WinQuerySysColor( HWND_DESKTOP, SYSCLR_DIALOGBACKGROUND, 0L );
    clr.shadow    = WinQuerySysColor( HWND_DESKTOP, SYSCLR_BUTTONDARK, 0L );
    clr.light     = WinQuerySysColor( HWND_DESKTOP, SYSCLR_BUTTONLIGHT, 0L );
    clr.grey_text = clr.shadow;
    clr.bar       = WinQuerySysColor( HWND_DESKTOP, SYSCLR_ACTIVETITLE, 0L );
}

static void SunkenRect( HPS hps, LONG left, LONG top, LONG right, LONG bottom )
{
    LineTD( hps, left, bottom - 1, left, top, clr.shadow );
    LineTD( hps, left, top, right - 1, top, clr.shadow );
    LineTD( hps, right - 1, top, right - 1, bottom - 1, clr.light );
    LineTD( hps, right - 1, bottom - 1, left, bottom - 1, clr.light );
}

static void EtchedLine( HPS hps, LONG left, LONG right, LONG y )
{
    if ( right <= left ) return;
    LineTD( hps, left, y,     right - 1, y,     clr.shadow );
    LineTD( hps, left, y + 1, right - 1, y + 1, clr.light );
}

/*===========================================================================
 * Measures
 *=========================================================================*/

static LONG ScrollBarWidth( void )
{
    LONG w = WinQuerySysValue( HWND_DESKTOP, SV_CXVSCROLL );
    return ( w > 0 ) ? w : 16;
}

static LONG ValueColumn( LONG pane_width )
{
    LONG x    = PANE_MARGIN + char_width * 34;
    LONG half = pane_width / 2;

    if ( half > PANE_MARGIN * 4 && x > half ) x = half;
    return x;
}

/* One place decides how tall each row kind is, so measuring, painting and
   placing the button can never disagree about it. */

static LONG RowHeight( int kind )
{
    switch ( kind ) {
        case RK_HEADING:    return line_height + 10;
        case RK_BLANK:      return line_height / 2;
        case RK_GAUGE:      return line_height + 6;
        case RK_BUTTON:     return PANE_BUTTON_H + 8;
        case RK_END:        return 0;
        default:            break;
    }
    return line_height;
}

/*===========================================================================
 * The page header: illustration, title and caption
 *=========================================================================*/

static void LoadPageBitmap( HWND hwnd, int category )
{
    BITMAPINFOHEADER bmp;
    HPS              hps;
    int              id;

    if ( hPageBitmap != NULLHANDLE ) {
        GpiDeleteBitmap( hPageBitmap );
        hPageBitmap   = NULLHANDLE;
        bitmap_width  = 0;
        bitmap_height = 0;
    }

    id = RvCategoryBitmap( category );
    if ( id == 0 ) return;

    /* GpiLoadBitmap needs a PS to know what device format to build for; the
       bitmap belongs to the process afterwards, not to this PS */
    hps = WinGetPS( hwnd );
    if ( hps == NULLHANDLE ) return;
    hPageBitmap = GpiLoadBitmap( hps, NULLHANDLE, (ULONG)id, 0L, 0L );
    WinReleasePS( hps );

    if ( hPageBitmap == NULLHANDLE || hPageBitmap == GPI_ERROR ) {
        hPageBitmap = NULLHANDLE;
        return;
    }

    memset( &bmp, 0, sizeof( bmp ) );
    bmp.cbFix = sizeof( bmp );
    if ( GpiQueryBitmapParameters( hPageBitmap, &bmp ) ) {
        bitmap_width  = (LONG)bmp.cx;
        bitmap_height = (LONG)bmp.cy;
    }
}

static void ComputeHeaderHeight( void )
{
    LONG text_block = line_height * 2 + 20;

    header_height = ( bitmap_height > 0 ) ? bitmap_height + 16 : text_block;
    if ( header_height < text_block ) header_height = text_block;
}

static void PaintHeader( HPS hps, LONG width )
{
    LONG        text_x = PANE_MARGIN;
    LONG        text_y;
    const char* caption;

    FillTD( hps, 0, 0, width, header_height, clr.face );

    if ( hPageBitmap != NULLHANDLE ) {
        LONG left   = PANE_MARGIN;
        LONG top    = ( header_height - bitmap_height ) / 2 - 2;
        LONG right, bottom;

        if ( top < 4 ) top = 4;
        right  = left + bitmap_width + 4;
        bottom = top + bitmap_height + 4;

        /* the artwork is drawn on black, so it is given a black plate to sit
           on rather than being left to float on the grey of the band */
        FillTD( hps, left, top, right, bottom, PANE_BLACK );
        SunkenRect( hps, left, top, right, bottom );
        BitmapTD( hps, hPageBitmap, left + 2, top + 2, bitmap_height );

        text_x = right + 14;
    }

    caption = ( pane_category >= 0 && pane_category < CAT_COUNT )
              ? CategoryCaptions[ pane_category ] : "";

    text_y = ( header_height - ( fonts[ FONT_TITLE ].height + line_height + 4 ) ) / 2;
    if ( text_y < 6 ) text_y = 6;

    TextTD( hps, FONT_TITLE, text_x, text_y, RvCategoryName( pane_category ), clr.text );
    text_y += fonts[ FONT_TITLE ].height + 2;
    TextTD( hps, FONT_TEXT, text_x, text_y, caption, clr.grey_text );

    EtchedLine( hps, 0, width, header_height - 2 );
}

/*===========================================================================
 * Row painters
 *=========================================================================*/

static void PaintGauge( HPS hps, LONG width, LONG y, const report_row_t* row )
{
    LONG value_x = ValueColumn( width );
    LONG bar_w   = GAUGE_WIDTH;
    LONG top, bottom, fill_right;
    char text[ RPT_VALUE_MAX + 8 ];

    TextTD( hps, FONT_TEXT, PANE_MARGIN, y + 2, row->label, clr.text );

    if ( value_x + bar_w + 90 > width ) bar_w = width - value_x - 90;
    if ( bar_w < 40 ) bar_w = 40;

    top    = y + 2;
    bottom = y + line_height;

    FillTD( hps, value_x, top, value_x + bar_w, bottom, PANE_WHITE );

    fill_right = value_x + 1 + ( ( bar_w - 2 ) * row->pct ) / 100;
    FillTD( hps, value_x + 1, top + 1, fill_right, bottom - 1, clr.bar );

    SunkenRect( hps, value_x, top, value_x + bar_w, bottom );

    if ( row->value[ 0 ] != '\0' )
        sprintf( text, "%d%%  %s", row->pct, row->value );
    else
        sprintf( text, "%d%%", row->pct );
    TextTD( hps, FONT_TEXT, value_x + bar_w + 8, y + 2, text, clr.text );
}

/*---------------------------------------------------------------------------
 * PaintColumns - a tab-separated row, each cell at its column stop
 *
 * Windows' TabbedTextOut did this; GPI has no tab expansion, so the cells are
 * split out and drawn one by one at the same stops.  A column named in
 * 'align' (report_row_t.align, RIGHT_COL bits) is right-justified instead,
 * ending a character short of the next stop.
 *-------------------------------------------------------------------------*/

static void PaintColumns( HPS hps, int font, LONG y, const char* text, int align,
                          LONG right )
{
    char cell[ RPT_VALUE_MAX ];
    int  column = 0;

    while ( *text != '\0' || column == 0 ) {
        const char* tab = strchr( text, '\t' );
        int         len = ( tab != NULL ) ? (int)( tab - text ) : (int)strlen( text );
        LONG        x, next;

        if ( len > (int)sizeof( cell ) - 1 ) len = (int)sizeof( cell ) - 1;
        memcpy( cell, text, (size_t)len );
        cell[ len ] = '\0';

        x = PANE_MARGIN + ( ( column == 0 ) ? 0
                            : fixed_width * TableStops[ ( column - 1 < TABLE_STOPS )
                                                        ? column - 1 : TABLE_STOPS - 1 ] );
        /* a cell stops where the next column starts, so a long one cannot
           run underneath its neighbour; the last runs to the edge */
        next = ( tab != NULL && column < TABLE_STOPS )
               ? PANE_MARGIN + fixed_width * TableStops[ column ] - 4 : right;

        if ( align & RIGHT_COL( column ) ) {
            LONG stop = ( column < TABLE_STOPS ) ? TableStops[ column ] : TABLE_END;
            LONG end  = PANE_MARGIN + fixed_width * ( stop - 1 );

            TextTD( hps, font, end - TextWidth( hps, font, cell ), y, cell, clr.text );
        }
        else {
            TextClipTD( hps, font, x, y, next, cell, clr.text );
        }

        if ( tab == NULL ) break;
        text = tab + 1;
        ++column;
    }
}

static void PaintRow( HPS hps, LONG width, LONG y, const report_row_t* row )
{
    LONG value_x = ValueColumn( width );

    switch ( row->kind ) {

        case RK_HEADING:
            TextTD( hps, FONT_BOLD, PANE_MARGIN, y + 6, row->label, clr.text );
            EtchedLine( hps, PANE_MARGIN + TextWidth( hps, FONT_BOLD, row->label ) + 8,
                        width - PANE_MARGIN, y + 6 + line_height / 2 );
            break;

        case RK_ITEM:
        case RK_SUBITEM: {
            LONG x = ( row->kind == RK_SUBITEM ) ? PANE_MARGIN + char_width * 2
                                                 : PANE_MARGIN;

            /* Clipped short of the value column: a long label - an
               environment variable name, most often - would otherwise run on
               underneath its own value. */
            TextClipTD( hps, FONT_TEXT, x, y, value_x - char_width, row->label, clr.text );
            TextClipTD( hps, FONT_TEXT, value_x, y, width, row->value, clr.text );
            break;
        }

        case RK_COLHDR:
            PaintColumns( hps, FONT_BOLD, y, row->value, row->align, width );
            EtchedLine( hps, PANE_MARGIN, width - PANE_MARGIN, y + line_height - 2 );
            break;

        case RK_COLROW:
            PaintColumns( hps, FONT_FIXED, y, row->value, row->align, width );
            break;

        case RK_NOTE:
            TextTD( hps, FONT_TEXT, PANE_MARGIN, y, row->value, clr.grey_text );
            break;

        case RK_GAUGE:
            PaintGauge( hps, width, y, row );
            break;

        case RK_BUTTON:
            /* Nothing to draw: the row only reserves the space, and the real
               control paints itself.  PositionPaneButton puts it here. */
            break;

        default:
            break;
    }
}

/*===========================================================================
 * Measuring and scrolling
 *=========================================================================*/

static void MeasureContent( void )
{
    int i;

    content_height = 8;
    for ( i = 0; i < report_rows; ++i ) {
        content_height += RowHeight( report[ i ].kind );
    }
    content_height += 8;
}

static LONG VisibleHeight( HWND hwnd )
{
    RECTL rcl;
    LONG  visible;

    WinQueryWindowRect( hwnd, &rcl );
    visible = rcl.yTop - header_height;
    return ( visible < 1 ) ? 1 : visible;
}

static LONG ScrollRange( HWND hwnd )
{
    LONG range = content_height - VisibleHeight( hwnd );
    return ( range < 0 ) ? 0 : range;
}

static void UpdateScrollBar( HWND hwnd )
{
    LONG range   = ScrollRange( hwnd );
    LONG visible = VisibleHeight( hwnd );

    if ( scroll_pos > range ) scroll_pos = range;
    if ( scroll_pos < 0 )     scroll_pos = 0;

    if ( hScroll == NULLHANDLE ) return;

    sb_unit = content_height / 30000L + 1;

    WinSendMsg( hScroll, SBM_SETSCROLLBAR,
                MPFROMSHORT( (SHORT)( scroll_pos / sb_unit ) ),
                MPFROM2SHORT( 0, (SHORT)( range / sb_unit ) ) );
    WinSendMsg( hScroll, SBM_SETTHUMBSIZE,
                MPFROM2SHORT( (SHORT)( visible / sb_unit ),
                              (SHORT)( ( content_height > 0 ? content_height : 1 ) / sb_unit + 1 ) ),
                MPVOID );
}

/*---------------------------------------------------------------------------
 * PositionPaneButton - move the real control to wherever its row landed
 *
 * A child window cannot be drawn by PaintRow along with everything else, so
 * it has to be moved to where the RK_BUTTON row falls.  This walks the rows
 * exactly as WM_PAINT does - same starting y, same RowHeight - so the two
 * can never disagree about where that row is.  Pages that emit no such row
 * just hide it, and so does a scroll that takes the row up under the header
 * band.  Called after anything that moves the rows: a new page, a refresh, a
 * resize, a scroll.
 *
 * The position is remembered in PM coordinates, not top-down ones: a resize
 * moves the button in PM terms while leaving its top-down place unchanged,
 * and it still has to move.
 *-------------------------------------------------------------------------*/

/* BTN_NONE and BTN_UNKNOWN must be different values: "there is no button on
   this page" is a state the position test can legally match and skip,
   whereas "not looked yet" must never match, or a page change from a page
   with a button to one without returns early and leaves the button on
   screen, sitting on top of the next page (as it did in the Win16 build). */
#define BTN_NONE        (-32767L)
#define BTN_UNKNOWN     (-32766L)

static LONG button_pm_y = BTN_UNKNOWN;

static void PositionPaneButton( HWND hwnd )
{
    const report_row_t* row  = NULL;
    LONG                want = BTN_NONE;
    LONG                y;
    int                 i;

    if ( hPaneButton == NULLHANDLE ) return;
    if ( !WinIsWindowVisible( hwnd ) ) return;

    {
        RECTL rcl;

        WinQueryWindowRect( hwnd, &rcl );
        pane_h = rcl.yTop;
    }

    if ( report != NULL ) {
        y = header_height + 8 - scroll_pos;
        for ( i = 0; i < report_rows; ++i ) {
            if ( report[ i ].kind == RK_BUTTON ) {
                if ( y + 4 >= header_height ) {
                    row  = &report[ i ];
                    want = pane_h - ( y + 4 ) - PANE_BUTTON_H;
                }
                break;
            }
            y += RowHeight( report[ i ].kind );
        }
    }

    /* Do nothing unless something moved.  This is called from WM_PAINT, and
       a move invalidates the pane, which paints again: without this test
       that is a loop that never settles. */
    if ( want == button_pm_y ) return;
    button_pm_y = want;

    if ( row == NULL ) {
        WinShowWindow( hPaneButton, FALSE );
        return;
    }

    WinSetWindowText( hPaneButton, (PCSZ)row->label );
    WinSetWindowPos( hPaneButton, HWND_TOP, PANE_MARGIN, want,
                     PANE_BUTTON_W, PANE_BUTTON_H, SWP_MOVE | SWP_SIZE | SWP_SHOW );
}

static void ScrollTo( HWND hwnd, LONG pos )
{
    LONG range = ScrollRange( hwnd );

    if ( pos < 0 )     pos = 0;
    if ( pos > range ) pos = range;
    if ( pos == scroll_pos ) return;

    scroll_pos = pos;
    if ( hScroll != NULLHANDLE )
        WinSendMsg( hScroll, SBM_SETPOS, MPFROMSHORT( (SHORT)( scroll_pos / sb_unit ) ),
                    MPVOID );

    /* only the body moves; the header band stays where it is */
    {
        RECTL rcl;

        WinQueryWindowRect( hwnd, &rcl );
        rcl.yTop -= header_height;
        WinInvalidateRect( hwnd, &rcl, FALSE );
    }
    PositionPaneButton( hwnd );
}

/*===========================================================================
 * Public entry points
 *=========================================================================*/

BOOL PaneRegisterClass( HAB habApp )
{
    /* CS_CLIPCHILDREN: the scrollbar and the button paint themselves, and
       the pane's own painting must leave them alone */
    return WinRegisterClass( habApp, (PCSZ)PANE_CLASS, PaneWndProc,
                             CS_SIZEREDRAW | CS_CLIPCHILDREN, 0 );
}

/*---------------------------------------------------------------------------
 * The wait pointer
 *
 * Most pages are built faster than the eye can follow, but the Drives page
 * asks every letter for its free space, and an empty diskette drive or a
 * slow redirector can each take a noticeable part of a second.  Building a
 * page is synchronous and reads no messages, so nothing can put the arrow
 * back before EndWait does.
 *-------------------------------------------------------------------------*/

static HPOINTER BeginWait( void )
{
    HPOINTER previous = WinQueryPointer( HWND_DESKTOP );

    WinSetPointer( HWND_DESKTOP, WinQuerySysPointer( HWND_DESKTOP, SPTR_WAIT, FALSE ) );
    return previous;
}

static void EndWait( HPOINTER previous )
{
    WinSetPointer( HWND_DESKTOP, ( previous != NULLHANDLE ) ? previous
                   : WinQuerySysPointer( HWND_DESKTOP, SPTR_ARROW, FALSE ) );
}

void PaneSetCategory( HWND hPane, int category )
{
    HPOINTER previous;

    if ( category < 0 || category >= CAT_COUNT ) return;

    pane_category = category;
    scroll_pos    = 0;
    button_pm_y   = BTN_UNKNOWN;    /* new page: re-evaluate, caption and all */

    previous = BeginWait();

    LoadPageBitmap( hPane, category );
    ComputeHeaderHeight();

    if ( report != NULL ) {
        report_rows = RvBuildReport( category, report, RPT_MAX_ROWS );
    }

    MeasureContent();
    UpdateScrollBar( hPane );
    PositionPaneButton( hPane );

    /* Painted before the pointer goes back, so the hourglass is up until the
       page it stands for is actually on screen. */
    WinInvalidateRect( hPane, NULL, FALSE );
    WinUpdateWindow( hPane );

    EndWait( previous );
}

void PaneRefresh( HWND hPane )
{
    HPOINTER previous = BeginWait();
    LONG     keep     = scroll_pos;

    if ( report != NULL && pane_category >= 0 ) {
        report_rows = RvBuildReport( pane_category, report, RPT_MAX_ROWS );
    }

    MeasureContent();
    scroll_pos  = keep;
    button_pm_y = BTN_UNKNOWN;      /* the page may have gained or lost it  */
    UpdateScrollBar( hPane );
    PositionPaneButton( hPane );
    WinInvalidateRect( hPane, NULL, FALSE );
    WinUpdateWindow( hPane );

    EndWait( previous );
}

/*===========================================================================
 * Window procedure
 *=========================================================================*/

MRESULT EXPENTRY PaneWndProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg ) {

    case WM_CREATE:
        ChooseFonts( hwnd );

        report = (report_row_t*)calloc( RPT_MAX_ROWS, sizeof( report_row_t ) );
        if ( report == NULL ) {
            WinMessageBox( HWND_DESKTOP, hwnd,
                           (PCSZ)"There is not enough memory to build the report.",
                           (PCSZ)APP_NAME, 0, MB_OK | MB_ERROR );
        }

        hScroll = WinCreateWindow( hwnd, WC_SCROLLBAR, (PCSZ)"",
                                   WS_VISIBLE | SBS_VERT,
                                   0, 0, 0, 0,
                                   hwnd, HWND_TOP, IDC_PANESCROLL, NULL, NULL );

        /* Created hidden: it stays hidden until a page asks for it.  The
           caption is set from the row that asks. */
        hPaneButton = WinCreateWindow( hwnd, WC_BUTTON, (PCSZ)"",
                                       BS_PUSHBUTTON | WS_TABSTOP,
                                       0, 0, PANE_BUTTON_W, PANE_BUTTON_H,
                                       hwnd, HWND_TOP, IDC_PANEBUTTON, NULL, NULL );
        if ( hPaneButton != NULLHANDLE ) {
            WinSetPresParam( hPaneButton, PP_FONTNAMESIZE,
                             (ULONG)strlen( UI_FONT ) + 1, (PVOID)UI_FONT );
        }
        return (MRESULT)FALSE;

    case WM_COMMAND:
        if ( SHORT1FROMMP( mp1 ) == IDC_PANEBUTTON ) {
            /* Pass it up as the menu command it already is, but marked as
               coming from a push button.  The frame reads that as "the user
               pressed a button that says exactly this" and skips the
               confirmation the menu item and F9 ask for. */
            WinPostMsg( WinQueryWindow( hwnd, QW_PARENT ), WM_COMMAND,
                        MPFROMSHORT( IDM_VIEW_BENCHMARK ),
                        MPFROM2SHORT( CMDSRC_PUSHBUTTON, TRUE ) );
            return (MRESULT)FALSE;
        }
        break;

    case WM_SIZE: {
        RECTL rcl;
        LONG  sbw = ScrollBarWidth();

        WinQueryWindowRect( hwnd, &rcl );
        pane_h = rcl.yTop;

        if ( hScroll != NULLHANDLE ) {
            WinSetWindowPos( hScroll, HWND_TOP, rcl.xRight - sbw, 0, sbw, rcl.yTop,
                             SWP_MOVE | SWP_SIZE | SWP_SHOW );
        }
        UpdateScrollBar( hwnd );
        PositionPaneButton( hwnd );
        return (MRESULT)FALSE;
    }

    case WM_SYSCOLORCHANGE:
    case WM_PRESPARAMCHANGED:
        WinInvalidateRect( hwnd, NULL, FALSE );
        break;

    /* WM_PAINT covers every pixel, so letting PM erase first only adds a
       flash */
    case WM_ERASEBACKGROUND:
        return (MRESULT)FALSE;

    case WM_PAINT: {
        HPS   hps;
        RECTL rclPaint, rcl;
        LONG  width;
        LONG  y;
        int   i;

        hps = WinBeginPaint( hwnd, NULLHANDLE, &rclPaint );

        /* a cached PS comes back in index mode with no fonts: both are
           re-established on every paint */
        GpiCreateLogColorTable( hps, 0, LCOLF_RGB, 0, 0, NULL );
        GpiSetBackMix( hps, BM_LEAVEALONE );
        CreatePaintFonts( hps );
        ReadColours();

        WinQueryWindowRect( hwnd, &rcl );
        pane_h = rcl.yTop;
        width  = rcl.xRight - ScrollBarWidth();

        FillTD( hps, 0, header_height, width, pane_h, clr.window );

        /* The body first and the header band last: a row scrolled part way
           up under the band is simply painted over by it, which is all the
           clipping the Windows build needed IntersectClipRect for. */
        if ( report != NULL ) {
            y = header_height + 8 - scroll_pos;
            for ( i = 0; i < report_rows; ++i ) {
                LONG h = RowHeight( report[ i ].kind );

                if ( y + h >= header_height && y <= pane_h ) {
                    PaintRow( hps, width, y, &report[ i ] );
                }
                y += h;
                if ( y > pane_h ) break;
            }
        }

        PaintHeader( hps, width );

        DeletePaintFonts( hps );
        WinEndPaint( hps );

        /* After WinEndPaint, never between Begin and End: this can move and
           show a child window.  It settles after one repaint because it
           does nothing when nothing has moved. */
        PositionPaneButton( hwnd );
        return (MRESULT)FALSE;
    }

    case WM_VSCROLL: {
        LONG visible = VisibleHeight( hwnd );

        switch ( SHORT2FROMMP( mp2 ) ) {
            case SB_LINEUP:         ScrollTo( hwnd, scroll_pos - line_height ); break;
            case SB_LINEDOWN:       ScrollTo( hwnd, scroll_pos + line_height ); break;
            case SB_PAGEUP:         ScrollTo( hwnd, scroll_pos - visible );     break;
            case SB_PAGEDOWN:       ScrollTo( hwnd, scroll_pos + visible );     break;
            case SB_SLIDERTRACK:
            case SB_SLIDERPOSITION:
                ScrollTo( hwnd, (LONG)(SHORT)SHORT1FROMMP( mp2 ) * sb_unit );
                break;
            default:
                break;
        }
        return (MRESULT)FALSE;
    }

    /* One message for the keyboard: the virtual keys scroll, key-ups and
       everything else go on to the default handling. */
    case WM_CHAR: {
        USHORT fs = SHORT1FROMMP( mp1 );

        if ( !( fs & KC_KEYUP ) && ( fs & KC_VIRTUALKEY ) ) {
            LONG visible = VisibleHeight( hwnd );

            switch ( SHORT2FROMMP( mp2 ) ) {
                case VK_UP:       ScrollTo( hwnd, scroll_pos - line_height ); return (MRESULT)TRUE;
                case VK_DOWN:     ScrollTo( hwnd, scroll_pos + line_height ); return (MRESULT)TRUE;
                case VK_PAGEUP:   ScrollTo( hwnd, scroll_pos - visible );     return (MRESULT)TRUE;
                case VK_PAGEDOWN: ScrollTo( hwnd, scroll_pos + visible );     return (MRESULT)TRUE;
                case VK_HOME:     ScrollTo( hwnd, 0 );                        return (MRESULT)TRUE;
                case VK_END:      ScrollTo( hwnd, ScrollRange( hwnd ) );      return (MRESULT)TRUE;
                default:          break;
            }
        }
        break;
    }

    case WM_BUTTON1DOWN:
        WinSetFocus( HWND_DESKTOP, hwnd );
        break;

    case WM_DESTROY:
        if ( hPageBitmap != NULLHANDLE ) {
            GpiDeleteBitmap( hPageBitmap );
            hPageBitmap = NULLHANDLE;
        }
        if ( report != NULL ) {
            free( report );
            report = NULL;
        }
        report_rows = 0;
        return (MRESULT)FALSE;

    default:
        break;
    }

    return WinDefWindowProc( hwnd, msg, mp1, mp2 );
}
