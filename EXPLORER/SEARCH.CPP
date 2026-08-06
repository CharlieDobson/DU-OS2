/*===========================================================================
 * SEARCH.CPP  -  Modeless search window: pattern match, subfolder recursion
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 *   CreateWindow(WS_OVERLAPPED|...)  ->  WinCreateStdWindow(FCF_*)
 *   "STATIC"/"EDIT"/"BUTTON"/"LISTBOX" -> WC_STATIC / WC_ENTRYFIELD /
 *                                         WC_BUTTON / WC_LISTBOX
 *   LB_ADDSTRING / LB_RESETCONTENT   ->  LM_INSERTITEM / LM_DELETEALL
 *   WM_CTLCOLOR                      ->  gone; PM controls take the system
 *                                        dialog colours by themselves
 *   fixed control coordinates        ->  a WM_SIZE layout, because PM
 *                                        measures y from the bottom and the
 *                                        window is resizable
 *
 * The pattern pass and the subdirectory pass are two DosFindFirst searches
 * with two HDIRs, which on OS/2 is simply allowed.
 *
 * THE SEARCH RUNS ON A WORKER THREAD (..\..\COMMON\PMWORKER.H).  Searching a
 * whole drive with subfolders on is the longest thing this program does, and
 * PM has one input queue: a window that stops servicing it freezes the whole
 * desktop, not just itself.
 *
 * The worker cannot touch the listbox, so results go through a queue: the
 * worker appends packed strings under a mutex, and a timer on the UI thread
 * drains whatever has accumulated into the listbox.  Results therefore appear
 * AS THEY ARE FOUND.
 *===========================================================================*/
#define INCL_WIN
#define INCL_DOSFILEMGR
#define INCL_DOSPROCESS
#define INCL_DOSSEMAPHORES
#define INCL_DOSERRORS

#include <os2.h>
#include <string.h>
#include <stdio.h>

#include "res/resource.h"
#include "explorer.h"
#include "search.h"
#include "pmworker.h"

HWND hwndSearchFrame = NULLHANDLE;

static HWND hwndSearchClient = NULLHANDLE;

/*==================================================================
 * The search worker and its result queue
 *
 *   A fixed queue, not a growing one.  The UI drains it five times a second,
 *   so it only fills when results arrive faster than that -- and when it
 *   does, the worker waits rather than dropping anything or reallocating
 *   under a lock.  A search that outruns the drain is bounded by the disk,
 *   not by this buffer.
 *==================================================================*/

#define SRCH_QUEUE_BYTES    16384
#define TID_SEARCHPROGRESS  3
#define SRCH_TIMER_MS       200

static PMWORKER g_Worker;
static int      g_bWorkerReady = 0;

static HMTX     g_hmtxQueue    = NULLHANDLE;
static char     g_szQueue[SRCH_QUEUE_BYTES];
static ULONG    g_cbQueue      = 0;     /* bytes used, guarded by the mutex */

static struct {
    char szFolder[CCHMAXPATH];
    char szPattern[CCHMAXPATH];
    BOOL bSubfolders;
    long cFound;
} g_Job;

static void SearchWorkerBody(void *arg);
static void DoSearch(const char *pszFolder, const char *pszPattern,
                     BOOL bSubfolders);

static void EnsureWorker(void)
{
    if (!g_bWorkerReady) {
        PmWorkerInit(&g_Worker);
        DosCreateMutexSem(NULL, &g_hmtxQueue, 0, FALSE);
        g_bWorkerReady = 1;
    }
}

BOOL SearchBusy(void)
{
    EnsureWorker();
    return PmWorkerIsBusy(&g_Worker);
}

BOOL SearchShutdown(void)
{
    EnsureWorker();

    if (!PmWorkerIsBusy(&g_Worker))
        return TRUE;

    return PmWorkerWait(&g_Worker, 5000);
}

/* Control layout constants */
#define LBL_X       8
#define EDIT_X      80
#define EDIT_H      22
#define ROW_GAP     26
#define BTN_W       70
#define BTN_H       24
#define MARGIN      8
#define WIN_W       380
#define WIN_H       360

