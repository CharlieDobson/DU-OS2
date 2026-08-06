/*====================================================================
 * GAPBUF.H -- Large-file text engine for Enhanced Notepad (OS/2 PM)
 *
 * A gap-buffer document store with a logical-line index and a full
 * undo/redo stack.  Knows nothing about pixels, fonts, or word wrap;
 * the edit control (EDITCTL.C) layers display on top of this.
 *
 * Capacities:
 *   - Document limited only by available memory (offsets are ULONG, so
 *     up to ~4 GB in principle).
 *   - Any single logical line may be arbitrarily long.
 *
 * Coordinates:
 *   A "document position" is a 0-based ULONG offset into the logical
 *   text, 0 .. TbLength().  Internally the line index stores *raw*
 *   buffer offsets (positions in the physical buffer, which contains
 *   the gap); callers never see raw offsets.
 *
 * File I/O is DosOpen / DosRead / DosWrite.
 *
 * Compiler: Open Watcom 1.9  (wcc386 -bt=os2)
 *====================================================================*/

#ifndef GAPBUF_H
#define GAPBUF_H

#include <os2.h>

/*--------------------------------------------------------------------
 * Undo record.  One user-visible edit (possibly several coalesced
 * keystrokes) is stored as a single record.  Records form a doubly
 * linked list; pCur marks the boundary between "done" (undoable) and
 * "undone" (redoable) edits.
 *------------------------------------------------------------------*/
#define UNDO_INSERT   1     /* text was inserted at pos           */
#define UNDO_DELETE   2     /* text was deleted at pos            */

typedef struct tagUNDOREC
{
    USHORT  type;           /* UNDO_INSERT | UNDO_DELETE          */
    ULONG   pos;            /* document position of the edit      */
    ULONG   len;            /* length of the affected text        */
    char   *pText;          /* saved copy of the affected text    */
    ULONG   caretBefore;    /* caret position before the edit     */
    ULONG   caretAfter;     /* caret position after the edit      */
    USHORT  group;          /* coalescing group id                */
    struct tagUNDOREC *pPrev;
    struct tagUNDOREC *pNext;
} UNDOREC;

/*--------------------------------------------------------------------
 * The document object.
 *------------------------------------------------------------------*/
typedef struct
{
    /* ---- gap buffer ---- */
    char   *pBuf;           /* text buffer base pointer           */
    ULONG   cap;            /* total capacity in bytes            */
    ULONG   gapStart;       /* raw offset where the gap begins    */
    ULONG   gapEnd;         /* raw offset of first byte past gap  */

    /* ---- logical-line index (raw offsets, ascending) ---- */
    ULONG  *pLines;         /* raw offset of each line's start    */
    LONG    nLines;         /* number of logical lines (>= 1)     */
    LONG    nLineCap;       /* entries allocated in pLines        */

    /* ---- undo / redo ---- */
    UNDOREC    *pUndoHead;  /* oldest record                      */
    UNDOREC    *pUndoTail;  /* newest record                      */
    UNDOREC    *pCur;       /* last *applied* record; NULL = none */
    LONG        nUndo;      /* record count (for capping)         */
    USHORT      curGroup;   /* group id counter                   */
    BOOL        bCoalesce;  /* may the next insert merge?         */

    BOOL        bModified;  /* dirty flag                         */
} TEXTBUF;

/*--------------------------------------------------------------------
 * Lifecycle
 *------------------------------------------------------------------*/
BOOL  TbInit  (TEXTBUF *tb);          /* empty 1-line document      */
void  TbFree  (TEXTBUF *tb);          /* release all memory         */
void  TbClear (TEXTBUF *tb);          /* reset to empty, keep alloc */

/*--------------------------------------------------------------------
 * Queries
 *------------------------------------------------------------------*/
ULONG TbLength   (TEXTBUF *tb);                 /* char count       */
char  TbCharAt   (TEXTBUF *tb, ULONG pos);      /* char at pos      */
LONG  TbLineCount(TEXTBUF *tb);                 /* logical lines    */
LONG  TbLineFromPos(TEXTBUF *tb, ULONG pos);    /* line holding pos */
ULONG TbLineStart(TEXTBUF *tb, LONG line);      /* doc pos of start */
ULONG TbLineEnd  (TEXTBUF *tb, LONG line);      /* doc pos of EOL   */
ULONG TbLineLength(TEXTBUF *tb, LONG line);     /* excl. newline    */

/* Copy [pos, pos+len) into dest (a buffer supplied by caller). */
void  TbGetRange (TEXTBUF *tb, ULONG pos, ULONG len, char *dest);

/*--------------------------------------------------------------------
 * Editing (all record undo automatically)
 *   caret* are the pre/post caret positions the control wants
 *   restored on undo/redo; pass the control's current caret.
 *------------------------------------------------------------------*/
BOOL  TbInsert(TEXTBUF *tb, ULONG pos, const char *src, ULONG len,
               ULONG caretBefore, ULONG caretAfter);
BOOL  TbDelete(TEXTBUF *tb, ULONG pos, ULONG len,
               ULONG caretBefore, ULONG caretAfter);

/* Break undo coalescing (call on caret move, focus change, etc.). */
void  TbBreakUndo(TEXTBUF *tb);

/*--------------------------------------------------------------------
 * Undo / redo.  Return TRUE and write the caret position to restore
 * into *pCaret; FALSE if nothing to undo/redo.
 *------------------------------------------------------------------*/
BOOL  TbCanUndo(TEXTBUF *tb);
BOOL  TbCanRedo(TEXTBUF *tb);
BOOL  TbUndo   (TEXTBUF *tb, ULONG *pCaret);
BOOL  TbRedo   (TEXTBUF *tb, ULONG *pCaret);

/* Rebuild the line index from the document bytes (load-time scanner). */
void  TbReindex(TEXTBUF *tb);

/*--------------------------------------------------------------------
 * File I/O (returns FALSE on error; sets *pErr to a code below).
 *------------------------------------------------------------------*/
#define TBERR_NONE    0
#define TBERR_OPEN    1     /* cannot open file                   */
#define TBERR_MEMORY  2     /* out of memory                      */
#define TBERR_READ    3     /* read error                         */
#define TBERR_WRITE   4     /* write error                        */

BOOL  TbLoadFile(TEXTBUF *tb, const char *path, int *pErr);
BOOL  TbSaveFile(TEXTBUF *tb, const char *path, int *pErr);

/*--------------------------------------------------------------------
 * Search.  Scans forward from startPos for needle.  On a hit writes
 * the match position to *pFound and returns TRUE.
 *------------------------------------------------------------------*/
BOOL  TbFind(TEXTBUF *tb, ULONG startPos, const char *needle,
             BOOL bMatchCase, ULONG *pFound);

/*--------------------------------------------------------------------
 * Dirty flag
 *------------------------------------------------------------------*/
BOOL  TbIsModified (TEXTBUF *tb);
void  TbSetModified(TEXTBUF *tb, BOOL bMod);

#endif /* GAPBUF_H */
