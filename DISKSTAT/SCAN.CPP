/*===========================================================================
 * SCAN.CPP  -  DiskStat disk scanner (OS/2): tree, drive query, expand/collapse
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * THREADING.  The scan runs on a worker thread (see ..\COMMON\PMWORKER.H):
 *
 *      ScanDrive / ExpandNode      UI thread; reset state, start the worker,
 *                                  return at once
 *      ScanWorkerBody              worker; ScanDriveWork / ExpandNodeWork
 *      ScanJobFinished             UI thread, from WMU_WORKER_DONE
 *
 * The worker makes NO PM calls.  Everything here is Dos* and plain C; the
 * Win and Gpi half lives in PMMAIN.CPP.
 *
 * The worker owns tree_nodes[] for the duration and there is no lock, so the
 * paint path must not read it meanwhile.  It does not: DRAW.CPP returns early
 * on !data_valid, and data_valid is FALSE from the moment a job starts until
 * ScanJobFinished sets it.  THAT GUARD IS LOAD-BEARING, not cosmetic.
 *
 * !! STACK !!  ScanDirectory recurses one ~600-byte frame per directory level.
 * The 256 KB in BUILD\MKOS2.CMD sizes the MAIN thread only, so the worker asks
 * for PMWORKER_STACK (the same 256 KB) explicitly.  Do not shrink either.
 *===========================================================================*/
#define INCL_DOSFILEMGR
#define INCL_DOSMISC
#define INCL_DOSERRORS
#define INCL_WIN
#define INCL_GPI

#include <os2.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "diskstat.h"
#include "pmworker.h"
#include "res/resource.h"

// --------------------------------------------------------------------------
// Limits
// --------------------------------------------------------------------------

#define MAX_SUBDIRS     64      /* children collected per ExpandNode          */
#define MAX_RECURSE     64      /* runaway guard on ScanDirectory recursion   */

#define FIND_ATTRS      ( FILE_NORMAL | FILE_READONLY | FILE_HIDDEN \
                        | FILE_SYSTEM | FILE_DIRECTORY | FILE_ARCHIVED )

// --------------------------------------------------------------------------
// File-local prototypes
// --------------------------------------------------------------------------

static unsigned long ScanDirectory( const char *path, int level );
static void          GetDiskSpaceInfo( int drive );
static int           HasSubdirectories( const char *path );
static int           IsDotDir( const char *name );
static int           IsScannableDrive( int drive );

static void          ScanWorkerBody( void *arg );
static void          ScanDriveWork ( void );
static void          ExpandNodeWork( int idx );
static void          SetProgress   ( const char *path );

// --------------------------------------------------------------------------
// Worker state
//
//   One worker, two kinds of job.  g_job* are written by the UI thread before
//   the worker starts and only read by the worker afterwards, so they need no
//   guarding: PmWorkerStart is the handover point.
// --------------------------------------------------------------------------

#define SCANJOB_DRIVE   1               /* rescan current_drive from scratch  */
#define SCANJOB_EXPAND  2               /* size one node's children           */

static PMWORKER      g_worker;
static int           g_worker_ready = 0;
static int           g_job_kind     = 0;
static int           g_job_node     = -1;

// --------------------------------------------------------------------------
// Progress path
//
//   Written by the worker on every directory it enters, read by the UI thread
//   from a WM_TIMER (see PMMAIN.CPP).  Pulled rather than pushed because a
//   posted message per directory would flood the queue on a large volume, and
//   WinPostMsg starts failing when a queue fills.
//
//   NO MUTEX, DELIBERATELY.  The buffer is one byte longer than the longest
//   path it can be given and that last byte is never written, so it is ALWAYS
//   terminated inside its own storage however badly a read and a write
//   interleave; the worst case is one tick showing a mix of two paths.
//   Shrinking it to exactly CCHMAXPATH would break that.
// --------------------------------------------------------------------------

static char          g_prog_path[CCHMAXPATH + 1] = { 0 };

static void SetProgress( const char *path )
{
    strncpy( g_prog_path, path, CCHMAXPATH );
    g_prog_path[CCHMAXPATH] = '\0';
}

