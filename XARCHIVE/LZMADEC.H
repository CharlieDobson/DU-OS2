/*===========================================================================
 * LZMADEC.H  -  LZMA1 and LZMA2 decoders (buffered and streaming)
 * Target: MSVC 2.2  Win32s
 *
 * Implements the LZMA1 codec (used by .7z folders with the LZMA coder, id
 * 03 01 01) and the LZMA2 chunked container (coder id 21).
 *
 * Two ways to drive it:
 *
 *   - BUFFERED (LzmaDecode / Lzma2Decode): the whole uncompressed stream is
 *     decoded into the caller's dst buffer, which also serves as the match
 *     dictionary.  Simple, but memory is the size of the output - used for the
 *     small encoded archive header.
 *
 *   - STREAMING (LzmaDecodeStream / Lzma2DecodeStream): compressed bytes are
 *     pulled through a read callback and decoded bytes are pushed to an emit
 *     callback as they are produced.  The dictionary is a ring buffer of the
 *     coder's declared dictionary size, so memory is bounded by the DICTIONARY
 *     rather than by the size of the archive's contents - a 500 MB folder
 *     decodes in the same few megabytes as a 5 MB one.
 *===========================================================================*/

#ifndef LZMADEC_H
#define LZMADEC_H

#include "arcdefs.h"

/* Sink for streamed output: receives each run of decoded bytes in order.
 * Return 0 to abort the decode (the decoder then returns SZ_ERR_CANCEL). */
typedef int (*LzmaEmit)( void *user, const Byte *data, UInt32 len );

/* Source for streamed input: fill buf with up to len compressed bytes and
 * return how many were read; 0 means end of data (treated as truncation). */
typedef UInt32 (*LzmaRead)( void *user, Byte *buf, UInt32 len );

/*
 * LzmaDecode - decompress one LZMA1 stream into a buffer.
 *
 * props    - 5-byte LZMA properties (lclppb byte + 4-byte little-endian
 *            dictionary size, as stored in the 7z coder attributes)
 * propsSize- must be 5
 * src      - compressed bytes for this stream
 * srcLen   - number of compressed bytes
 * dst      - output buffer, exactly dstLen bytes
 * dstLen   - known uncompressed size of the stream
 *
 * Returns SZ_OK, or SZ_ERR_UNSUPPORTED / SZ_ERR_MEMORY / SZ_ERR_DATA.
 */
int LzmaDecode( const Byte *props, UInt32 propsSize,
                const Byte *src, UInt32 srcLen,
                Byte *dst, UInt32 dstLen );

/*
 * Lzma2Decode - decompress one LZMA2 stream into a buffer.
 *
 * dictProp - the single LZMA2 property byte (dictionary-size code).  The
 *            dictionary is the dst buffer, so this is informational only;
 *            the caller is expected to have validated dstLen against its cap.
 * src      - compressed LZMA2 chunk stream
 * srcLen   - number of compressed bytes
 * dst      - output buffer, exactly dstLen bytes
 * dstLen   - known total uncompressed size
 *
 * Returns SZ_OK, or SZ_ERR_UNSUPPORTED / SZ_ERR_MEMORY / SZ_ERR_DATA.
 */
int Lzma2Decode( Byte dictProp,
                 const Byte *src, UInt32 srcLen,
                 Byte *dst, UInt32 dstLen );

/*
 * LzmaDecodeStream / Lzma2DecodeStream - the same two codecs, reading through
 * 'read' and writing through 'emit'.
 *
 * packSize   - number of compressed bytes in this stream (the read callback is
 *              never asked for more than this in total)
 * unpackSize - known total uncompressed size
 * dictSize   - dictionary size declared by the coder; the ring buffer is this
 *              big, or unpackSize when that is smaller
 *
 * Returns SZ_OK, or SZ_ERR_UNSUPPORTED / SZ_ERR_MEMORY / SZ_ERR_DATA /
 * SZ_ERR_CANCEL (the emit callback asked to stop).
 */
int LzmaDecodeStream( const Byte *props, UInt32 propsSize,
                      LzmaRead read, void *readUser, UInt32 packSize,
                      UInt32 unpackSize, UInt32 dictSize,
                      LzmaEmit emit, void *emitUser );

int Lzma2DecodeStream( Byte dictProp,
                       LzmaRead read, void *readUser, UInt32 packSize,
                       UInt32 unpackSize, UInt32 dictSize,
                       LzmaEmit emit, void *emitUser );

#endif /* LZMADEC_H */
