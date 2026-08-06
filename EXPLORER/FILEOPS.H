/*===========================================================================
 * FILEOPS.H  -  File operation declarations: copy, move, delete, rename
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *===========================================================================*/

#ifndef FILEOPS_H
#define FILEOPS_H

MRESULT EXPENTRY NewDirDlgProc   (HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2);
MRESULT EXPENTRY RenameDlgProc   (HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2);
MRESULT EXPENTRY CopyMoveDlgProc (HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2);
MRESULT EXPENTRY OverwriteDlgProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2);

BOOL IsExecFile(const char *pszName);
BOOL HasAssociation(const char *pszFullPath);
void OpenFileItem(HWND hwnd, int nIdx);

void DoNewDir(HWND hwnd);
void DoRename(HWND hwnd, int nIdx);

/* ASYNCHRONOUS.  These confirm with the user, start a worker thread and
   return -- the files are NOT gone when they come back, and the view is not
   refreshed until FileOpsOnDone runs. */
void DoDelete(HWND hwnd, int nIdx);
void DoDeleteSelected(HWND hwnd);

void DoClipboardOp(HWND hwnd, int nIdx, ULONG ulOp);
void DoClipboardOpSelected(HWND hwnd, ULONG ulOp);
void DoPaste(HWND hwnd);

/* Copy or move every path in pList into pszDstDir, which must end with a
   backslash.  ASYNCHRONOUS, as above.  The caller keeps ownership of pList:
   a private copy is taken for the worker, because this returns long before
   the transfer is finished. */
void DoPasteFromList(HWND hwnd, const EXPPATHLIST *pList, BOOL bIsMove,
                     const char *pszDstDir);

/*---------------------------------------------------------------------------
 * Worker plumbing.  All UI-thread only; wire these into the client window
 * procedure (see PMMAIN.CPP).
 *
 *   WM_TIMER         -> FileOpsOnTimer
 *   WMU_WORKER_ASK   -> FileOpsOnTimer
 *   WMU_WORKER_DONE  -> FileOpsOnTimer
 *   WM_CLOSE         -> FileOpsShutdown
 *
 * All three go to the same place ON PURPOSE.  FileOpsOnTimer polls the
 * worker's latches and calls FileOpsOnAsk / FileOpsOnDone itself, each at most
 * once per job, so the two messages are only a way of making it look sooner
 * than the next tick.  Nothing breaks if PM never delivers them -- which is
 * not hypothetical: the worker posts them from a thread that owns no anchor
 * block.  Do not "simplify" this by calling FileOpsOnDone straight from the
 * message handler; that reintroduces the dependency and a dropped message
 * then reads as a hung copy.
 *
 * Progress is PULLED off the worker by this timer rather than pushed as a
 * message per file: a paste of ten thousand files would otherwise post ten
 * thousand messages, and an OS/2 message queue is finite.  The id has to be
 * distinct from the search window's timer (SEARCH.CPP), even though they are
 * owned by different windows.
 *-------------------------------------------------------------------------*/

#define TID_FILEPROGRESS    2

BOOL FileOpsBusy    (void);
BOOL FileOpsShutdown(void);                         /* cancel + wait         */
void FileOpsOnTimer (HWND hwnd);                    /* the whole UI half     */
void FileOpsOnAsk   (HWND hwnd, ULONG ulQuestion);  /* overwrite prompt      */
void FileOpsOnDone  (HWND hwnd, BOOL bCancelled);   /* report + refresh      */

/* Build a path-list block over the current selection, or over one item.
   Returns a shared-memory block the caller must DosFreeMem, or NULL. */
EXPPATHLIST *BuildPathList(int nOnlyIdx, ULONG ulOp);

/* Start a program or hand a document to the Workplace Shell. */
BOOL LaunchProgram(HWND hwnd, const char *pszExe, const char *pszWorkDir);

#endif /* FILEOPS_H */
