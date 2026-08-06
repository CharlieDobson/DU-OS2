/*====================================================================
 * GAPBUF.C -- Large-file text engine for Enhanced Notepad (OS/2 PM)
 *
 * Implements the TEXTBUF object declared in GAPBUF.H: a gap buffer in
 * flat 32-bit memory, a logical-line index kept in *raw* buffer
 * coordinates, and a doubly linked undo/redo stack.
 *
 * Raw vs. document coordinates
 * ----------------------------
 * The physical buffer is:  [ before-gap | gap | after-gap ]
 * A "raw" offset indexes the physical buffer; a "document" position
 * indexes the logical text (before-gap + after-gap, gap skipped).
 *   raw  = doc                      when doc <  gapStart
 *   raw  = doc + gapSize            when doc >= gapStart
 *   doc  = raw                      when raw <= gapStart
 *   doc  = raw - gapSize            when raw >= gapEnd
 * No line ever starts inside the gap, so line raw offsets are always
 * convertible.  Storing line starts as *raw* offsets means plain
 * typing (which only slides gapStart) leaves every line entry
 * untouched; only the gap-relocation and newline paths adjust them.
 *
 * Memory is a flat malloc'd char*.  File I/O is DosOpen / DosRead /
 * DosWrite / DosSetFilePtr; line endings are normalized CRLF -> LF on
 * load and expanded LF -> CRLF on save, which is what OS/2 text files
 * use.
 *
 * Compiler: Open Watcom 1.9  (wcc386 -bt=os2)
 *====================================================================*/

#define INCL_DOSFILEMGR
#define INCL_DOSERRORS

#include <os2.h>
#include <string.h>
#include <stdlib.h>
#include "gapbuf.h"

/*--------------------------------------------------------------------
 * Tunables
 *------------------------------------------------------------------*/
#define TB_INIT_GAP   0x4000L      /* 16 KB initial gap             */
#define TB_GROW       0x10000L     /* grow the gap by >= 64 KB      */
#define TB_LINE_INIT  512L         /* initial line-table entries    */
#define TB_UNDO_MAX   400L         /* cap on undo records           */
#define TB_IOCHUNK    0x8000L      /* 32 KB I/O conversion chunk    */

/*--------------------------------------------------------------------
 * Forward declarations (internal)
 *------------------------------------------------------------------*/
static void RebuildLines(TEXTBUF *tb);

/*--------------------------------------------------------------------
 * Small inline-ish helpers
 *------------------------------------------------------------------*/
static ULONG GapSize(TEXTBUF *tb) { return tb->gapEnd - tb->gapStart; }
static ULONG DocLen (TEXTBUF *tb) { return tb->cap - (tb->gapEnd - tb->gapStart); }

/* raw offset -> document position.  A raw offset == gapStart is the
   gap boundary (e.g. the empty final line when the file ends in a
   newline and the gap sits at the end); it maps to gapStart, NOT to
   the after-gap region.  Using '<=' rather than '<' keeps that
   boundary line from being mislocated by a whole gap-size. */
static ULONG RawToDoc(TEXTBUF *tb, ULONG raw)
{
    return (raw <= tb->gapStart) ? raw : raw - GapSize(tb);
}

/*====================================================================
 * Line-table primitives
 *   pLines[0] is unused; line 0 always starts at document 0.
 *   pLines[i] (i >= 1) holds the RAW start offset of line i.
 *====================================================================*/

static BOOL EnsureLineCap(TEXTBUF *tb, LONG need)
{
    ULONG *np;
    if (need <= tb->nLineCap)
        return TRUE;

    while (tb->nLineCap < need)
        tb->nLineCap += (tb->nLineCap >> 1) + TB_LINE_INIT;

    np = (ULONG *)realloc(tb->pLines, (ULONG)tb->nLineCap * sizeof(ULONG));
    if (!np)
        return FALSE;                       /* old pLines still valid */
    tb->pLines = np;
    return TRUE;
}

