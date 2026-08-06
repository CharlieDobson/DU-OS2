/*===========================================================================
 * DISKSTAT.H  -  DiskStat (OS/2 PM) platform header: layout, IDs, globals
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * The portable disk model (tree_node_t, constants, scanner API) lives in the
 * shared DISKMODL.HPP; this header adds the PM layout constants, control IDs,
 * the RGB palette, the global model instances, and the DRAW.CPP API.
 *
 * !! PM COORDINATES !!  Everything below is in PM's bottom-left origin space:
 * y grows UPWARDS and a RECTL is (xLeft, yBottom, xRight, yTop) with yTop /
 * xRight exclusive.  Do not read a y here as if it were a top-down client y.
 *===========================================================================*/
#ifndef DISKSTAT_H
#define DISKSTAT_H

// Pulled in here as well as in each .CPP, so that including this header alone
// still yields the full Win/Gpi API rather than a silently reduced one.
#ifndef INCL_WIN
#define INCL_WIN
#endif
#ifndef INCL_GPI
#define INCL_GPI
#endif

#include <os2.h>
#include "diskmodl.hpp"         // shared: tree_node_t, constants, scanner API

// --------------------------------------------------------------------------
// Layout constants  (pixels)
// --------------------------------------------------------------------------

#define LEFT_PANE_MIN       160     /* floor so pie + divider still draw      */
#define LEFT_PANE_MAX       340     /* cap so a wide window feeds the tree    */
#define RIGHT_PANE_MIN      320     /* keep the 3 columns readable            */
#define STATUSBAR_HEIGHT    22
#define TOOLBAR_HEIGHT      32
#define PIE_MARGIN          12
#define PIE_DEPTH           16
#define INDENT_WIDTH        16      /* pixels of indent per tree level        */
#define EXPAND_BOX_SIZE     9

// --------------------------------------------------------------------------
// Control IDs.  (ID_DISKSTAT -- the frame's window ID, and hence the number PM
// uses to find the MENU / ICON / ACCELTABLE resources -- lives in
// RES\RESOURCE.H alongside the resources it names.)
// --------------------------------------------------------------------------

#define IDC_DRVCOMBO        1001
#define IDC_REFRESHBTN      1002
#define IDC_TREEPANE        1004
#define IDC_TREESCROLL      1005

#define RIGHTPANE_CLASS     "DiskStatRightPane"

// --------------------------------------------------------------------------
// Palette.  The PS is switched to LCOLF_RGB mode on every paint, so colours
// are plain 0x00RRGGBB values.  (Prefixed DS_ because PM already owns the
// CLR_* namespace in <pmgpi.h>.)
// --------------------------------------------------------------------------

#define DS_BLACK        0x00000000L
#define DS_WHITE        0x00FFFFFFL
#define DS_FACE         0x00C0C0C0L     /* 3-D control face                   */
#define DS_SHADOW       0x00808080L     /* 3-D shadow / separators            */
#define DS_ALTROW       0x00F0F0F0L     /* banded tree row                    */

#define DS_USED         0x000000C0L     /* pie: used space                    */
#define DS_USED_DK      0x00000080L
#define DS_SEL          0x00E0E000L     /* pie: selected folder               */
#define DS_SEL_DK       0x00A0A000L
#define DS_FREE         0x0000C000L     /* pie: free space                    */
#define DS_FREE_DK      0x00008000L

#define DS_SELROW       0x00000080L     /* tree: selected row background      */
#define DS_FOLDER_TAB   0x00C0C000L
#define DS_FOLDER_BODY  0x00FFFF80L
#define DS_FOLDER_EDGE  0x00808000L

// --------------------------------------------------------------------------
// Global data (defined in PMMAIN.CPP)
// --------------------------------------------------------------------------

extern HAB              hab;                /* anchor block, for the pump     */
extern HWND             hwndFrame;
extern HWND             hwndClient;
extern HWND             hDriveCombo;
extern HWND             hRightPane;
extern HWND             hTreeScroll;

extern int              current_drive;      /* 1 = A:, 2 = B:, 3 = C:, ...    */
extern unsigned long    total_disk_space;
extern unsigned long    free_disk_space;
extern unsigned long    used_disk_space;

extern tree_node_t      tree_nodes[MAX_TREE_NODES];
extern int              tree_count;
extern int              selected_node;      /* -1 = none                      */

extern BOOL             scan_in_progress;
extern BOOL             data_valid;
extern BOOL             disk_size_clamped;  /* volume exceeded the 32-bit model */

extern int              folder_scroll_pos;

// --------------------------------------------------------------------------
// DRAW.CPP
//
// DsRowHeight / DsHeaderHeight / DsIndentX are shared with PMMAIN.CPP so the
// tree's hit-testing and its painting cannot disagree about where a row or an
// expand box actually is.
// --------------------------------------------------------------------------

void DrawPieChart  ( HPS hps, PRECTL prcl );
void DrawPieLegend ( HPS hps, PRECTL prcl );
void DrawTreeView  ( HPS hps, PRECTL prcl );
void DrawStatusBar ( HPS hps, PRECTL prcl );

void FormatSize    ( unsigned long bytes, char *buffer );   /* "1234.5 MB"    */

LONG DsRowHeight   ( HPS hps );             /* one tree row, in pixels        */
LONG DsHeaderHeight( HPS hps );             /* the "Folder / Size / %" header */
LONG DsIndentX     ( PRECTL prcl, int depth );  /* x of a row's expand box    */

void DsSetRgbMode  ( HPS hps );             /* LCOLF_RGB + transparent text   */

#endif // DISKSTAT_H
