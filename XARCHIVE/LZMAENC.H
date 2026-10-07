/*===========================================================================
 * LZMAENC.H  -  LZMA1 and LZMA2 encoder
 * Target: MSVC 2.2  Win32s
 *
 * The compressing half of LZMADEC.C, for the .7z archives XArchive writes.
 * LzmaEncode produces a plain LZMA1 stream with no end marker - the form a 7z
 * folder holds, where the unpacked size is in the header - which any 7-Zip
 * since the first one can read; the archive header is packed that way, as
 * 7-Zip packs its own.  Lzma2Encode wraps the same coding in LZMA2's chunks,
 * which is what the data folders use (7-Zip 9.04 and later read it).
 *
 * The input is PULLED through a read callback and the output PUSHED through
 * a write callback, so a solid folder of any size goes through in memory
 * bounded by the dictionary.  That memory is the whole story of this encoder:
 * about 11.5 bytes per byte of dictionary (the window, plus a binary tree of
 * two links per position, plus the hash heads), which is why the archive
 * writer picks the dictionary from what the machine can spare rather than
 * from a fixed table - see LzmaEncMemNeeded.
 *
 * The parse is 7-Zip's "normal and above" one: a binary-tree match finder
 * that keeps every earlier occurrence of the next four bytes sorted, and an
 * optimal parser that prices each choice (literal, short repeat, repeated
 * distance, new match, and a match followed by a literal and a repeat) in the
 * coder's own probabilities and keeps the cheapest path through the next few
 * thousand bytes.
 *===========================================================================*/

#ifndef LZMAENC_H
#define LZMAENC_H

#include "arcdefs.h"

/* Fill 'buf' with up to 'len' input bytes and return how many; 0 means the
 * input has ended.  A short read is not the end - only 0 is.  A reader that
 * hits an error keeps its own note of it and returns 0: the encoder finishes
 * a stream the caller then throws away. */
typedef UInt32 (*LzmaEncRead)( void *user, Byte *buf, UInt32 len );

/* Where compressed bytes go.  Return 0 to stop (SZ_ERR_WRITE). */
typedef int (*LzmaEncWrite)( void *user, const Byte *data, UInt32 len );

/* Called every 64 KB or so of input, with the running totals.  Return 0 to
 * cancel (SZ_ERR_CANCEL).  May be NULL. */
typedef int (*LzmaEncProgress)( void *user, UInt32 inDone, UInt32 outDone );

typedef struct {
    UInt32 dictSize;        /* 64 KB .. 64 MB                              */
    unsigned lc, lp, pb;    /* 3, 0, 2 - the defaults everything uses       */
    unsigned fastBytes;     /* 5 .. 273: a match this long is taken at once */
    unsigned cutValue;      /* how many tree nodes a search may visit       */
} LzmaEncProps;

/* The defaults XArchive writes with: lc3 lp0 pb2, 64 fast bytes, cut 48 -
 * what 7-Zip uses for its two highest levels. */
void   LzmaEncPropsInit( LzmaEncProps *p, UInt32 dictSize );

/* The five property bytes a 7z LZMA coder record carries. */
void   LzmaEncPropsBytes( const LzmaEncProps *p, Byte out[5] );

/* The one property byte a 7z LZMA2 coder record carries: the dictionary size
 * rounded up to the next one LZMA2 can name (two or three times a power of
 * two, so a power of two goes in exactly). */
Byte   Lzma2EncPropByte( const LzmaEncProps *p );

/* Bytes the encoder allocates for this dictionary, give or take a few KB. */
UInt32 LzmaEncMemNeeded( UInt32 dictSize );

/* Compress everything 'read' supplies.  *inTotal / *outTotal receive the
 * byte counts (either may be NULL).  Returns SZ_OK, SZ_ERR_MEMORY,
 * SZ_ERR_WRITE, SZ_ERR_CANCEL or SZ_ERR_DATA - the last meaning the encoder
 * caught itself about to emit a match the data does not contain, which would
 * be a bug here and is refused rather than written. */
int    LzmaEncode( const LzmaEncProps *props,
                   LzmaEncRead read, void *readUser,
                   LzmaEncWrite write, void *writeUser,
                   LzmaEncProgress prog, void *progUser,
                   UInt32 *inTotal, UInt32 *outTotal );

/* The same, framed as LZMA2 - what 7-Zip has written by default since 9.x
 * and what the archive writer uses for its folders.  The coded steps are
 * the same; they are cut into chunks of at most 2 MB in and 64 KB out, each
 * a range-coded run of its own with a few bytes of header, and a chunk that
 * came out no smaller than it went in is sent as stored bytes instead.  The
 * stream ends with LZMA2's 0 byte.  Arguments and results as LzmaEncode.
 * The chunk is built in the buffer LZMA writes through anyway; the only
 * extra memory is a copy of the model for the stored case, about 16 KB,
 * which LzmaEncMemNeeded already counts. */
int    Lzma2Encode( const LzmaEncProps *props,
                    LzmaEncRead read, void *readUser,
                    LzmaEncWrite write, void *writeUser,
                    LzmaEncProgress prog, void *progUser,
                    UInt32 *inTotal, UInt32 *outTotal );

#endif /* LZMAENC_H */