/* First line index (>= 1) whose raw offset is >= raw, else nLines. */
static LONG LowerBoundRaw(TEXTBUF *tb, ULONG raw)
{
    LONG lo = 1, hi = tb->nLines - 1, res = tb->nLines;
    while (lo <= hi)
    {
        LONG mid = (lo + hi) >> 1;
        if (tb->pLines[mid] >= raw) { res = mid; hi = mid - 1; }
        else                        { lo = mid + 1; }
    }
    return res;
}

/* Add delta to every line entry whose raw offset is in [rawLo, rawHi).
   Entries are ascending in raw, so we bsearch the low bound and walk
   only the affected slice. */
static void AdjustLineRange(TEXTBUF *tb, ULONG rawLo, ULONG rawHi, LONG delta)
{
    LONG i = LowerBoundRaw(tb, rawLo);
    for (; i < tb->nLines; i++)
    {
        ULONG r = tb->pLines[i];
        if (r >= rawHi) break;
        tb->pLines[i] = (ULONG)((LONG)r + delta);
    }
}

/*====================================================================
 * Gap movement and growth
 *====================================================================*/

/* Grow the buffer so the gap holds at least 'need' bytes.  gapStart
   is preserved (caller has already positioned the gap). */
static BOOL EnsureGap(TEXTBUF *tb, ULONG need)
{
    ULONG oldCap, newCap, tailLen, addl;
    char *np;

    if (GapSize(tb) >= need)
        return TRUE;

    oldCap  = tb->cap;
    newCap  = oldCap + (need - GapSize(tb)) + TB_GROW;
    tailLen = oldCap - tb->gapEnd;
    addl    = newCap - oldCap;

    np = (char *)realloc(tb->pBuf, newCap);
    if (!np)
        return FALSE;                       /* old pBuf still valid */
    tb->pBuf = np;

    /* Slide the after-gap tail to the very end, enlarging the gap. */
    memmove(tb->pBuf + (newCap - tailLen), tb->pBuf + tb->gapEnd, tailLen);
    AdjustLineRange(tb, tb->gapEnd, oldCap, (LONG)addl);

    tb->gapEnd = newCap - tailLen;
    tb->cap    = newCap;
    return TRUE;
}

/* Move the gap so it begins at document position 'pos'. */
static void MoveGap(TEXTBUF *tb, ULONG pos)
{
    ULONG gs = GapSize(tb);
    ULONG n;

    if (pos == tb->gapStart)
        return;

    if (pos < tb->gapStart)
    {
        /* gap slides toward start: move [pos, gapStart) up past the gap */
        n = tb->gapStart - pos;
        memmove(tb->pBuf + tb->gapEnd - n, tb->pBuf + pos, n);
        AdjustLineRange(tb, pos, tb->gapStart, (LONG)gs);
        tb->gapStart  = pos;
        tb->gapEnd   -= n;
    }
    else
    {
        /* gap slides toward end: move [gapEnd, gapEnd+n) down before gap */
        n = pos - tb->gapStart;
        memmove(tb->pBuf + tb->gapStart, tb->pBuf + tb->gapEnd, n);
        AdjustLineRange(tb, tb->gapEnd, tb->gapEnd + n, -(LONG)gs);
        tb->gapStart += n;
        tb->gapEnd   += n;
    }
}

/*====================================================================
 * Low-level insert / delete (no undo recording)
 *====================================================================*/

