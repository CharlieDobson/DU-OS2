/*===========================================================================
 * RESOURCE.H  -  Setup for OS/2 (PM) resource identifiers
 *
 * ID_SETUP is the icon resource.  Setup has no frame window of its own, so
 * the number only matters twice: the ICON statement in SETUP.RC, and the
 * ICON controls on the Welcome and Complete pages that show it.  The
 * Workplace Shell also takes it as the icon of SETUP.EXE itself.
 *===========================================================================*/
#ifndef SETUP_RESOURCE_H
#define SETUP_RESOURCE_H

#define ID_SETUP                        1

/* dialogs - the pages, in the order they are shown */
#define IDD_WELCOME                     100
#define IDD_PROGRESS                    101
#define IDD_OPTIONS                     102
#define IDD_COMPLETE                    103

/* controls */
#define IDC_STATIC                      -1
#define IDC_ICON                        200
#define IDC_DRIVE                       201
#define IDC_FOLDER                      202
#define IDC_NOTE                        203     /* folder name, filled in at run time */
#define IDC_STATUS                      204
#define IDC_GAUGE                       205
#define IDC_OPT_ENHNOTE                 206
#define IDC_NOTE1                       207     /* "The programs are in the ...", Complete page */

#endif /* SETUP_RESOURCE_H */