// --------------------------------------------------------------------------
// Scanner worker plumbing - all of this runs on the UI thread
// --------------------------------------------------------------------------

static void EnsureWorker( void )
{
    if ( !g_worker_ready ) {
        PmWorkerInit( &g_worker );
        g_worker_ready = 1;
    }
}

BOOL ScanBusy( void )
{
    EnsureWorker();
    return PmWorkerIsBusy( &g_worker );
}

void ScanCancel( void )
{
    EnsureWorker();
    PmWorkerRequestCancel( &g_worker );
}

const char *ScanProgressPath( void )
{
    return g_prog_path;
}

// --------------------------------------------------------------------------
// ScanPollDone - has the running job ended?
//
//   TRUE exactly once per job.  Called from the progress timer as well as
//   from the WMU_WORKER_DONE handler, so that a completion message that never
//   arrives -- the worker posts it from a thread that owns no anchor block --
//   cannot leave a finished scan looking like a hung one.
// --------------------------------------------------------------------------

BOOL ScanPollDone( BOOL *pfCancelled )
{
    EnsureWorker();
    return PmWorkerPollDone( &g_worker, NULL, pfCancelled );
}

// --------------------------------------------------------------------------
// ScanShutdown - stop the worker before the window it reports to goes away
//
//   Called from WM_CLOSE.  A worker still writing tree_nodes[] after the
//   frame is destroyed would be writing into a torn-down process, and would
//   post WMU_WORKER_DONE to a dead window handle.
// --------------------------------------------------------------------------

BOOL ScanShutdown( void )
{
    EnsureWorker();

    if ( !PmWorkerIsBusy( &g_worker ) )
        return TRUE;

    return PmWorkerWait( &g_worker, 5000 );
}

// --------------------------------------------------------------------------
// ScanJobFinished - fold a completed job back into the model
//
//   Called on the UI thread from WMU_WORKER_DONE, which is the point at which
//   the worker has stopped touching tree_nodes[] and the paint path may read
//   it again.
//
//   A cancelled SCANJOB_DRIVE leaves a half-sized tree, which would be a lie
//   dressed as data, so the model is emptied.  A cancelled SCANJOB_EXPAND is
//   different: its children are already inserted and merely have zero sizes
//   past the point where it stopped -- incomplete but not wrong -- so they
//   are kept.
// --------------------------------------------------------------------------

void ScanJobFinished( BOOL fCancelled )
{
    if ( g_job_kind == SCANJOB_DRIVE && fCancelled ) {
        tree_count    = 0;
        selected_node = -1;
        data_valid    = FALSE;
    }
    else {
        data_valid = TRUE;
    }

    g_job_kind       = 0;
    g_job_node       = -1;
    scan_in_progress = FALSE;
    g_prog_path[0]   = '\0';
}

// --------------------------------------------------------------------------
// ScanWorkerBody - the worker thread's entry point
//
//   !! NO PM CALLS BELOW THIS LINE !!  Not one Win* or Gpi*.  The UI is driven
//   entirely from PMMAIN.CPP's timer and its WMU_WORKER_DONE handler.
// --------------------------------------------------------------------------

static void ScanWorkerBody( void *arg )
{
    (void)arg;

    if ( g_job_kind == SCANJOB_DRIVE )
        ScanDriveWork();
    else if ( g_job_kind == SCANJOB_EXPAND )
        ExpandNodeWork( g_job_node );

    PmWorkerEnd( &g_worker, 0 );
}

// --------------------------------------------------------------------------
// IsDotDir - true for "." and ".."
//
//   Compares the whole name: testing name[0] != '.' would also throw away
//   legitimate HPFS directories such as ".config".
// --------------------------------------------------------------------------

static int IsDotDir( const char *name )
{
    return ( name[0] == '.'
             && ( name[1] == '\0'
                  || ( name[1] == '.' && name[2] == '\0' ) ) );
}

