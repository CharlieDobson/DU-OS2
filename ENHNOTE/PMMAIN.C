/*====================================================================
 * PMMAIN.C -- Enhanced Notepad for OS/2 2.x / Warp
 *             (32-bit Presentation Manager)
 *
 * Features:
 *   - File: New, Open, Save, Save As, Exit
 *   - Edit: Undo, Redo, Cut, Copy, Paste, Delete, Select All, Word Wrap
 *   - Search: Find, Find Next
 *   - Custom EnhEdit control: multi-megabyte files, full undo/redo
 *   - Command-line file argument
 *
 * The editing surface is our own EnhEdit control (EDITCTL.C) over a flat
 * gap buffer, so the file size ceiling is memory, not a system edit
 * control.
 *
 *      WinMain / RegisterClass       ->  main / WinRegisterClass
 *      CreateWindow(WS_OVERLAPPED..) ->  WinCreateStdWindow(FCF_*)
 *      GetMessage / DispatchMessage  ->  WinGetMsg / WinDispatchMsg
 *      TranslateAccelerator          ->  FCF_ACCELTABLE (the frame does it)
 *      WM_INITMENUPOPUP              ->  WM_INITMENU + MM_SETITEMATTR
 *      GetOpenFileName / GetSaveFile ->  WinFileDlg
 *      DialogBox                     ->  WinDlgBox
 *      MessageBox                    ->  WinMessageBox
 *      SetWindowText(main)           ->  WinSetWindowText(frame)
 *      CTL3D32.DLL for a 3-D look    ->  dropped; PM controls already are
 *
 * Compiler: Open Watcom 1.9  (wcc386 -bt=os2)
 *====================================================================*/

#define INCL_WIN
#define INCL_WINSTDFILE
#define INCL_DOSMISC
#define INCL_DOSERRORS

#include <os2.h>
#include <string.h>
#include <stdio.h>

#include "editctl.h"
#include "res/resource.h"

/*====================================================================
 * Globals
 *====================================================================*/

static HAB   hab       = NULLHANDLE;
static HWND  hwndFrame = NULLHANDLE;
static HWND  hwndClient= NULLHANDLE;
static HWND  hwndEdit  = NULLHANDLE;

static char  szFileName[CCHMAXPATH];    /* current path (empty=untitled) */
static char  szFindText[128];           /* last search string            */
static BOOL  bFindCase = FALSE;         /* case-sensitive search flag    */

/*====================================================================
 * String constants
 *====================================================================*/
static char szAppTitle[] = "Enhanced Notepad";
static char szWndClass[] = "EnhNotepad";
static char szUntitled[] = "(Untitled)";

/*====================================================================
 * Forward declarations
 *====================================================================*/
MRESULT EXPENTRY ClientWndProc(HWND, ULONG, MPARAM, MPARAM);
MRESULT EXPENTRY FindDlgProc  (HWND, ULONG, MPARAM, MPARAM);
MRESULT EXPENTRY AboutDlgProc (HWND, ULONG, MPARAM, MPARAM);

static BOOL  DoOpenFile(HWND, const char *);
static BOOL  DoSaveFile(HWND, const char *);
static BOOL  DoSave    (HWND);
static BOOL  DoSaveAs  (HWND);
static BOOL  DoOpen    (HWND);
static void  DoNew     (HWND);
static BOOL  QuerySave (HWND);
static void  DoFindNext(HWND);
static void  DoFind    (HWND);
static void  UpdateTitle(void);
static char *FilePart  (char *);

/*====================================================================
 * CreateStdFrame - WinCreateStdWindow, with the resource-backed frame
 *                  flags isolated and the PM error reported
 *
 *   FCF_MENU, FCF_ICON and FCF_ACCELTABLE each send the frame off to
 *   load a resource by the frame's window id, and a load that fails
 *   takes the whole create down with it.  All WinCreateStdWindow then
 *   says is NULLHANDLE - which is how a program that is otherwise
 *   perfectly healthy ends up doing nothing but putting up "Window
 *   creation failed".
 *
 *   So: try the lot; if that fails, try again without each resource
 *   flag in turn, and finally without any of them.  The program runs -
 *   one icon or menu short - and szDiag (256 bytes, the caller's) names
 *   the resource PM would not load and carries the PM error code.
 *   szDiag comes back empty when the first attempt worked, which is the
 *   normal case.
 *====================================================================*/

