/*===========================================================================
 * TOOLBAR.H  -  Toolbar and path combo declarations for Explorer
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *===========================================================================*/

#ifndef TOOLBAR_H
#define TOOLBAR_H

#define TB_HEIGHT   32      /* total height of the toolbar strip, in pixels */

extern HWND hwndToolbar;
extern HWND hwndPathCombo;

BOOL RegisterToolbarClass(HAB habAnchor);
HWND CreateToolbar(HWND hwndParent);
void LayoutToolbar(HWND hwndParent);

#endif /* TOOLBAR_H */
