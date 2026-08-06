/*===========================================================================
 * DISKMODL.HPP  -  DiskStat shared disk model: tree node, constants, scanner
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * A node holds a full path component (CCHMAXPATHCOMP), not an 8.3 name, so a
 * long HPFS/JFS directory survives into the model; the views truncate on
 * drawing instead, measured against the actual column width via
 * GpiQueryTextBox rather than by a fixed character count.
 *===========================================================================*/
#ifndef DISKMODL_HPP
#define DISKMODL_HPP

// --------------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------------

#define APP_NAME            "Disk Statistics"
#define MAX_TREE_NODES      256

/* One path component + NUL.  Must stay == CCHMAXPATHCOMP from <bsedos.h>;
   spelled numerically here so the model header stays free of <os2.h>. */
#define MAX_NAME_LEN        256

/* Deepest nesting BuildNodePath can reassemble.  A node deeper than this
   cannot be reached anyway: ExpandNode refuses to descend past it. */
#define MAX_TREE_DEPTH      16

#define NODE_HAS_CHILDREN   0x01
#define NODE_EXPANDED       0x02

// --------------------------------------------------------------------------
// Disk model
//
// Sizes are 32-bit, matching ULONG cbFile in OS/2's FILEFINDBUF3.  Volumes
// larger than 4 GB are CLAMPED rather than wrapped -- see GetDiskSpaceInfo()
// in SCAN.CPP.
// --------------------------------------------------------------------------

typedef struct {
    char            name[MAX_NAME_LEN]; /* one path component + NUL           */
    unsigned long   total_size;         /* total bytes in this subtree        */
    unsigned char   depth;              /* nesting level (0 = root child)     */
    unsigned char   flags;              /* NODE_HAS_CHILDREN | NODE_EXPANDED  */
} tree_node_t;

// --------------------------------------------------------------------------
// Scanner API (SCAN.CPP)
//
// ScanDrive and ExpandNode are ASYNCHRONOUS.  They validate, reset the model,
// start the worker thread and return at once -- the tree is not built when
// they come back.  The UI learns that a job has ended by calling ScanPollDone
// (see ..\COMMON\PMWORKER.H), from its progress timer AND from the
// WMU_WORKER_DONE handler; whichever gets there first wins, and ScanPollDone
// answers TRUE only once.  ScanJobFinished must run before the model is read
// again.
//
// While a job runs, data_valid is FALSE and NOTHING may read tree_nodes[]:
// the worker owns it and there is no lock.  DRAW.CPP's early-out on
// !data_valid is what enforces that.
// --------------------------------------------------------------------------

void CollapseNode( int idx );                   /* drop a node's descendants  */
void BuildNodePath( int idx, char *path );      /* "X:\a\b"; >= CCHMAXPATH    */

void ScanDrive( HWND hwnd );                    /* START a scan of the drive  */
void ExpandNode( int idx, HWND hwnd );          /* START expanding a node     */
void PopulateDriveCombo( HWND hwnd );           /* fill the drive combobox    */

/* Worker plumbing.  All of these are UI-thread only. */
BOOL        ScanBusy        ( void );           /* a job is running           */
void        ScanCancel      ( void );           /* ask it to stop             */
BOOL        ScanShutdown    ( void );           /* cancel + wait, for WM_CLOSE*/
BOOL        ScanPollDone    ( BOOL *pfCancelled );/* TRUE once, job has ended */
void        ScanJobFinished ( BOOL fCancelled );/* fold the result back in    */
const char *ScanProgressPath( void );           /* directory being scanned    */

#endif // DISKMODL_HPP
