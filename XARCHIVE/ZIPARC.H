/*===========================================================================
 * ZIPARC.H  -  PKZIP (.zip) parsing and extraction
 * Target: MSVC 2.2  Win32s
 *
 * A subset extractor for PKZIP 2.04g archives: stored (method 0) and deflated
 * (method 8) entries, with sub-directory paths and self-extracting (stub-
 * prefixed) archives.  No encryption, no Zip64, no multi-disk.  Ported from
 * the DOS/Win16 UNZIP module; presents the same open/list/extract shape as
 * SZARC so the GUI can treat 7z and zip uniformly.  Result codes and progress
 * callback are shared with SZARC (SZ_ERR_* / SzProgress).
 *===========================================================================*/

#ifndef ZIPARC_H
#define ZIPARC_H

#include "arcdefs.h"
#include "szarc.h"      /* SzProgress, and the shared SZ_ERR_* result codes */

typedef struct {
    char   name[SZ_MAX_NAME];   /* archive-relative path (back-slashes)     */
    UInt32 size;                /* uncompressed size                        */
    UInt32 packed;              /* compressed size                          */
    UInt32 crc;                 /* expected CRC-32                          */
    int    methodCode;          /* 0 = stored, 8 = deflated                 */
    UInt16 modDate, modTime;    /* MS-DOS packed modification date/time     */
    UInt32 attrib;              /* external attributes (low byte = DOS bits) */
    int    isDir;               /* directory entry                          */
} ZipEntry;

typedef struct ZipArchive ZipArchive;

/* Open and parse the central directory.  *out must be released with
 * ZipClose().  Returns SZ_OK or an SZ_ERR_* code (SZ_ERR_SIG if not a zip). */
int  ZipOpen( const char *path, ZipArchive **out );

int  ZipNumEntries( ZipArchive *z );

/* Volumes joined at open: 1 for an ordinary file, N for a .zip.001/.002/... set. */
int  ZipVolumeCount( ZipArchive *z );
const ZipEntry *ZipGetEntry( ZipArchive *z, int index );

/* Decompress one entry entirely into a freshly malloc'd buffer (used to unwrap
 * a WinImage .imz, which is a zip holding a single disk image).  On SZ_OK,
 * *outBuf must be free()d by the caller and *outLen is the byte length; the
 * CRC is verified.  Fails with SZ_ERR_UNSUPPORTED for encrypted / non
 * store-or-deflate entries. */
int  ZipExtractToMemory( ZipArchive *z, int index,
                         unsigned char **outBuf, UInt32 *outLen );

/* Extract all / a chosen subset (entry indices) beneath destDir, verifying
 * CRCs.  prog is called once per entry; return 0 from it to cancel. */
int  ZipExtractAll( ZipArchive *z, const char *destDir,
                    SzProgress prog, void *user );
int  ZipExtractItems( ZipArchive *z, const int *indices, int count,
                      const char *destDir, SzProgress prog, void *user );

/* The zip's own comment - the bytes after the end-of-central-directory record,
 * the one place a zip keeps free text - or NULL when it has none.  Owned by the
 * archive and valid until ZipClose. */
const char *ZipComment( ZipArchive *z );

/* Peak bytes one extraction needs; see ArcMemNeeded in ARCFILE.H.  The answer
 * does not depend on the archive - zip streams through a fixed window - so z
 * may be NULL, which ArcMemStartupOk relies on. */
UInt32 ZipMemNeeded( ZipArchive *z );

/*---- Encryption ----------------------------------------------------------- *
 * Zip has two incompatible schemes and this module reads both: PKWARE's
 * original ZipCrypto, and WinZip AES (AE-1/AE-2) at 128 and 256 bits.  AES-192
 * is refused with SZ_ERR_UNSUPPORTED rather than mis-decrypted - see ARCCRYP.C
 * for why that gap is deliberate.
 *
 * The intended flow is: open, notice ZipNeedsPassword, prompt, set it, extract.
 * Extraction of an encrypted entry with no password set returns
 * SZ_ERR_PASSWORD, so a front end that would rather prompt lazily can simply
 * extract and react to that - and one that forgets to handle it gets a clear
 * error rather than a corrupt file.
 *--------------------------------------------------------------------------*/

/* Set (or with NULL / "" clear) the password used for every encrypted entry.
 * Copied into the archive; the caller's buffer need not outlive the call. */
void ZipSetPassword( ZipArchive *z, const char *pw );

/* Does this entry carry the encrypted flag? */
int  ZipEntryEncrypted( ZipArchive *z, int index );

/* Does ANY entry need a password?  What a front end asks after opening, to
 * decide whether to prompt before the user picks anything. */
int  ZipNeedsPassword( ZipArchive *z );

void ZipClose( ZipArchive *z );

#endif /* ZIPARC_H */
