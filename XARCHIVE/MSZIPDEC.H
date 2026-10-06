/*===========================================================================
 * MSZIPDEC.H  -  MSZIP block decoder (deflate with a carried-over window)
 * Target: MSVC 2.2  Win32s, and the Open Watcom DOS/32A and OS/2 builds
 *
 * MSZIP is Microsoft's packaging of deflate (RFC 1951) for cabinets and for
 * COMPRESS.EXE's KWAJ files.  The data comes in blocks of at most 32 KB of
 * output.  Each block is the two bytes "CK" followed by a COMPLETE deflate
 * stream - it ends with a final block - but the stream is compressed with
 * the previous blocks' output as its dictionary, so back-references may
 * reach into earlier blocks.  That carried-over history is the one thing a
 * plain inflater does not do, and why this is a decoder object rather than a
 * function: create one per folder (or per KWAJ file), feed it the blocks in
 * order, free it at the end.
 *
 * Works from memory buffers on both sides, so the caller decides where the
 * compressed bytes come from - a cabinet's CFDATA blocks or KWAJ's
 * length-prefixed chunks.
 *===========================================================================*/

#ifndef MSZIPDEC_H
#define MSZIPDEC_H

#include "arcdefs.h"

#define MSZIP_BLOCK_MAX  32768U     /* most one block can produce */

typedef struct MszipDec MszipDec;

/* SZ_OK or SZ_ERR_MEMORY. */
int  MszipCreate( MszipDec **out );
void MszipFree( MszipDec *dec );

/* Decode one block.  'in' starts at the "CK" signature.  Up to outCap bytes
 * (MSZIP_BLOCK_MAX is always enough) go to 'out' and *outLen says how many.
 * *inUsed, when not NULL, is how many input bytes the block took, signature
 * included, rounded up to a whole byte - for a caller that reads blocks out
 * of one continuous stream rather than from separately sized records.
 * SZ_ERR_DATA for anything malformed: a missing signature, a bad code, a
 * distance reaching back past the start of the data, or a stream that needs
 * more bytes than the block holds. */
int  MszipDecodeBlock( MszipDec *dec, const Byte *in, UInt32 inLen,
                       Byte *out, UInt32 outCap, UInt32 *outLen,
                       UInt32 *inUsed );

/* Bytes one decoder holds, for ArcMemNeeded. */
UInt32 MszipMemNeeded( void );

#endif /* MSZIPDEC_H */
