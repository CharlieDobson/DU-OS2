/*===========================================================================
 * FILEOPS.CPP  -  File operations: copy, move, delete, rename, clipboard
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 *   _lopen/_lread/_lwrite copy loop      ->  DosCopy  (see below)
 *   copy-then-delete to move a file      ->  DosMove within a volume
 *   _dos_getfileattr / _dos_setfileattr  ->  DosQueryPathInfo / DosSetPathInfo
 *   remove / rename / _mkdir / _rmdir    ->  DosDelete / DosMove /
 *                                            DosCreateDir / DosDeleteDir
 *   _dos_getdiskfree                     ->  DosQueryFSInfo(FSIL_ALLOC)
 *   ShellExecute("open")                 ->  WinQueryObject + WinOpenObject,
 *                                            WinStartApp for programs
 *   FindExecutable                       ->  WinQueryObject
 *   GlobalAlloc + clipboard              ->  DosAllocSharedMem(OBJ_GIVEABLE)
 *                                            + WinSetClipbrdData(CFI_POINTER)
 *   DialogBox / CreateDialog             ->  WinDlgBox / WinLoadDlg
 *
 * EXTENDED ATTRIBUTES SURVIVE A COPY, and that is why DosCopy is used rather
 * than a read/write loop.  A hand-rolled copy carries nothing but the
 * contents: on HPFS the destination arrives stripped of .LONGNAME, .TYPE,
 * .ICON, .SUBJECT and every Workplace Shell setting hung off the file, and on
 * any file system it loses its timestamps and attribute byte too.  A
 * directory is made by DosCreateDir, which cannot carry EAs, so CopyDirEAs
 * walks the folder's own across afterwards.  A move within one volume is
 * DosMove -- a rename, so there is nothing to lose and nothing to copy.
 *
 * The tree walks recurse straight out of their own find loop: OS/2 hands out
 * an independent HDIR per search, so there is no need to buffer child names
 * first and no cap on how many subdirectories a level may have.
 *
 * THE TRANSFER RUNS ON A WORKER THREAD (..\..\COMMON\PMWORKER.H):
 *
 *      DoPasteFromList / DoDelete*    UI thread; confirm, then start a worker
 *      FileWorkerBody                 worker; PasteWork / DeleteWork
 *      FileOpsOnAsk                   UI thread; the overwrite prompt
 *      FileOpsOnDone                  UI thread; report and refresh
 *
 * The worker makes no PM calls.  Two consequences:
 *
 *   * "Overwrite?" is a modal question in the middle of the work, so the
 *     worker posts it with PmWorkerAsk and BLOCKS until the UI thread
 *     answers.  Every path through FileOpsOnAsk must end in PmWorkerAnswer.
 *
 *   * There are no per-file error boxes.  Failures are counted and the first
 *     is kept; FileOpsOnDone reports once, at the end.
 *===========================================================================*/
#define INCL_WIN
#define INCL_WINWORKPLACE
#define INCL_WINCLIPBOARD
#define INCL_DOSFILEMGR
#define INCL_DOSMEMMGR
#define INCL_DOSPROCESS
#define INCL_DOSERRORS

#include <os2.h>
#include <string.h>
#include <stdio.h>

#include "res/resource.h"
#include "explorer.h"
#include "dirview.h"
#include "fileops.h"
#include "pmworker.h"

/* WinOpenObject's view parameter.  The Workplace Shell defines OPEN_DEFAULT
   as 0, but Open Watcom's <pmwp.h> declares the function without the
   constants, so it is spelled out here. */
#define WP_OPEN_DEFAULT 0UL

#define FIND_ATTRS  (FILE_NORMAL   | FILE_READONLY | FILE_HIDDEN | \
                     FILE_SYSTEM   | FILE_ARCHIVED | FILE_DIRECTORY)

/*------------------------------------------------------------------
  IsExecFile - TRUE for the things OS/2 will start directly
------------------------------------------------------------------*/
BOOL IsExecFile(const char *pszName)
{
    size_t n = strlen(pszName);

    if (n < 4)
        return FALSE;

    return (BOOL)(stricmp(pszName + n - 4, ".EXE") == 0 ||
                  stricmp(pszName + n - 4, ".COM") == 0 ||
                  stricmp(pszName + n - 4, ".CMD") == 0 ||
                  stricmp(pszName + n - 4, ".BAT") == 0);
}

/*------------------------------------------------------------------
  BuildFullPath - szCurrentPath + name
------------------------------------------------------------------*/
static void BuildFullPath(char *pszOut, const char *pszName)
{
    strcpy(pszOut, szCurrentPath);
    EnsureTrailingSlash(pszOut);
    strcat(pszOut, pszName);
}

/*------------------------------------------------------------------
  PathTail - the file-name component of a path
------------------------------------------------------------------*/
static const char *PathTail(const char *pszPath)
{
    const char *p = pszPath + strlen(pszPath);

    while (p > pszPath && *(p - 1) != '\\' && *(p - 1) != '/')
        p--;

    return p;
}

/*------------------------------------------------------------------
  QueryAttr / ClearReadOnly - the DosSetPathInfo dance
------------------------------------------------------------------*/
static BOOL QueryAttr(const char *pszPath, ULONG *pAttr)
{
    FILESTATUS3 fs3;

    if (DosQueryPathInfo((PCSZ)pszPath, FIL_STANDARD,
                         (PVOID)&fs3, sizeof(fs3)) != NO_ERROR)
        return FALSE;

    *pAttr = fs3.attrFile;
    return TRUE;
}

static void SetAttr(const char *pszPath, ULONG ulAttr)
{
    FILESTATUS3 fs3;

    if (DosQueryPathInfo((PCSZ)pszPath, FIL_STANDARD,
                         (PVOID)&fs3, sizeof(fs3)) != NO_ERROR)
        return;

    fs3.attrFile = ulAttr;
    DosSetPathInfo((PCSZ)pszPath, FIL_STANDARD,
                   (PVOID)&fs3, sizeof(fs3), 0UL);
}

static void ClearReadOnly(const char *pszPath)
{
    ULONG ulAttr;

    if (QueryAttr(pszPath, &ulAttr) && (ulAttr & FILE_READONLY))
        SetAttr(pszPath, ulAttr & ~(ULONG)FILE_READONLY);
}

static BOOL IsDirectory(const char *pszPath)
{
    ULONG ulAttr;

    return (BOOL)(QueryAttr(pszPath, &ulAttr) &&
                  (ulAttr & FILE_DIRECTORY) != 0);
}

static BOOL IsDotEntry(const char *pszName)
{
    return (BOOL)(pszName[0] == '.' &&
                  (pszName[1] == '\0' ||
                   (pszName[1] == '.' && pszName[2] == '\0')));
}

/*------------------------------------------------------------------
  JoinPath - dir + "\" + name, into a CCHMAXPATH buffer
------------------------------------------------------------------*/
static void JoinPath(char *pszOut, const char *pszDir, const char *pszName)
{
    strcpy(pszOut, pszDir);
    EnsureTrailingSlash(pszOut);
    strcat(pszOut, pszName);
}

/*==================================================================
 * The worker
 *
 *   One worker, two kinds of job.  g_Job is written by the UI thread before
 *   the worker starts and read by the worker afterwards; PmWorkerStart is the
 *   handover, so neither needs a lock.  The results at the bottom go the
 *   other way and are read only once the worker has stopped, from
 *   FileOpsOnDone.
 *==================================================================*/

#define FILEJOB_PASTE       1           /* copy or move a path list         */
#define FILEJOB_DELETE      2           /* delete a path list               */

#define ASK_OVERWRITE       1           /* PmWorkerAsk question code        */

