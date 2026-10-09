/* UNZIP.HPP - PKZIP 2.04g compatible extraction, Open Watcom 1.9, OS/2 */
#ifndef UNZIP_HPP
#define UNZIP_HPP

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes */
#define UZ_OK           0
#define UZ_ERR_OPEN     1   /* cannot open zip file */
#define UZ_ERR_FORMAT   2   /* not a valid ZIP file */
#define UZ_ERR_NOFILE   3   /* named file not found in archive */
#define UZ_ERR_CREATE   4   /* cannot create output file */
#define UZ_ERR_READ     5   /* read error */
#define UZ_ERR_WRITE    6   /* write error */
#define UZ_ERR_CRC      7   /* CRC mismatch */
#define UZ_ERR_MEMORY   8   /* out of memory */
#define UZ_ERR_METHOD   9   /* unsupported compression method */
#define UZ_ERR_DISK     10  /* disk-span or split archive */
#define UZ_ERR_CANCEL   11  /* extraction aborted by progress callback */

/*
 * UZ_PROGRESS - optional progress / yield callback.
 *
 * Called periodically while an entry is being written out.
 *   done  - bytes of the current entry written so far
 *   total - uncompressed size of the current entry (0 if unknown)
 *   user  - the caller-supplied context pointer
 *
 * Return non-zero to continue, or 0 to abort (ExtractZip then returns
 * UZ_ERR_CANCEL).  The callback is the place to pump PM messages so the
 * progress page repaints and Cancel is seen during extraction.
 */
typedef int (*UZ_PROGRESS)(unsigned long done, unsigned long total,
                           void *user);

/*
 * ExtractZip - extract one or all files from a PKZIP 2.04g archive.
 *
 * pszZip      - path to the .ZIP file (or self-extracting EXE archive)
 * pszTarget   - file name to extract (no path), or NULL to extract all
 * pszDestDir  - directory to extract into, or NULL/"" for current directory.
 *               May end with or without a trailing slash.  Sub-directory
 *               paths stored in the archive are recreated beneath it.
 * pfnProgress - progress/yield callback, or NULL for none
 * pUser       - context pointer passed through to pfnProgress
 *
 * Returns UZ_OK (0) on success, or a UZ_ERR_* code.
 *
 * Compatible with PKZIP 2.04g: stored (0) and deflated (8) entries.
 * Encrypted entries return UZ_ERR_METHOD.
 * Restores sub-directory paths embedded in the archive.
 */
int ExtractZip(const char *pszZip, const char *pszTarget,
               const char *pszDestDir,
               UZ_PROGRESS pfnProgress, void *pUser);

#ifdef __cplusplus
}
#endif

#endif /* UNZIP_HPP */
