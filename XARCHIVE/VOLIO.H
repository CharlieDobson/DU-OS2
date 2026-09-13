/*===========================================================================
 * VOLIO.H  -  N split volumes presented as one seekable stream
 * Target: MSVC 2.2  Win32s
 *
 * For the formats whose volumes are a PURE BYTE SPLIT of a single archive -
 * 7z's .7z.001/.002/... and the .zip.001/... that 7-Zip writes - the whole
 * set concatenates back into a valid archive.  Only the first volume carries
 * the signature; the rest are raw continuations.  So the parsers need no new
 * format knowledge at all: they only need an I/O layer that hides the joins.
 *
 * RAR IS NOT LIKE THIS and must not be routed through here.  Every RAR volume
 * is a complete little archive with its own marker and main header, and an
 * entry that spans volumes is split with continuation flags - concatenating
 * them produces rubbish.  RARARC / RAR5ARC walk the volumes themselves.
 *
 * The call shapes deliberately mirror stdio (VolRead takes fread's argument
 * order) so a parser converts by swapping names, not by restructuring.
 *
 * ONE FILE HANDLE is held open however many volumes there are: DOS gives a
 * process very few, and a 40-volume set must not eat them.  Crossing a volume
 * boundary closes one and opens the next, which is cheap because reads are
 * overwhelmingly sequential within a volume.
 *===========================================================================*/

#ifndef VOLIO_H
#define VOLIO_H

#include "arcdefs.h"
#include <stdio.h>

typedef struct VolFile VolFile;

/* Open path as a stream.  If it names one volume of a .NNN set the whole set
 * is joined, starting from .001 even when a later volume was the one named;
 * anything else opens as itself and behaves exactly as the single file did.
 * SZ_OK, or SZ_ERR_OPEN when the named file cannot be read. */
int    VolOpen( const char *path, VolFile **out );
void   VolClose( VolFile *v );

/* fread's shape and meaning: returns the count of whole items read.  Reads
 * run across volume joins transparently. */
UInt32 VolRead( void *buf, UInt32 size, UInt32 count, VolFile *v );

/* SEEK_SET / SEEK_CUR / SEEK_END over the joined stream.  0 on success. */
int    VolSeek( VolFile *v, long off, int whence );
long   VolTell( VolFile *v );

/* Total bytes across every volume found. */
long   VolSize( VolFile *v );

/* How many volumes were joined.  1 means an ordinary single file - callers
 * use this to decide whether a short read is a TRUNCATED archive or a MISSING
 * VOLUME, which are different things to tell the user. */
int    VolCount( VolFile *v );

/* RAR volume names.  VolRarFirstName rewinds any volume of a set to its first
 * (part7 -> part1, .r03 -> .rar); VolRarNextName steps one on.  Both return 0
 * when there is no such name.  See the block comment in VOLIO.C for the two
 * naming schemes and why the new one must be tested first. */
int    VolRarFirstName( const char *cur, char *buf, int buflen );
int    VolRarNextName( const char *cur, char *buf, int buflen );

/* Open an EXPLICIT list of files as one joined stream, in the order given.
 *
 * This is for RAR, whose volumes are NOT a byte split and so cannot be read
 * as one archive - but which still wants the handle management: a RAR set can
 * run to dozens of volumes and DOS hands out very few file handles, so the
 * same one-open-handle discipline applies.  RARARC parses each volume on its
 * own and addresses data by (volume, offset), which VolVolumeStart turns into
 * a position in this stream.
 *
 * The paths are copied; the caller keeps ownership of its array. */
int    VolOpenList( const char *const *paths, int n, VolFile **out );

/* Logical offset at which volume i begins, and its length. */
long   VolVolumeStart( VolFile *v, int i );
long   VolVolumeLen( VolFile *v, int i );

/* fgetc over the joined stream: EOF (-1) at the end, and across a join the
 * next volume's first byte rather than an EOF.  The zip decoders pull a byte
 * at a time, so this is the hot path for them. */
int    VolGetc( VolFile *v );

/* Does the NAME look like one volume of a split set (ends .001, .002, ...)?
 * Separate from VolCount because the two answer different questions: a user
 * holding only foo.7z.001 of six has a set of ONE joined volume but is quite
 * plainly missing the rest, and telling them the archive is corrupt would have
 * them throw away a perfectly good first sixth. */
int    VolIsSet( VolFile *v );

/* The first volume's path, for messages. */
const char *VolPath( VolFile *v );

#endif /* VOLIO_H */
