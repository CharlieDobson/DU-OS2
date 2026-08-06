/*===========================================================================
 * RESOURCE.H  -  Explorer (OS/2 PM) resource identifiers
 *
 * Renumbered from the Windows script only where OS/2 forces it.  PM reserves
 * DID_OK (1) and DID_CANCEL (2) for dialog buttons, and every SUBMENU needs
 * an id of its own because WM_INITMENU names the submenu rather than passing
 * a position index.
 *===========================================================================*/

#ifndef RESOURCE_H
#define RESOURCE_H

/*---------------------------------------------------------------------------
 * The frame's window id.  FCF_ICON, FCF_MENU and FCF_ACCELTABLE all look
 * their resources up by this number, so the ICON, the MENU and the
 * ACCELTABLE must every one carry it.
 *-------------------------------------------------------------------------*/
#define ID_EXPLORER                     100

/*---------------------------------------------------------------------------
 * Menu bar submenus.  OS/2 submenus carry ids and WM_INITMENU names the
 * submenu in mp1, so nothing here depends on a menu's position.
 *-------------------------------------------------------------------------*/
#define IDM_MENU_FILE                   110
#define IDM_MENU_NEW                    111
#define IDM_MENU_EDIT                   112
#define IDM_MENU_VIEW                   113
#define IDM_MENU_NETWORK                114
#define IDM_MENU_HELP                   115

/* File menu */
#define IDM_FILE_EXIT                   101
#define IDM_FILE_NEW_DIR                102
#define IDM_FILE_RENAME                 103
#define IDM_FILE_DELETE                 104
#define IDM_FILE_BACK                   105
#define IDM_FILE_PROPS                  106
#define IDM_FILE_SEARCH                 107
#define IDM_FILE_NEW_WINDOW             108
#define IDM_FILE_REFRESH                109

/* Edit menu */
#define IDM_EDIT_CUT                    201
#define IDM_EDIT_COPY                   202
#define IDM_EDIT_PASTE                  203

/* View menu.  nCurrentView holds one of the first four. */
#define IDM_VIEW_SMALL                  401
#define IDM_VIEW_MEDIUM                 402
#define IDM_VIEW_LARGE                  403
#define IDM_VIEW_DETAILS                404
#define IDM_VIEW_SHOW_HIDDEN            405

/* Network menu */
#define IDM_NETWORK_CONNECT             1201
#define IDM_NETWORK_DISCONNECT          1202

/* Help menu */
#define IDM_HELP_ABOUT                  901

/* Context (button 2) menus and their commands */
#define IDM_CTXMENU_DRIVE               1150
#define IDM_CTXMENU_FILE                1151
#define IDM_CTX_OPEN                    1101
#define IDM_CTX_PROPS                   1102
#define IDM_CTX_DISCONNECT              1104

/*---------------------------------------------------------------------------
 * Bitmaps.  These are the Windows 3.x .BMP files unchanged: a 40-byte
 * BITMAPINFOHEADER is byte-for-byte an OS/2 BITMAPINFOHEADER2 truncated at a
 * field boundary (cbFix = 40), and the two file headers have the same layout,
 * so GpiLoadBitmap reads them directly.
 *-------------------------------------------------------------------------*/
#define IDB_TB_BACK                     501
#define IDB_TB_CUT                      502
#define IDB_TB_COPY                     503
#define IDB_TB_PASTE                    504
#define IDB_TB_RENAME                   505
#define IDB_TB_DELETE                   506
#define IDB_TB_PROPS                    507
#define IDB_TB_FIND                     508

#define IDB_SM_FILE                     601
#define IDB_SM_FOLDER                   602
#define IDB_SM_DRVLOCAL                 603
#define IDB_SM_DRVNET                   604
#define IDB_SM_DRVFLOP                  605
#define IDB_SM_DRVCDROM                 606
#define IDB_MD_FILE                     611
#define IDB_MD_FOLDER                   612
#define IDB_MD_DRVLOCAL                 613
#define IDB_MD_DRVNET                   614
#define IDB_MD_DRVFLOP                  615
#define IDB_MD_DRVCDROM                 616
#define IDB_LG_FILE                     621
#define IDB_LG_FOLDER                   622
#define IDB_LG_DRVLOCAL                 623
#define IDB_LG_DRVNET                   624
#define IDB_LG_DRVFLOP                  625
#define IDB_LG_DRVCDROM                 626