#define FIND_FILE_ATTRS (FILE_NORMAL | FILE_READONLY | FILE_HIDDEN | \
                         FILE_SYSTEM | FILE_ARCHIVED)
#define FIND_ALL_ATTRS  (FIND_FILE_ATTRS | FILE_DIRECTORY)

/*------------------------------------------------------------------
  AddResultToList - append one full path to the listbox (UI THREAD)
------------------------------------------------------------------*/
static void AddResultToList(HWND hwnd, const char *pszPath)
{
    WinSendDlgItemMsg(hwnd, IDC_SEARCH_RESULTS, LM_INSERTITEM,
                      MPFROMLONG(LIT_END), MPFROMP(pszPath));
}

/*------------------------------------------------------------------
  QueueResult - hand one hit to the UI thread (WORKER THREAD)

    Blocks while the queue is full rather than dropping the result: the UI
    drains every 200 ms, so the wait is short and self-limiting.  The cancel
    flag is re-checked in the wait, so a Cancel is not held up by a queue the
    UI is no longer draining.
------------------------------------------------------------------*/
static void QueueResult(const char *pszPath)
{
    ULONG cb = (ULONG)strlen(pszPath) + 1;

    if (cb > SRCH_QUEUE_BYTES)
        return;                         /* absurdly long: skip it */

    for (;;) {
        BOOL bQueued = FALSE;

        if (DosRequestMutexSem(g_hmtxQueue, 5000) == NO_ERROR) {
            if (g_cbQueue + cb <= SRCH_QUEUE_BYTES) {
                memcpy(g_szQueue + g_cbQueue, pszPath, cb);
                g_cbQueue += cb;
                bQueued = TRUE;
            }
            DosReleaseMutexSem(g_hmtxQueue);
        }
        else {
            return;                     /* cannot lock: drop rather than hang */
        }

        if (bQueued || PmWorkerCancelled(&g_Worker))
            return;

        DosSleep(20);                   /* full: let the UI catch up */
    }
}

/*------------------------------------------------------------------
  SearchOnTimer - drain the queue into the listbox (UI THREAD)

    The pending bytes are copied out under the lock and the listbox is filled
    afterwards, so the worker is never blocked waiting on LM_INSERTITEM.
------------------------------------------------------------------*/
static void SearchOnTimer(HWND hwnd)
{
    char        szBatch[SRCH_QUEUE_BYTES];
    ULONG       cb = 0;
    const char *p;

    if (g_hmtxQueue == NULLHANDLE)
        return;

    if (DosRequestMutexSem(g_hmtxQueue, SEM_INDEFINITE_WAIT) != NO_ERROR)
        return;

    cb = g_cbQueue;
    if (cb > 0) {
        memcpy(szBatch, g_szQueue, cb);
        g_cbQueue = 0;
    }

    DosReleaseMutexSem(g_hmtxQueue);

    for (p = szBatch; (ULONG)(p - szBatch) < cb; p += strlen(p) + 1)
        AddResultToList(hwnd, p);
}

/*------------------------------------------------------------------
  SearchFinish - the search ended (UI THREAD)

    Called only after PmWorkerPollDone has answered TRUE, which it does once
    per search, so the caller does not have to care whether it got here from
    the timer or from WMU_WORKER_DONE.  The timer is the copy that matters:
    the worker posts that message from a thread with no anchor block, and a
    search whose completion is lost would sit with the button stuck on "Stop"
    and no "(no files found)" ever appearing.
------------------------------------------------------------------*/
static void SearchFinish(HWND hwnd, BOOL bCancelled)
{
    WinStopTimer(hab, hwnd, TID_SEARCHPROGRESS);

    /* One last drain: hits found between the final tick and the end */
    SearchOnTimer(hwnd);

    WinSetDlgItemText(hwnd, IDC_SEARCH_BTN_GO, (PCSZ)"~Search");

    if (g_Job.cFound == 0)
        AddResultToList(hwnd, bCancelled ? "(search stopped)"
                                         : "(no files found)");
    else if (bCancelled)
        AddResultToList(hwnd, "(search stopped)");
}