// --------------------------------------------------------------------------
// GetDiskSpaceInfo - queries volume size and free space
//
//   FSALLOCATE reports the volume in allocation units and the product can
//   overflow the 32-bit byte model.  Rather than wrap silently, the totals are
//   computed in 64 bits and CLAMPED, with disk_size_clamped raised so the
//   status bar can mark the figure as a ceiling.
// --------------------------------------------------------------------------

static void GetDiskSpaceInfo( int drive )
{
    FSALLOCATE fsa;
    unsigned __int64 bpu, ull_total, ull_free;

    total_disk_space  = 0L;
    free_disk_space   = 0L;
    used_disk_space   = 0L;
    disk_size_clamped = FALSE;

    if ( DosQueryFSInfo( (ULONG)drive, FSIL_ALLOC,
                         (PVOID)&fsa, sizeof( fsa ) ) != NO_ERROR )
        return;

    bpu = (unsigned __int64)fsa.cSectorUnit * (unsigned __int64)fsa.cbSector;

    ull_total = (unsigned __int64)fsa.cUnit      * bpu;
    ull_free  = (unsigned __int64)fsa.cUnitAvail * bpu;

    if ( ull_total > 0xFFFFFFFFui64 ) {
        disk_size_clamped = TRUE;
        ull_total = 0xFFFFFFFFui64;
    }
    if ( ull_free > ull_total )
        ull_free = ull_total;

    total_disk_space = (unsigned long)ull_total;
    free_disk_space  = (unsigned long)ull_free;
    used_disk_space  = total_disk_space - free_disk_space;
}

// --------------------------------------------------------------------------
// HasSubdirectories - quick check whether a path contains any subdirectory
// --------------------------------------------------------------------------

static int HasSubdirectories( const char *path )
{
    HDIR         hdir = HDIR_CREATE;
    FILEFINDBUF3 ff;
    ULONG        cnt = 1;
    char         search[CCHMAXPATH];
    int          found = 0;

    sprintf( search, "%s\\*", path );

    if ( DosFindFirst( (PCSZ)search, &hdir, FIND_ATTRS,
                       (PVOID)&ff, sizeof( ff ), &cnt,
                       FIL_STANDARD ) == NO_ERROR ) {
        do {
            if ( ( ff.attrFile & FILE_DIRECTORY ) && !IsDotDir( ff.achName ) ) {
                found = 1;
                break;
            }
            cnt = 1;
        } while ( DosFindNext( hdir, (PVOID)&ff, sizeof( ff ), &cnt )
                  == NO_ERROR );

        DosFindClose( hdir );
    }

    return found;
}

// --------------------------------------------------------------------------
// ScanDirectory - recursively sums the bytes below 'path'
//
//   Recurses straight out of its own find loop: OS/2's per-search HDIR
//   survives the nested DosFindFirst, so there is no need to buffer the child
//   names first and no cap on how many subdirectories contribute to a total.
//
//   WORKER THREAD.  The cancel flag is polled once per directory entered and
//   once per find iteration.  DosFindClose still runs on the way out of every
//   level, so a cancel leaks no search handles.
// --------------------------------------------------------------------------

static unsigned long ScanDirectory( const char *path, int level )
{
    HDIR          hdir = HDIR_CREATE;
    FILEFINDBUF3  ff;
    ULONG         cnt = 1;
    char          search[CCHMAXPATH];
    char          subpath[CCHMAXPATH];
    unsigned long dir_total = 0L;

    if ( level >= MAX_RECURSE ) return 0L;
    if ( PmWorkerCancelled( &g_worker ) ) return 0L;

    SetProgress( path );

    sprintf( search, "%s\\*", path );

    if ( DosFindFirst( (PCSZ)search, &hdir, FIND_ATTRS,
                       (PVOID)&ff, sizeof( ff ), &cnt,
                       FIL_STANDARD ) != NO_ERROR )
        return 0L;

    do {
        if ( PmWorkerCancelled( &g_worker ) ) break;

        if ( ff.attrFile & FILE_DIRECTORY ) {
            if ( !IsDotDir( ff.achName )
                 && strlen( path ) + 1 + ff.cchName < CCHMAXPATH ) {
                sprintf( subpath, "%s\\%s", path, ff.achName );
                dir_total += ScanDirectory( subpath, level + 1 );
            }
        }
        else {
            dir_total += ff.cbFile;
        }
        cnt = 1;
    } while ( DosFindNext( hdir, (PVOID)&ff, sizeof( ff ), &cnt )
              == NO_ERROR );

    DosFindClose( hdir );
    return dir_total;
}

