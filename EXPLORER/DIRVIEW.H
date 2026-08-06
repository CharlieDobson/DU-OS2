/*===========================================================================
 * DIRVIEW.H  -  Directory contents enumeration and the FILEITEM cache
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *===========================================================================*/

#ifndef DIRVIEW_H
#define DIRVIEW_H

/* Sized for HPFS directories, not for a 64 KB segment.  See the SHORT
   scrollbar note in PAINT.CPP: raising this again needs the scroll scaling
   there to be checked. */
#define MAX_FILES   2048

typedef struct _FILEITEM {
    char     szName[CCHMAXPATHCOMP];  /* file name; HPFS names are long     */
    FDATE    fdate;                   /* last write date                    */
    FTIME    ftime;                   /* last write time                    */
    ULONG    cbFile;                  /* size in bytes; 0 for directories   */
    ULONG    attrFile;                /* FILE_READONLY / _HIDDEN / ...      */
    BOOL     bIsDir;                  /* TRUE if this entry is a directory  */
    HPOINTER hIcon;                   /* WinLoadFileIcon result, or NULL    */
} FILEITEM;

int       EnumDirectory(const char *pszPath, BOOL bShowHiddenFiles);
int       GetFileCount(void);
FILEITEM *GetFileItem(int i);
void      FreeFileIcons(void);

#endif /* DIRVIEW_H */
