/*===========================================================================
 * QTMDEC.H  -  Quantum decoder, in the form cabinets use it
 * Target: MSVC 2.2  Win32s, and the Open Watcom DOS/32A and OS/2 builds
 *
 * Quantum is David Stafford's compressor, which Microsoft licensed for some
 * of its mid-1990s cabinets before LZX replaced it.  It is an LZ77 window of
 * 1 KB to 2 MB (2^10 .. 2^21) whose literals, match lengths and match
 * positions are all coded ARITHMETICALLY, through adaptive frequency models,
 * rather than with Huffman codes.  Microsoft never published the format; it
 * is known from Matthew Russotto's reverse-engineering notes.
 *
 * As with LZX, a cabinet folder is one stream cut into 32 KB frames, one per
 * CFDATA block.  The window and the models carry across frames; the
 * arithmetic coder restarts at the start of each.  Create one decoder per
 * folder and hand it the frames in order.
 *===========================================================================*/

#ifndef QTMDEC_H
#define QTMDEC_H

#include "arcdefs.h"

#define QTM_MIN_WINDOW  10
#define QTM_MAX_WINDOW  21
#define QTM_FRAME_SIZE  32768U

typedef struct QtmDec QtmDec;

/* SZ_OK, SZ_ERR_MEMORY, or SZ_ERR_UNSUPPORTED for a window outside 10..21. */
int  QtmCreate( int windowBits, QtmDec **out );
void QtmFree( QtmDec *dec );

/* Decode one frame: exactly outLen bytes (at most QTM_FRAME_SIZE) from the
 * block's compressed bytes.  SZ_ERR_DATA when they do not describe that. */
int  QtmDecodeFrame( QtmDec *dec, const Byte *in, UInt32 inLen,
                     Byte *out, UInt32 outLen );

/* Bytes a decoder with this window holds, for ArcMemNeeded. */
UInt32 QtmMemNeeded( int windowBits );

#endif /* QTMDEC_H */
