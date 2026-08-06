/*===========================================================================
 * RESOURCE.H  -  DiskStat (OS/2 PM) resource identifiers
 *
 * ID_DISKSTAT is doing double duty: it is the frame window's ID, which is how
 * PM decides which MENU, ICON and ACCELTABLE resources belong to the window.
 * All three therefore carry that same number in DISKSTAT.RC -- changing one
 * without the others silently loses the menu, the icon or F5.
 *===========================================================================*/
#ifndef DISKSTAT_RESOURCE_H
#define DISKSTAT_RESOURCE_H

#define ID_DISKSTAT                     1

/* dialogs */
#define IDD_ABOUT                       103
#define IDD_SCANNING                    104

/* dialog controls */
#define IDC_SCANTEXT                    1003
#define IDC_SCANPATH                    1006    /* directory being scanned    */
#define IDC_STATIC                      -1

/* submenus (PM wants an ID on a SUBMENU) */
#define IDM_FILE                        200
#define IDM_HELP                        201

/* commands */
#define IDM_FILE_REFRESH                40001
#define IDM_FILE_EXIT                   40002
#define IDM_HELP_ABOUT                  40200

#endif /* DISKSTAT_RESOURCE_H */
