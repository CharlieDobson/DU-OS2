/*===========================================================================
 * PMWORKER.H  -  a background worker thread for the OS/2 PM ports
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * Long operations run here instead of on the UI thread.  PM has a SINGLE
 * INPUT QUEUE: an application that stops servicing it freezes the whole
 * desktop's mouse and keyboard, not just itself.
 *
 * THE RULES THIS MODULE ENFORCES
 *
 *   * THE WORKER MAKES NO PM CALLS.  Not one Win* or Gpi* call from the
 *     worker body -- only Dos* work and plain C.  Never WinSendMsg from a
 *     worker: it blocks until the UI thread answers, and the UI thread may be
 *     waiting on the worker, which is a deadlock.
 *
 *     This module posts WMU_WORKER_DONE / WMU_WORKER_ASK from the worker, but
 *     NOTHING DEPENDS ON THOSE POSTS ARRIVING.  Every one of them is paired
 *     with a latch the UI thread polls from the timer it is already running
 *     (PmWorkerPollDone / PmWorkerPollAsk), and the latch is what actually
 *     drives the UI; the post only makes the response immediate instead of
 *     one tick late.  That is deliberate.  Whether WinPostMsg is legal from a
 *     thread that never called WinInitialize is genuinely disputed -- IBM's
 *     rule is that a thread must own an anchor block before it calls any Win
 *     API -- and a lost completion message is invisible: the work finishes,
 *     the progress dialog stays up forever, and it reads as a hung scan.
 *
 *   * PROGRESS IS PULLED, NOT PUSHED.  The worker updates a plain shared
 *     buffer; the UI thread reads it from a WM_TIMER.  Posting a message per
 *     file would flood the queue on a big job -- OS/2 message queues are
 *     finite and WinPostMsg starts failing when one fills.  The buffer needs
 *     no mutex because the worst outcome of a torn read is one frame of
 *     garbled label text; see the note on the progress buffer in each caller.
 *
 *   * THE UI THREAD OWNS THE DATA WHILE THE WORKER IS IDLE, THE WORKER OWNS
 *     IT WHILE IT RUNS.  There is no locking, so each caller must make sure
 *     its paint path reads nothing the worker is building.
 *
 *   * CANCELLATION IS COOPERATIVE.  The worker body must call
 *     PmWorkerCancelled() often enough that a cancel feels immediate, and
 *     unwind cleanly when it returns TRUE.  Nothing is ever killed from
 *     outside: DosKillThread would leave handles and heap blocks stranded.
 *
 * REQUIREMENTS
 *
 *   * <os2.h> must be included BEFORE this header (it uses HWND, TID, BOOL).
 *     This header deliberately does not include it, so that it cannot clip
 *     the INCL_* selection the including file has already made.
 *   * Compile every module of the program with -bm.  _beginthread needs the
 *     multithreaded runtime; without it errno, strtok and the heap are not
 *     thread-safe and the failure is intermittent rather than loud.
 *===========================================================================*/
#ifndef PMWORKER_H
#define PMWORKER_H

/*---------------------------------------------------------------------------
 * Posted to the owner window, once, when a job finishes.
 *
 *   mp1 = ULONG result the body passed to PmWorkerEnd
 *   mp2 = TRUE if the job stopped because it was cancelled
 *
 * WM_USER is 0x1000 in PM; +900 keeps this clear of the control IDs and the
 * per-app WM_USER messages these programs already define.
 *-------------------------------------------------------------------------*/

#define WMU_WORKER_DONE         (WM_USER + 900)

/*---------------------------------------------------------------------------
 * Posted to the owner window when the body needs an answer from the user.
 *
 *   mp1 = the ULONG question code the body passed to PmWorkerAsk
 *   mp2 = unused
 *
 * The handler puts up whatever dialog it likes -- it is on the UI thread, so
 * a modal WinDlgBox is fine -- and MUST finish by calling PmWorkerAnswer.
 * Until it does, the worker is blocked inside PmWorkerAsk.
 *-------------------------------------------------------------------------*/

#define WMU_WORKER_ASK          (WM_USER + 901)

/*---------------------------------------------------------------------------
 * Stack for a worker, in bytes.
 *
 * !! THIS IS NOT THE LINKER'S STACK !!  "option stack=262144" in MKOS2.CMD
 * sizes the MAIN thread only; a thread started here gets exactly what is
 * asked for below.  DiskStat's ScanDirectory and Explorer's CopyTree /
 * DeleteTree / DoSearch all recurse one frame per directory level -- give a
 * worker a default stack and it blows up on a deep tree.  Keep the two the
 * same 256 KB; changing one alone means a deep tree finds the shorter.
 *-------------------------------------------------------------------------*/

#define PMWORKER_STACK          262144

