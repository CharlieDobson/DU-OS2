/*===========================================================================
 * LZXDEC.H  -  LZX decoder, in the form cabinets use it
 * Target: MSVC 2.2  Win32s, and the Open Watcom DOS/32A and OS/2 builds
 *
 * LZX is the method of Microsoft's later cabinets: a sliding window of 32 KB
 * to 2 MB (2^15 .. 2^21), Huffman-coded literals and matches with three
 * repeated-offset slots, three block types, and an optional rewrite of x86
 * CALL targets (the "E8 translation") that makes executables pack better.
 *
 * A cabinet folder is one LZX stream cut into frames of 32 KB of output, and
 * each CFDATA block holds exactly one frame.  The decoder keeps everything
 * that crosses a frame - the window, the trees, the repeated offsets, a block
 * still in progress - and the caller hands it one frame's compressed bytes
 * at a time, in order.  This is how Microsoft's own FDI drives it.
 *
 * Create one decoder per folder; a folder always starts a fresh stream.
 *===========================================================================*/

#ifndef LZXDEC_H
#define LZXDEC_H

#include "arcdefs.h"

#define LZX_MIN_WINDOW  15
#define LZX_MAX_WINDOW  21
#define LZX_FRAME_SIZE  32768U

typedef struct LzxDec LzxDec;

/* SZ_OK, SZ_ERR_MEMORY, or SZ_ERR_UNSUPPORTED for a window outside 15..21. */
int  LzxCreate( int windowBits, LzxDec **out );
void LzxFree( LzxDec *dec );

/* Decode one frame: 'in' is the block's compressed bytes and exactly outLen
 * bytes (at most LZX_FRAME_SIZE) are produced into 'out'.  SZ_ERR_DATA when
 * the bits do not describe that many bytes. */
int  LzxDecodeFrame( LzxDec *dec, const Byte *in, UInt32 inLen,
                     Byte *out, UInt32 outLen );

/* Bytes a decoder with this window holds, for ArcMemNeeded. */
UInt32 LzxMemNeeded( int windowBits );

#endif /* LZXDEC_H */
