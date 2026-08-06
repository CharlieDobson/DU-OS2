/*===========================================================================
 * SEARCH.H  -  Search window class registration and creation
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *===========================================================================*/

#ifndef SEARCH_H
#define SEARCH_H

/* The search window's FRAME, so it can be raised or destroyed as a whole */
extern HWND hwndSearchFrame;

BOOL RegisterSearchClass(HAB habAnchor);
HWND OpenSearchWindow(HWND hwndOwner);

/* The search runs on a worker thread that posts to the search window.  If the
   application is closed while the search window is still up, that worker has
   to be stopped too -- the search window's own WM_CLOSE handles the case
   where the user closes it directly. */
BOOL SearchBusy    (void);
BOOL SearchShutdown(void);

#endif /* SEARCH_H */
