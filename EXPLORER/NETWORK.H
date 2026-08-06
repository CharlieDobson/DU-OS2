/*===========================================================================
 * NETWORK.H  -  Network drive connect/disconnect declarations for Explorer
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *===========================================================================*/

#ifndef NETWORK_H
#define NETWORK_H

/* TRUE when a redirector is present - that is, when NETAPI32.DLL loaded and
   the entry points we need resolved.  A base OS/2 install has no such DLL,
   and the Network menu greys out. */
BOOL IsNetworkPresent(void);

/* The Connect Network Drive modal dialog.  On success it calls NetUseAdd and
   saves the path to the history in OS2.INI. */
void OpenConnectNetDrive(HWND hwnd);

/* The Disconnect Network Drive modal dialog: every mapped remote drive,
   multiple selection. */
void OpenDisconnectNetDrive(HWND hwnd);

/* Disconnect one drive with no dialog, for the context menu.  Warns first
   if the redirector reports open files. */
void DisconnectNetDriveDirect(HWND hwnd, char cDrive);

/* Restore the connections saved with "Reconnect at startup".
   Call once from WM_CREATE, after EnumDrives. */
void ReconnectNetDrives(HWND hwnd);

#endif /* NETWORK_H */