#define FCF_RESOURCE_FLAGS  (FCF_MENU | FCF_ICON | FCF_ACCELTABLE)

static HWND CreateStdFrame(HAB habApp, PCSZ pszClass, PCSZ pszTitle,
                           ULONG flWanted, ULONG idRes, HWND *phwndClient,
                           char *szDiag)
{
    static const ULONG  aflRes[3]  = { FCF_MENU, FCF_ICON, FCF_ACCELTABLE };
    static const char  *apszRes[3] = { "FCF_MENU", "FCF_ICON",
                                       "FCF_ACCELTABLE" };
    ULONG flCreate, flPresent, err;
    HWND  hwnd;
    int   i;

    szDiag[0] = '\0';

    flCreate = flWanted;
    hwnd = WinCreateStdWindow(HWND_DESKTOP, 0L, &flCreate, pszClass, pszTitle,
                              0L, NULLHANDLE, idRes, phwndClient);
    if (hwnd != NULLHANDLE)
        return hwnd;

    /* WinGetLastError both reports and clears, so take it once, here. */
    err       = (ULONG)WinGetLastError(habApp) & 0xFFFFL;
    flPresent = flWanted & FCF_RESOURCE_FLAGS;

    for (i = 0; i < 3; i++)
    {
        if (!(flPresent & aflRes[i])) continue;

        flCreate = flWanted & ~aflRes[i];
        hwnd = WinCreateStdWindow(HWND_DESKTOP, 0L, &flCreate, pszClass,
                                  pszTitle, 0L, NULLHANDLE, idRes,
                                  phwndClient);
        if (hwnd != NULLHANDLE)
        {
            sprintf(szDiag,
                    "The window could only be created with %s dropped, so PM "
                    "will not load that resource (id %lu) out of the .EXE.\n\n"
                    "PM error 0x%04lX.",
                    apszRes[i], (unsigned long)idRes, (unsigned long)err);
            return hwnd;
        }
    }

    /* No single flag to blame; if more than one is in play, try with all
       of them off before giving up.                                     */
    if (flPresent != 0 && flPresent != aflRes[0] &&
        flPresent != aflRes[1] && flPresent != aflRes[2])
    {
        flCreate = flWanted & ~FCF_RESOURCE_FLAGS;
        hwnd = WinCreateStdWindow(HWND_DESKTOP, 0L, &flCreate, pszClass,
                                  pszTitle, 0L, NULLHANDLE, idRes,
                                  phwndClient);
        if (hwnd != NULLHANDLE)
        {
            sprintf(szDiag,
                    "The window could only be created with the menu, icon and "
                    "accelerator flags all dropped: none of the .EXE's "
                    "resources for id %lu will load.\n\nPM error 0x%04lX.",
                    (unsigned long)idRes, (unsigned long)err);
            return hwnd;
        }
    }

    sprintf(szDiag, "WinCreateStdWindow failed with PM error 0x%04lX.",
            (unsigned long)err);
    return NULLHANDLE;
}

/*====================================================================
 * main
 *====================================================================*/