// --------------------------------------------------------------------------
// BuildNodePath - reconstructs the full path for a tree node by walking
//   backwards through the flat array to find an ancestor at each depth level.
//   'path' must have room for CCHMAXPATH bytes.
// --------------------------------------------------------------------------

void BuildNodePath( int idx, char *path )
{
    static char parts[MAX_TREE_DEPTH][MAX_NAME_LEN];    /* 4 KB - keep off the
                                                           recursion stack   */
    int d, i, j;

    d = tree_nodes[idx].depth;
    if ( d >= MAX_TREE_DEPTH ) d = MAX_TREE_DEPTH - 1;

    strcpy( parts[d], tree_nodes[idx].name );

    // walk backwards to collect ancestors
    for ( i = idx - 1; i >= 0 && d > 0; i-- ) {
        if ( tree_nodes[i].depth == d - 1 ) {
            d--;
            strcpy( parts[d], tree_nodes[i].name );
        }
    }

    // assemble "X:\part0\part1\..."
    path[0] = (char)( 'A' + current_drive - 1 );
    path[1] = ':';
    path[2] = '\0';

    for ( j = 0; j <= (int)tree_nodes[idx].depth && j < MAX_TREE_DEPTH; j++ ) {
        if ( strlen( path ) + 1 + strlen( parts[j] ) >= CCHMAXPATH ) break;
        strcat( path, "\\" );
        strcat( path, parts[j] );
    }
}

// --------------------------------------------------------------------------
// ExpandNode - start expanding a tree node (UI thread)
//
//   The eligibility tests are cheap and are done here, on the UI thread, so
//   that a click that cannot expand anything costs nothing and leaves no
//   "Scanning" dialog to dismiss.
//
//   data_valid drops for the duration.  An expand shifts the whole tail of
//   tree_nodes[] down to make room and then fills the sizes in one at a time;
//   painting during that would show a half-built array.
// --------------------------------------------------------------------------

void ExpandNode( int idx, HWND hwnd )
{
    (void)hwnd;                         /* PMMAIN drives the UI now          */

    EnsureWorker();

    if ( PmWorkerIsBusy( &g_worker ) ) return;

    if ( idx < 0 || idx >= tree_count ) return;
    if ( tree_nodes[idx].flags & NODE_EXPANDED ) return;
    if ( !( tree_nodes[idx].flags & NODE_HAS_CHILDREN ) ) return;
    if ( tree_nodes[idx].depth + 1 >= MAX_TREE_DEPTH ) {
        tree_nodes[idx].flags &= ~NODE_HAS_CHILDREN;
        return;
    }

    g_job_kind = SCANJOB_EXPAND;
    g_job_node = idx;

    scan_in_progress = TRUE;
    data_valid       = FALSE;
    g_prog_path[0]   = '\0';

    if ( !PmWorkerStart( &g_worker, ScanWorkerBody, NULL,
                         PMWORKER_STACK, hwndClient ) ) {
        /* No thread: leave the model exactly as it was and say so by
           restoring the flags.  The caller sees ScanBusy() == FALSE. */
        g_job_kind       = 0;
        g_job_node       = -1;
        scan_in_progress = FALSE;
        data_valid       = TRUE;
    }
}

// --------------------------------------------------------------------------
// ExpandNodeWork - the expand itself (worker thread)
// --------------------------------------------------------------------------

