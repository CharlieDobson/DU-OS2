/*===========================================================================
 * DEFLENC.H  -  Deflate encoder (RFC 1951), PKZIP 2.04g -ex strength
 * Target: MSVC 2.2  Win32s
 *
 * The compressing half of what ZIPARC.C's Inflate undoes.  One encoder is one
 * raw deflate stream - no zlib or gzip framing, because a zip entry carries
 * none.  Input is PUSHED in pieces (DeflFeed) and compressed bytes come out
 * through a write callback as each block is finished, so a file of any size
 * goes through in the same fixed amount of memory: a 64 KB window, two 64 KB
 * hash tables and the symbol buffer for one block, about 230 KB in all.
 *
 * The parse is the "maximum" one PKZIP 2.04g calls -ex: every position is
 * looked up, matches are lazily evaluated (a match is only taken if the next
 * position does not start a longer one), and the hash chain is followed 4096
 * deep.  Each block is then sent whichever way is smallest - stored, the
 * fixed Huffman codes, or codes built for that block - so data that will not
 * compress costs five bytes a block and no more.
 *===========================================================================*/

#ifndef DEFLENC_H
#define DEFLENC_H

#include "arcdefs.h"

/* Where compressed bytes go.  Return 0 to stop: the encoder then fails every
 * later call with SZ_ERR_WRITE. */
typedef int (*DeflWrite)( void *user, const Byte *data, UInt32 len );

typedef struct DeflEnc DeflEnc;

/* Allocate an encoder.  SZ_OK or SZ_ERR_MEMORY. */
int  DeflCreate( DeflEnc **out );

/* Start a new stream, sending its output to 'write'.  An encoder is reused
 * for every entry of a zip, so the tables are allocated once. */
void DeflReset( DeflEnc *d, DeflWrite write, void *user );

/* Compress 'len' more bytes.  SZ_OK, or SZ_ERR_WRITE when the callback
 * refused. */
int  DeflFeed( DeflEnc *d, const Byte *data, UInt32 len );

/* End the stream: everything still pending goes out, in a final block. */
int  DeflFinish( DeflEnc *d );

/* Bytes written through the callback since DeflReset. */
UInt32 DeflOutBytes( DeflEnc *d );

void DeflFree( DeflEnc *d );

/* What DeflCreate allocates, for the memory estimate. */
UInt32 DeflMemNeeded( void );

#endif /* DEFLENC_H */
