/*===========================================================================
 * RAR5ARC.H  -  RAR 5.x parsing and extraction
 * Target: MSVC 2.2  Win32s
 *
 * PHASE 1: parses the RAR5 container (vint-encoded block headers), lists
 * entries, and extracts STORED (method 0) files.  Compressed entries list but
 * return SZ_ERR_UNSUPPORTED on extraction (the RAR5 unpack engine is a later
 * phase).  Encryption and multi-volume sets are supported.  Presents the same
 * open/list/
 * extract shape as the other backends and shares SZ_ERR_* / SzProgress.
 *
 * RAR5 stores modification time as a Unix timestamp, which we convert to a
 * Windows FILETIME (as 7z uses), so the ARCFILE date/attr code treats a RAR5
 * entry like a 7z one.
 *===========================================================================*/

#ifndef RAR5ARC_H
#define RAR5ARC_H

#include "arcdefs.h"
#include "szarc.h"
#include "arccryp.h"    /* AC_MAX_PW */

typedef struct {
    char   name[SZ_MAX_NAME];
    UInt32 size;
    UInt32 packed;
    UInt32 crc;
    int    hasCrc;
    int    methodCode;          /* 0 = stored, 1..5 = compression level      */
    UInt32 mtimeLo, mtimeHi;    /* last-modified FILETIME (valid if hasMtime) */
    int    hasMtime;
    UInt32 attrib;              /* low byte = DOS attribute bits             */
    int    isDir;
} Rar5Entry;

typedef struct Rar5Archive Rar5Archive;

int  Rar5Open( const char *path, Rar5Archive **out );

/* Open with a password already in hand, for -hp archives whose block chain is
 * enciphered past the crypt header - Rar5Open on one of those returns
 * SZ_ERR_PASSWORD.  A wrong password returns SZ_ERR_BADPASS, settled by the
 * check value in the crypt header before anything is decrypted. */
int  Rar5OpenPw( const char *path, const char *pw, Rar5Archive **out );

int  Rar5NumEntries( Rar5Archive *r );

/* Peak bytes one extraction needs; see ArcMemNeeded in ARCFILE.H. */
UInt32 Rar5MemNeeded( Rar5Archive *r );

/* Volumes in the set: 1 for an ordinary archive, N for a .partN.rar set. */
int  Rar5VolumeCount( Rar5Archive *r );
const Rar5Entry *Rar5GetEntry( Rar5Archive *r, int index );
int  Rar5ExtractAll( Rar5Archive *r, const char *destDir,
                     SzProgress prog, void *user );
int  Rar5ExtractItems( Rar5Archive *r, const int *indices, int count,
                       const char *destDir, SzProgress prog, void *user );
void Rar5Close( Rar5Archive *r );

/*---- Encryption ----------------------------------------------------------- *
 * RAR5 encrypts with AES-256-CBC keyed by PBKDF2-HMAC-SHA256, with a fresh
 * salt and IV per FILE.  Unlike 7z and RAR3 it usually carries an 8-byte
 * password check value, so a wrong password is rejected before any data is
 * decoded rather than inferred afterwards from a failed CRC.
 *
 * Encrypted HEADERS (WinRAR -hp) are handled too: the archive carries a crypt
 * header in the clear with one salt for the whole block chain, and everything
 * after it is enciphered.  Rar5Open returns SZ_ERR_PASSWORD for one of those -
 * distinguishable from SZ_ERR_UNSUPPORTED, so a front end knows to prompt
 * rather than give up - and Rar5OpenPw takes the password.
 *--------------------------------------------------------------------------*/
void Rar5SetPassword( Rar5Archive *r, const char *pw );
int  Rar5EntryEncrypted( Rar5Archive *r, int index );
int  Rar5NeedsPassword( Rar5Archive *r );

#endif /* RAR5ARC_H */