/*---------------------------------------------------------------------------
 * A worker.  One instance per concurrent job; these programs each have one.
 *
 * fRunning / fCancel are volatile because two threads read and write them and
 * the optimiser would otherwise be free to cache either in a register for the
 * whole of a scan loop.  Aligned 32-bit loads and stores are atomic on the
 * 386, so a plain flag needs no interlock.
 *-------------------------------------------------------------------------*/

typedef struct pmworker_s {
    volatile ULONG  fRunning;        /* worker body has not returned yet     */
    volatile ULONG  fCancel;         /* UI asked the body to stop            */
    TID             tid;             /* thread id from _beginthread          */
    HWND            hwndNotify;      /* gets WMU_WORKER_DONE / _ASK          */
    HEV             hevAnswer;       /* posted by the UI to release an ask   */
    volatile ULONG  ulAnswer;        /* the UI thread's reply                */

    /* Latches.  These are what the UI thread actually acts on; the posted
       messages above are only a way of getting it to look sooner.  Each is
       raised by the worker and cleared by the UI when it takes the event, so
       the event fires exactly once however many times it is polled and
       whether or not the matching post ever arrived. */
    volatile ULONG  fDonePending;    /* job ended, UI has not reaped it yet  */
    volatile ULONG  ulResult;        /* what the body passed to PmWorkerEnd  */
    volatile ULONG  fDoneCancelled;  /* that job ended via a cancel          */
    volatile ULONG  fAskPending;     /* body is blocked inside PmWorkerAsk   */
    volatile ULONG  ulQuestion;      /* what it is asking                    */
} PMWORKER;

#ifdef __cplusplus
extern "C" {
#endif

/* Call once, before the first PmWorkerStart. */
void PmWorkerInit( PMWORKER *pw );

/* Start pfnBody on a new thread with cbStack bytes of stack, passing pArg.
   hwndNotify receives WMU_WORKER_DONE when the body calls PmWorkerEnd.
   Returns FALSE (and leaves the worker idle) if the thread could not start.
   Refuses, returning FALSE, if a job is already running. */
BOOL PmWorkerStart( PMWORKER *pw, void ( *pfnBody )( void * ), void *pArg,
                    unsigned cbStack, HWND hwndNotify );

/* --- called from the UI thread ------------------------------------------ */

BOOL PmWorkerIsBusy       ( PMWORKER *pw );
void PmWorkerRequestCancel( PMWORKER *pw );

/* Ask the body to stop and wait up to ulTimeoutMs for it to actually return.
   TRUE if it did.  For WM_CLOSE: a worker still touching globals after main
   has torn down its window is how a clean exit turns into a trap. */
BOOL PmWorkerWait( PMWORKER *pw, ULONG ulTimeoutMs );

/* Reap a finished job.  TRUE exactly once per job, on the first call made
   after the body returned; fills in the result and whether it was cancelled.
   Call it from BOTH the WMU_WORKER_DONE handler and the progress timer -- the
   latch makes that safe, and it is what stops a lost post from leaving the
   program waiting forever on work that has already finished. */
BOOL PmWorkerPollDone( PMWORKER *pw, ULONG *pulResult, BOOL *pfCancelled );

/* TRUE exactly once when the body is blocked in PmWorkerAsk waiting to be
   answered.  Same reasoning, same pairing with WMU_WORKER_ASK.  Taking the
   question clears the latch, so a timer that fires again while the dialog is
   up does not put up a second one. */
BOOL PmWorkerPollAsk( PMWORKER *pw, ULONG *pulQuestion );

/* --- called from the worker body ---------------------------------------- */

/* Poll this in every loop that can run long. */
BOOL PmWorkerCancelled( PMWORKER *pw );

/* Ask the UI thread a question and block until it answers.  Posts
   WMU_WORKER_ASK( ulQuestion ) and waits for PmWorkerAnswer.

   Returns ulAnswerIfCancelled instead if the job is cancelled while waiting,
   if the post fails, or if there is no owner window.  The wait is a poll loop
   rather than an unbounded DosWaitEventSem precisely so that a cancel can
   break it: PmWorkerWait runs ON the UI thread, so a worker blocked forever
   on an answer the UI thread can no longer deliver would deadlock the exit
   path. */
ULONG PmWorkerAsk( PMWORKER *pw, ULONG ulQuestion, ULONG ulAnswerIfCancelled );

/* --- called from the UI thread, out of the WMU_WORKER_ASK handler ------- */

void PmWorkerAnswer( PMWORKER *pw, ULONG ulAnswer );

/* The body's LAST action.  Marks the worker idle, then posts
   WMU_WORKER_DONE( ulResult, wasCancelled ) to the owner window. */
void PmWorkerEnd( PMWORKER *pw, ULONG ulResult );

#ifdef __cplusplus
}
#endif

#endif /* PMWORKER_H */
