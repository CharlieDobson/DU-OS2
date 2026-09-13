/*===========================================================================
 * ARCFILE.H  -  Format-agnostic archive front door
 * Target: MSVC 2.2  Win32s
 *
 * Sniffs an archive's signature and dispatches to the 7z (SZARC) or zip
 * (ZIPARC) backend, presenting one open/list/extract interface so the GUI
 * does not care which format it is looking at.  Result codes and the progress
 * callback are the shared SZ_ERR_* / SzProgress.
 *===========================================================================*/

#ifndef ARCFILE_H
#define ARCFILE_H

#include "arcdefs.h"
#include "szarc.h"      /* SzProgress */

typedef struct ArcFile ArcFile;

/* Open and parse; *out must be released with ArcClose().  SZ_OK or SZ_ERR_*. */
int  ArcOpen( const char *path, ArcFile **out );

/*---- Encrypted archives ---------------------------------------------------- *
 * Four formats here carry encryption XArchive can read: 7z (AES-256), zip
 * (PKWARE ZipCrypto, and WinZip AES at 128/256), RAR3/RAR4 (AES-128) and RAR5
 * (AES-256).  A disk image has no such concept.
 *
 * There are two moments a password can be needed, and a front end has to
 * handle both:
 *
 *   AT OPEN, when the archive's own DIRECTORY is encrypted - a 7z made with
 *   -mhe=on, or a RAR (either generation) made with -hp.  The names themselves
 *   are inside the encrypted stream, so ArcOpen returns SZ_ERR_PASSWORD and
 *   the archive cannot even be listed.  Prompt, then call ArcOpenPw.
 *
 *   AT EXTRACT.  The usual case: the archive lists fine and the entry data is
 *   encrypted.  ArcNeedsPassword answers straight after opening, so the prompt
 *   can come before the user picks anything; a front end that would rather
 *   prompt lazily can instead extract and react to SZ_ERR_PASSWORD.
 *
 * SZ_ERR_BADPASS means a password was supplied and is wrong - re-prompt.  The
 * distinction from SZ_ERR_PASSWORD is what lets the second prompt say "that
 * one was wrong" instead of asking again as though nothing had happened.
 *--------------------------------------------------------------------------*/
int  ArcOpenPw( const char *path, const char *pw, ArcFile **out );

/* Set (or with NULL / "" clear) the password for subsequent operations. */
void ArcSetPassword( ArcFile *a, const char *pw );

/* Does this archive contain anything encrypted? */
int  ArcNeedsPassword( ArcFile *a );

int  ArcNumEntries( ArcFile *a );
const char *ArcEntryName( ArcFile *a, int index );
UInt32      ArcEntrySize( ArcFile *a, int index );
int         ArcEntryIsDir( ArcFile *a, int index );

/* Extra columns for the list view.  ArcEntryPacked returns 0xFFFFFFFF when the
 * packed size is unknown; ArcEntryDate / ArcEntryAttr write "" when the field
 * is absent. */
UInt32      ArcEntryPacked( ArcFile *a, int index );
const char *ArcEntryMethod( ArcFile *a, int index );
void        ArcEntryDate( ArcFile *a, int index, char *buf, int buflen );
void        ArcEntryAttr( ArcFile *a, int index, char *buf, int buflen );

int  ArcExtractAll( ArcFile *a, const char *destDir,
                    SzProgress prog, void *user );
int  ArcExtractItems( ArcFile *a, const int *indices, int count,
                      const char *destDir, SzProgress prog, void *user );

/* Integrity test: decode and CRC-check every entry without writing any files.
 * Returns SZ_OK if all entries verify, SZ_ERR_CRC / SZ_ERR_DATA on corruption,
 * SZ_ERR_UNSUPPORTED if an entry uses a feature we cannot decode, or
 * SZ_ERR_CANCEL.  Uses the same progress callback as extraction. */
int  ArcTestAll( ArcFile *a, SzProgress prog, void *user );

/* The archive's own comment, or NULL when it has none.  Owned by the archive and
 * valid until ArcClose.  Only zip and RAR4 have a field for one; the rest always
 * answer NULL, which a front end should report as "no comment" and not as a
 * failure. */
const char *ArcComment( ArcFile *a );

/* The same comment with every line ending written as CRLF.
 *
 * Both GUI ports need this and neither can show ArcComment's raw bytes: a
 * Windows multi-line edit control and an OS/2 MLE each break lines on CRLF
 * and run a lone LF straight on, which turns a twenty-line comment into one
 * very long one.  Archives carry whichever ending the writing tool used, and
 * a comment typed on a DOS machine, a Unix machine and a Mac can all three
 * end up in the same archive.  The DOS TUI needs none of this because its own
 * text viewer already handles all three.
 *
 * Returns the bytes needed INCLUDING the terminator, so a caller can ask with
 * buf == NULL to size a buffer and then ask again.  Copies at most bufLen
 * bytes, always terminating when bufLen > 0.  Returns 0 when there is no
 * comment. */