static void ExpandNodeWork( int idx )
{
    char          path[CCHMAXPATH];
    char          search[CCHMAXPATH];
    char          subpath[CCHMAXPATH];
    HDIR          hdir = HDIR_CREATE;
    FILEFINDBUF3  ff;
    ULONG         cnt = 1;
    char        (*subdirs)[MAX_NAME_LEN];
    int           sub_count = 0;
    int           insert_at, avail, n, i, j;
    unsigned char new_depth;

    // 16 KB of names: on the heap, not on a stack ScanDirectory is about to
    // recurse down.
    subdirs = (char (*)[MAX_NAME_LEN])malloc( MAX_SUBDIRS * MAX_NAME_LEN );
    if ( !subdirs ) return;

    BuildNodePath( idx, path );
    new_depth = (unsigned char)( tree_nodes[idx].depth + 1 );

    // collect the child directory names
    sprintf( search, "%s\\*", path );
    if ( DosFindFirst( (PCSZ)search, &hdir, FIND_ATTRS,
                       (PVOID)&ff, sizeof( ff ), &cnt,
                       FIL_STANDARD ) == NO_ERROR ) {
        do {
            if ( ( ff.attrFile & FILE_DIRECTORY )
                 && !IsDotDir( ff.achName )
                 && sub_count < MAX_SUBDIRS ) {
                strncpy( subdirs[sub_count], ff.achName, MAX_NAME_LEN - 1 );
                subdirs[sub_count][MAX_NAME_LEN - 1] = '\0';
                sub_count++;
            }
            cnt = 1;
        } while ( DosFindNext( hdir, (PVOID)&ff, sizeof( ff ), &cnt )
                  == NO_ERROR );

        DosFindClose( hdir );
    }

    if ( sub_count == 0 ) {
        tree_nodes[idx].flags &= ~NODE_HAS_CHILDREN;
        free( subdirs );
        return;
    }

    // limit to available space
    avail = MAX_TREE_NODES - tree_count;
    n = ( sub_count < avail ) ? sub_count : avail;
    if ( n <= 0 ) {
        free( subdirs );
        return;
    }

    insert_at = idx + 1;

    // shift existing nodes to make room for n children
    if ( insert_at < tree_count ) {
        memmove( &tree_nodes[insert_at + n],
                 &tree_nodes[insert_at],
                 sizeof( tree_node_t ) * ( tree_count - insert_at ) );
    }

    // adjust the cursor if it sat at or below the insertion point
    if ( selected_node >= insert_at )
        selected_node += n;

    tree_count += n;

    // Populate new child nodes.  The names and depths are filled in for ALL of
    // them first, so that a cancel part-way through the sizing still leaves a
    // structurally complete array -- just one with zero sizes past the point
    // where it stopped.
    for ( i = 0; i < n; i++ ) {
        tree_node_t *node = &tree_nodes[insert_at + i];

        strcpy( node->name, subdirs[i] );
        node->depth      = new_depth;
        node->flags      = 0;
        node->total_size = 0L;
    }

    for ( i = 0; i < n; i++ ) {
        tree_node_t *node = &tree_nodes[insert_at + i];

        if ( PmWorkerCancelled( &g_worker ) ) break;

        sprintf( subpath, "%s\\%s", path, subdirs[i] );
        node->total_size = ScanDirectory( subpath, 0 );

        if ( HasSubdirectories( subpath ) )
            node->flags |= NODE_HAS_CHILDREN;
    }

    // sort new children by size descending (bubble sort)
    for ( i = 0; i < n - 1; i++ ) {
        for ( j = 0; j < n - 1 - i; j++ ) {
            int a = insert_at + j;
            int b = insert_at + j + 1;
            if ( tree_nodes[a].total_size < tree_nodes[b].total_size ) {
                tree_node_t temp = tree_nodes[a];
                tree_nodes[a] = tree_nodes[b];
                tree_nodes[b] = temp;
            }
        }
    }

    tree_nodes[idx].flags |= NODE_EXPANDED;

    free( subdirs );
}

// --------------------------------------------------------------------------
// CollapseNode - removes all of a node's descendants from the flat array.
// --------------------------------------------------------------------------