#define FILE_TIMER_MS       200

static PMWORKER g_Worker;
static int      g_bWorkerReady = 0;

static struct {
    int          nKind;
    EXPPATHLIST *pList;                 /* the WORKER frees this            */
    BOOL         bIsMove;
    BOOL         bClearClipOnDone;      /* the paste came from a Cut        */
    char         szDstDir[CCHMAXPATH];

    /* filled in by the worker, read by FileOpsOnDone */
    int          nErrors;
    char         szError[2 * CCHMAXPATH + 160];
} g_Job;

static BOOL StartFileJob(HWND hwnd, int nKind, EXPPATHLIST *pList,
                         BOOL bIsMove, const char *pszDstDir);

static void EnsureWorker(void)
{
    if (!g_bWorkerReady) {
        PmWorkerInit(&g_Worker);
        g_bWorkerReady = 1;
    }
}

BOOL FileOpsBusy(void)
{
    EnsureWorker();
    return PmWorkerIsBusy(&g_Worker);
}

/*------------------------------------------------------------------
  RecordError - remember the first failure, count the rest

    A worker cannot put a message box up, and a box per failed file would
    mean a hundred dialogs for a hundred unwritable files anyway.
------------------------------------------------------------------*/
static void RecordError(const char *pszFmt, const char *pszA, const char *pszB)
{
    g_Job.nErrors++;

    if (g_Job.szError[0] == '\0')
        sprintf(g_Job.szError, pszFmt, pszA, pszB);
}

/*==================================================================
 * Copy / move progress dialog
 *
 *   hwndDlg belongs to the UI thread and is only ever touched there.
 *   bOverwriteAll belongs to the worker.  The counters and the file name are
 *   written by the worker and read by the UI thread's timer -- see the note
 *   on the name buffer below.
 *==================================================================*/

static struct {
    HWND hwndDlg;                       /* UI thread only                   */
    BOOL bOverwriteAll;                 /* worker only                      */
    int  nCurrent;                      /* worker writes, UI reads          */
    int  nTotal;
} g_CopyProg;

/* The file the worker is on.  One byte longer than the longest path it can be
   given, and that byte is never written, so the buffer is always terminated
   inside its own storage however a read and a write interleave: the worst
   case is one timer tick showing a mix of two names.  A mutex here would buy
   nothing but a deadlock risk against PmWorkerAsk. */
static char g_szProgFile[CCHMAXPATH + 1];

