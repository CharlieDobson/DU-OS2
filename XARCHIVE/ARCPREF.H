/*===========================================================================
 * ARCPREF.H  -  Persisted user preferences (XARCHIVE.INI)
 * Target: MSVC 2.2 Win32s / Open Watcom DOS / Open Watcom OS/2 PM
 *
 * One small INI file shared by all three front ends, so a setting made in the
 * DOS TUI is the same setting the Win32s and PM builds read.  Deliberately
 * hand-rolled rather than GetPrivateProfileString: that API exists only on
 * Windows, and this file is compiled byte-identical into the DOS and OS/2
 * trees like the format backends are.
 *
 * Format (INI, one section):
 *
 *     ; XArchive preferences
 *     [Options]
 *     ExtractFolders=1
 *     FolderView=1
 *     AskShortNames=0
 *     GraphicsMode=0
 *     CompressFormat=zip
 *     CompressFolders=1
 *
 * FolderView is a DISPLAY setting: 1 browses the archive a folder at a time,
 * 0 lists every entry at once with its full stored path.  It is kept here
 * rather than in the front end so all three front ends can agree on it; a
 * front end with no folder browsing simply ignores it.
 *
 * MemoryLimitMB was here and is GONE (2026-09-06).  The budget is measured
 * from the machine - see ArcMaxDictSize in ARCDEFS.H - and a hand-set cap
 * could only make that measurement worse: too low and the program refused
 * archives the machine could have handled, too high and it promised memory
 * that was never there.  An existing key is stripped on the next save.
 *
 * Keys this module does not recognise are preserved across a save, so a
 * newer build's settings are not destroyed by an older one.  A key this
 * module has RETIRED is not foreign and is not preserved (see IsKnownKey).
 *
 * Where the file lives is the caller's business - the three platforms find
 * their own executable differently (argv[0], GetModuleFileName,
 * DosQueryModuleName).  Call ArcPrefSetFileFromExe() with whatever the
 * platform gives you and it derives the INI path alongside it.
 *===========================================================================*/

#ifndef ARCPREF_H
#define ARCPREF_H

/* Point the module at its INI.  'exePath' is the running executable's path;
 * the INI is placed in the same directory, named XARCHIVE.INI (8.3-clean for
 * the DOS build).  A NULL or path-less argument falls back to the current
 * directory.  Safe to call before ArcPrefLoad; if never called, the current
 * directory is used. */
void ArcPrefSetFileFromExe( const char *exePath );

/* The resolved INI path, for diagnostics / About boxes.  Never NULL. */
const char *ArcPrefFile( void );

/* Folder browsing (see the format note above).  Unlike ExtractFolders, which
 * lives in the backend as the flatten flag, this one has nowhere else to
 * live, so the module holds it: default 1. */
int  ArcPrefFolderView( void );
void ArcPrefSetFolderView( int on );

/* Whether the front end asks the user to name each long file it has to
 * shorten during extraction, instead of auto-renaming it to LONGFI~1.TXT.
 * Default 0 (auto-rename), because that is the only behaviour that can run
 * unattended.  Like FolderView this is a front-end setting kept here so all
 * three front ends agree; the backend hook it drives is
 * ArcSetShortNamePrompt in ARCDEFS.H. */
int  ArcPrefAskShortNames( void );
void ArcPrefSetAskShortNames( int on );

/* Whether the DOS front end puts its screen up in a GRAPHICS mode (VGA
 * 640x480, the card's own font, icons in the list) instead of text.  Default
 * 0: text is what every machine can do, and a graphics screen costs sixty
 * times as many writes per character, so it is asked for rather than
 * assumed.  /G on the command line turns it on for one run without changing
 * this; the Options menu changes this.
 *
 * Kept here with the other two front-end settings so the file has one home,
 * even though only one of the three front ends can act on it - the Win32s
 * and PM builds have a window manager and no use for the idea.  A front end
 * that cannot do it simply never reads it, exactly as a front end with no
 * folder browsing ignores FolderView. */
int  ArcPrefGraphics( void );
void ArcPrefSetGraphics( int on );

/* What the Compress dialog offers first, remembered from the last archive
 * made: the format (1 = zip, 2 = 7z - ARC_CF_ZIP / ARC_CF_7Z in ARCCOMP.H)
 * and whether folder names go in (1) or the archive is flat (0).  Stored as
 * CompressFormat=zip|7z and CompressFolders=1|0. */
int  ArcPrefCompressFormat( void );
void ArcPrefSetCompressFormat( int fmt );
int  ArcPrefCompressFolders( void );
void ArcPrefSetCompressFolders( int on );

/* Read the INI and apply every setting it holds.  A missing or unreadable
 * file is not an error - the built-in defaults simply stand. */
void ArcPrefLoad( void );

/* Write the current settings back.  Returns 1 on success, 0 if the file could
 * not be written (a read-only medium, a full disk); callers may ignore it,
 * since failing to persist a preference is not worth interrupting the user. */
int  ArcPrefSave( void );

#endif /* ARCPREF_H */