void CollapseNode( int idx )
{
    int end_idx;
    int remove_count;
    unsigned char d;

    if ( idx < 0 || idx >= tree_count ) return;
    if ( !( tree_nodes[idx].flags & NODE_EXPANDED ) ) return;

    d = tree_nodes[idx].depth;

    // find first node past all descendants
    end_idx = idx + 1;
    while ( end_idx < tree_count && tree_nodes[end_idx].depth > d )
        end_idx++;

    remove_count = end_idx - ( idx + 1 );
    if ( remove_count <= 0 ) {
        tree_nodes[idx].flags &= ~NODE_EXPANDED;
        return;
    }

    // cursor inside the vanishing subtree: clear it, as the GUI build did
    if ( selected_node > idx && selected_node < end_idx )
        selected_node = -1;
    else if ( selected_node >= end_idx )
        selected_node -= remove_count;

    // shift remaining nodes up
    if ( end_idx < tree_count ) {
        memmove( &tree_nodes[idx + 1],
                 &tree_nodes[end_idx],
                 sizeof( tree_node_t ) * ( tree_count - end_idx ) );
    }

    tree_count -= remove_count;
    tree_nodes[idx].flags &= ~NODE_EXPANDED;
}

// --------------------------------------------------------------------------
// ScanDrive - start a rescan of current_drive (UI thread)
//
//   Resets the model here rather than on the worker, so that the instant this
//   returns the paint path already sees an empty tree and data_valid FALSE.
//   PMMAIN.CPP puts up the "Scanning" dialog and starts the progress timer
//   when it sees ScanBusy().
// --------------------------------------------------------------------------

void ScanDrive( HWND hwnd )
{
    (void)hwnd;                         /* PMMAIN drives the UI now          */

    EnsureWorker();

    if ( PmWorkerIsBusy( &g_worker ) ) return;

    g_job_kind = SCANJOB_DRIVE;
    g_job_node = -1;

    scan_in_progress  = TRUE;
    data_valid        = FALSE;
    folder_scroll_pos = 0;
    tree_count        = 0;
    selected_node     = -1;
    g_prog_path[0]    = '\0';

    memset( tree_nodes, 0, sizeof( tree_node_t ) * MAX_TREE_NODES );

    if ( !PmWorkerStart( &g_worker, ScanWorkerBody, NULL,
                         PMWORKER_STACK, hwndClient ) ) {
        g_job_kind       = 0;
        scan_in_progress = FALSE;
        data_valid       = FALSE;
    }
}

// --------------------------------------------------------------------------
// ScanDriveWork - the scan itself (worker thread)
// --------------------------------------------------------------------------

static void ScanDriveWork( void )
{
    HDIR         hdir = HDIR_CREATE;
    FILEFINDBUF3 ff;
    ULONG        cnt = 1;
    char         root_path[8];
    char         search_path[16];
    char         dir_path[CCHMAXPATH];
    int          i, j;

    GetDiskSpaceInfo( current_drive );

    // build root path (e.g. "C:")
    root_path[0] = (char)( 'A' + current_drive - 1 );
    root_path[1] = ':';
    root_path[2] = '\0';

    sprintf( search_path, "%s\\*", root_path );

    // collect the root directory names
    if ( DosFindFirst( (PCSZ)search_path, &hdir, FIND_ATTRS,
                       (PVOID)&ff, sizeof( ff ), &cnt,
                       FIL_STANDARD ) == NO_ERROR ) {
        do {
            if ( ( ff.attrFile & FILE_DIRECTORY )
                 && !IsDotDir( ff.achName )
                 && tree_count < MAX_TREE_NODES ) {
                strncpy( tree_nodes[tree_count].name, ff.achName,
                         MAX_NAME_LEN - 1 );
                tree_nodes[tree_count].name[MAX_NAME_LEN - 1] = '\0';
                tree_nodes[tree_count].total_size = 0L;
                tree_nodes[tree_count].depth = 0;
                tree_nodes[tree_count].flags = 0;
                tree_count++;
            }
            cnt = 1;
        } while ( DosFindNext( hdir, (PVOID)&ff, sizeof( ff ), &cnt )
                  == NO_ERROR );

        DosFindClose( hdir );
    }

    // now size each root directory
    for ( i = 0; i < tree_count; i++ ) {
        if ( PmWorkerCancelled( &g_worker ) ) return;

        sprintf( dir_path, "%s\\%s", root_path, tree_nodes[i].name );
        tree_nodes[i].total_size = ScanDirectory( dir_path, 0 );

        if ( HasSubdirectories( dir_path ) )
            tree_nodes[i].flags |= NODE_HAS_CHILDREN;
    }

    // sort root nodes by size descending (bubble sort)
    for ( i = 0; i < tree_count - 1; i++ ) {
        for ( j = 0; j < tree_count - 1 - i; j++ ) {
            if ( tree_nodes[j].total_size < tree_nodes[j + 1].total_size ) {
                tree_node_t temp = tree_nodes[j];
                tree_nodes[j] = tree_nodes[j + 1];
                tree_nodes[j + 1] = temp;
            }
        }
    }
}