/*------------------------------------------------------------------
  DoSearch - list files under pszFolder matching pszPattern (WORKER)

    Recurses into subdirectories when bSubfolders is set.  The pattern pass
    and the subdirectory pass are two DosFindFirst searches with two HDIRs,
    which on OS/2 is simply allowed.

    !! NO PM CALLS !!  Hits go to QueueResult, not to the listbox.
------------------------------------------------------------------*/
static void DoSearch(const char *pszFolder,
                     const char *pszPattern, BOOL bSubfolders)
{
    HDIR         hdir = HDIR_CREATE;
    FILEFINDBUF3 ff;
    ULONG        cnt  = 1;
    char         szSearch[CCHMAXPATH];
    char         szFull[CCHMAXPATH];

    if (PmWorkerCancelled(&g_Worker))
        return;

    /* ---- pass 1: files matching the pattern ---- */
    strcpy(szSearch, pszFolder);
    EnsureTrailingSlash(szSearch);
    strcat(szSearch, pszPattern);

    if (DosFindFirst((PCSZ)szSearch, &hdir, FIND_FILE_ATTRS,
                     (PVOID)&ff, sizeof(ff), &cnt,
                     FIL_STANDARD) == NO_ERROR) {
        do {
            if (PmWorkerCancelled(&g_Worker))
                break;

            if (!(ff.attrFile & FILE_DIRECTORY)) {
                strcpy(szFull, pszFolder);
                EnsureTrailingSlash(szFull);
                strcat(szFull, ff.achName);
                QueueResult(szFull);
                g_Job.cFound++;
            }
            cnt = 1;
        } while (DosFindNext(hdir, (PVOID)&ff, sizeof(ff), &cnt) == NO_ERROR);

        DosFindClose(hdir);
    }

    if (!bSubfolders)
        return;

    /* ---- pass 2: recurse ---- */
    hdir = HDIR_CREATE;
    cnt  = 1;

    strcpy(szSearch, pszFolder);
    EnsureTrailingSlash(szSearch);
    strcat(szSearch, "*");

    if (DosFindFirst((PCSZ)szSearch, &hdir, FIND_ALL_ATTRS,
                     (PVOID)&ff, sizeof(ff), &cnt,
                     FIL_STANDARD) == NO_ERROR) {
        do {
            if (PmWorkerCancelled(&g_Worker))
                break;

            if ((ff.attrFile & FILE_DIRECTORY) && ff.achName[0] != '.') {
                strcpy(szFull, pszFolder);
                EnsureTrailingSlash(szFull);
                strcat(szFull, ff.achName);
                DoSearch(szFull, pszPattern, TRUE);
            }
            cnt = 1;
        } while (DosFindNext(hdir, (PVOID)&ff, sizeof(ff), &cnt) == NO_ERROR);

        DosFindClose(hdir);
    }
}

/*------------------------------------------------------------------
  SearchWorkerBody - the worker thread's entry point
------------------------------------------------------------------*/
static void SearchWorkerBody(void *arg)
{
    (void)arg;

    DoSearch(g_Job.szFolder, g_Job.szPattern, g_Job.bSubfolders);

    PmWorkerEnd(&g_Worker, (ULONG)g_Job.cFound);
}