/* Insert 'len' bytes of 'src' at document position 'pos'. */
static BOOL RawInsert(TEXTBUF *tb, ULONG pos, const char *src, ULONG len)
{
    ULONG base, k, nNew;
    LONG  idx;

    MoveGap(tb, pos);
    if (!EnsureGap(tb, len))
        return FALSE;

    base = tb->gapStart;                    /* == pos, raw */
    memcpy(tb->pBuf + base, src, len);

    /* Count newlines to size the line-table insertion. */
    nNew = 0;
    for (k = 0; k < len; k++)
        if (tb->pBuf[base + k] == '\n') nNew++;

    if (nNew == 0)                          /* fast path: no new lines */
    {
        tb->gapStart = base + len;
        return TRUE;
    }

    idx = TbLineFromPos(tb, pos) + 1;       /* first new line index    */

    if (EnsureLineCap(tb, tb->nLines + (LONG)nNew))
    {
        LONG i, j;
        for (i = tb->nLines - 1; i >= idx; i--)   /* open a gap        */
            tb->pLines[i + nNew] = tb->pLines[i];
        j = idx;                                  /* fill new starts   */
        for (k = 0; k < len; k++)
            if (tb->pBuf[base + k] == '\n')
                tb->pLines[j++] = base + k + 1;
        tb->nLines  += nNew;
        tb->gapStart = base + len;
    }
    else
    {
        /* Line table couldn't grow: commit the text, then make the
           document contiguous and rebuild the index from scratch. */
        tb->gapStart = base + len;
        MoveGap(tb, DocLen(tb));
        RebuildLines(tb);
    }
    return TRUE;
}

/* Delete 'len' bytes starting at document position 'pos'. */
static void RawDelete(TEXTBUF *tb, ULONG pos, ULONG len)
{
    LONG first, last, removed, i;

    MoveGap(tb, pos);

    /* Lines whose start is in (pos, pos+len] disappear. */
    first = TbLineFromPos(tb, pos) + 1;
    last  = TbLineFromPos(tb, pos + len);
    if (last >= first)
    {
        removed = last - first + 1;
        for (i = last + 1; i < tb->nLines; i++)
            tb->pLines[i - removed] = tb->pLines[i];
        tb->nLines -= removed;
    }

    tb->gapEnd += len;                       /* swallow the bytes */
}

/*====================================================================
 * Undo stack
 *====================================================================*/

static char *CopyToHeap(const char *src, ULONG len)
{
    char *p = (char *)malloc(len ? len : 1);
    if (!p) return NULL;
    if (len) memcpy(p, src, len);
    return p;
}

static void FreeRec(UNDOREC *r)
{
    if (r)
    {
        if (r->pText) free(r->pText);
        free(r);
    }
}

/* Discard the redo tail (everything newer than pCur). */
static void TruncateRedo(TEXTBUF *tb)
{
    UNDOREC *r = tb->pCur ? tb->pCur->pNext : tb->pUndoHead;
    while (r)
    {
        UNDOREC *nx = r->pNext;
        FreeRec(r);
        tb->nUndo--;
        r = nx;
    }
    if (tb->pCur) { tb->pCur->pNext = NULL; tb->pUndoTail = tb->pCur; }
    else          { tb->pUndoHead = tb->pUndoTail = NULL; }
}

static void PushUndo(TEXTBUF *tb, UNDOREC *r)
{
    TruncateRedo(tb);

    r->pPrev = tb->pUndoTail;
    r->pNext = NULL;
    if (tb->pUndoTail) tb->pUndoTail->pNext = r;
    else               tb->pUndoHead = r;
    tb->pUndoTail = r;
    tb->pCur = r;
    tb->nUndo++;

    /* Enforce the cap by dropping the oldest records. */
    while (tb->nUndo > TB_UNDO_MAX && tb->pUndoHead != tb->pCur)
    {
        UNDOREC *old = tb->pUndoHead;
        tb->pUndoHead = old->pNext;
        if (tb->pUndoHead) tb->pUndoHead->pPrev = NULL;
        FreeRec(old);
        tb->nUndo--;
    }
}

/* Append 'len' bytes to a record's saved text (for insert coalescing). */
static BOOL AppendUndoText(UNDOREC *r, const char *src, ULONG len)
{
    char *p = (char *)realloc(r->pText, r->len + len);
    if (!p) return FALSE;
    r->pText = p;
    memcpy(p + r->len, src, len);
    return TRUE;
}

