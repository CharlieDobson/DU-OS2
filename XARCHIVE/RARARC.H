/*===========================================================================
 * RARARC.H  -  RAR (2.x/3.x, "RAR4") parsing and extraction
 * Target: MSVC 2.2  Win32s
 *
 * Parses the RAR4 container (marker + block headers), lists entries, and
 * extracts them: STORED (method 0x30) and the RAR2/RAR3 unpackers, solid
 * chains included.  RAR5 archives are detected and handed to RAR5ARC.C.
 *
 * Encrypted entries (AES-128-CBC) are supported, and so are encrypted HEADERS
 * (-hp) - see RarOpenPw.  Multi-volume sets (.partN.rar and .rar/.r00/...) are
 * joined automatically from any volume.  Presents the same open/list/extract
 * shape as SZARC/ZIPARC and shares the SZ_ERR_* codes + SzProgress callback.
 *===========================================================================*/

#ifndef RARARC_H
#define RARARC_H

#include "arcdefs.h"
#include "szarc.h"      /* SzProgress + shared SZ_ERR_* result codes */

typedef struct {
    char   name[SZ_MAX_NAME];   /* archive-relative path (back-slashes)     */
    UInt32 size;                /* uncompressed size                        */
    UInt32 packed;              /* compressed size                          */
    UInt32 crc;                 /* expected CRC-32                          */
    int    methodCode;          /* RAR method byte (0x30 = stored)          */
    int    unpVer;              /* unpack version (29/36 = RAR3 "v3")       */
    UInt16 modDate, modTime;    /* MS-DOS packed modification date/time     */
    UInt32 attrib;              /* file attributes (low byte = DOS bits)    */
    int    isDir;               /* directory entry                          */
} RarEntry;

typedef struct RarArchive RarArchive;

/* Open and parse the block headers.  *out must be released with RarClose().
 * Returns SZ_OK or an SZ_ERR_* code (SZ_ERR_SIG if not a RAR archive,
 * SZ_ERR_UNSUPPORTED for RAR5 until that container is implemented). */
int  RarOpen( const char *path, RarArchive **out );

/* Open with a password already in hand.  Needed for -hp archives, where the
 * block chain past the main header is enciphered and there is no directory to
 * read without it: RarOpen on one of those returns SZ_ERR_PASSWORD, and a
 * wrong password here returns SZ_ERR_BADPASS - encrypted headers carry a CRC,
 * so unlike encrypted entry data they can say so straight away. */
int  RarOpenPw( const char *path, const char *pw, RarArchive **out );


int  RarNumEntries( RarArchive *r );

/* Volumes in the set: 1 for an ordinary archive, N for .partN.rar / .r00 ... */
int  RarVolumeCount( RarArchive *r );
const RarEntry *RarGetEntry( RarArchive *r, int index );

int  RarExtractAll( RarArchive *r, const char *destDir,
                    SzProgress prog, void *user );
int  RarExtractItems( RarArchive *r, const int *indices, int count,
                      const char *destDir, SzProgress prog, void *user );

/* Encryption.  RAR3 is AES-128-CBC and has NO password check value, so a
 * wrong password cannot be reported until a CRC fails: an encrypted entry
 * with no password set returns SZ_ERR_PASSWORD, and one whose CRC fails after
 * decryption returns SZ_ERR_BADPASS.  Passwords over 28 characters take the
 * RAR 2.9 "broken SHA-1" path - see ARCCRYP.H. */
void RarSetPassword( RarArchive *r, const char *pw );
int  RarEntryEncrypted( RarArchive *r, int index );
int  RarNeedsPassword( RarArchive *r );

/* The archive comment, or NULL when there is none or it is PACKED rather than
 * stored - unpacking one would mean pointing the RAR decoder at a header, which
 * is not worth it for free text.  Owned by the archive. */
const char *RarComment( RarArchive *r );

/* Peak bytes one extraction needs; see ArcMemNeeded in ARCFILE.H. */
UInt32 RarMemNeeded( RarArchive *r );

void RarClose( RarArchive *r );

#endif /* RARARC_H */