/*---------------------------------------------------------------------------
 * Dialogs
 *-------------------------------------------------------------------------*/
#define IDD_ABOUT                       300
#define IDD_RENAME                      301
#define IDD_NEWDIR                      302
#define IDD_FILEPROPERTIES              2000
#define IDD_DIRPROPERTIES               2001
#define IDD_COPYMOVE                    2002
#define IDD_DRIVEPROPERTIES             2003
#define IDD_CONNECTNETDRIVE             2004
#define IDD_DISCONNECTNETDRIVE          2005
#define IDD_OVERWRITE                   2007

/*---------------------------------------------------------------------------
 * Child window and dialog control ids
 *-------------------------------------------------------------------------*/
#define IDC_STATIC                      -1

/* Toolbar */
#define IDC_TB_PATHCOMBO                1001

/* Client-owned scrollbars.  PM has no WS_VSCROLL style for an ordinary
   window, so the content area drives two WC_SCROLLBAR children of its own. */
#define IDC_VSCROLL                     1002
#define IDC_HSCROLL                     1003

/* Search window */
#define IDC_SEARCH_PATTERN              1010
#define IDC_SEARCH_FOLDER               1011
#define IDC_SEARCH_SUBFOLDERS           1012
#define IDC_SEARCH_RESULTS              1013
#define IDC_SEARCH_BTN_GO               1014
#define IDC_SEARCH_BTN_CLOSE            1015
#define IDC_SEARCH_LBL_PATTERN          1016
#define IDC_SEARCH_LBL_FOLDER           1017

/* Rename / New directory */
#define IDC_RENAME_LABEL                1020
#define IDC_RENAME_EDIT                 1021
#define IDC_NEWDIR_EDIT                 1022

/* File properties */
#define IDC_FILENAME                    3006
#define IDC_PATHNAME                    3007
#define IDC_LASTCHANGE                  3008
#define IDC_FILESIZE                    3011
#define IDC_EASIZE                      3014
#define IDC_READONLY                    3000
#define IDC_ARCHIVE                     3001
#define IDC_HIDDEN                      3002
#define IDC_SYSTEM                      3003

/* Directory properties */
#define IDC_DIRECTORYNAME               3040
#define IDC_DIRECTORYPATH               3041
#define IDC_DIRECTORYLASTCHANGE         3042
#define IDC_DIRREADONLY                 3043
#define IDC_DIRARCHIVE                  3044
#define IDC_DIRHIDDEN                   3045
#define IDC_DIRSYSTEM                   3046

/* Copy / move progress */
#define IDC_COPYDLG_OPER                3020
#define IDC_COPYDLG_FROM                3021
#define IDC_COPYDLG_TO                  3022
#define IDC_COPYDLG_COUNT               3023

/* Drive properties */
#define IDC_DRIVEPROPS_LETTER           3030
#define IDC_DRIVEPROPS_LABEL            3031
#define IDC_DRIVEPROPS_FSTYPE           3032
#define IDC_DRIVEPROPS_BPS              3033
#define IDC_DRIVEPROPS_SPC              3034
#define IDC_DRIVEPROPS_CLUSTERS         3035
#define IDC_DRIVEPROPS_TOTAL            3036
#define IDC_DRIVEPROPS_FREE             3037

/* Network dialogs */
#define IDC_NET_LOCALDRIVE              3050
#define IDC_NET_PATH                    3051
#define IDC_NET_DISCONNLIST             3052
#define IDC_NET_BROWSE                  3053
#define IDC_NET_SHARES_LABEL            3054
#define IDC_NET_SHARES                  3055
#define IDC_RECONNECTATSTART            3056

/* Overwrite prompt.  "All" is a fourth button, so it needs an id of its own;
   PM's dialog return is whatever WinDismissDlg was given. */
#define IDC_OW_MSG                      3060
#define IDC_OW_YES                      3061
#define IDC_OW_NO                       3062
#define IDC_OW_ALL                      3063

#endif /* RESOURCE_H */