/*====================================================================
 * Public: lifecycle
 *====================================================================*/

BOOL TbInit(TEXTBUF *tb)
{
    memset(tb, 0, sizeof(TEXTBUF));

    tb->cap  = TB_INIT_GAP;
    tb->pBuf = (char *)malloc(tb->cap);
    if (!tb->pBuf) return FALSE;
    tb->gapStart = 0;
    tb->gapEnd   = tb->cap;                 /* whole buffer is gap   */

    tb->nLineCap = TB_LINE_INIT;
    tb->pLines = (ULONG *)malloc((ULONG)tb->nLineCap * sizeof(ULONG));
    if (!tb->pLines) { free(tb->pBuf); tb->pBuf = NULL; return FALSE; }
    tb->nLines = 1;                         /* one empty line        */

    tb->pUndoHead = tb->pUndoTail = tb->pCur = NULL;
    tb->nUndo = 0;
    tb->curGroup = 0;
    tb->bCoalesce = FALSE;
    tb->bModified = FALSE;
    return TRUE;
}

static void FreeUndoAll(TEXTBUF *tb)
{
    UNDOREC *r = tb->pUndoHead;
    while (r) { UNDOREC *nx = r->pNext; FreeRec(r); r = nx; }
    tb->pUndoHead = tb->pUndoTail = tb->pCur = NULL;
    tb->nUndo = 0;
    tb->bCoalesce = FALSE;
}

void TbFree(TEXTBUF *tb)
{
    FreeUndoAll(tb);
    if (tb->pBuf)   { free(tb->pBuf);   tb->pBuf = NULL; }
    if (tb->pLines) { free(tb->pLines); tb->pLines = NULL; }
}

void TbClear(TEXTBUF *tb)
{
    FreeUndoAll(tb);
    tb->gapStart = 0;
    tb->gapEnd   = tb->cap;
    tb->nLines   = 1;
    tb->bModified = FALSE;
}

/*====================================================================
 * Public: queries
 *====================================================================*/

ULONG TbLength(TEXTBUF *tb)       { return DocLen(tb); }
LONG  TbLineCount(TEXTBUF *tb)    { return tb->nLines; }
BOOL  TbIsModified(TEXTBUF *tb)   { return tb->bModified; }
void  TbSetModified(TEXTBUF *tb, BOOL b) { tb->bModified = b; }

char TbCharAt(TEXTBUF *tb, ULONG pos)
{
    ULONG raw = (pos < tb->gapStart) ? pos : pos + GapSize(tb);
    return tb->pBuf[raw];
}

ULONG TbLineStart(TEXTBUF *tb, LONG line)
{
    if (line <= 0)            return 0;
    if (line >= tb->nLines)   return DocLen(tb);
    return RawToDoc(tb, tb->pLines[line]);
}

ULONG TbLineEnd(TEXTBUF *tb, LONG line)
{
    if (line >= tb->nLines - 1)
        return DocLen(tb);                  /* last line: to EOF     */
    return TbLineStart(tb, line + 1) - 1;   /* just before the '\n'  */
}

ULONG TbLineLength(TEXTBUF *tb, LONG line)
{
    return TbLineEnd(tb, line) - TbLineStart(tb, line);
}

LONG TbLineFromPos(TEXTBUF *tb, ULONG pos)
{
    LONG lo = 1, hi = tb->nLines - 1, res = 0;
    while (lo <= hi)
    {
        LONG mid = (lo + hi) >> 1;
        if (TbLineStart(tb, mid) <= pos) { res = mid; lo = mid + 1; }
        else                             { hi = mid - 1; }
    }
    return res;
}

void TbGetRange(TEXTBUF *tb, ULONG pos, ULONG len, char *dest)
{
    ULONG gs = GapSize(tb);

    if (len == 0) return;

    if (pos + len <= tb->gapStart)          /* wholly before gap     */
    {
        memcpy(dest, tb->pBuf + pos, len);
    }
    else if (pos >= tb->gapStart)           /* wholly after gap      */
    {
        memcpy(dest, tb->pBuf + pos + gs, len);
    }
    else                                    /* straddles the gap     */
    {
        ULONG first = tb->gapStart - pos;
        memcpy(dest, tb->pBuf + pos, first);
        memcpy(dest + first, tb->pBuf + tb->gapEnd, len - first);
    }
}