// --------------------------------------------------------------------------
// IsScannableDrive - true for a fixed or network drive that is ready
//
//   OS/2 has no GetDriveType, so this asks the file system two questions: is
//   the volume ready (DosQueryFSInfo), and is it something other than a CD
//   (the attached FSD name).  A not-ready removable fails the first test.
// --------------------------------------------------------------------------

static int IsScannableDrive( int drive )
{
    FSALLOCATE fsa;
    char       dev[3];
    ULONG      cb;
    /* FSQBUFFER2's three trailing names are variable length; over-allocate. */
    BYTE       buf[sizeof( FSQBUFFER2 ) + 3 * CCHMAXPATH];
    PFSQBUFFER2 pfsq = (PFSQBUFFER2)buf;

    if ( DosQueryFSInfo( (ULONG)drive, FSIL_ALLOC,
                         (PVOID)&fsa, sizeof( fsa ) ) != NO_ERROR )
        return 0;

    dev[0] = (char)( 'A' + drive - 1 );
    dev[1] = ':';
    dev[2] = '\0';

    cb = sizeof( buf );
    if ( DosQueryFSAttach( (PCSZ)dev, 0, FSAIL_QUERYNAME, pfsq, &cb )
         == NO_ERROR ) {
        /* szFSDName follows szName in the packed tail */
        const char *fsd = (const char *)pfsq->szName + pfsq->cbName + 1;
        if ( strcmp( fsd, "CDFS" ) == 0 )
            return 0;
    }

    return 1;
}

// --------------------------------------------------------------------------
// PopulateDriveCombo - fills the toolbar combobox with scannable drives
//
//   Floppies (A: / B:) are skipped.
// --------------------------------------------------------------------------

void PopulateDriveCombo( HWND hwnd )
{
    ULONG ulCurrent = 0, ulMap = 0;
    int   d, idx;
    char  label[8];

    (void)hwnd;

    WinSendMsg( hDriveCombo, LM_DELETEALL, MPVOID, MPVOID );

    if ( DosQueryCurrentDisk( &ulCurrent, &ulMap ) != NO_ERROR )
        return;

    for ( d = 3; d <= 26; d++ ) {
        if ( !( ulMap & ( 1UL << ( d - 1 ) ) ) ) continue;
        if ( !IsScannableDrive( d ) ) continue;

        sprintf( label, "%c:", 'A' + d - 1 );

        idx = (int)(LONG)WinSendMsg( hDriveCombo, LM_INSERTITEM,
                                     MPFROMSHORT( LIT_END ),
                                     MPFROMP( label ) );
        if ( idx < 0 ) continue;

        WinSendMsg( hDriveCombo, LM_SETITEMHANDLE,
                    MPFROMSHORT( (SHORT)idx ), MPFROMLONG( (LONG)d ) );

        if ( d == current_drive ) {
            WinSendMsg( hDriveCombo, LM_SELECTITEM,
                        MPFROMSHORT( (SHORT)idx ), MPFROMSHORT( TRUE ) );
        }
    }
}
