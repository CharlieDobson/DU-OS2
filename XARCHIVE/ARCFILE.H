/*===========================================================================
 * ARCFILE.H  -  Format-agnostic archive front door
 * Target: MSVC 2.2  Win32s
 *
 * Sniffs an archive's signature and dispatches to the 7z (SZARC) or zip
 * (ZIPARC) backend, presenting one open/list/extract interface so the GUI
 * does not care which format it is looking at.  Result codes and the progress
 * callback are the shared SZ_ERR_* / SzProgress.
 *===========================================================================*/

#ifndef ARCFILE_H
#define ARCFILE_H

#include "arcdefs.h"
#include "szarc.h"      /* SzProgress */

typedef struct ArcFile ArcFile;

/* Open and parse; *out must be released with ArcClose().  SZ_OK or SZ_ERR_*. */
int  ArcOpen( const char *path, ArcFile **out );

int  ArcNumEntries( ArcFile *a );
const char *ArcEntryName( ArcFile *a, int index );
UInt32      ArcEntrySize( ArcFile *a, int index );
int         ArcEntryIsDir( ArcFile *a, int index );

/* Extra columns for the list view.  ArcEntryPacked returns 0xFFFFFFFF when the
 * packed size is unknown; ArcEntryDate / ArcEntryAttr write "" when the field
 * is absent. */
UInt32      ArcEntryPacked( ArcFile *a, int index );
const char *ArcEntryMethod( ArcFile *a, int index );
void        ArcEntryDate( ArcFile *a, int index, char *buf, int buflen );
void        ArcEntryAttr( ArcFile *a, int index, char *buf, int buflen );

int  ArcExtractAll( ArcFile *a, const char *destDir,
                    SzProgress prog, void *user );
int  ArcExtractItems( ArcFile *a, const int *indices, int count,
                      const char *destDir, SzProgress prog, void *user );

/* Integrity test: decode and CRC-check every entry without writing any files.
 * Returns SZ_OK if all entries verify, SZ_ERR_CRC / SZ_ERR_DATA on corruption,
 * SZ_ERR_UNSUPPORTED if an entry uses a feature we cannot decode, or
 * SZ_ERR_CANCEL.  Uses the same progress callback as extraction. */
int  ArcTestAll( ArcFile *a, SzProgress prog, void *user );

void ArcClose( ArcFile *a );
const char *ArcErrorText( int code );

/* When an operation on this archive returns SZ_ERR_UNSUPPORTED, this returns a
 * human-readable explanation tailored to the archive's format: which features
 * ARE handled and which are not.  Safe to call with a NULL archive. */
const char *ArcUnsupportedHint( ArcFile *a );

/* Explanation for SZ_ERR_NORAM: the archive needs a compression dictionary
 * bigger than this build will hold in memory.  Unlike SZ_ERR_MEMORY (an
 * allocation that failed) this one is a refusal, and the user can act on it
 * by re-creating the archive with a smaller dictionary. */
const char *ArcNoRamHint( void );

/* Short display name of the archive's format, e.g. "7z", "Zip", "RAR", "RAR5".
 * Safe to call with a NULL archive (returns "?"). */
const char *ArcFormatName( ArcFile *a );

#endif /* ARCFILE_H */