/*------------------------------------------------------------------
  LayoutSearch - place the controls; called from WM_SIZE

    PM measures y from the bottom, so the rows are laid out downwards from
    yTop rather than nailed to fixed constants.
------------------------------------------------------------------*/
static void LayoutSearch(HWND hwnd)
{
    RECTL rcl;
    LONG  cx, cy, y, editW, lbTop, lbBottom;

    WinQueryWindowRect(hwnd, &rcl);
    cx = rcl.xRight - rcl.xLeft;
    cy = rcl.yTop   - rcl.yBottom;

    editW = cx - EDIT_X - MARGIN;
    if (editW < 60) editW = 60;

    /* row 1: "Search for:" */
    y = cy - MARGIN - EDIT_H;

    WinSetWindowPos(WinWindowFromID(hwnd, IDC_SEARCH_LBL_PATTERN), HWND_TOP,
                    LBL_X, y, EDIT_X - LBL_X - 4, EDIT_H,
                    SWP_MOVE | SWP_SIZE | SWP_SHOW);

    WinSetWindowPos(WinWindowFromID(hwnd, IDC_SEARCH_PATTERN), HWND_TOP,
                    EDIT_X, y, editW, EDIT_H,
                    SWP_MOVE | SWP_SIZE | SWP_SHOW);

    /* row 2: "In folder:" */
    y -= ROW_GAP;

    WinSetWindowPos(WinWindowFromID(hwnd, IDC_SEARCH_LBL_FOLDER), HWND_TOP,
                    LBL_X, y, EDIT_X - LBL_X - 4, EDIT_H,
                    SWP_MOVE | SWP_SIZE | SWP_SHOW);

    WinSetWindowPos(WinWindowFromID(hwnd, IDC_SEARCH_FOLDER), HWND_TOP,
                    EDIT_X, y, editW, EDIT_H,
                    SWP_MOVE | SWP_SIZE | SWP_SHOW);

    /* row 3: the subfolders checkbox */
    y -= ROW_GAP;
    WinSetWindowPos(WinWindowFromID(hwnd, IDC_SEARCH_SUBFOLDERS), HWND_TOP,
                    EDIT_X, y, 200, 20,
                    SWP_MOVE | SWP_SIZE | SWP_SHOW);

    /* buttons along the bottom */
    WinSetWindowPos(WinWindowFromID(hwnd, IDC_SEARCH_BTN_GO), HWND_TOP,
                    cx - (BTN_W * 2) - MARGIN - 4, MARGIN, BTN_W, BTN_H,
                    SWP_MOVE | SWP_SIZE | SWP_SHOW);

    WinSetWindowPos(WinWindowFromID(hwnd, IDC_SEARCH_BTN_CLOSE), HWND_TOP,
                    cx - BTN_W - MARGIN, MARGIN, BTN_W, BTN_H,
                    SWP_MOVE | SWP_SIZE | SWP_SHOW);

    /* the results list fills what is left */
    lbBottom = MARGIN + BTN_H + MARGIN;
    lbTop    = y - MARGIN;

    if (lbTop < lbBottom + 20)
        lbTop = lbBottom + 20;

    WinSetWindowPos(WinWindowFromID(hwnd, IDC_SEARCH_RESULTS), HWND_TOP,
                    MARGIN, lbBottom, cx - 2 * MARGIN, lbTop - lbBottom,
                    SWP_MOVE | SWP_SIZE | SWP_SHOW);
}

