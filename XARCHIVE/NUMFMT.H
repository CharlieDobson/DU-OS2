/*===========================================================================
 * NUMFMT.H  -  File sizes written the way this machine writes numbers
 * Target: MSVC 2.2 Win32s / Open Watcom DOS32A / Open Watcom OS/2 PM
 *
 * A six-figure byte count is unreadable without digit grouping, and the
 * grouping character is not the same everywhere: a comma in the UK and the
 * US, a full stop across much of Europe, a space in France and the Nordics,
 * an apostrophe in Switzerland.  Every one of the three target systems will
 * say which it uses, and all three are asked rather than assumed.
 *
 * Only the SEPARATOR is localised here, not the grouping width.  All three
 * systems group by three, and MS-DOS does so in its own message formatter -
 * see $M_FIRST_THOU / $M_SECOND_THOU / $M_THIRD_THOU in DOS6SRC\inc\sysmsg.inc,
 * which are simply 3, 6 and 9.  Scripts that group differently (the Indian
 * 2,2,3 among them) are not reachable on these systems' country tables, so
 * writing a general grouping engine would be inventing a requirement.
 *===========================================================================*/

#ifndef NUMFMT_H
#define NUMFMT_H

#include "arcdefs.h"

/* Two sizes, because the two formatters have very different worst cases and
 * one number for both would either overflow or waste a lot of memory in the
 * per-entry arrays the list views keep.
 *
 * NUM_FMT_U32_MAX  is enough for NumFmt of any UInt32: ten digits, three
 *                  separators of up to NUM_SEP_MAX-1 bytes each, terminator.
 * NUM_FMT_MAX      is enough for NumFmtD of any double %.0f will spell.
 *
 * All three systems' country tables in fact hold a SINGLE character, and the
 * DOS one is a two-byte ASCIIZ field that could in principle hold more.  The
 * three-byte allowance is for that: a buffer sized on "nobody would do that"
 * is how an overflow gets written. */
#define NUM_FMT_U32_MAX  24
#define NUM_FMT_MAX      64

#define NUM_SEP_MAX  4

/* The separator in force.  Never NULL; "" means grouping is switched off,
 * which is what a system reporting no separator is asking for.  The first
 * call asks the operating system, and the answer is kept.
 *
 * THERE IS NO OVERRIDE, by design.  There used to be two: an XARCSEP
 * environment variable, honoured by all three front ends, and a /sep= switch
 * on the DOS command line.  Neither earnt its keep.  The machine already
 * knows the answer and all three front ends ask it the same way, so a second
 * place to set it is only a second place for a listing on one system to
 * disagree with a listing on another - and with what DIR prints on either.
 * A user who wants different numbers has a country setting to change, and
 * changing it fixes DIR at the same time. */
const char *NumGroupSep( void );

/* Write v into buf with the separator in force and return buf.  buf must
 * have room for NUM_FMT_U32_MAX bytes.  Always succeeds. */
char *NumFmt( UInt32 v, char *buf );

/* The same, for a byte TOTAL.  Totals are accumulated as doubles because an
 * archive's contents can add up past what UInt32 holds even when no single
 * entry does, so the front ends already carry them that way.  Negative and
 * non-finite values render as "0" rather than as something alarming. */
char *NumFmtD( double v, char *buf );

/* As NumFmt, but right-aligned in a field 'width' wide, space-padded on the
 * left.  A number too wide for the field is NOT truncated - it is returned
 * in full and the column grows, because a wrong size is worse than a ragged
 * column.  buf must have room for width+1 bytes, or NUM_FMT_MAX, whichever
 * is larger. */
char *NumFmtRight( UInt32 v, char *buf, int width );

#endif /* NUMFMT_H */
