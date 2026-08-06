/*===========================================================================
 * PMWORKER.C  -  background worker thread for the OS/2 PM ports
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * See PMWORKER.H for the rules a worker body must obey.  Compiled with
 * wcc386 (plain C) so that both the C++ and the C ports can link it; the
 * header carries extern "C" for the former.
 *
 * !! BUILD !!  Every module of the program must be compiled with -bm.  This
 * one alone is not enough: -bm selects the multithreaded runtime for the
 * whole image, and mixing is what makes errno and the heap unsafe.
 *===========================================================================*/
#define INCL_DOSPROCESS
#define INCL_DOSSEMAPHORES
#define INCL_DOSERRORS
#define INCL_WIN

#include <os2.h>
#include <process.h>

#include "pmworker.h"

/* Poll granularity for PmWorkerWait.  Short enough that shutdown does not
   feel stalled, long enough not to spin the CPU while waiting. */
#define PMWORKER_POLL_MS        25

/*---------------------------------------------------------------------------
 * PmWorkerInit
 *-------------------------------------------------------------------------*/

void PmWorkerInit( PMWORKER *pw )
{
    pw->fRunning       = 0;
    pw->fCancel        = 0;
    pw->tid            = 0;
    pw->hwndNotify     = NULLHANDLE;
    pw->ulAnswer       = 0;
    pw->hevAnswer      = NULLHANDLE;
    pw->fDonePending   = 0;
    pw->ulResult       = 0;
    pw->fDoneCancelled = 0;
    pw->fAskPending    = 0;
    pw->ulQuestion     = 0;

    /* Unnamed, private, initially reset.  Created once and kept for the life
       of the process -- these workers are per-program singletons, so there is
       no close path to get wrong.  If it cannot be created, PmWorkerAsk
       degrades to "the user said cancel", which is the safe answer. */
    if ( DosCreateEventSem( NULL, &pw->hevAnswer, 0, FALSE ) != NO_ERROR )
        pw->hevAnswer = NULLHANDLE;
}

/*---------------------------------------------------------------------------
 * PmWorkerStart
 *
 * fRunning is raised BEFORE _beginthread, not inside the body: if the caller
 * checked PmWorkerIsBusy immediately after starting a job, a flag raised by
 * the new thread might not be set yet and a second job would slip through.
 * On a failed start it is lowered again.
 *
 * _beginthread, not DosCreateThread.  DosCreateThread makes a thread the C
 * runtime knows nothing about, so errno, strtok, rand and the heap's
 * per-thread bookkeeping are all wrong on it -- and wrong intermittently,
 * which is the worst way for it to be wrong.  The Watcom OS/2 signature is
 *
 *      int _beginthread( void (*start)(void *), void *stack_bottom,
 *                        unsigned stack_size, void *arglist );
 *
 * where stack_bottom is a 16-bit leftover: pass NULL on 32-bit and the
 * runtime allocates stack_size bytes itself.  Returns the thread id, or -1.
 *-------------------------------------------------------------------------*/

BOOL PmWorkerStart( PMWORKER *pw, void ( *pfnBody )( void * ), void *pArg,
                    unsigned cbStack, HWND hwndNotify )
{
    int tid;

    if ( pw->fRunning )
        return FALSE;

    /* Clear last job's latches here rather than at the end of the previous
       one: a caller that never polled the old result would otherwise have it
       reported against the new job. */
    pw->fCancel      = 0;
    pw->fDonePending = 0;
    pw->fAskPending  = 0;
    pw->hwndNotify   = hwndNotify;
    pw->fRunning     = 1;

    tid = _beginthread( pfnBody, NULL, cbStack, pArg );

    if ( tid == -1 ) {
        pw->fRunning = 0;
        pw->tid      = 0;
        return FALSE;
    }

    pw->tid = (TID)tid;
    return TRUE;
}

/*---------------------------------------------------------------------------
 * UI-thread side
 *-------------------------------------------------------------------------*/

BOOL PmWorkerIsBusy( PMWORKER *pw )
{
    return (BOOL)( pw->fRunning != 0 );
}

void PmWorkerRequestCancel( PMWORKER *pw )
{
    pw->fCancel = 1;
}

/*---------------------------------------------------------------------------
 * PmWorkerPollDone / PmWorkerPollAsk - the UI thread's authoritative view
 *
 * Both test fRunning as well as the latch.  PmWorkerEnd raises fDonePending
 * BEFORE it lowers fRunning, so anything that observes fRunning == 0 is
 * guaranteed to observe the latch too, and the pair cannot be seen half-set.
 * Requiring both means a job is never reaped while its thread is still inside
 * PmWorkerEnd.
 *-------------------------------------------------------------------------*/

BOOL PmWorkerPollDone( PMWORKER *pw, ULONG *pulResult, BOOL *pfCancelled )
{
    if ( pw->fRunning || !pw->fDonePending )
        return FALSE;

    if ( pulResult )   *pulResult   = pw->ulResult;
    if ( pfCancelled ) *pfCancelled = (BOOL)( pw->fDoneCancelled != 0 );

    pw->fDonePending = 0;
    return TRUE;
}

BOOL PmWorkerPollAsk( PMWORKER *pw, ULONG *pulQuestion )
{
    if ( !pw->fAskPending )
        return FALSE;

    if ( pulQuestion ) *pulQuestion = pw->ulQuestion;

    /* Cleared on the way out, not in PmWorkerAnswer: the answering dialog is
       modal and pumps the queue, so this timer can fire again underneath it.
       Clearing here is what stops that turning into a second dialog. */
    pw->fAskPending = 0;
    return TRUE;
}

