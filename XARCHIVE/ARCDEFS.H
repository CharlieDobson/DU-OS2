/*===========================================================================
 * ARCDEFS.H  -  Shared types, limits and result codes for the Win32s
 *              XArchive extractor.
 * Target: MSVC 2.2  Win32s
 *===========================================================================*/

#ifndef ARCDEFS_H
#define ARCDEFS_H

/*---- Fixed-width integer types ------------------------------------------- *
 * MSVC 2.2 is a 32-bit compiler; int/long are 32 bits.  We deliberately do
 * NOT use __int64 (support is unreliable on this toolchain): the 7z format's
 * 64-bit numbers are read but clamped to 32 bits, with an overflow guard that
 * rejects anything that would not fit (irrelevant for era-sized archives).
 *-------------------------------------------------------------------------- */
typedef unsigned char  Byte;
typedef unsigned short UInt16;
typedef unsigned int   UInt32;
typedef int            Int32;

/*---- Result codes -------------------------------------------------------- */
#define SZ_OK             0
#define SZ_ERR_OPEN       1   /* cannot open the archive file            */
#define SZ_ERR_READ       2   /* read error / truncated file             */
#define SZ_ERR_SIG        3   /* not a 7z archive (bad signature)        */
#define SZ_ERR_CRC        4   /* CRC mismatch (header or data)           */
#define SZ_ERR_FORMAT     5   /* malformed / unexpected header structure */
#define SZ_ERR_UNSUPPORTED 6  /* feature we do not implement             */
#define SZ_ERR_MEMORY     7   /* out of memory                           */
#define SZ_ERR_TOOBIG     8   /* exceeds a configured limit              */
#define SZ_ERR_WRITE      9   /* cannot create / write an output file    */
#define SZ_ERR_DATA       10  /* corrupt compressed data                 */
#define SZ_ERR_CANCEL     11  /* user cancelled                          */
#define SZ_ERR_NORAM      12  /* needs more RAM than this build can use  *
                               * (an LZMA dictionary over SZ_MAX_DICT_   *
                               * SIZE).  Distinct from SZ_ERR_MEMORY:    *
                               * that is "the allocation failed", this   *
                               * is "we refuse to attempt it".           */

/*---- Configurable limits (keep memory manageable on era hardware) -------- *
 * The extractor refuses archives that would exceed these, with a clear
 * message, rather than attempting an allocation that would thrash or fail.
 *
 * Two different kinds of limit live here, and the difference matters:
 *
 *   - What must fit in RAM at once: the LZMA dictionary and any whole-file
 *     buffer.  These are the real memory ceiling; raising them raises what the
 *     program asks the system for.
 *   - What merely has to be readable: the 7z folder sizes.  Folders are
 *     decoded as a stream straight to disk (SzStreamFolder), so their size
 *     costs no memory - the cap is only a sanity bound against a corrupt
 *     header, and can be generous.
 *-------------------------------------------------------------------------- */
#define SZ_MAX_FILES        16384               /* entries we will list     */
#define SZ_MAX_FOLDERS      256                 /* coded folders            */
#define SZ_MAX_NAME         260                 /* stored path length       */

/* A folder is not one packed stream: a BCJ2 folder consumes four (main, call
 * and jump inputs plus the raw range-coder stream).  Pack streams therefore
 * need their own bound rather than borrowing SZ_MAX_FOLDERS. */
#define SZ_MAX_PACKSTREAMS  1024                /* packed streams total     */
#define SZ_MAX_CODERS       4                   /* coders in one folder     */
#define SZ_MAX_FOLDER_STRMS 8                   /* in/out streams per folder*/

/* In RAM at once */
#define SZ_MAX_DICT_SIZE    (32UL * 1024 * 1024) /* LZMA sliding dictionary */
#define SZ_MAX_BUFFER_SIZE  (32UL * 1024 * 1024) /* biggest whole-file buffer:
                                                    a RAR entry, a mounted
                                                    disk image, or the 7z
                                                    encoded header           */

/* Streamed, so these bound the format rather than the memory use */
#define SZ_MAX_UNPACK_SIZE  (1024UL * 1024 * 1024)  /* 7z folder output */
#define SZ_MAX_PACK_SIZE    (1024UL * 1024 * 1024)  /* 7z folder input  */

/*---- Filesystem-safe output names ----------------------------------------- *
 * Implemented in ARCFILE.C, called by every backend's output-path builder.
 * Rewrites an entry's stored path into one that is safe to create on the
 * extraction target: separators normalised to '\', characters the filesystem
 * forbids replaced, "." / ".." components neutralised, DOS device names
 * (CON, NUL, COM1, ...) disarmed, and - when the target only takes 8.3 names
 * (the DOS build always; the Win32 build when running on Win32s) - every
 * component truncated to an upper-case 8.3 name.
 *-------------------------------------------------------------------------- */
void ArcFsName( char *dst, int dstSize, const char *name );

/*---- Extract with or without the stored folder structure ------------------ *
 * Off (the default) reproduces the archive's directory tree under the
 * destination.  On, every file is dropped straight into the destination and
 * stored directory entries are not created at all - "extract without paths".
 * ArcFsName reduces a name to its final component, so all five backends
 * inherit it from the one place they already share; each backend asks
 * ArcFlattenPaths() before honouring a DIRECTORY entry.
 *
 * Flattening makes name collisions much likelier (a\readme + b\readme both
 * become readme), which the existing overwrite prompt handles.
 *-------------------------------------------------------------------------- */
void ArcSetFlattenPaths( int on );
int  ArcFlattenPaths( void );

/*---- Overwrite confirmation ----------------------------------------------- *
 * When extraction is about to write over a file that already exists, the
 * front end can be asked what to do.  It registers a prompt callback once at
 * startup; the callback returns one of the ARC_OW_* answers.  The two "all"
 * answers latch for the rest of that extraction operation (the latch resets
 * on every ArcExtractAll / ArcExtractItems call).  With no callback
 * registered, existing files are silently overwritten - the old behaviour,
 * kept for the CLI.
 *
 * ArcWantWrite is for the backends: called with the would-be output path
 * before creating a file, it returns 1 to (over)write and 0 to skip the
 * entry.  It only prompts when the file exists.
 *-------------------------------------------------------------------------- */
#define ARC_OW_YES     0   /* overwrite this file            */
#define ARC_OW_NO      1   /* keep this file                 */
#define ARC_OW_YESALL  2   /* overwrite this and all further */
#define ARC_OW_NOALL   3   /* keep this and all further      */

typedef int (*ArcOverwriteFn)( void *user, const char *path );

void ArcSetOverwritePrompt( ArcOverwriteFn fn, void *user );
int  ArcWantWrite( const char *path );

#endif /* ARCDEFS_H */
