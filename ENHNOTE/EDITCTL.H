/*====================================================================
 * EDITCTL.H -- Custom multiline edit control for Enhanced Notepad
 *              (OS/2 2.x / Warp Presentation Manager)
 *
 * A window class ("EnhEdit") wrapping the GAPBUF text engine, giving
 * a Notepad-style editing surface with no 64 KB limit, word wrap, and
 * full undo/redo.  Create it like any child window:
 *
 *     hwndEdit = WinCreateWindow(hwndParent, ENHEDIT_CLASS, NULL,
 *                    WS_VISIBLE, 0, 0, cx, cy,
 *                    hwndParent, HWND_TOP, IDC_EDIT, NULL, NULL);
 *
 * There is no WS_VSCROLL / WS_HSCROLL to hang on the window: PM has no
 * such style for an arbitrary window.
 * The control therefore creates its own WC_SCROLLBAR children inside
 * its rectangle and insets the text area around them, so the host
 * still deals with a single window.
 *
 * The owner receives WM_CONTROL with the ENHEN_CHANGE notification
 * code whenever the text is modified.
 *
 * Compiler: Open Watcom 1.9  (wcc386 -bt=os2)
 *====================================================================*/

#ifndef EDITCTL_H
#define EDITCTL_H

#include <os2.h>

#define ENHEDIT_CLASS  "EnhEdit"

/* WM_CONTROL notification code sent to the owner after an edit. */
#define ENHEN_CHANGE   0x0100

/* Register the window class once per process (FALSE on failure).
   The HAB is retained for the clipboard calls. */
BOOL EditRegister(HAB hab);

/*--------------------------------------------------------------------
 * Operations addressed by the control's HWND.  These are what the
 * host app's menu handlers call.
 *------------------------------------------------------------------*/

/* File */
BOOL EditLoad   (HWND hwnd, const char *path); /* replace w/ file    */
BOOL EditSave   (HWND hwnd, const char *path); /* write to file      */
void EditNewFile(HWND hwnd);                   /* clear to empty     */

/* Dirty flag */
BOOL EditIsModified (HWND hwnd);
void EditSetModified(HWND hwnd, BOOL bMod);

/* Word wrap */
void EditSetWordWrap(HWND hwnd, BOOL bWrap);
BOOL EditGetWordWrap(HWND hwnd);

/* Undo / redo */
BOOL EditCanUndo(HWND hwnd);
BOOL EditCanRedo(HWND hwnd);
void EditUndo   (HWND hwnd);
void EditRedo   (HWND hwnd);

/* Clipboard / selection */
void EditCut      (HWND hwnd);
void EditCopy     (HWND hwnd);
void EditPaste    (HWND hwnd);
void EditDeleteSel(HWND hwnd);
void EditSelectAll(HWND hwnd);
BOOL EditHasSel   (HWND hwnd);              /* is there a selection?   */
BOOL EditCanPaste (HWND hwnd);              /* CF_TEXT available?      */

/* Search: find 'needle' from just after the current selection.
   Returns TRUE and selects the hit, else FALSE. */
BOOL EditFindNext(HWND hwnd, const char *needle, BOOL bMatchCase);

#endif /* EDITCTL_H */