MRESULT EXPENTRY CopyMoveDlgProc(HWND hwnd, ULONG msg,
                                 MPARAM mp1, MPARAM mp2)
{
    switch (msg) {

    case WM_COMMAND:
        if (SHORT1FROMMP(mp1) == DID_CANCEL) {
            PmWorkerRequestCancel(&g_Worker);
            WinEnableWindow(WinWindowFromID(hwnd, DID_CANCEL), FALSE);
            return (MRESULT)FALSE;
        }
        break;

    case WM_CLOSE:
        PmWorkerRequestCancel(&g_Worker);
        return (MRESULT)FALSE;          /* keep it up; we destroy it */
    }

    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

/*------------------------------------------------------------------
  CopyProg_Init - put the progress dialog up (UI thread)

    nTotal is not known yet when this runs: counting the files means walking
    every tree in the list, which is itself slow enough to belong on the
    worker.  The dialog therefore opens saying "Preparing" and the count
    appears on the first timer tick after the worker has finished counting.
------------------------------------------------------------------*/
static void CopyProg_Init(HWND hwndParent, BOOL bIsMove, const char *pszDstDir)
{
    char szOper[CCHMAXPATH + 80];

    g_CopyProg.bOverwriteAll = FALSE;
    g_CopyProg.nCurrent      = 0;
    g_CopyProg.nTotal        = 0;
    g_szProgFile[0]          = '\0';

    g_CopyProg.hwndDlg = WinLoadDlg(HWND_DESKTOP, hwndParent,
                                    CopyMoveDlgProc, NULLHANDLE,
                                    IDD_COPYMOVE, NULL);
    if (g_CopyProg.hwndDlg == NULLHANDLE)
        return;

    WinSetWindowText(g_CopyProg.hwndDlg,
                     (PCSZ)(bIsMove ? "Moving" : "Copying"));

    sprintf(szOper, "%s to %s", bIsMove ? "Moving" : "Copying", pszDstDir);

    WinSetDlgItemText(g_CopyProg.hwndDlg, IDC_COPYDLG_OPER,  (PCSZ)szOper);
    WinSetDlgItemText(g_CopyProg.hwndDlg, IDC_COPYDLG_TO,    (PCSZ)pszDstDir);
    WinSetDlgItemText(g_CopyProg.hwndDlg, IDC_COPYDLG_COUNT,
                      (PCSZ)"Preparing ...");

    WinShowWindow(g_CopyProg.hwndDlg, TRUE);
}

/*------------------------------------------------------------------
  CopyProg_Update - record which file is being handled (WORKER thread)

    No PM calls, and no pump.  The UI reads what this leaves behind, on its
    own timer.
------------------------------------------------------------------*/
static void CopyProg_Update(const char *pszSrc)
{
    const char *pszTail = PathTail(pszSrc);

    g_CopyProg.nCurrent++;

    strncpy(g_szProgFile, pszTail, CCHMAXPATH);
    g_szProgFile[CCHMAXPATH] = '\0';
}

/*------------------------------------------------------------------
  FileOpsOnTimer - the whole UI half of a running job (UI thread)

    Progress text, the overwrite prompt and the completion, in that order.
    This is the ONLY entry point that matters: WMU_WORKER_ASK and
    WMU_WORKER_DONE both just call it, and the latches inside PMWORKER decide
    whether there is anything to do.  Written that way because the worker
    posts those messages from a thread that never called WinInitialize -- if
    PM drops them, this timer still finds the job, and a copy that has
    actually finished stops looking like a hung one.
------------------------------------------------------------------*/
void FileOpsOnTimer(HWND hwnd)
{
    char  szCount[64];
    ULONG ulQuestion = 0;
    BOOL  bCancelled = FALSE;

    if (g_CopyProg.hwndDlg != NULLHANDLE) {
        WinSetDlgItemText(g_CopyProg.hwndDlg, IDC_COPYDLG_FROM,
                          (PCSZ)g_szProgFile);

        if (g_CopyProg.nTotal > 0)
            sprintf(szCount, "File %d of %d",
                    g_CopyProg.nCurrent, g_CopyProg.nTotal);
        else
            sprintf(szCount, "Preparing ...");

        WinSetDlgItemText(g_CopyProg.hwndDlg, IDC_COPYDLG_COUNT,
                          (PCSZ)szCount);
    }

    /* The prompt first: the worker is blocked until it is answered, so
       finding a question and a completion in the same tick is impossible. */
    if (PmWorkerPollAsk(&g_Worker, &ulQuestion))
        FileOpsOnAsk(hwnd, ulQuestion);

    if (PmWorkerPollDone(&g_Worker, NULL, &bCancelled))
        FileOpsOnDone(hwnd, bCancelled);
}

static void CopyProg_End(void)
{
    if (g_CopyProg.hwndDlg != NULLHANDLE) {
        WinDestroyWindow(g_CopyProg.hwndDlg);
        g_CopyProg.hwndDlg = NULLHANDLE;
    }
}

/*==================================================================
 * Overwrite prompt
 *==================================================================*/

static char g_szOwMsg[CCHMAXPATH + 120];

MRESULT EXPENTRY OverwriteDlgProc(HWND hwnd, ULONG msg,
                                  MPARAM mp1, MPARAM mp2)
{
    switch (msg) {

    case WM_INITDLG:
        WinSetDlgItemText(hwnd, IDC_OW_MSG, (PCSZ)g_szOwMsg);
        return (MRESULT)FALSE;

    case WM_COMMAND: {
        USHORT id = SHORT1FROMMP(mp1);

        if (id == IDC_OW_YES || id == IDC_OW_NO ||
            id == IDC_OW_ALL || id == DID_CANCEL) {
            WinDismissDlg(hwnd, id);
            return (MRESULT)FALSE;
        }
        break;
    }
    }

    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

/*------------------------------------------------------------------
  AskOverwrite - returns IDC_OW_YES / _NO / _ALL / DID_CANCEL

    WORKER THREAD.  It cannot put a dialog up itself, so it fills in the
    message, hands the question to the UI thread and blocks until the answer
    comes back.  If the job is cancelled while it waits -- including by the
    WM_CLOSE path, which stops running a message loop and could never deliver
    an answer -- PmWorkerAsk returns DID_CANCEL and the copy unwinds.
------------------------------------------------------------------*/
static ULONG AskOverwrite(const char *pszDst)
{
    ULONG ulRet;

    sprintf(g_szOwMsg,
            "The file '%s' already exists in the destination. "
            "Do you want to overwrite it?",
            PathTail(pszDst));

    ulRet = PmWorkerAsk(&g_Worker, ASK_OVERWRITE, DID_CANCEL);

    if (ulRet == IDC_OW_ALL)
        g_CopyProg.bOverwriteAll = TRUE;
    else if (ulRet == DID_CANCEL)
        PmWorkerRequestCancel(&g_Worker);

    return ulRet;
}

/*------------------------------------------------------------------
  FileOpsOnAsk - answer the worker's question (UI thread)

    Called from the WMU_WORKER_ASK handler.  Every path through here must end
    in PmWorkerAnswer, or the worker stays blocked until something cancels it.
------------------------------------------------------------------*/
void FileOpsOnAsk(HWND hwnd, ULONG ulQuestion)
{
    ULONG ulRet;

    if (ulQuestion != ASK_OVERWRITE) {
        PmWorkerAnswer(&g_Worker, DID_CANCEL);
        return;
    }

    /* Get the progress dialog out of the way of the prompt */
    if (g_CopyProg.hwndDlg != NULLHANDLE)
        WinShowWindow(g_CopyProg.hwndDlg, FALSE);

    ulRet = (ULONG)WinDlgBox(HWND_DESKTOP, hwnd, OverwriteDlgProc,
                             NULLHANDLE, IDD_OVERWRITE, NULL);

    if (g_CopyProg.hwndDlg != NULLHANDLE)
        WinShowWindow(g_CopyProg.hwndDlg, TRUE);

    PmWorkerAnswer(&g_Worker, ulRet);
}

/*==================================================================
 * Copying
 *==================================================================*/

/*------------------------------------------------------------------
  CopyOneFile - copy a single file, prompting before an overwrite

    Returns TRUE on success or on a deliberate skip, FALSE on an I/O error
    or on Cancel (in which case g_CopyProg.bCancelled is set).

    DosCopy, NOT a DosRead/DosWrite loop.  A hand-rolled copy silently throws
    away everything about a file that is not its contents: on HPFS the
    EXTENDED ATTRIBUTES -- .LONGNAME, .TYPE, .ICON, .SUBJECT and every
    Workplace Shell setting hung off the file -- and on any file system the
    timestamps and the attribute byte.  DosCopy carries all of it, and being
    a single kernel call it is faster too.

    Progress is per file (the callers call CopyProg_Update before this), so
    doing the whole file in one call costs no feedback.
------------------------------------------------------------------*/
static BOOL CopyOneFile(const char *pszSrc, const char *pszDst)
{
    ULONG ulAttr;

    if (QueryAttr(pszDst, &ulAttr)) {
        /* The destination is there already */
        if (!g_CopyProg.bOverwriteAll) {
            ULONG ulAsk = AskOverwrite(pszDst);
            if (ulAsk == IDC_OW_NO)
                return TRUE;            /* skip: not an error */
            if (ulAsk == DID_CANCEL)
                return FALSE;
        }
        ClearReadOnly(pszDst);
    }

    /* DCPY_EXISTING: the prompt above has already settled the overwrite.
       Not DCPY_FAILEAS -- a destination that cannot hold EAs (a FAT disk,
       or a redirector that does not support them) should still get the
       file rather than refuse the copy.  */
    return (BOOL)(DosCopy((PCSZ)pszSrc, (PCSZ)pszDst, DCPY_EXISTING)
                  == NO_ERROR);
}

/*------------------------------------------------------------------
  CopyDirEAs - carry a directory's own extended attributes across

    DosCopy does this for files by itself, but DosCreateDir starts a
    directory empty, so a copied folder would arrive stripped of the EAs the
    Workplace Shell keeps on it -- its icon, view settings and background.

    None of this is load-bearing.  Every failure path simply returns and
    leaves the new directory with the EAs it already had, which is none:
    exactly what it would have had anyway.
------------------------------------------------------------------*/
static void CopyDirEAs(const char *pszSrc, const char *pszDst)
{
    FILESTATUS4 fs4;
    EAOP2       eaop;
    PBYTE       pBuf  = NULL;
    PGEA2LIST   pGea;
    PFEA2LIST   pFea;
    PDENA2      pDena;
    PGEA2       pGeaEnt = NULL;
    ULONG       cbBuf, ulCount, oGea, i;

    /* Cheapest exit first: an empty EA list is just its own length field */
    if (DosQueryPathInfo((PCSZ)pszSrc, FIL_QUERYEASIZE,
                         (PVOID)&fs4, sizeof(fs4)) != NO_ERROR)
        return;

    if (fs4.cbList <= sizeof(ULONG))
        return;                         /* nothing to carry */

    /* One allocation, three buffers: the enumerated names, the GEA2 list
       naming them, and the FEA2 list that comes back with the values.  */
    cbBuf = fs4.cbList + 1024UL;

    if (DosAllocMem((PPVOID)&pBuf, cbBuf * 3UL,
                    PAG_COMMIT | PAG_READ | PAG_WRITE) != NO_ERROR)
        return;

    pGea = (PGEA2LIST)(pBuf + cbBuf);
    pFea = (PFEA2LIST)(pBuf + cbBuf * 2UL);

    /* ---- 1. what are the EAs called? ---- */
    ulCount = 256UL;
    if (DosEnumAttribute(ENUMEA_REFTYPE_PATH, (PVOID)pszSrc, 1UL,
                         (PVOID)pBuf, cbBuf, &ulCount,
                         ENUMEA_LEVEL_NO_VALUE) != NO_ERROR
        || ulCount == 0UL) {
        DosFreeMem(pBuf);
        return;
    }

    /* ---- 2. name them all in a GEA2 list ---- */
    pDena = (PDENA2)pBuf;
    oGea  = 0UL;

    for (i = 0UL; i < ulCount; i++) {

        ULONG cbEntry = sizeof(ULONG) + sizeof(BYTE) + pDena->cbName + 1UL;

        cbEntry = (cbEntry + 3UL) & ~3UL;       /* entries are DWORD aligned */

        if (sizeof(ULONG) + oGea + cbEntry > cbBuf)
            break;                              /* refuse to overrun */

        pGeaEnt = (PGEA2)((PBYTE)pGea->list + oGea);
        pGeaEnt->oNextEntryOffset = cbEntry;
        pGeaEnt->cbName           = pDena->cbName;
        memcpy(pGeaEnt->szName, pDena->szName, (size_t)pDena->cbName + 1);

        oGea += cbEntry;

        if (pDena->oNextEntryOffset == 0UL)
            break;

        pDena = (PDENA2)((PBYTE)pDena + pDena->oNextEntryOffset);
    }

    if (pGeaEnt == NULL) {
        DosFreeMem(pBuf);
        return;
    }

    pGeaEnt->oNextEntryOffset = 0UL;             /* last entry ends the list */
    pGea->cbList = sizeof(ULONG) + oGea;

    /* ---- 3. fetch the values, then plant them on the destination ---- */
    pFea->cbList = cbBuf;                        /* room available to PM */

    eaop.fpGEA2List = pGea;
    eaop.fpFEA2List = pFea;
    eaop.oError     = 0UL;

    if (DosQueryPathInfo((PCSZ)pszSrc, FIL_QUERYEASFROMLIST,
                         (PVOID)&eaop, sizeof(eaop)) == NO_ERROR) {

        eaop.fpGEA2List = NULL;                  /* setting, not querying */
        DosSetPathInfo((PCSZ)pszDst, FIL_QUERYEASIZE,
                       (PVOID)&eaop, sizeof(eaop), 0UL);
    }

    DosFreeMem(pBuf);
}

/*------------------------------------------------------------------
  CopyTree - copy the tree at pszSrc to pszDst
------------------------------------------------------------------*/
static BOOL CopyTree(const char *pszSrc, const char *pszDst)
{
    HDIR         hdir = HDIR_CREATE;
    FILEFINDBUF3 ff;
    ULONG        cnt  = 1;
    char         szPat[CCHMAXPATH];
    char         szSrcFull[CCHMAXPATH];
    char         szDstFull[CCHMAXPATH];
    BOOL         bOK  = TRUE;

    if (PmWorkerCancelled(&g_Worker))
        return FALSE;

    if (DosCreateDir((PCSZ)pszDst, NULL) != NO_ERROR)
        return FALSE;

    CopyDirEAs(pszSrc, pszDst);

    JoinPath(szPat, pszSrc, "*");

    if (DosFindFirst((PCSZ)szPat, &hdir, FIND_ATTRS,
                     (PVOID)&ff, sizeof(ff), &cnt,
                     FIL_STANDARD) != NO_ERROR)
        return TRUE;                    /* empty directory: still a success */

    do {
        if (IsDotEntry(ff.achName))
            goto next;

        JoinPath(szSrcFull, pszSrc, ff.achName);
        JoinPath(szDstFull, pszDst, ff.achName);

        if (ff.attrFile & FILE_DIRECTORY) {
            if (!CopyTree(szSrcFull, szDstFull)) {
                bOK = FALSE;
                break;
            }
        }
        else {
            CopyProg_Update(szSrcFull);
            if (PmWorkerCancelled(&g_Worker)) {
                bOK = FALSE;
                break;
            }
            if (!CopyOneFile(szSrcFull, szDstFull)) {
                bOK = FALSE;
                break;
            }
        }

        if (PmWorkerCancelled(&g_Worker)) {
            bOK = FALSE;
            break;
        }

    next:
        cnt = 1;

    } while (DosFindNext(hdir, (PVOID)&ff, sizeof(ff), &cnt) == NO_ERROR);

    DosFindClose(hdir);
    return bOK;
}

/*------------------------------------------------------------------
  MoveByRename - move within one volume by renaming, not by copying

    A move on the same drive has no business reading and rewriting the data:
    DosMove is a directory operation, so it is instant however large the file
    or tree, it is atomic -- there is no window where the file exists twice
    or neither -- and it cannot lose extended attributes because nothing is
    ever rewritten.

    Returns TRUE when the item has been dealt with (renamed, or deliberately
    skipped), FALSE to say "not done - fall back to copy and delete", which
    is what happens for a move across volumes.
------------------------------------------------------------------*/
static BOOL MoveByRename(const char *pszSrc, const char *pszDst)
{
    ULONG ulAttr;

    if (QueryAttr(pszDst, &ulAttr)) {

        /* Same question the copy path asks, so a move and a copy prompt
           alike.  A directory in the way is left to the tree code.  */
        if (ulAttr & FILE_DIRECTORY)
            return FALSE;

        if (!g_CopyProg.bOverwriteAll) {
            ULONG ulAsk = AskOverwrite(pszDst);
            if (ulAsk == IDC_OW_NO)
                return TRUE;                    /* skip: nothing to do */
            if (ulAsk == DID_CANCEL)
                return TRUE;    /* the cancel flag stops the outer loop */
        }

        ClearReadOnly(pszDst);
        if (DosDelete((PCSZ)pszDst) != NO_ERROR)
            return FALSE;                       /* let the copy path try */
    }

    return (BOOL)(DosMove((PCSZ)pszSrc, (PCSZ)pszDst) == NO_ERROR);
}

/*------------------------------------------------------------------
  DeleteTree - delete everything under pszPath, then pszPath itself
------------------------------------------------------------------*/
static BOOL DeleteTree(const char *pszPath)
{
    HDIR         hdir = HDIR_CREATE;
    FILEFINDBUF3 ff;
    ULONG        cnt  = 1;
    char         szPat[CCHMAXPATH];
    char         szFull[CCHMAXPATH];
    BOOL         bOK  = TRUE;

    /* A cancelled delete stops where it is.  There is no way to put back what
       has already gone, so the caller reports it as a partial delete rather
       than pretending either that it finished or that it never started. */
    if (PmWorkerCancelled(&g_Worker))
        return FALSE;

    JoinPath(szPat, pszPath, "*");

    if (DosFindFirst((PCSZ)szPat, &hdir, FIND_ATTRS,
                     (PVOID)&ff, sizeof(ff), &cnt,
                     FIL_STANDARD) == NO_ERROR) {
        do {
            if (IsDotEntry(ff.achName))
                goto next;

            JoinPath(szFull, pszPath, ff.achName);

            if (ff.attrFile & FILE_DIRECTORY) {
                if (!DeleteTree(szFull))
                    bOK = FALSE;
            }
            else {
                CopyProg_Update(szFull);
                ClearReadOnly(szFull);
                if (DosDelete((PCSZ)szFull) != NO_ERROR)
                    bOK = FALSE;
            }

            if (PmWorkerCancelled(&g_Worker)) {
                bOK = FALSE;
                break;
            }

        next:
            cnt = 1;

        } while (DosFindNext(hdir, (PVOID)&ff, sizeof(ff), &cnt) == NO_ERROR);

        DosFindClose(hdir);
    }

    if (!bOK)
        return FALSE;

    ClearReadOnly(pszPath);
    return (BOOL)(DosDeleteDir((PCSZ)pszPath) == NO_ERROR);
}

/*------------------------------------------------------------------
  CountTreeFiles / TreeSize - what the progress bar and the space
  check need to know before anything is written
------------------------------------------------------------------*/
static int CountTreeFiles(const char *pszPath)
{
    HDIR         hdir = HDIR_CREATE;
    FILEFINDBUF3 ff;
    ULONG        cnt  = 1;
    char         szPat[CCHMAXPATH];
    char         szFull[CCHMAXPATH];
    int          nTotal = 0;

    JoinPath(szPat, pszPath, "*");

    if (DosFindFirst((PCSZ)szPat, &hdir, FIND_ATTRS,
                     (PVOID)&ff, sizeof(ff), &cnt,
                     FIL_STANDARD) != NO_ERROR)
        return 0;

    do {
        if (IsDotEntry(ff.achName))
            goto next;

        if (ff.attrFile & FILE_DIRECTORY) {
            JoinPath(szFull, pszPath, ff.achName);
            nTotal += CountTreeFiles(szFull);
        }
        else {
            nTotal++;
        }

    next:
        cnt = 1;

    } while (DosFindNext(hdir, (PVOID)&ff, sizeof(ff), &cnt) == NO_ERROR);

    DosFindClose(hdir);
    return nTotal;
}

static unsigned __int64 TreeSize(const char *pszPath)
{
    HDIR             hdir = HDIR_CREATE;
    FILEFINDBUF3     ff;
    ULONG            cnt  = 1;
    char             szPat[CCHMAXPATH];
    char             szFull[CCHMAXPATH];
    unsigned __int64 cbTotal = 0ui64;

    JoinPath(szPat, pszPath, "*");

    if (DosFindFirst((PCSZ)szPat, &hdir, FIND_ATTRS,
                     (PVOID)&ff, sizeof(ff), &cnt,
                     FIL_STANDARD) != NO_ERROR)
        return 0ui64;

    do {
        if (IsDotEntry(ff.achName))
            goto next;

        if (ff.attrFile & FILE_DIRECTORY) {
            JoinPath(szFull, pszPath, ff.achName);
            cbTotal += TreeSize(szFull);
        }
        else {
            cbTotal += (unsigned __int64)ff.cbFile;
        }

    next:
        cnt = 1;

    } while (DosFindNext(hdir, (PVOID)&ff, sizeof(ff), &cnt) == NO_ERROR);

    DosFindClose(hdir);
    return cbTotal;
}

/*------------------------------------------------------------------
  DriveFreeBytes - free space on the volume holding pszPath
------------------------------------------------------------------*/
static unsigned __int64 DriveFreeBytes(const char *pszPath)
{
    FSALLOCATE fsa;
    char       cDrive = pszPath[0];
    ULONG      ulDrive;

    if (cDrive >= 'a' && cDrive <= 'z')
        cDrive = (char)(cDrive - 'a' + 'A');

    if (cDrive < 'A' || cDrive > 'Z')
        return 0ui64;

    ulDrive = (ULONG)(cDrive - 'A' + 1);

    memset(&fsa, 0, sizeof(fsa));
    if (DosQueryFSInfo(ulDrive, FSIL_ALLOC,
                       (PVOID)&fsa, sizeof(fsa)) != NO_ERROR)
        return 0ui64;

    return (unsigned __int64)fsa.cUnitAvail *
           (unsigned __int64)fsa.cSectorUnit *
           (unsigned __int64)fsa.cbSector;
}

static BOOL SameDrive(const char *pszA, const char *pszB)
{
    char a = pszA[0], b = pszB[0];

    if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
    if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');

    return (BOOL)(a == b);
}

/*------------------------------------------------------------------
  FormatBytes - a byte count with the digits grouped in threes

    Windows read the group separator out of WIN.INI [intl] sThousand.  OS/2
    keeps the same setting in the "PM_National" application of OS2.INI.
------------------------------------------------------------------*/
static void FormatBytes(unsigned __int64 cb, char *pszOut)
{
    char  szSep[8];
    char  szDigits[32];
    char  szGrouped[48];
    ULONG cbSep = sizeof(szSep);
    int   nSepLen, nDigits, nOut = 0, i, k;

    szSep[0] = '\0';
    PrfQueryProfileString(HINI_USERPROFILE, (PCSZ)"PM_National",
                          (PCSZ)"sThousand", (PCSZ)",",
                          (PVOID)szSep, cbSep);
    if (szSep[0] == '\0')
        strcpy(szSep, ",");

    nSepLen = (int)strlen(szSep);

    sprintf(szDigits, "%lu", (unsigned long)cb);
    nDigits = (int)strlen(szDigits);

    for (i = 0; i < nDigits; i++) {

        if (i > 0 && ((nDigits - i) % 3) == 0) {
            for (k = 0; k < nSepLen && nOut < (int)sizeof(szGrouped) - 4; k++)
                szGrouped[nOut++] = szSep[k];
        }

        if (nOut < (int)sizeof(szGrouped) - 2)
            szGrouped[nOut++] = szDigits[i];
    }

    szGrouped[nOut] = '\0';
    sprintf(pszOut, "%s bytes", szGrouped);
}

/*==================================================================
 * Launching
 *==================================================================*/

/*------------------------------------------------------------------
  LaunchProgram - start an executable through PM's session manager
------------------------------------------------------------------*/
BOOL LaunchProgram(HWND hwnd, const char *pszExe, const char *pszWorkDir)
{
    PROGDETAILS pd;

    memset(&pd, 0, sizeof(pd));

    pd.Length            = sizeof(pd);
    pd.progt.progc       = PROG_DEFAULT;
    pd.progt.fbVisible   = SHE_VISIBLE;
    pd.pszExecutable     = (PSZ)pszExe;
    pd.pszParameters     = (PSZ)NULL;
    pd.pszStartupDir     = (PSZ)pszWorkDir;
    pd.pszTitle          = (PSZ)NULL;

    return (BOOL)(WinStartApp(hwnd, &pd, NULL, NULL,
                              SAF_INSTALLEDCMDLINE) != NULLHANDLE);
}

/*------------------------------------------------------------------
  HasAssociation - TRUE if the Workplace Shell knows this path

    Windows asked FindExecutable whether an application was registered for
    the extension.  On OS/2 the equivalent question is whether the shell has
    an object for the file, which is also what OpenFileItem then opens.
------------------------------------------------------------------*/
BOOL HasAssociation(const char *pszFullPath)
{
    return (BOOL)(WinQueryObject((PCSZ)pszFullPath) != NULLHANDLE);
}

/*------------------------------------------------------------------
  OpenFileItem - navigate into a directory, or open/launch a file
------------------------------------------------------------------*/
void OpenFileItem(HWND hwnd, int nIdx)
{
    FILEITEM *pFile;
    HOBJECT   hobj;
    char      szFull[CCHMAXPATH];
    char      szMsg[CCHMAXPATH + 80];

    pFile = GetFileItem(nIdx);
    if (pFile == NULL)
        return;

    if (pFile->bIsDir) {
        BuildFullPath(szFull, pFile->szName);
        NavigateTo(hwnd, szFull);
        return;
    }

    BuildFullPath(szFull, pFile->szName);

    /* A program starts; anything else goes to the shell, which resolves
       the association exactly as ShellExecute("open") used to. */
    if (IsExecFile(pFile->szName)) {
        if (LaunchProgram(hwnd, szFull, szCurrentPath))
            return;
    }

    hobj = WinQueryObject((PCSZ)szFull);
    if (hobj != NULLHANDLE) {
        if (WinOpenObject(hobj, WP_OPEN_DEFAULT, TRUE))
            return;
    }

    sprintf(szMsg, "There is no application associated with '%s'.",
            pFile->szName);
    ExpMessageBox(hwnd, szMsg, MB_OK | MB_WARNING);
}

/*==================================================================
 * New directory
 *==================================================================*/

static char g_szNewDirName[CCHMAXPATHCOMP];

MRESULT EXPENTRY NewDirDlgProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {

    case WM_INITDLG:
        g_szNewDirName[0] = '\0';
        return (MRESULT)FALSE;

    case WM_COMMAND:
        if (SHORT1FROMMP(mp1) == DID_OK) {
            WinQueryDlgItemText(hwnd, IDC_NEWDIR_EDIT,
                                (LONG)sizeof(g_szNewDirName),
                                (PSZ)g_szNewDirName);
            WinDismissDlg(hwnd, DID_OK);
            return (MRESULT)FALSE;
        }
        if (SHORT1FROMMP(mp1) == DID_CANCEL) {
            g_szNewDirName[0] = '\0';
            WinDismissDlg(hwnd, DID_CANCEL);
            return (MRESULT)FALSE;
        }
        break;
    }

    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

void DoNewDir(HWND hwnd)
{
    char szFull[CCHMAXPATH];
    char szMsg[CCHMAXPATH + 80];

    if (szCurrentPath[0] == '\0')
        return;

    if (WinDlgBox(HWND_DESKTOP, hwnd, NewDirDlgProc,
                  NULLHANDLE, IDD_NEWDIR, NULL) != DID_OK)
        return;

    if (g_szNewDirName[0] == '\0')
        return;

    JoinPath(szFull, szCurrentPath, g_szNewDirName);

    if (DosCreateDir((PCSZ)szFull, NULL) != NO_ERROR) {
        sprintf(szMsg, "Could not create directory '%s'.", g_szNewDirName);
        ExpMessageBox(hwnd, szMsg, MB_OK | MB_WARNING);
    }
    else {
        NavigateTo(hwnd, szCurrentPath);
    }
}

/*==================================================================
 * Rename
 *==================================================================*/

static struct {
    char szLabel[CCHMAXPATH + 32];
    char szNewName[CCHMAXPATHCOMP];
    BOOL bOK;
} g_Rename;

MRESULT EXPENTRY RenameDlgProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {

    case WM_INITDLG:
        WinSetDlgItemText(hwnd, IDC_RENAME_LABEL, (PCSZ)g_Rename.szLabel);
        WinSetDlgItemText(hwnd, IDC_RENAME_EDIT,  (PCSZ)g_Rename.szNewName);
        WinSendDlgItemMsg(hwnd, IDC_RENAME_EDIT, EM_SETSEL,
                          MPFROM2SHORT(0, 32767), MPVOID);
        return (MRESULT)FALSE;

    case WM_COMMAND:
        if (SHORT1FROMMP(mp1) == DID_OK) {
            WinQueryDlgItemText(hwnd, IDC_RENAME_EDIT,
                                (LONG)sizeof(g_Rename.szNewName),
                                (PSZ)g_Rename.szNewName);
            g_Rename.bOK = TRUE;
            WinDismissDlg(hwnd, DID_OK);
            return (MRESULT)FALSE;
        }
        if (SHORT1FROMMP(mp1) == DID_CANCEL) {
            g_Rename.bOK = FALSE;
            WinDismissDlg(hwnd, DID_CANCEL);
            return (MRESULT)FALSE;
        }
        break;
    }

    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

void DoRename(HWND hwnd, int nIdx)
{
    FILEITEM *pFile;
    char      szOld[CCHMAXPATH];
    char      szNew[CCHMAXPATH];
    char      szMsg[2 * CCHMAXPATHCOMP + 80];

    pFile = GetFileItem(nIdx);
    if (pFile == NULL)
        return;

    sprintf(g_Rename.szLabel, "Renaming: %s", pFile->szName);
    strcpy(g_Rename.szNewName, pFile->szName);
    g_Rename.bOK = FALSE;

    WinDlgBox(HWND_DESKTOP, hwnd, RenameDlgProc,
              NULLHANDLE, IDD_RENAME, NULL);

    if (!g_Rename.bOK || g_Rename.szNewName[0] == '\0')
        return;

    if (stricmp(g_Rename.szNewName, pFile->szName) == 0)
        return;

    JoinPath(szOld, szCurrentPath, pFile->szName);
    JoinPath(szNew, szCurrentPath, g_Rename.szNewName);

    if (DosMove((PCSZ)szOld, (PCSZ)szNew) != NO_ERROR) {
        sprintf(szMsg, "Could not rename '%s' to '%s'.",
                pFile->szName, g_Rename.szNewName);
        ExpMessageBox(hwnd, szMsg, MB_OK | MB_WARNING);
    }
    else {
        NavigateTo(hwnd, szCurrentPath);
    }
}

/*==================================================================
 * Delete
 *==================================================================*/

/*------------------------------------------------------------------
  DoDelete / DoDeleteSelected - confirm here, delete on the worker

    ASYNCHRONOUS.  The confirmation is asked on the UI thread, where a modal
    box belongs; everything after it is a tree walk that can take minutes on a
    deep folder, so it goes to the worker.  Neither function refreshes the
    view -- FileOpsOnDone does, once, when the work is actually finished.
------------------------------------------------------------------*/
void DoDelete(HWND hwnd, int nIdx)
{
    FILEITEM    *pFile;
    EXPPATHLIST *pList;
    char         szMsg[CCHMAXPATHCOMP + 120];

    pFile = GetFileItem(nIdx);
    if (pFile == NULL)
        return;

    if (FileOpsBusy()) {
        ExpMessageBox(hwnd,
            "Another file operation is still running. Wait for it to "
            "finish, or cancel it, and try again.",
            MB_OK | MB_INFORMATION);
        return;
    }

    sprintf(szMsg, "Are you sure you want to delete the %s '%s'?",
            pFile->bIsDir ? "folder" : "file", pFile->szName);

    if (ExpMessageBox(hwnd, szMsg, MB_YESNO | MB_QUERY) != MBID_YES)
        return;

    pList = BuildPathList(nIdx, CLIP_OP_CUT);
    if (pList == NULL)
        return;

    StartFileJob(hwnd, FILEJOB_DELETE, pList, FALSE, NULL);
}

void DoDeleteSelected(HWND hwnd)
{
    EXPPATHLIST *pList;
    char         szMsg[CCHMAXPATHCOMP + 120];
    int          nCount = GetFileCount();
    int          nSel   = 0;
    int          i;

    for (i = 0; i < nCount; i++)
        if (abSelectedFiles[i]) nSel++;

    if (nSel == 0)
        return;

    if (nSel == 1) {
        for (i = 0; i < nCount; i++) {
            if (abSelectedFiles[i]) {
                DoDelete(hwnd, i);      /* keeps the single-item wording */
                return;
            }
        }
        return;
    }

    if (FileOpsBusy()) {
        ExpMessageBox(hwnd,
            "Another file operation is still running. Wait for it to "
            "finish, or cancel it, and try again.",
            MB_OK | MB_INFORMATION);
        return;
    }

    sprintf(szMsg, "Are you sure you want to delete %d items?", nSel);
    if (ExpMessageBox(hwnd, szMsg, MB_YESNO | MB_QUERY) != MBID_YES)
        return;

    pList = BuildPathList(-1, CLIP_OP_CUT);
    if (pList == NULL)
        return;

    StartFileJob(hwnd, FILEJOB_DELETE, pList, FALSE, NULL);
}

/*==================================================================
 * Path lists and the clipboard
 *==================================================================*/

/*------------------------------------------------------------------
  BuildPathList - pack the selection (or one item) into shared memory

    Shared, not private: this same block is what goes on the clipboard, and
    the clipboard's owner is PM, not us.
------------------------------------------------------------------*/
EXPPATHLIST *BuildPathList(int nOnlyIdx, ULONG ulOp)
{
    EXPPATHLIST *pList;
    FILEITEM    *pFile;
    char         szFull[CCHMAXPATH];
    char        *pCur;
    ULONG        cbTotal = EXP_PATHLIST_HDR;
    ULONG        cPaths  = 0;
    int          nCount  = GetFileCount();
    int          i, nLen;

    /* Pass one: measure */
    for (i = 0; i < nCount; i++) {

        if (nOnlyIdx >= 0 ? (i != nOnlyIdx) : !abSelectedFiles[i])
            continue;

        pFile = GetFileItem(i);
        if (pFile == NULL)
            continue;

        BuildFullPath(szFull, pFile->szName);
        cbTotal += (ULONG)strlen(szFull) + 1UL;
        cPaths++;
    }

    if (cPaths == 0)
        return NULL;

    pList = NULL;
    if (DosAllocSharedMem((PPVOID)&pList, NULL, cbTotal,
                          PAG_COMMIT | PAG_READ | PAG_WRITE | OBJ_GIVEABLE)
        != NO_ERROR)
        return NULL;

    pList->cbTotal = cbTotal;
    pList->ulOp    = ulOp;
    pList->cPaths  = cPaths;

    /* Pass two: fill */
    pCur = pList->achPaths;

    for (i = 0; i < nCount; i++) {

        if (nOnlyIdx >= 0 ? (i != nOnlyIdx) : !abSelectedFiles[i])
            continue;

        pFile = GetFileItem(i);
        if (pFile == NULL)
            continue;

        BuildFullPath(szFull, pFile->szName);
        nLen = (int)strlen(szFull);

        strcpy(pCur, szFull);
        pCur += nLen + 1;
    }

    return pList;
}

/*------------------------------------------------------------------
  PutOnClipboard - hand a path list to PM

    WinSetClipbrdData with CFI_POINTER takes ownership of the block, so it
    is freed here only when the clipboard could not be opened.
------------------------------------------------------------------*/
static void PutOnClipboard(EXPPATHLIST *pList)
{
    if (pList == NULL)
        return;

    if (WinOpenClipbrd(hab)) {
        WinEmptyClipbrd(hab);
        WinSetClipbrdData(hab, (ULONG)pList, uClipFormat, CFI_POINTER);
        WinCloseClipbrd(hab);
    }
    else {
        DosFreeMem(pList);
    }
}

void DoClipboardOp(HWND hwnd, int nIdx, ULONG ulOp)
{
    (void)hwnd;
    PutOnClipboard(BuildPathList(nIdx, ulOp));
}

void DoClipboardOpSelected(HWND hwnd, ULONG ulOp)
{
    (void)hwnd;
    PutOnClipboard(BuildPathList(-1, ulOp));
}

/*==================================================================
 * Paste
 *==================================================================*/

/*------------------------------------------------------------------
  PasteWork - the transfer itself (WORKER thread)

    The preflight walk is in here too, not on the UI thread: counting the
    files in a tree and adding up its bytes means walking the whole thing,
    which on a big folder is exactly as slow as the copy it precedes.  That is
    why the dialog says "Preparing" until the count is known.
------------------------------------------------------------------*/
static void PasteWork(void)
{
    const EXPPATHLIST *pList    = g_Job.pList;
    const char        *pszDstDir = g_Job.szDstDir;
    BOOL               bIsMove   = g_Job.bIsMove;
    const char        *pCur;
    char               szSrc[CCHMAXPATH];
    char               szDst[CCHMAXPATH];
    unsigned __int64   cbNeeded = 0ui64;
    unsigned __int64   cbFree;
    int                nTotalFiles = 0;
    ULONG              i;
    BOOL               bOK;

    /* ---- preflight: how many files, and is there room for them ---- */
    pCur = pList->achPaths;

    for (i = 0; i < pList->cPaths; i++) {

        BOOL bIsDir;

        if (PmWorkerCancelled(&g_Worker))
            return;

        strcpy(szSrc, pCur);
        pCur += strlen(pCur) + 1;

        bIsDir = IsDirectory(szSrc);

        if (bIsDir)
            nTotalFiles += CountTreeFiles(szSrc);
        else
            nTotalFiles++;

        /* A move within one volume needs no extra space */
        if (bIsMove && SameDrive(szSrc, pszDstDir))
            continue;

        if (bIsDir) {
            cbNeeded += TreeSize(szSrc);
        }
        else {
            FILESTATUS3 fs3;

            if (DosQueryPathInfo((PCSZ)szSrc, FIL_STANDARD,
                                 (PVOID)&fs3, sizeof(fs3)) == NO_ERROR)
                cbNeeded += (unsigned __int64)fs3.cbFile;
        }
    }

    /* The count is live from here on, so the dialog stops saying "Preparing" */
    g_CopyProg.nTotal = nTotalFiles;

    if (cbNeeded > 0ui64) {

        cbFree = DriveFreeBytes(pszDstDir);

        if (cbNeeded > cbFree) {
            char szNeeded[64];
            char szAvail[64];

            FormatBytes(cbNeeded, szNeeded);
            FormatBytes(cbFree,   szAvail);

            /* Reported once, by FileOpsOnDone: a worker has no message box */
            g_Job.nErrors++;
            sprintf(g_Job.szError,
                    "Not enough space on drive %c:.\nRequired: %s\n"
                    "Available: %s",
                    pszDstDir[0], szNeeded, szAvail);
            return;
        }
    }

    /* ---- the transfer ---- */
    pCur = pList->achPaths;

    for (i = 0; i < pList->cPaths; i++) {

        BOOL        bIsDir;
        const char *pName;

        if (PmWorkerCancelled(&g_Worker))
            break;

        strcpy(szSrc, pCur);
        pCur += strlen(pCur) + 1;

        pName = PathTail(szSrc);

        strcpy(szDst, pszDstDir);
        EnsureTrailingSlash(szDst);
        strcat(szDst, pName);

        bIsDir = IsDirectory(szSrc);

        if (!bIsDir) {
            CopyProg_Update(szSrc);
            if (PmWorkerCancelled(&g_Worker))
                break;
        }

        /* Within one volume a move is a rename - see MoveByRename */
        if (bIsMove && SameDrive(szSrc, szDst) &&
            MoveByRename(szSrc, szDst))
            continue;

        bOK = bIsDir ? CopyTree(szSrc, szDst)
                     : CopyOneFile(szSrc, szDst);

        if (PmWorkerCancelled(&g_Worker))
            break;

        if (!bOK) {
            RecordError(bIsMove ? "Could not move '%s' to '%s'."
                                : "Could not copy '%s' to '%s'.",
                        pName, pszDstDir);
        }
        else if (bIsMove) {
            if (bIsDir) {
                DeleteTree(szSrc);
            }
            else {
                ClearReadOnly(szSrc);
                DosDelete((PCSZ)szSrc);
            }
        }
    }
}

/*------------------------------------------------------------------
  DeleteWork - delete every path in the list (WORKER thread)
------------------------------------------------------------------*/
static void DeleteWork(void)
{
    const EXPPATHLIST *pList = g_Job.pList;
    const char        *pCur  = pList->achPaths;
    char               szFull[CCHMAXPATH];
    ULONG              i;

    for (i = 0; i < pList->cPaths; i++) {

        if (PmWorkerCancelled(&g_Worker))
            break;

        strcpy(szFull, pCur);
        pCur += strlen(pCur) + 1;

        g_CopyProg.nCurrent++;

        if (IsDirectory(szFull)) {
            if (!DeleteTree(szFull))
                RecordError("Could not fully delete folder '%s'.%s",
                            PathTail(szFull), "");
        }
        else {
            CopyProg_Update(szFull);
            ClearReadOnly(szFull);
            if (DosDelete((PCSZ)szFull) != NO_ERROR)
                RecordError("Could not delete file '%s'.%s",
                            PathTail(szFull), "");
        }
    }
}

/*------------------------------------------------------------------
  FileWorkerBody - the worker thread's entry point

    !! NO PM CALLS BELOW THIS LINE !!  The only exceptions are inside
    PmWorkerAsk and PmWorkerEnd, which post rather than send.

    The path list is freed here rather than by the caller: DoPasteFromList
    returns long before the work is finished, so the caller cannot know when
    the block stops being read.
------------------------------------------------------------------*/
static void FileWorkerBody(void *arg)
{
    (void)arg;

    if (g_Job.nKind == FILEJOB_PASTE)
        PasteWork();
    else if (g_Job.nKind == FILEJOB_DELETE)
        DeleteWork();

    if (g_Job.pList != NULL) {
        DosFreeMem((PVOID)g_Job.pList);
        g_Job.pList = NULL;
    }

    PmWorkerEnd(&g_Worker, 0);
}

/*------------------------------------------------------------------
  StartFileJob - hand a path list to the worker (UI thread)

    Takes ownership of pList in every case, including failure, so that no
    caller has to reason about who frees it on which path.
------------------------------------------------------------------*/
static BOOL StartFileJob(HWND hwnd, int nKind, EXPPATHLIST *pList,
                         BOOL bIsMove, const char *pszDstDir)
{
    EnsureWorker();

    if (pList == NULL || pList->cPaths == 0 || PmWorkerIsBusy(&g_Worker)) {
        if (pList != NULL)
            DosFreeMem((PVOID)pList);
        return FALSE;
    }

    g_Job.nKind             = nKind;
    g_Job.pList             = pList;
    g_Job.bIsMove           = bIsMove;
    g_Job.bClearClipOnDone  = FALSE;
    g_Job.nErrors           = 0;
    g_Job.szError[0]        = '\0';

    strcpy(g_Job.szDstDir, (pszDstDir != NULL) ? pszDstDir : "");

    CopyProg_Init(hwnd, bIsMove, (pszDstDir != NULL) ? pszDstDir : "");

    if (!PmWorkerStart(&g_Worker, FileWorkerBody, NULL,
                       PMWORKER_STACK, hwndClient)) {
        CopyProg_End();
        DosFreeMem((PVOID)pList);
        g_Job.pList = NULL;
        ExpMessageBox(hwnd, "Could not start the file operation.",
                      MB_OK | MB_WARNING);
        return FALSE;
    }

    WinStartTimer(hab, hwndClient, TID_FILEPROGRESS, FILE_TIMER_MS);
    return TRUE;
}

/*------------------------------------------------------------------
  FileOpsOnDone - the worker finished (UI thread)

    The single place a completed job is reported and the view refreshed.  The
    old code did both from inside the loop, per item.
------------------------------------------------------------------*/
void FileOpsOnDone(HWND hwnd, BOOL bCancelled)
{
    char szMsg[2 * CCHMAXPATH + 220];

    WinStopTimer(hab, hwndClient, TID_FILEPROGRESS);
    CopyProg_End();

    if (g_Job.nErrors > 0) {
        if (g_Job.nErrors == 1)
            strcpy(szMsg, g_Job.szError);
        else
            sprintf(szMsg, "%s\n\n%d further item(s) also failed.",
                    g_Job.szError, g_Job.nErrors - 1);

        ExpMessageBox(hwnd, szMsg, MB_OK | MB_WARNING);
    }
    else if (bCancelled && g_Job.nKind == FILEJOB_DELETE) {
        /* Deletes cannot be undone, so a half-finished one is worth saying */
        ExpMessageBox(hwnd,
            "The delete was stopped part-way through. Some items have "
            "already been deleted.",
            MB_OK | MB_INFORMATION);
    }

    /* A completed Cut-and-Paste empties the clipboard, so the same items
       cannot be pasted again from a place they have just left.  A cancelled
       one does not: some of them are still there. */
    if (g_Job.bClearClipOnDone && !bCancelled && WinOpenClipbrd(hab)) {
        WinEmptyClipbrd(hab);
        WinCloseClipbrd(hab);
    }

    g_Job.nKind            = 0;
    g_Job.nErrors          = 0;
    g_Job.bClearClipOnDone = FALSE;

    NavigateTo(hwnd, szCurrentPath);
}

/*------------------------------------------------------------------
  FileOpsShutdown - stop the worker before its window goes away
------------------------------------------------------------------*/
BOOL FileOpsShutdown(void)
{
    EnsureWorker();

    if (!PmWorkerIsBusy(&g_Worker))
        return TRUE;

    return PmWorkerWait(&g_Worker, 5000);
}

/*------------------------------------------------------------------
  DoPasteFromList - copy or move every path in pList into pszDstDir

    ASYNCHRONOUS.  This starts the transfer and returns; it does not finish
    it.  The caller must NOT free pList afterwards -- see StartFileJob.
------------------------------------------------------------------*/
void DoPasteFromList(HWND hwnd, const EXPPATHLIST *pList, BOOL bIsMove,
                     const char *pszDstDir)
{
    EXPPATHLIST *pOwned;
    ULONG        cb;

    if (pList == NULL || pList->cPaths == 0)
        return;

    if (FileOpsBusy()) {
        ExpMessageBox(hwnd,
            "Another file operation is still running. Wait for it to "
            "finish, or cancel it, and try again.",
            MB_OK | MB_INFORMATION);
        return;
    }

    /* The caller keeps ownership of what it passed in, and the worker outlives
       this call, so take a private copy for the worker to own and free.
       cbTotal covers the header as well as the packed names. */
    cb = pList->cbTotal;

    if (cb < EXP_PATHLIST_HDR || cb > EXP_PATHLIST_MAX_BYTES)
        return;

    if (DosAllocMem((PPVOID)&pOwned, cb,
                    PAG_COMMIT | PAG_READ | PAG_WRITE) != NO_ERROR)
        return;

    memcpy(pOwned, pList, cb);

    StartFileJob(hwnd, FILEJOB_PASTE, pOwned, bIsMove, pszDstDir);
}

/*------------------------------------------------------------------
  DoPaste - paste the clipboard's path list into the current directory

    The block on the clipboard belongs to PM and may live in another
    process's shared memory, so it is copied out before the clipboard is
    closed and any file I/O begins.
------------------------------------------------------------------*/
void DoPaste(HWND hwnd)
{
    EXPPATHLIST *pClip;
    EXPPATHLIST *pLocal = NULL;
    char         szDstDir[CCHMAXPATH];
    BOOL         bWasCut = FALSE;

    if (szCurrentPath[0] == '\0')
        return;

    if (!WinOpenClipbrd(hab))
        return;

    pClip = (EXPPATHLIST *)WinQueryClipbrdData(hab, uClipFormat);

    if (pClip != NULL &&
        pClip->cbTotal >= EXP_PATHLIST_HDR &&
        pClip->cbTotal <= EXP_PATHLIST_MAX_BYTES) {

        if (DosAllocMem((PPVOID)&pLocal, pClip->cbTotal,
                        PAG_COMMIT | PAG_READ | PAG_WRITE) == NO_ERROR)
            memcpy(pLocal, pClip, pClip->cbTotal);
    }

    WinCloseClipbrd(hab);

    if (pLocal == NULL)
        return;

    bWasCut = (BOOL)(pLocal->ulOp == CLIP_OP_CUT);

    strcpy(szDstDir, szCurrentPath);
    EnsureTrailingSlash(szDstDir);

    DoPasteFromList(hwnd, pLocal, bWasCut, szDstDir);

    DosFreeMem(pLocal);

    /* After a cut, the clipboard has to be emptied so the same items cannot
       be pasted a second time into somewhere they no longer exist -- but the
       move has not happened yet, so that decision belongs to FileOpsOnDone.
       Recorded here because only this path knows the paste came from a cut
       on the clipboard rather than from a drag. */
    g_Job.bClearClipOnDone = (BOOL)(bWasCut && FileOpsBusy());

    /* No NavigateTo: FileOpsOnDone refreshes the view when the work is
       actually finished. */
}