/*---------------------------------------------------------------------------
 * PmWorkerWait - ask the body to stop, then wait for it to actually return
 *
 * Polling with DosSleep rather than DosWaitThread: DosWaitThread blocks with
 * no timeout, so a body wedged inside a call that never returns (a dead
 * network redirector, say) would hang the exit path forever.  A bounded wait
 * lets the caller decide what to do about a worker that will not stop.
 *
 * This DOES block the UI thread for as long as it waits, which is exactly
 * what is wanted at shutdown -- the alternative is pumping messages into
 * windows that are in the middle of being destroyed.
 *-------------------------------------------------------------------------*/

BOOL PmWorkerWait( PMWORKER *pw, ULONG ulTimeoutMs )
{
    ULONG ulWaited = 0;

    pw->fCancel = 1;

    while ( pw->fRunning && ulWaited < ulTimeoutMs ) {
        DosSleep( PMWORKER_POLL_MS );
        ulWaited += PMWORKER_POLL_MS;
    }

    return (BOOL)( pw->fRunning == 0 );
}

/*---------------------------------------------------------------------------
 * Worker-thread side
 *-------------------------------------------------------------------------*/

BOOL PmWorkerCancelled( PMWORKER *pw )
{
    return (BOOL)( pw->fCancel != 0 );
}

/*---------------------------------------------------------------------------
 * PmWorkerAsk - block until the UI thread answers a question
 *
 * The semaphore is reset before the question is posted, not after: reset it
 * afterwards and an answer that arrived quickly would be thrown away and the
 * worker would wait for a second one that never comes.
 *
 * ulAnswer is pre-loaded with the cancel answer so that every early-out path
 * below -- no semaphore, already cancelled, post failed -- yields the same
 * safe result without a special case.
 *-------------------------------------------------------------------------*/

ULONG PmWorkerAsk( PMWORKER *pw, ULONG ulQuestion, ULONG ulAnswerIfCancelled )
{
    ULONG ulPostCount = 0;

    pw->ulAnswer = ulAnswerIfCancelled;

    if ( pw->hevAnswer == NULLHANDLE || pw->hwndNotify == NULLHANDLE )
        return ulAnswerIfCancelled;
    if ( pw->fCancel )
        return ulAnswerIfCancelled;

    DosResetEventSem( pw->hevAnswer, &ulPostCount );

    /* Latch first, then nudge.  If the post is a no-op the UI thread still
       finds the question on its next timer tick. */
    pw->ulQuestion  = ulQuestion;
    pw->fAskPending = 1;

    WinPostMsg( pw->hwndNotify, WMU_WORKER_ASK,
                MPFROMLONG( ulQuestion ), MPVOID );

    for ( ;; ) {
        if ( DosWaitEventSem( pw->hevAnswer, PMWORKER_POLL_MS ) == NO_ERROR ) {
            pw->fAskPending = 0;
            return pw->ulAnswer;
        }

        /* Timed out.  If the UI thread has meanwhile given up on us -- which
           is what WM_CLOSE does, from inside PmWorkerWait -- then it is no
           longer running a message loop and the answer will never come. */
        if ( pw->fCancel ) {
            pw->fAskPending = 0;
            return ulAnswerIfCancelled;
        }
    }
}

/*---------------------------------------------------------------------------
 * PmWorkerAnswer - UI thread's reply, from the WMU_WORKER_ASK handler
 *-------------------------------------------------------------------------*/

void PmWorkerAnswer( PMWORKER *pw, ULONG ulAnswer )
{
    pw->ulAnswer = ulAnswer;

    if ( pw->hevAnswer != NULLHANDLE )
        DosPostEventSem( pw->hevAnswer );
}

/*---------------------------------------------------------------------------
 * PmWorkerEnd
 *
 * ORDER MATTERS, in three ways:
 *
 *   1. The result and fDoneCancelled are stored before fDonePending is
 *      raised, so a poll that sees the latch sees a complete record.
 *   2. fDonePending is raised before fRunning is lowered, so a poll that sees
 *      the worker idle is guaranteed to see the latch as well.  Reversed,
 *      there is a window in which the job looks finished and unreapable, and
 *      the UI waits forever for an event it has already stepped past.
 *   3. fRunning is lowered before the post, so that by the time the UI thread
 *      runs its handler the worker already reads as idle and a new job can be
 *      started from there without being refused.
 *
 * The post itself is best-effort and its return value is deliberately
 * ignored -- see the header.  PmWorkerPollDone is what makes the job land.
 *-------------------------------------------------------------------------*/

void PmWorkerEnd( PMWORKER *pw, ULONG ulResult )
{
    HWND hwnd      = pw->hwndNotify;
    BOOL fCanceled = (BOOL)( pw->fCancel != 0 );

    pw->ulResult       = ulResult;
    pw->fDoneCancelled = (ULONG)( fCanceled ? 1 : 0 );
    pw->fAskPending    = 0;
    pw->fDonePending   = 1;

    pw->fRunning = 0;

    if ( hwnd != NULLHANDLE )
        WinPostMsg( hwnd, WMU_WORKER_DONE,
                    MPFROMLONG( ulResult ), MPFROMLONG( (ULONG)fCanceled ) );
}
