/*===========================================================================
 * RESOURCE.H  -  Reveal (OS/2 PM) resource identifiers
 *
 * ID_REVEAL is doing double duty: it is the frame window's ID, which is how
 * PM decides which MENU, ICON and ACCELTABLE resources belong to the window.
 * All three therefore carry that same number in REVEAL.RC -- changing one
 * without the others silently loses the menu, the icon or the F-keys.
 *===========================================================================*/
#ifndef REVEAL_RESOURCE_H
#define REVEAL_RESOURCE_H

#define ID_REVEAL                       1

/* Category illustrations.  IDB_CPU..IDB_NX586 are picked at run time from
   the detected processor (RvCategoryBitmap in PMDATA.CPP); the rest are
   fixed per category.  Same numbers as the Windows builds. */
#define IDB_SYSTEM                      200
#define IDB_CPU                         201
#define IDB_386                         202
#define IDB_486                         203
#define IDB_I486                        204
#define IDB_AM486DX                     205
#define IDB_CX486                       206
#define IDB_TI486                       207
#define IDB_PENTIUM                     208
#define IDB_PPRO                        209
#define IDB_AMDK6                       210
#define IDB_CX6X86                      211
#define IDB_NX586                       212
#define IDB_MEMORY                      213
#define IDB_DRIVES                      215
#define IDB_NETWORK                     216
#define IDB_PORTS                       217
#define IDB_BENCHMARK                   218

/* dialogs */
#define IDD_ABOUT                       400
#define IDD_SUPPORT                     401

/* child windows */
#define IDC_STATIC                      -1
#define IDC_CATLIST                     1000
#define IDC_PANEBUTTON                  1001    /* the pane's push button   */
#define IDC_PANE                        1002
#define IDC_PANESCROLL                  1003

/* submenus (PM wants an ID on a SUBMENU) */
#define IDM_FILE                        300
#define IDM_VIEW                        301
#define IDM_HELP                        302

/* commands */
#define IDM_FILE_EXIT                   40003
#define IDM_VIEW_REFRESH                40010
#define IDM_VIEW_BENCHMARK              40011
#define IDM_HELP_ABOUT                  40020
#define IDM_HELP_SUPPORT                40021

/* One command per category, IDM_VIEW_FIRST + reveal_category_t.  The File
   menu in REVEAL.RC spells these out as numbers, in category order. */
#define IDM_VIEW_FIRST                  40100
#define IDM_VIEW_LAST                   40199

#endif /* REVEAL_RESOURCE_H */