/*====================================================================
 * Public: editing
 *====================================================================*/

void TbBreakUndo(TEXTBUF *tb) { tb->bCoalesce = FALSE; }

BOOL TbInsert(TEXTBUF *tb, ULONG pos, const char *src, ULONG len,
              ULONG caretBefore, ULONG caretAfter)
{
    UNDOREC *r;

    if (len == 0) return TRUE;

    /* Coalesce a single, non-newline character onto the previous
       insert if it continues immediately after it. */
    if (tb->bCoalesce && tb->pCur == tb->pUndoTail &&
        tb->pCur && tb->pCur->type == UNDO_INSERT &&
        tb->pCur->pos + tb->pCur->len == pos &&
        len == 1 && src[0] != '\n')
    {
        if (!AppendUndoText(tb->pCur, src, len)) return FALSE;
        if (!RawInsert(tb, pos, src, len))       return FALSE;
        tb->pCur->len        += len;
        tb->pCur->caretAfter  = caretAfter;
        tb->bModified = TRUE;
        return TRUE;
    }

    r = (UNDOREC *)malloc(sizeof(UNDOREC));
    if (!r) return FALSE;
    memset(r, 0, sizeof(UNDOREC));
    r->type        = UNDO_INSERT;
    r->pos         = pos;
    r->len         = len;
    r->caretBefore = caretBefore;
    r->caretAfter  = caretAfter;
    r->group       = tb->curGroup;
    r->pText       = CopyToHeap(src, len);
    if (!r->pText) { free(r); return FALSE; }

    if (!RawInsert(tb, pos, src, len)) { FreeRec(r); return FALSE; }

    PushUndo(tb, r);
    tb->bCoalesce = TRUE;
    tb->bModified = TRUE;
    return TRUE;
}

BOOL TbDelete(TEXTBUF *tb, ULONG pos, ULONG len,
              ULONG caretBefore, ULONG caretAfter)
{
    UNDOREC *r;
    char *p;

    if (len == 0) return TRUE;

    /* Capture the doomed text so undo can put it back. */
    p = (char *)malloc(len);
    if (!p) return FALSE;
    TbGetRange(tb, pos, len, p);

    r = (UNDOREC *)malloc(sizeof(UNDOREC));
    if (!r) { free(p); return FALSE; }
    memset(r, 0, sizeof(UNDOREC));
    r->type        = UNDO_DELETE;
    r->pos         = pos;
    r->len         = len;
    r->caretBefore = caretBefore;
    r->caretAfter  = caretAfter;
    r->group       = tb->curGroup;
    r->pText       = p;

    RawDelete(tb, pos, len);

    PushUndo(tb, r);
    tb->bCoalesce = FALSE;      /* deletes don't coalesce (for now) */
    tb->bModified = TRUE;
    return TRUE;
}

/*====================================================================
 * Public: undo / redo
 *====================================================================*/

BOOL TbCanUndo(TEXTBUF *tb) { return tb->pCur != NULL; }
BOOL TbCanRedo(TEXTBUF *tb)
{
    return (tb->pCur ? tb->pCur->pNext : tb->pUndoHead) != NULL;
}

BOOL TbUndo(TEXTBUF *tb, ULONG *pCaret)
{
    UNDOREC *r = tb->pCur;
    if (!r) return FALSE;

    if (r->type == UNDO_INSERT)
        RawDelete(tb, r->pos, r->len);      /* saved text kept for redo */
    else
        RawInsert(tb, r->pos, r->pText, r->len);

    if (pCaret) *pCaret = r->caretBefore;
    tb->pCur = r->pPrev;
    tb->bCoalesce = FALSE;
    tb->bModified = TRUE;
    TbReindex(tb);              /* keep the line index authoritative */
    return TRUE;
}