int main(int argc, char *argv[])
{
    HMQ   hmq;
    QMSG  qmsg;
    ULONG flFrameFlags;
    char  szDiag[256];

    szFileName[0] = '\0';
    szFindText[0] = '\0';

    hab = WinInitialize(0);
    if (hab == NULLHANDLE) return 1;

    hmq = WinCreateMsgQueue(hab, 0);
    if (hmq == NULLHANDLE) { WinTerminate(hab); return 1; }

    /* A bad path on the command line must not raise a hard-error popup. */
    DosError(FERR_DISABLEHARDERR);

    if (!WinRegisterClass(hab, (PCSZ)szWndClass, ClientWndProc,
                          CS_SIZEREDRAW, 0) ||
        !EditRegister(hab))
    {
        WinDestroyMsgQueue(hmq);
        WinTerminate(hab);
        return 1;
    }

    flFrameFlags = FCF_TITLEBAR | FCF_SYSMENU  | FCF_SIZEBORDER
                 | FCF_MINMAX   | FCF_TASKLIST | FCF_SHELLPOSITION
                 | FCF_MENU     | FCF_ICON     | FCF_ACCELTABLE;

    hwndFrame = CreateStdFrame(hab, (PCSZ)szWndClass, (PCSZ)szAppTitle,
                               flFrameFlags, ID_ENHNOTE,
                               &hwndClient, szDiag);
    if (hwndFrame == NULLHANDLE)
    {
        WinMessageBox(HWND_DESKTOP, HWND_DESKTOP, (PCSZ)szDiag,
                      (PCSZ)szAppTitle, 0, MB_OK | MB_ERROR);
        WinDestroyMsgQueue(hmq);
        WinTerminate(hab);
        return 1;
    }

    if (szDiag[0])
        WinMessageBox(HWND_DESKTOP, HWND_DESKTOP, (PCSZ)szDiag,
                      (PCSZ)szAppTitle, 0, MB_OK | MB_WARNING);

    /* Open a file named on the command line. */
    if (argc > 1 && argv[1][0])
        DoOpenFile(hwndClient, argv[1]);
    else
        UpdateTitle();

    WinShowWindow(hwndFrame, TRUE);
    WinSetFocus(HWND_DESKTOP, hwndEdit);

    while (WinGetMsg(hab, &qmsg, NULLHANDLE, 0, 0))
        WinDispatchMsg(hab, &qmsg);

    WinDestroyWindow(hwndFrame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}

/*====================================================================
 * FilePart -- pointer to the filename component within a path
 *====================================================================*/
static char *FilePart(char *pszPath)
{
    char *p    = pszPath;
    char *last = pszPath;

    while (*p)
    {
        if (*p == '\\' || *p == '/' || *p == ':') last = p + 1;
        p++;
    }
    return last;
}

/*====================================================================
 * UpdateTitle -- set the frame caption to "name - Enhanced Notepad"
 *====================================================================*/
static void UpdateTitle(void)
{
    char  buf[CCHMAXPATH + 64];
    char *p = szFileName[0] ? FilePart(szFileName) : szUntitled;

    sprintf(buf, "%s - %s", p, szAppTitle);
    WinSetWindowText(hwndFrame, (PCSZ)buf);
}

/*====================================================================
 * A message box in the app's voice
 *====================================================================*/
static ULONG Say(HWND hwnd, const char *text, ULONG flags)
{
    return WinMessageBox(HWND_DESKTOP, hwnd, (PCSZ)text,
                         (PCSZ)szAppTitle, 0, flags | MB_MOVEABLE);
}

/*====================================================================
 * DoOpenFile / DoSaveFile
 *====================================================================*/
static BOOL DoOpenFile(HWND hwnd, const char *pszFile)
{
    char msg[CCHMAXPATH + 64];

    if (!EditLoad(hwndEdit, pszFile))
    {
        sprintf(msg, "Cannot open:\n%s", pszFile);
        Say(hwnd, msg, MB_OK | MB_ICONEXCLAMATION);
        return FALSE;
    }

    strcpy(szFileName, pszFile);
    UpdateTitle();
    return TRUE;
}

static BOOL DoSaveFile(HWND hwnd, const char *pszFile)
{
    char msg[CCHMAXPATH + 64];

    if (!EditSave(hwndEdit, pszFile))
    {
        sprintf(msg, "Cannot write:\n%s", pszFile);
        Say(hwnd, msg, MB_OK | MB_ICONEXCLAMATION);
        return FALSE;
    }
    return TRUE;
}

/*====================================================================
 * FileDialog -- WinFileDlg wrapper standing in for GetOpen/SaveFileName
 *
 *   szFullFile doubles as input and output: seeding it with a pattern
 *   sets the initial filter, seeding it with a path preselects that
 *   file.  The save flavour prompts on overwrite by itself, so there is
 *   no overwrite-prompt flag to set.
 *====================================================================*/
static BOOL FileDialog(HWND hwnd, char *pszFile, BOOL bSave)
{
    FILEDLG fd;

    memset(&fd, 0, sizeof(fd));
    fd.cbSize   = sizeof(FILEDLG);
    fd.fl       = FDS_CENTER |
                  (bSave ? FDS_SAVEAS_DIALOG : FDS_OPEN_DIALOG);
    fd.pszTitle = (PSZ)(bSave ? "Save As" : "Open");

    if (pszFile[0]) strcpy(fd.szFullFile, pszFile);
    else            strcpy(fd.szFullFile, "*.TXT");

    if (WinFileDlg(HWND_DESKTOP, hwnd, &fd) == NULLHANDLE)
        return FALSE;
    if (fd.lReturn != DID_OK)
        return FALSE;

    strcpy(pszFile, fd.szFullFile);
    return TRUE;
}

/*====================================================================
 * DoSave / DoSaveAs / DoOpen / DoNew / QuerySave
 *====================================================================*/
static BOOL DoSave(HWND hwnd)
{
    if (!szFileName[0]) return DoSaveAs(hwnd);
    return DoSaveFile(hwnd, szFileName);
}

static BOOL DoSaveAs(HWND hwnd)
{
    char szFile[CCHMAXPATH];

    strcpy(szFile, szFileName);
    if (!FileDialog(hwnd, szFile, TRUE))
        return FALSE;

    strcpy(szFileName, szFile);
    UpdateTitle();
    return DoSaveFile(hwnd, szFileName);
}

static BOOL DoOpen(HWND hwnd)
{
    char szFile[CCHMAXPATH];

    if (!QuerySave(hwnd)) return FALSE;

    szFile[0] = '\0';
    if (!FileDialog(hwnd, szFile, FALSE))
        return FALSE;

    return DoOpenFile(hwnd, szFile);
}

static void DoNew(HWND hwnd)
{
    if (!QuerySave(hwnd)) return;

    szFileName[0] = '\0';
    EditNewFile(hwndEdit);
    UpdateTitle();
}

/* If modified, ask the user to save.  FALSE = the user cancelled. */
static BOOL QuerySave(HWND hwnd)
{
    char  buf[CCHMAXPATH + 64];
    char *pName;
    ULONG r;

    if (!EditIsModified(hwndEdit)) return TRUE;

    pName = szFileName[0] ? FilePart(szFileName) : szUntitled;
    sprintf(buf, "Save changes to %s?", pName);
    r = Say(hwnd, buf, MB_YESNOCANCEL | MB_QUERY);

    if (r == MBID_YES) return DoSave(hwnd);
    if (r == MBID_NO)  return TRUE;
    return FALSE;                           /* MBID_CANCEL           */
}

/*====================================================================
 * Find
 *====================================================================*/
static void DoFind(HWND hwnd)
{
    if (WinDlgBox(HWND_DESKTOP, hwnd, FindDlgProc,
                  NULLHANDLE, IDD_FIND, NULL) == DID_OK)
        DoFindNext(hwnd);
}

static void DoFindNext(HWND hwnd)
{
    char msg[196];

    if (!szFindText[0]) { DoFind(hwnd); return; }

    if (!EditFindNext(hwndEdit, szFindText, bFindCase))
    {
        sprintf(msg, "Cannot find \"%s\".", szFindText);
        Say(hwnd, msg, MB_OK | MB_INFORMATION);
    }
}

/*====================================================================
 * Menu item state
 *====================================================================*/
static void MenuEnable(HWND hwndMenu, USHORT id, BOOL bEnable)
{
    WinSendMsg(hwndMenu, MM_SETITEMATTR,
               MPFROM2SHORT((SHORT)id, TRUE),
               MPFROM2SHORT(MIA_DISABLED,
                            (SHORT)(bEnable ? 0 : MIA_DISABLED)));
}

static void MenuCheck(HWND hwndMenu, USHORT id, BOOL bCheck)
{
    WinSendMsg(hwndMenu, MM_SETITEMATTR,
               MPFROM2SHORT((SHORT)id, TRUE),
               MPFROM2SHORT(MIA_CHECKED,
                            (SHORT)(bCheck ? MIA_CHECKED : 0)));
}

/*====================================================================
 * ClientWndProc
 *====================================================================*/
MRESULT EXPENTRY ClientWndProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg)
    {

    /*----------------------------------------------------------------
     * The edit control fills the client area.  It is sized here rather
     * than left to WM_SIZE: WinCreateStdWindow creates the client
     * already at its final size, so a WM_SIZE need never arrive.
     *----------------------------------------------------------------*/
    case WM_CREATE:
    {
        RECTL rcl;

        WinQueryWindowRect(hwnd, &rcl);
        hwndEdit = WinCreateWindow(hwnd, (PCSZ)ENHEDIT_CLASS, (PCSZ)"",
                       WS_VISIBLE, 0, 0,
                       rcl.xRight - rcl.xLeft, rcl.yTop - rcl.yBottom,
                       hwnd, HWND_TOP, IDC_EDIT, NULL, NULL);
        if (hwndEdit == NULLHANDLE)
            return (MRESULT)TRUE;           /* abort window creation */
        return (MRESULT)FALSE;
    }

    case WM_SIZE:
        if (hwndEdit != NULLHANDLE)
            WinSetWindowPos(hwndEdit, HWND_TOP, 0, 0,
                            (LONG)SHORT1FROMMP(mp2),
                            (LONG)SHORT2FROMMP(mp2),
                            SWP_MOVE | SWP_SIZE | SWP_SHOW);
        return (MRESULT)FALSE;

    case WM_SETFOCUS:
        if (SHORT1FROMMP(mp2) && hwndEdit != NULLHANDLE)
            WinSetFocus(HWND_DESKTOP, hwndEdit);
        return (MRESULT)FALSE;

    /*----------------------------------------------------------------
     * Grey / check the Edit menu as it drops down.  PM names the
     * submenu in mp1, so there is no positional index to keep in step
     * with the resource script.
     *----------------------------------------------------------------*/
    case WM_INITMENU:
        if (SHORT1FROMMP(mp1) == IDM_EDIT)
        {
            HWND hwndMenu = HWNDFROMMP(mp2);
            BOOL bSel     = EditHasSel(hwndEdit);

            MenuEnable(hwndMenu, IDM_EDIT_UNDO,  EditCanUndo(hwndEdit));
            MenuEnable(hwndMenu, IDM_EDIT_REDO,  EditCanRedo(hwndEdit));
            MenuEnable(hwndMenu, IDM_EDIT_CUT,   bSel);
            MenuEnable(hwndMenu, IDM_EDIT_COPY,  bSel);
            MenuEnable(hwndMenu, IDM_EDIT_PASTE, EditCanPaste(hwndEdit));
            MenuEnable(hwndMenu, IDM_EDIT_DELETE, bSel);
            MenuEnable(hwndMenu, IDM_EDIT_SELECTALL, TRUE);
            MenuCheck (hwndMenu, IDM_EDIT_WORDWRAP,
                       EditGetWordWrap(hwndEdit));
        }
        return (MRESULT)FALSE;

    /*----------------------------------------------------------------
     * Menu items and accelerators
     *----------------------------------------------------------------*/
    case WM_COMMAND:
        switch (SHORT1FROMMP(mp1))
        {
        /* File */
        case IDM_FILE_NEW:      DoNew(hwnd);    break;
        case IDM_FILE_OPEN:     DoOpen(hwnd);   break;
        case IDM_FILE_SAVE:     DoSave(hwnd);   break;
        case IDM_FILE_SAVEAS:   DoSaveAs(hwnd); break;
        case IDM_FILE_EXIT:
            WinPostMsg(hwnd, WM_CLOSE, MPVOID, MPVOID);
            break;

        /* Edit */
        case IDM_EDIT_UNDO:      EditUndo(hwndEdit);      break;
        case IDM_EDIT_REDO:      EditRedo(hwndEdit);      break;
        case IDM_EDIT_CUT:       EditCut(hwndEdit);       break;
        case IDM_EDIT_COPY:      EditCopy(hwndEdit);      break;
        case IDM_EDIT_PASTE:     EditPaste(hwndEdit);     break;
        case IDM_EDIT_DELETE:    EditDeleteSel(hwndEdit); break;
        case IDM_EDIT_SELECTALL: EditSelectAll(hwndEdit); break;
        case IDM_EDIT_WORDWRAP:
            EditSetWordWrap(hwndEdit, !EditGetWordWrap(hwndEdit));
            break;

        /* Search */
        case IDM_SEARCH_FIND:     DoFind(hwnd);     break;
        case IDM_SEARCH_FINDNEXT: DoFindNext(hwnd); break;

        /* Help */
        case IDM_HELP_ABOUT:
            WinDlgBox(HWND_DESKTOP, hwnd, AboutDlgProc,
                      NULLHANDLE, IDD_ABOUT, NULL);
            break;
        }
        return (MRESULT)FALSE;

    /*----------------------------------------------------------------
     * WM_CLOSE: prompt to save.  Swallowing the message (rather than
     * letting WinDefWindowProc have it) is what keeps the window alive
     * when the user cancels.
     *----------------------------------------------------------------*/
    case WM_CLOSE:
        if (QuerySave(hwnd))
            WinPostMsg(hwnd, WM_QUIT, MPVOID, MPVOID);
        return (MRESULT)FALSE;
    }

    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

