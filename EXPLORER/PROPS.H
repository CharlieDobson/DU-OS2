/*===========================================================================
 * PROPS.H  -  File, directory, and drive properties dialog declarations
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *===========================================================================*/

#ifndef PROPS_H
#define PROPS_H

MRESULT EXPENTRY FilePropsDlgProc (HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2);
MRESULT EXPENTRY DirPropsDlgProc  (HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2);
MRESULT EXPENTRY DrivePropsDlgProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2);

void DoFileProperties (HWND hwnd, int nIdx);
void DoDirProperties  (HWND hwnd, int nIdx);
void DoProperties     (HWND hwnd, int nIdx);
void DoDriveProperties(HWND hwnd, int nDriveIdx);

/* Shared with the status bar and the file dialogs: a DOS-packed date and
   time rendered with the user's OS2.INI PM_National settings. */
void FormatDateTime(FDATE fdate, FTIME ftime, char *pszOut);

#endif /* PROPS_H */