BOOL TbRedo(TEXTBUF *tb, ULONG *pCaret)
{
    UNDOREC *r = tb->pCur ? tb->pCur->pNext : tb->pUndoHead;
    if (!r) return FALSE;

    if (r->type == UNDO_INSERT)
        RawInsert(tb, r->pos, r->pText, r->len);
    else
        RawDelete(tb, r->pos, r->len);

    if (pCaret) *pCaret = r->caretAfter;
    tb->pCur = r;
    tb->bCoalesce = FALSE;
    tb->bModified = TRUE;
    TbReindex(tb);              /* keep the line index authoritative */
    return TRUE;
}

/* Rebuild the logical-line index from scratch by scanning the whole
   document.  Uses the same scanner as file load, so the line table is
   guaranteed to match the byte content.  Called after undo/redo as a
   safety net against any incremental line-tracking error. */
void TbReindex(TEXTBUF *tb)
{
    MoveGap(tb, DocLen(tb));                 /* make text contiguous */
    RebuildLines(tb);
}

/*====================================================================
 * Public: file I/O
 *
 * Sharing modes are the ones a text editor wants: deny-write while
 * reading, deny-write while writing, so a second copy of the editor
 * cannot corrupt the file mid-save.
 *====================================================================*/

static void RebuildLines(TEXTBUF *tb)
{
    /* Assumes the whole document is contiguous before the gap
       (gapStart == docLen), as it is right after a load. */
    ULONG n = tb->gapStart;
    ULONG r;
    LONG  k = 1;

    tb->nLines = 1;
    for (r = 0; r < n; r++)
    {
        if (tb->pBuf[r] == '\n')
        {
            if (!EnsureLineCap(tb, k + 1)) break;
            tb->pLines[k++] = r + 1;
        }
    }
    tb->nLines = k;
}

/* Put the document back into a valid empty state.  Needed when a load
   fails AFTER the buffer has been reallocated: gapEnd would otherwise
   still point into the old capacity. */
static void ResetToEmpty(TEXTBUF *tb)
{
    FreeUndoAll(tb);
    tb->gapStart = 0;
    tb->gapEnd   = tb->cap;
    tb->nLines   = 1;
    tb->bModified = FALSE;
}

BOOL TbLoadFile(TEXTBUF *tb, const char *path, int *pErr)
{
    HFILE  hf = NULLHANDLE;
    ULONG  ulAction = 0, cbRead = 0, ulSize = 0, ulPos = 0;
    ULONG  newCap;
    char  *np;
    ULONG  i, j;

    if (pErr) *pErr = TBERR_NONE;

    if (DosOpen((PCSZ)path, &hf, &ulAction, 0L, FILE_NORMAL,
                OPEN_ACTION_OPEN_IF_EXISTS,
                OPEN_ACCESS_READONLY | OPEN_SHARE_DENYWRITE,
                NULL) != NO_ERROR)
    {
        if (pErr) *pErr = TBERR_OPEN;
        return FALSE;
    }

    if (DosSetFilePtr(hf, 0L, FILE_END, &ulSize) != NO_ERROR) ulSize = 0;
    DosSetFilePtr(hf, 0L, FILE_BEGIN, &ulPos);

    /* Reallocate the buffer to hold the file plus a fresh gap. */
    newCap = ulSize + TB_INIT_GAP;
    np = (char *)realloc(tb->pBuf, newCap);
    if (!np)
    {
        DosClose(hf);
        if (pErr) *pErr = TBERR_MEMORY;
        return FALSE;                       /* buffer untouched      */
    }
    tb->pBuf = np;
    tb->cap  = newCap;

    if (ulSize > 0)
    {
        if (DosRead(hf, tb->pBuf, ulSize, &cbRead) != NO_ERROR ||
            cbRead != ulSize)
        {
            DosClose(hf);
            ResetToEmpty(tb);
            if (pErr) *pErr = TBERR_READ;
            return FALSE;
        }
    }
    DosClose(hf);

    /* Normalize CRLF -> LF in place (dst j never overtakes src i). */
    i = 0; j = 0;
    while (i < ulSize)
    {
        char c = tb->pBuf[i];
        if (c == '\r' && i + 1 < ulSize && tb->pBuf[i + 1] == '\n')
            { i++; continue; }              /* drop the CR           */
        tb->pBuf[j++] = c;
        i++;
    }

    FreeUndoAll(tb);
    tb->gapStart = j;
    tb->gapEnd   = tb->cap;                 /* gap after the text    */
    RebuildLines(tb);
    tb->bModified = FALSE;
    return TRUE;
}

