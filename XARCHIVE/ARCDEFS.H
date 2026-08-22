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
#define SZ_ERR_TOOBIG     8   /* past a STRUCTURAL limit: too many entries,
                               * too many folders/coders/pack streams, or a
                               * size the format should never reach.  Nothing
                               * to do with how much memory there is - that
                               * is SZ_ERR_NORAM below.                    */
#define SZ_ERR_WRITE      9   /* cannot create / write an output file    */
#define SZ_ERR_DATA       10  /* corrupt compressed data                 */
#define SZ_ERR_CANCEL     11  /* user cancelled                          */
#define SZ_ERR_NORAM      12  /* needs more RAM in one block than the
                               * measured budget allows: an LZMA dictionary
                               * over SZ_MAX_DICT_SIZE, or a whole-stream
                               * buffer over SZ_MAX_BUFFER_SIZE.  Distinct
                               * from SZ_ERR_MEMORY only in WHO said no:
                               * SZ_ERR_MEMORY is "the allocation was tried
                               * and failed", this is "the budget says do
                               * not try".  Both mean the user is short of
                               * memory, and both must be reported that way
                               * - never as SZ_ERR_TOOBIG, which would send
                               * them off to shrink an archive instead.   */

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
/*---- How many entries we will list ---------------------------------------- *
 * This was a flat 16384 and it was the wrong shape of limit, for the same
 * reason the old 32 MB memory ceiling was: nothing about the formats stops at
 * 16384, and nothing in this program is sized by it any more.  Every backend
 * grows its entry table to what the archive actually holds; 16384 was only
 * ever the number at which we refused to try.
 *
 * What an entry really costs is MEMORY, and a lot of it, because each entry
 * record carries a fixed SZ_MAX_NAME name field - about 300 bytes apiece, so
 * the old cap was really "4.9 MB of entry table".  So the cap is now derived
 * from the measured budget the same way the dictionary is, and a machine with
 * room lists an archive with room.
 *
 * ARC_MIN_ENTRIES is a floor, so this is never WORSE than the old fixed cap
 * even on a machine that measures tiny.  ARC_MAX_ENTRIES is the sanity bound
 * that a corrupt header runs into: an entry count past this is not a large
 * archive, it is a damaged one, and it must be refused without first trying
 * to allocate for it.
 *-------------------------------------------------------------------------- */
#define ARC_MIN_ENTRIES     16384
#define ARC_MAX_ENTRIES     1000000UL

UInt32 ArcMaxEntries( void );

/* Verdict on an entry count a header just declared.  SZ_OK to go ahead,
 * SZ_ERR_NORAM for "plausible, but more than this machine can hold", and
 * SZ_ERR_TOOBIG for "past anything real" - the two are different problems
 * and want different messages. */
int    ArcCheckEntryCount( UInt32 n );

#define SZ_MAX_FILES        ArcMaxEntries()
#define SZ_MAX_FOLDERS      256                 /* coded folders            */
#define SZ_MAX_NAME         260                 /* stored path length       */

/* A folder is not one packed stream: a BCJ2 folder consumes four (main, call
 * and jump inputs plus the raw range-coder stream).  Pack streams therefore
 * need their own bound rather than borrowing SZ_MAX_FOLDERS. */
#define SZ_MAX_PACKSTREAMS  1024                /* packed streams total     */
#define SZ_MAX_CODERS       4                   /* coders in one folder     */
#define SZ_MAX_FOLDER_STRMS 8                   /* in/out streams per folder*/

/*---- In RAM at once: measured, not assumed -------------------------------- *
 * These two were fixed 32 MB constants, so a machine with 8 MB thrashed and a
 * machine with 8 GB was told to go and re-create its archive.  They are now
 * decided at run time from how much memory the program can actually get:
 *
 *   Win32/Win32s  GlobalMemoryStatus (free physical + pagefile, bounded by
 *                 the free address space).
 *   DOS           DPMI INT 31h AX=0500h, largest free block, cross-checked by
 *                 asking the heap for it; if there is no DPMI answer, the
 *                 heap probe alone decides.
 *   OS/2          DosQuerySysInfo, free physical memory.
 *
 * Measured once on first use and then latched, so nothing is re-probed in the
 * middle of an extraction.  ArcMemSetLimit overrides the measurement (the
 * MemoryLimitMB preference, 0 = measure); ArcMemLimitMB reports what is in
 * force so a message can quote the real number instead of a baked-in one.
 *
 * The floor is what the program insists on being allowed to try even on a
 * machine that reports almost nothing free; the ceiling is 7-Zip's own largest
 * dictionary, past which no real archive exists.
 *-------------------------------------------------------------------------- */
#define ARC_MEM_FLOOR       (4UL * 1024 * 1024)
#define ARC_MEM_CEILING     (1536UL * 1024 * 1024)

UInt32 ArcMaxDictSize( void );      /* LZMA sliding dictionary               */
UInt32 ArcMaxBufferSize( void );    /* biggest whole-file buffer: a RAR entry,
                                       a mounted disk image, the 7z header   */
UInt32 ArcMemLimitMB( void );       /* the budget in whole MB, for messages  */
void   ArcMemSetLimit( UInt32 mb ); /* 0 = measure the machine (the default) */

#define SZ_MAX_DICT_SIZE    ArcMaxDictSize()
#define SZ_MAX_BUFFER_SIZE  ArcMaxBufferSize()

/* Streamed, so these bound the format rather than the memory use */
#define SZ_MAX_UNPACK_SIZE  (1024UL * 1024 * 1024)  /* 7z folder output */
#define SZ_MAX_PACK_SIZE    (1024UL * 1024 * 1024)  /* 7z folder input  */

/*---- Filesystem-safe output names ----------------------------------------- *
 * Implemented in ARCFILE.C, called by every backend's output-path builder.
 * Rewrites an entry's stored path into one that is safe to create on the
 * extraction target: separators normalised to '\', characters the filesystem
 * forbids replaced, "." / ".." components neutralised, DOS device names
 * (CON, NUL, COM1, ...) disarmed, and, when the target only takes 8.3 names
 * (the DOS build always; the Win32 build when running on Win32s), every
 * component reduced to an upper-case 8.3 name the way DOS 7 does it: a name
 * that already fits keeps itself, and one that has to be cut gets a numeric
 * tail, so "Long filename 1.txt" and "Long filename 2.txt" become
 * LONGFI~1.TXT and LONGFI~2.TXT instead of both becoming LONGFILE.TXT.
 *
 * The numbering is per destination folder and is remembered for the length of
 * one extraction, which ArcResetNames clears.  ArcExtractAll and
 * ArcExtractItems call it; nothing else needs to.
 *-------------------------------------------------------------------------- */
void ArcFsName( char *dst, int dstSize, const char *name );
void ArcResetNames( void );

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