UInt32 ArcCommentText( ArcFile *a, char *buf, UInt32 bufLen );

void ArcClose( ArcFile *a );
const char *ArcErrorText( int code );

/* When an operation on this archive returns SZ_ERR_UNSUPPORTED, this returns a
 * human-readable explanation tailored to the archive's format: which features
 * ARE handled and which are not.  Safe to call with a NULL archive. */
const char *ArcUnsupportedHint( ArcFile *a );

/* Explanation for SZ_ERR_NORAM: the archive needs more memory in one block
 * than this machine can spare - usually a compression dictionary, sometimes
 * a whole-stream buffer.  Unlike SZ_ERR_MEMORY (an allocation that was tried
 * and failed) this one is a refusal made before trying, but both mean the
 * same thing to the user and both should lead here.  The text quotes the
 * measured limit (ArcMemLimitMB), so it is rebuilt on each call and the
 * returned buffer is only good until the next one. */
const char *ArcNoRamHint( void );

/* Explanation for SZ_ERR_MEMORY: an allocation was attempted and the system
 * refused it, even though it was inside the budget.  Deliberately separate
 * from ArcNoRamHint, because the advice differs - here the archive is not
 * necessarily too demanding, the machine is just busier now than it was when
 * the budget was measured.  Same static-buffer caveat. */
const char *ArcMemFailHint( void );

/*---- Will this archive fit in this machine? ------------------------------- *
 * The answer to "how much RAM does XArchive need" is that there is no such
 * number, and that is the finding rather than a dodge.  A zip costs 44 KB
 * whether it holds two files or twenty thousand.  A .7z costs whatever its
 * largest folder declares, which is anything from 64 KB to 64 MB for two
 * archives that look identical in a listing.  A RAR costs its window - now
 * read from the archive rather than assumed to be RAR's 4 MB maximum - plus,
 * when it is not solid, one whole member held twice.
 *
 * The spread WITHIN a format is wider than the spread between formats, so a
 * per-format rule would be wrong in both directions at once.  These weigh an
 * OPEN archive, where the headers have been read and each backend can answer
 * from what its own decoder will really allocate.
 *
 * ArcMemNeeded  peak bytes one extraction will hold at once; 0 when there is
 *               nothing to say (a mounted disk image is already in memory).
 * ArcMemCheck   SZ_OK, or SZ_ERR_NORAM with both figures in KB so the message
 *               can name them.  Weighed against ArcMemLargest - the
 *               measurement - and NOT against the budget, whose floor exists
 *               precisely to be generous and would defeat the check on the
 *               machines it was written for.  Either pointer may be NULL.
 *
 * Call it before starting an extraction, not at startup: at startup nothing
 * is known.  The one thing that CAN be settled at startup is whether the
 * machine can extract anything at all -
 *
 * ArcMemStartupOk  1 if the machine can manage the cheapest extraction there
 *                  is - a zip - else 0, with
 *                  both figures in KB.  A front end should say so once and
 *                  carry on rather than refuse to run: listing costs a few KB
 *                  per entry and still works, and being able to see inside a
 *                  file you cannot unpack is worth something.
 *-------------------------------------------------------------------------- */
UInt32 ArcMemNeeded( ArcFile *a );
int    ArcMemCheck( ArcFile *a, UInt32 *needKB, UInt32 *haveKB );
int    ArcMemStartupOk( UInt32 *haveKB, UInt32 *needKB );

/* The wording for that warning, with both figures in it, so the three front
 * ends cannot quote different numbers at the user.  It stops short of naming
 * a command to run - that differs per front end - so the caller appends its
 * own closing sentence.  Static buffer: good until the next call. */
const char *ArcMemWarnText( void );

/* How many volumes the archive is spread over: 1 for an ordinary single file,
 * N for a multi-volume set that was joined at open.  A front end can show it
 * in Archive Info so "why is this archive in six files" has a visible answer.
 * Safe to call with a NULL archive (returns 1). */
int ArcVolumeCount( ArcFile *a );

/* Short display name of the archive's format, e.g. "7z", "Zip", "RAR", "RAR5".
 * Safe to call with a NULL archive (returns "?"). */
const char *ArcFormatName( ArcFile *a );

#endif /* ARCFILE_H */