/*====================================================================
 * FindDlgProc
 *====================================================================*/
MRESULT EXPENTRY FindDlgProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg)
    {
    case WM_INITDLG:
        WinSetDlgItemText(hwnd, IDC_FINDTEXT, (PCSZ)szFindText);
        WinSendDlgItemMsg(hwnd, IDC_MATCHCASE, BM_SETCHECK,
                          MPFROMSHORT((SHORT)(bFindCase ? 1 : 0)), MPVOID);
        WinSetFocus(HWND_DESKTOP, WinWindowFromID(hwnd, IDC_FINDTEXT));
        return (MRESULT)TRUE;               /* we set the focus       */

    case WM_COMMAND:
        switch (SHORT1FROMMP(mp1))
        {
        case DID_OK:
            WinQueryDlgItemText(hwnd, IDC_FINDTEXT,
                                (LONG)sizeof(szFindText), (PSZ)szFindText);
            bFindCase = (BOOL)(SHORT)(LONG)
                WinSendDlgItemMsg(hwnd, IDC_MATCHCASE, BM_QUERYCHECK,
                                  MPVOID, MPVOID);
            WinDismissDlg(hwnd, DID_OK);
            return (MRESULT)FALSE;
        case DID_CANCEL:
            WinDismissDlg(hwnd, DID_CANCEL);
            return (MRESULT)FALSE;
        }
        break;
    }

    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

/*====================================================================
 * AboutDlgProc
 *====================================================================*/
MRESULT EXPENTRY AboutDlgProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg)
    {
    case WM_COMMAND:
        if (SHORT1FROMMP(mp1) == DID_OK || SHORT1FROMMP(mp1) == DID_CANCEL)
        {
            WinDismissDlg(hwnd, TRUE);
            return (MRESULT)FALSE;
        }
        break;
    }

    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}
