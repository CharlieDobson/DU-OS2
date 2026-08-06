/*===========================================================================
 * DIRVIEW.CPP  -  Directory enumeration: sort dirs-first, cache program icons
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 *   _dos_findfirst / _dos_findnext  ->  DosFindFirst / DosFindNext / DosFindClose
 *   struct find_t                   ->  FILEFINDBUF3 (FIL_STANDARD)
 *   ExtractIcon (SHELL.DLL)         ->  WinLoadFileIcon / WinFreeFileIcon
 *   "*.*"                           ->  "*"
 *
 * That last one matters.  On FAT the two patterns are the same, but on HPFS
 * "*.*" only matches names that actually contain a dot, so a directory full
 * of extension-less files would have come up empty.
 *===========================================================================*/
#define INCL_DOSFILEMGR
#define INCL_DOSERRORS
#define INCL_WIN
#define INCL_WINWORKPLACE

#include <os2.h>
#include <string.h>
#include <stdio.h>

#include "explorer.h"
#include "dirview.h"

static FILEITEM aFiles[MAX_FILES];
static int      nFileCount = 0;

/* Everything the file system will show us; the hidden/system filter is
   applied per entry below so the View menu can toggle it. */
#define FIND_ATTRS  (FILE_NORMAL   | FILE_READONLY | FILE_HIDDEN | \
                     FILE_SYSTEM   | FILE_ARCHIVED | FILE_DIRECTORY)

/*------------------------------------------------------------------
  FreeFileIcons - hand back every icon WinLoadFileIcon gave us
------------------------------------------------------------------*/
void FreeFileIcons(void)
{
    int i;

    for (i = 0; i < nFileCount; i++) {
        if (aFiles[i].hIcon != NULLHANDLE) {
            WinFreeFileIcon(aFiles[i].hIcon);
            aFiles[i].hIcon = NULLHANDLE;
        }
    }
}

/*------------------------------------------------------------------
  IsDotEntry - TRUE for the "." and ".." directory entries
------------------------------------------------------------------*/
static BOOL IsDotEntry(const char *pszName)
{
    return (BOOL)(pszName[0] == '.' &&
                  (pszName[1] == '\0' ||
                   (pszName[1] == '.' && pszName[2] == '\0')));
}

/*------------------------------------------------------------------
  HasExt - TRUE if pszName ends with pszExt, ignoring case
------------------------------------------------------------------*/
static BOOL HasExt(const char *pszName, const char *pszExt)
{
    size_t nName = strlen(pszName);
    size_t nExt  = strlen(pszExt);

    if (nName < nExt)
        return FALSE;

    return (BOOL)(stricmp(pszName + nName - nExt, pszExt) == 0);
}

/*------------------------------------------------------------------
  IsProgram - TRUE for the file kinds worth asking the shell about
------------------------------------------------------------------*/
static BOOL IsProgram(const char *pszName)
{
    return (BOOL)(HasExt(pszName, ".EXE") ||
                  HasExt(pszName, ".COM") ||
                  HasExt(pszName, ".CMD") ||
                  HasExt(pszName, ".BAT"));
}

/*------------------------------------------------------------------
  EnumDirectory - enumerate pszPath, directories first then files,
                  alphabetically within each group.

    Returns the number of items found.

    Program icons come from WinLoadFileIcon, which reads the file's .ICON
    extended attribute and falls back to the Workplace Shell's default for
    the file's type.  It is called only for program files so that opening a
    large directory does not turn into one shell query per entry.
------------------------------------------------------------------*/
int EnumDirectory(const char *pszPath, BOOL bShowHiddenFiles)
{
    HDIR         hdir = HDIR_CREATE;
    FILEFINDBUF3 ff;
    ULONG        cnt;
    char         szPat[CCHMAXPATH];
    char         szFull[CCHMAXPATH];
    FILEITEM     tmp;
    int          i, j, best;

    FreeFileIcons();
    nFileCount = 0;

    /* Build the search pattern: path + "*" */
    strcpy(szPat, pszPath);
    EnsureTrailingSlash(szPat);
    strcat(szPat, "*");

    cnt = 1;
    if (DosFindFirst((PCSZ)szPat, &hdir, FIND_ATTRS,
                     (PVOID)&ff, sizeof(ff), &cnt,
                     FIL_STANDARD) != NO_ERROR) {
        return 0;
    }

    do {
        FILEITEM *pItem;

        if (IsDotEntry(ff.achName))
            goto next;

        if (!bShowHiddenFiles &&
            (ff.attrFile & (FILE_HIDDEN | FILE_SYSTEM)))
            goto next;

        pItem = &aFiles[nFileCount];

        strcpy(pItem->szName, ff.achName);
        pItem->fdate    = ff.fdateLastWrite;
        pItem->ftime    = ff.ftimeLastWrite;
        pItem->cbFile   = ff.cbFile;
        pItem->attrFile = ff.attrFile;
        pItem->bIsDir   = (BOOL)((ff.attrFile & FILE_DIRECTORY) ? TRUE : FALSE);
        pItem->hIcon    = NULLHANDLE;

        if (!pItem->bIsDir && IsProgram(pItem->szName)) {
            strcpy(szFull, pszPath);
            EnsureTrailingSlash(szFull);
            strcat(szFull, pItem->szName);
            pItem->hIcon = WinLoadFileIcon((PCSZ)szFull, TRUE);
        }

        nFileCount++;

    next:
        cnt = 1;

    } while (nFileCount < MAX_FILES &&
             DosFindNext(hdir, (PVOID)&ff, sizeof(ff), &cnt) == NO_ERROR);

    DosFindClose(hdir);

    /* Selection sort: directories first, then files; alpha within each */
    for (i = 0; i < nFileCount - 1; i++) {

        best = i;
        for (j = i + 1; j < nFileCount; j++) {

            if (aFiles[j].bIsDir && !aFiles[best].bIsDir) {
                best = j;
                continue;
            }
            if (!aFiles[j].bIsDir && aFiles[best].bIsDir)
                continue;

            if (stricmp(aFiles[j].szName, aFiles[best].szName) < 0)
                best = j;
        }

        if (best != i) {
            tmp          = aFiles[i];
            aFiles[i]    = aFiles[best];
            aFiles[best] = tmp;
        }
    }

    return nFileCount;
}

int GetFileCount(void)
{
    return nFileCount;
}

/*------------------------------------------------------------------
  GetFileItem - 0-based index; NULL if out of range
------------------------------------------------------------------*/
FILEITEM *GetFileItem(int i)
{
    if (i < 0 || i >= nFileCount)
        return NULL;

    return &aFiles[i];
}
