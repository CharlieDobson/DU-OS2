/*===========================================================================
 * RESOURCE.H  -  Enhanced Notepad (OS/2 PM) resource identifiers
 *
 * ID_ENHNOTE is the frame's window ID,
 * which is what FCF_MENU / FCF_ICON / FCF_ACCELTABLE use to find those
 * three resources -- all of them must carry this number.  IDM_FILE and
 * friends give the pulldowns IDs, which OS/2's SUBMENU requires and which
 * WM_INITMENU then reports.
 *===========================================================================*/

#ifndef RESOURCE_H
#define RESOURCE_H

#define ID_ENHNOTE                      100     /* frame / menu / icon / accel */

/* controls */
#define IDC_EDIT                        1

/* pulldowns (SUBMENU needs an ID; WM_INITMENU reports it) */
#define IDM_FILE                        10
#define IDM_EDIT                        11
#define IDM_SEARCH                      12
#define IDM_HELP                        13

/* File */
#define IDM_FILE_NEW                    201
#define IDM_FILE_OPEN                   202
#define IDM_FILE_SAVE                   203
#define IDM_FILE_SAVEAS                 204
#define IDM_FILE_EXIT                   205

/* Edit */
#define IDM_EDIT_UNDO                   301
#define IDM_EDIT_CUT                    302
#define IDM_EDIT_COPY                   303
#define IDM_EDIT_PASTE                  304
#define IDM_EDIT_DELETE                 305
#define IDM_EDIT_SELECTALL              306
#define IDM_EDIT_WORDWRAP               307
#define IDM_EDIT_REDO                   308

/* Search */
#define IDM_SEARCH_FIND                 401
#define IDM_SEARCH_FINDNEXT             402

/* Help */
#define IDM_HELP_ABOUT                  501

/* dialogs */
#define IDD_FIND                        600
#define IDD_ABOUT                       601

/* dialog controls */
#define IDC_FINDTEXT                    1000
#define IDC_MATCHCASE                   1001
#define IDC_STATIC                      -1

#endif /* RESOURCE_H */