/*------------------------------------------------------------------
  SearchWndProc
------------------------------------------------------------------*/
static MRESULT EXPENTRY SearchWndProc(HWND hwnd, ULONG msg,
                                      MPARAM mp1, MPARAM mp2)
{
    switch (msg) {

    case WM_CREATE:
        WinCreateWindow(hwnd, WC_STATIC, (PCSZ)"Search for:",
                        WS_VISIBLE | SS_TEXT | DT_RIGHT | DT_VCENTER,
                        LBL_X, 0, EDIT_X - LBL_X - 4, EDIT_H,
                        hwnd, HWND_TOP, IDC_SEARCH_LBL_PATTERN, NULL, NULL);

        WinCreateWindow(hwnd, WC_ENTRYFIELD, (PCSZ)"*",
                        WS_VISIBLE | WS_TABSTOP | ES_MARGIN | ES_AUTOSCROLL,
                        0, 0, 0, 0,
                        hwnd, HWND_TOP, IDC_SEARCH_PATTERN, NULL, NULL);

        WinCreateWindow(hwnd, WC_STATIC, (PCSZ)"In folder:",
                        WS_VISIBLE | SS_TEXT | DT_RIGHT | DT_VCENTER,
                        LBL_X, 0, EDIT_X - LBL_X - 4, EDIT_H,
                        hwnd, HWND_TOP, IDC_SEARCH_LBL_FOLDER, NULL, NULL);

        WinCreateWindow(hwnd, WC_ENTRYFIELD, (PCSZ)"C:\\",
                        WS_VISIBLE | WS_TABSTOP | ES_MARGIN | ES_AUTOSCROLL,
                        0, 0, 0, 0,
                        hwnd, HWND_TOP, IDC_SEARCH_FOLDER, NULL, NULL);

        WinCreateWindow(hwnd, WC_BUTTON, (PCSZ)"Include ~subfolders",
                        WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                        0, 0, 0, 0,
                        hwnd, HWND_TOP, IDC_SEARCH_SUBFOLDERS, NULL, NULL);

        WinCreateWindow(hwnd, WC_LISTBOX, (PCSZ)"",
                        WS_VISIBLE | WS_TABSTOP | LS_HORZSCROLL,
                        0, 0, 0, 0,
                        hwnd, HWND_TOP, IDC_SEARCH_RESULTS, NULL, NULL);

        WinCreateWindow(hwnd, WC_BUTTON, (PCSZ)"~Search",
                        WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON |
                        BS_DEFAULT,
                        0, 0, 0, 0,
                        hwnd, HWND_TOP, IDC_SEARCH_BTN_GO, NULL, NULL);

        WinCreateWindow(hwnd, WC_BUTTON, (PCSZ)"~Close",
                        WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        0, 0, 0, 0,
                        hwnd, HWND_TOP, IDC_SEARCH_BTN_CLOSE, NULL, NULL);

        WinCheckButton(hwnd, IDC_SEARCH_SUBFOLDERS, 1);

        /* WinCreateStdWindow sizes the client itself, so a client that lays
           its children out on WM_SIZE alone may never get one. */
        LayoutSearch(hwnd);
        return (MRESULT)FALSE;

    case WM_SIZE:
        LayoutSearch(hwnd);
        return (MRESULT)FALSE;

    case WM_COMMAND:
        switch (SHORT1FROMMP(mp1)) {

        case IDC_SEARCH_BTN_GO: {
            char  szPattern[CCHMAXPATH];
            char  szFolder[CCHMAXPATH];
            BOOL  bSubfolders;
            HWND  hwndList;
            int   nLen;

            WinQueryDlgItemText(hwnd, IDC_SEARCH_PATTERN,
                                (LONG)sizeof(szPattern), (PSZ)szPattern);
            WinQueryDlgItemText(hwnd, IDC_SEARCH_FOLDER,
                                (LONG)sizeof(szFolder), (PSZ)szFolder);

            bSubfolders = (BOOL)WinQueryButtonCheckstate(
                                    hwnd, IDC_SEARCH_SUBFOLDERS);

            /* "*" rather than "*.*": on HPFS the latter would miss every
               name that has no extension. */
            if (szPattern[0] == '\0')
                strcpy(szPattern, "*");

            /* Strip a trailing backslash unless it is a root ("C:\") */
            nLen = (int)strlen(szFolder);
            if (nLen > 3 && szFolder[nLen - 1] == '\\')
                szFolder[nLen - 1] = '\0';

            hwndList = WinWindowFromID(hwnd, IDC_SEARCH_RESULTS);

            /* A running search is stopped by the same button, which is
               relabelled while one is going. */
            if (SearchBusy()) {
                PmWorkerRequestCancel(&g_Worker);
                return (MRESULT)FALSE;
            }

            WinSendMsg(hwndList, LM_DELETEALL, MPVOID, MPVOID);

            EnsureWorker();

            strcpy(g_Job.szFolder,  szFolder);
            strcpy(g_Job.szPattern, szPattern);
            g_Job.bSubfolders = bSubfolders;
            g_Job.cFound      = 0;
            g_cbQueue         = 0;

            if (!PmWorkerStart(&g_Worker, SearchWorkerBody, NULL,
                               PMWORKER_STACK, hwnd)) {
                AddResultToList(hwnd, "(could not start the search)");
                return (MRESULT)FALSE;
            }

            WinSetDlgItemText(hwnd, IDC_SEARCH_BTN_GO, (PCSZ)"~Stop");
            WinStartTimer(hab, hwnd, TID_SEARCHPROGRESS, SRCH_TIMER_MS);

            return (MRESULT)FALSE;
        }

        case IDC_SEARCH_BTN_CLOSE:
            WinPostMsg(hwnd, WM_CLOSE, MPVOID, MPVOID);
            return (MRESULT)FALSE;
        }
        break;

    case WM_TIMER:
        if (SHORT1FROMMP(mp1) == TID_SEARCHPROGRESS) {
            BOOL bCancelled = FALSE;

            SearchOnTimer(hwnd);

            if (PmWorkerPollDone(&g_Worker, NULL, &bCancelled))
                SearchFinish(hwnd, bCancelled);

            return (MRESULT)FALSE;
        }
        break;

    /* ---- the search worker finished ----
       A prompt only; it saves up to one tick and does nothing if the timer
       has already reaped the job.  mp2 is ignored -- the latch carries the
       cancelled flag and cannot be delivered twice. */
    case WMU_WORKER_DONE: {
        BOOL bCancelled = FALSE;

        if (PmWorkerPollDone(&g_Worker, NULL, &bCancelled))
            SearchFinish(hwnd, bCancelled);

        return (MRESULT)FALSE;
    }

    case WM_CLOSE: {
        HWND hwndDoomed = hwndSearchFrame;

        /* The worker posts WMU_WORKER_DONE to this window and writes into
           g_Job, so it has to be stopped before the window goes -- otherwise
           it is posting to a destroyed handle.  Refuse to close rather than
           tear down underneath a worker that will not stop. */
        if (SearchBusy() && !SearchShutdown()) {
            ExpMessageBox(hwnd,
                "The search has not stopped yet. Try again in a moment.",
                MB_OK | MB_WARNING);
            return (MRESULT)FALSE;
        }

        WinStopTimer(hab, hwnd, TID_SEARCHPROGRESS);

        /* Clear the globals BEFORE the window goes, so nothing reached
           during the teardown can find a stale handle in them. */
        hwndSearchFrame  = NULLHANDLE;
        hwndSearchClient = NULLHANDLE;

        WinDestroyWindow(hwndDoomed);
        return (MRESULT)FALSE;
    }
    }

    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

/*------------------------------------------------------------------
  RegisterSearchClass
------------------------------------------------------------------*/
BOOL RegisterSearchClass(HAB habAnchor)
{
    return WinRegisterClass(habAnchor, (PCSZ)EXP_SEARCH_CLASS,
                            SearchWndProc, CS_SIZEREDRAW, 0);
}

/*------------------------------------------------------------------
  OpenSearchWindow - create the window, or raise the one already up
------------------------------------------------------------------*/
HWND OpenSearchWindow(HWND hwndOwner)
{
    ULONG flFrameFlags;

    if (hwndSearchFrame != NULLHANDLE) {
        WinSetWindowPos(hwndSearchFrame, HWND_TOP, 0, 0, 0, 0,
                        SWP_ZORDER | SWP_SHOW | SWP_RESTORE | SWP_ACTIVATE);
        return hwndSearchFrame;
    }

    flFrameFlags = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER
                 | FCF_MINBUTTON | FCF_TASKLIST;

    hwndSearchFrame = WinCreateStdWindow(HWND_DESKTOP, 0L, &flFrameFlags,
                                         (PCSZ)EXP_SEARCH_CLASS,
                                         (PCSZ)"Search",
                                         0L, NULLHANDLE, 0,
                                         &hwndSearchClient);

    if (hwndSearchFrame == NULLHANDLE)
        return NULLHANDLE;

    (void)hwndOwner;

    WinSetWindowPos(hwndSearchFrame, HWND_TOP,
                    80, 80, WIN_W, WIN_H,
                    SWP_MOVE | SWP_SIZE | SWP_SHOW | SWP_ACTIVATE);

    return hwndSearchFrame;
}