/* Write a contiguous region converting LF -> CRLF, through 'out'. */
static BOOL WriteRegion(HFILE hf, char *src, ULONG n, char *out)
{
    ULONG off = 0;
    while (off < n)
    {
        ULONG chunk = n - off;
        ULONG k, oi = 0, cbWritten = 0;
        if (chunk > TB_IOCHUNK) chunk = TB_IOCHUNK;

        for (k = 0; k < chunk; k++)
        {
            char c = src[off + k];
            if (c == '\n') out[oi++] = '\r';
            out[oi++] = c;
        }
        if (oi)
        {
            if (DosWrite(hf, out, oi, &cbWritten) != NO_ERROR ||
                cbWritten != oi)
                return FALSE;
        }
        off += chunk;
    }
    return TRUE;
}

BOOL TbSaveFile(TEXTBUF *tb, const char *path, int *pErr)
{
    HFILE  hf = NULLHANDLE;
    ULONG  ulAction = 0;
    char  *out;
    BOOL   ok = FALSE;

    if (pErr) *pErr = TBERR_NONE;

    /* out may double in size (every byte a '\n'); give it headroom. */
    out = (char *)malloc(TB_IOCHUNK * 2 + 4);
    if (!out)
    {
        if (pErr) *pErr = TBERR_MEMORY;
        return FALSE;
    }

    if (DosOpen((PCSZ)path, &hf, &ulAction, 0L, FILE_NORMAL,
                OPEN_ACTION_CREATE_IF_NEW | OPEN_ACTION_REPLACE_IF_EXISTS,
                OPEN_ACCESS_WRITEONLY | OPEN_SHARE_DENYWRITE,
                NULL) != NO_ERROR)
    {
        if (pErr) *pErr = TBERR_OPEN;
    }
    else
    {
        ok = WriteRegion(hf, tb->pBuf, tb->gapStart, out) &&
             WriteRegion(hf, tb->pBuf + tb->gapEnd,
                         tb->cap - tb->gapEnd, out);
        DosClose(hf);
        if (!ok && pErr) *pErr = TBERR_WRITE;
        if (ok) tb->bModified = FALSE;
    }

    free(out);
    return ok;
}

/*====================================================================
 * Public: search
 *====================================================================*/

BOOL TbFind(TEXTBUF *tb, ULONG startPos, const char *needle,
            BOOL bMatchCase, ULONG *pFound)
{
    ULONG n = DocLen(tb);
    ULONG m = (ULONG)strlen(needle);
    ULONG i;

    if (m == 0 || m > n) return FALSE;

    /* Make the whole document contiguous so a match can't span the
       gap and scanning is a straight pointer walk. */
    MoveGap(tb, n);                         /* gap parked at the end */

    for (i = startPos; i + m <= n; i++)
    {
        ULONG j;
        for (j = 0; j < m; j++)
        {
            char a = tb->pBuf[i + j];
            char b = needle[j];
            if (!bMatchCase)
            {
                if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
                if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
            }
            if (a != b) break;
        }
        if (j == m) { if (pFound) *pFound = i; return TRUE; }
    }
    return FALSE;
}
