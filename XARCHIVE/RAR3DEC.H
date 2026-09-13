/*===========================================================================
 * RAR3DEC.H  -  RAR 3.x (unpack version 3) LZ+Huffman decoder
 * Target: MSVC 2.2  Win32s
 *
 * Decodes the RAR2 (unpack v20) and RAR3 (unpack v29/36) LZ compression paths.
 * It does NOT implement PPMd (model H) blocks, the RAR3 filter virtual machine
 * (symbol 257), or RAR2 audio blocks; those return SZ_ERR_UNSUPPORTED.
 *
 * The decoder is adapted from the UnRAR source by Alexander L. Roshal.  The
 * UnRAR sources may be used freely to handle RAR archives, but MAY NOT be used
 * to develop a RAR (WinRAR) compatible archiver (see RAR3DEC.C for the full
 * notice).  The ring-window streaming and Rar2Ctx/Rar3Ctx API are original.
 *===========================================================================*/

#ifndef RAR3DEC_H
#define RAR3DEC_H

#include "arcdefs.h"

/*
 * Rar3Decode - decompress one RAR3 (unpack v3) stream, non-solid.
 * Rar2Decode - decompress one RAR2 (unpack v20) stream, non-solid.
 *   src/srcLen - the packed data
 *   dst/dstLen - output buffer, exactly dstLen bytes (the known unpacked size)
 * Returns SZ_OK, or SZ_ERR_UNSUPPORTED (v3 PPMd/filter, or v2 audio block),
 * SZ_ERR_DATA, SZ_ERR_MEMORY.
 */
int Rar3Decode( const Byte *src, UInt32 srcLen, Byte *dst, UInt32 dstLen );
int Rar2Decode( const Byte *src, UInt32 srcLen, Byte *dst, UInt32 dstLen );

/*
 * Stateful RAR2 decoder with a bounded ring window, for SOLID archives (and
 * used by the non-solid wrapper too).  Output is delivered one byte at a time
 * to an emit callback so the caller can stream to disk; the internal ring
 * window (a few MB) supplies match look-back, so memory does not grow with the
 * archive size.  Tables / offset history / window position persist across
 * files, while each file provides a fresh bit stream (Rar2SetInput).
 *
 * emit returns SZ_OK to continue or an SZ_ERR_* code to abort the decode.
 */
typedef int (*Rar2Emit)( void *user, Byte b );

typedef struct Rar2Ctx Rar2Ctx;
Rar2Ctx *Rar2Create( void );
Rar2Ctx *Rar2CreateSized( UInt32 winSize );
void     Rar2Free( Rar2Ctx *c );
void     Rar2SetInput( Rar2Ctx *c, const Byte *src, UInt32 srcLen );
void     Rar2Align( Rar2Ctx *c );
int      Rar2ReadTables( Rar2Ctx *c );

/* Decode until the absolute output position reaches endPos, emitting each
 * output byte.  Also used to push stored (uncompressed) bytes into the window
 * via Rar2Feed. */
int      Rar2Decode2( Rar2Ctx *c, Rar2Emit emit, void *user, UInt32 endPos );
int      Rar2Feed( Rar2Ctx *c, Rar2Emit emit, void *user, const Byte *data, UInt32 len );

/*
 * Stateful RAR3 (unpack v3) decoder - same streaming/ring-window contract as
 * the RAR2 context above, and used both by the non-solid Rar3Decode wrapper
 * and by SOLID extraction.  Tables / LZ offset history / window position
 * persist across files; each file provides a fresh bit stream (Rar3SetInput).
 * PPMd blocks and the filter VM return SZ_ERR_UNSUPPORTED.  Reuses Rar2Emit.
 */
typedef struct Rar3Ctx Rar3Ctx;
Rar3Ctx *Rar3Create( void );
Rar3Ctx *Rar3CreateSized( UInt32 winSize );
void     Rar3Free( Rar3Ctx *c );
void     Rar3SetInput( Rar3Ctx *c, const Byte *src, UInt32 srcLen );
int      Rar3ReadTables( Rar3Ctx *c );
int      Rar3Decode2( Rar3Ctx *c, Rar2Emit emit, void *user, UInt32 endPos );
int      Rar3Feed( Rar3Ctx *c, Rar2Emit emit, void *user, const Byte *data, UInt32 len );

/* Solid chains only.  Call Rar3EndOfFile after decoding each member to
 * consume the end-of-block trailer that closes it, then Rar3NeedTables to
 * ask whether the next member starts with fresh tables.  Skipping this
 * decodes the next member with stale tables - see RAR3DEC.C for why that is
 * silent rather than an error.  There is deliberately no RAR2 equivalent. */
void     Rar3EndOfFile( Rar3Ctx *c );
int      Rar3NeedTables( Rar3Ctx *c );

/*---- THE WINDOW IS THE ARCHIVE'S, NOT THE FORMAT'S ------------------------ *
 * Rar2Create / Rar3Create / Rar2Decode / Rar3Decode allocate RAR's LARGEST
 * possible window - 4 MB - because that is all a caller with no header in
 * hand can safely assume.  Every one of them costs four megabytes to unpack a
 * file that was compressed with -md64, and that single allocation was the
 * only reason a RAR of any size failed to open on a small machine while a 7z
 * with a 64 KB dictionary opened fine.
 *
 * A caller that HAS the file header knows better: RAR stores the dictionary
 * in bits 5-7 of the header flags as 64 KB << n (see RarWinSize in RARARC.C).
 * The ...Sized forms take that figure, round it up to a power of two and
 * clamp it to [64 KB, 4 MB].  Passing 0 means "no idea", which gives the 4 MB
 * maximum and the old behaviour.
 *
 * An UNDERSIZED window is safe to get wrong: the decoders reject any match
 * whose distance exceeds wmask + 1, so a window smaller than the stream needs
 * fails as SZ_ERR_DATA rather than quietly emitting wrong bytes.
 *-------------------------------------------------------------------------- */
/* What ...Sized will really allocate for a given request: the rounded,
 * clamped figure.  Exported so a memory estimate (RarMemNeeded) reports the
 * allocation rather than a second guess at it. */
UInt32   RarWindowFor( UInt32 want );

int      Rar2DecodeSized( const Byte *src, UInt32 srcLen, Byte *dst,
                          UInt32 dstLen, UInt32 winSize );
int      Rar3DecodeSized( const Byte *src, UInt32 srcLen, Byte *dst,
                          UInt32 dstLen, UInt32 winSize );

#endif /* RAR3DEC_H */
