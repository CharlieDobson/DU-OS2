/*===========================================================================
 * RESOURCE.H  -  XArchive (OS/2 PM) resource identifiers
 *
 * !! DO NOT RENUMBER !!  The command, dialog and control IDs match
 * the Win32s XArchive tree so the two stay diffable.  Added for PM:
 *
 *   ID_XARCHIVE  the frame's window ID.  FCF_MENU, FCF_ICON and
 *                FCF_ACCELTABLE all look their resource up by it, so the
 *                MENU, the ICON and the ACCELTABLE must all carry this
 *                number or the menu / icon / accelerators silently vanish.
 *   IDM_FILE etc. pulldown IDs, which OS/2's SUBMENU requires and which
 *                WM_INITMENU then reports in mp1 (replacing the positional
 *                index Windows' WM_INITMENUPOPUP used).
 *
 * Gone: IDC_TOOLBAR.  There is no COMCTL32 toolbar control on OS/2; the
 * button strip is built from ordinary WC_BUTTONs whose IDs are the menu
 * command IDs, so the WM_COMMAND handling is shared exactly as it was.
 *===========================================================================*/

#ifndef RESOURCE_H
#define RESOURCE_H

#define ID_XARCHIVE                     100   /* frame / menu / icon / accel */

/* pulldowns (SUBMENU needs an ID; WM_INITMENU reports it) */
#define IDM_FILE                        10
#define IDM_ARCHIVE                     11
#define IDM_HELP                        12

/* dialogs */
#define IDD_ABOUT                       103
#define IDD_PROGRESS                    104
#define IDD_INFO                        105
#define IDD_EXTRACT                     106
#define IDD_NEWFOLDER                   107
#define IDD_FOLDERPICK                  108
#define IDD_OVERWRITE                   109

/* commands (menu items and the toolbar buttons alike) */
#define IDM_FILE_OPEN                   201
#define IDM_FILE_EXIT                   204
#define IDM_FILE_CLOSE                  205
#define IDM_ARCHIVE_EXTRACT             206
#define IDM_ARCHIVE_INFO                207
#define IDM_ARCHIVE_TEST                208
#define IDM_HELP_ABOUT                  301

/* controls */
#define IDC_LIST                        1000
#define IDC_PROG_FILE                   1001
#define IDC_PROG_BAR                    1002
#define IDC_PROG_LABEL                  1003
#define IDC_INFO_TEXT                   1004
#define IDC_EXT_PATH                    1006
#define IDC_EXT_BROWSE                  1007
#define IDC_EXT_NEWDIR                  1008
#define IDC_NEWDIR_NAME                 1009
#define IDC_FP_LIST                     1010
#define IDC_FP_PATH                     1011
#define IDC_OW_FILE                     1012
#define IDC_OW_YESALL                   1013
#define IDC_OW_NOALL                    1014
/* PM-only: the Win32s dialog reuses the predefined IDYES / IDNO for its Yes
 * and No buttons; PM has no such stock IDs, so they get numbers here. */
#define IDC_OW_YES                      1015
#define IDC_OW_NO                       1016
#define IDC_STATIC                      -1

#endif /* RESOURCE_H */
