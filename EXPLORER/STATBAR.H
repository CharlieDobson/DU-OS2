/*===========================================================================
 * STATBAR.H  -  Status bar window declarations for Explorer
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *===========================================================================*/

#ifndef STATBAR_H
#define STATBAR_H

#define SB_HEIGHT   20      /* total height of the status bar, in pixels */

extern HWND hwndStatusBar;

BOOL RegisterStatusBarClass(HAB habAnchor);
HWND CreateStatusBar(HWND hwndParent);
void LayoutStatusBar(HWND hwndParent);

void UpdateStatusBar(char cDriveLetter);
void ClearStatusBar(void);
void UpdateStatusSelection(void);

#endif /* STATBAR_H */
