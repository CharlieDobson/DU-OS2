/*===========================================================================
 * DRIVES.CPP  -  Drive enumeration: presence, kind, and the path combo
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 *   GetDriveType(i)          ->  DosQueryCurrentDisk gives a bitmap of the
 *                                drives that exist; DosQueryFSAttach says
 *                                whether each one is local or remote and
 *                                which file system is attached to it.
 *
 *   INT 2Fh AX=150Bh MSCDEX  ->  gone.  The DOS build had to ask MSCDEX
 *                                which letters it owned because Windows
 *                                reported CD-ROMs as network drives.  OS/2
 *                                attaches CDs through a file system driver
 *                                called CDFS, which names itself.
 *
 * Neither call touches the media, so enumerating drives still does not spin
 * up a floppy or a CD.
 *===========================================================================*/
#define INCL_DOSFILEMGR
#define INCL_DOSMISC
#define INCL_DOSDEVICES
#define INCL_DOSERRORS
#define INCL_WIN

#include <os2.h>
#include <string.h>
#include <stdio.h>

#include "res/resource.h"
#include "drives.h"
#include "explorer.h"

static DRIVEINFO aDrives[MAX_DRIVES];
static int       nDriveCount = 0;

/*------------------------------------------------------------------
  BitmapIDsForType - map a DRV_* kind to the three bitmap ids
------------------------------------------------------------------*/
static void BitmapIDsForType(int nType, int *pSm, int *pMd, int *pLg)
{
    switch (nType) {

    case DRV_REMOVABLE:
        *pSm = IDB_SM_DRVFLOP;  *pMd = IDB_MD_DRVFLOP;  *pLg = IDB_LG_DRVFLOP;
        break;

    case DRV_REMOTE:
        *pSm = IDB_SM_DRVNET;   *pMd = IDB_MD_DRVNET;   *pLg = IDB_LG_DRVNET;
        break;

    case DRV_CDROM:
        *pSm = IDB_SM_DRVCDROM; *pMd = IDB_MD_DRVCDROM; *pLg = IDB_LG_DRVCDROM;
        break;

    case DRV_FIXED:
    case DRV_RAMDISK:
    default:
        *pSm = IDB_SM_DRVLOCAL; *pMd = IDB_MD_DRVLOCAL; *pLg = IDB_LG_DRVLOCAL;
        break;
    }
}

/*------------------------------------------------------------------
  QueryDriveKind - classify one drive from its attached file system

    DosQueryFSAttach reports the attachment, not the volume, so it answers
    without reading the disk.  iType separates local from remote, and the FSD
    name identifies a CD.  Nothing in OS/2 says "this block device is
    removable" without opening the drive as DASD, so A: and B: are taken as
    removable by convention.
------------------------------------------------------------------*/
static int QueryDriveKind(char cDrive, char *pszFSType, int cbFSType)
{
    /* FSQBUFFER2's three trailing names are variable length; over-allocate */
    BYTE        buf[sizeof(FSQBUFFER2) + 3 * CCHMAXPATH];
    PFSQBUFFER2 pfsq = (PFSQBUFFER2)buf;
    ULONG       cb;
    char        szDev[3];
    const char *pszFSD;
    int         nType;

    pszFSType[0] = '\0';

    szDev[0] = cDrive;
    szDev[1] = ':';
    szDev[2] = '\0';

    cb = sizeof(buf);
    if (DosQueryFSAttach((PCSZ)szDev, 0, FSAIL_QUERYNAME, pfsq, &cb)
        != NO_ERROR) {
        /* Present in the drive map but nothing will say what it is */
        return DRV_FIXED;
    }

    /* szFSDName follows szName in the packed tail */
    pszFSD = (const char *)pfsq->szName + pfsq->cbName + 1;

    strncpy(pszFSType, pszFSD, (size_t)cbFSType - 1);
    pszFSType[cbFSType - 1] = '\0';

    if (pfsq->iType == FSAT_REMOTEDRV)
        nType = DRV_REMOTE;
    else if (strcmp(pszFSD, "CDFS") == 0)
        nType = DRV_CDROM;
    else if (cDrive == 'A' || cDrive == 'B')
        nType = DRV_REMOVABLE;
    else
        nType = DRV_FIXED;

    return nType;
}

/*------------------------------------------------------------------
  EnumDrives - scan A-Z; returns the number present

    The drive map is not quite the list of drives the user has.  OS/2 keeps
    the PC convention of reporting B: on a machine with only one diskette
    drive, so that a program can address that drive as either letter -- so
    B: appears in the map on hardware that has no such drive.  Listing it
    gets the user a drive icon that can only ever answer "not ready".
    DosDevConfig counts the drives that physically exist, which is the one
    question the drive map does not answer.
------------------------------------------------------------------*/
int EnumDrives(void)
{
    ULONG ulCurrent = 0;
    ULONG ulMap     = 0;
    BYTE  bFloppies = 2;        /* if the count fails, believe the map */
    int   i;

    nDriveCount = 0;

    if (DosQueryCurrentDisk(&ulCurrent, &ulMap) != NO_ERROR)
        return 0;

    if (DosDevConfig(&bFloppies, DEVINFO_FLOPPY) != NO_ERROR)
        bFloppies = 2;

    for (i = 0; i < MAX_DRIVES; i++) {

        char cLetter = (char)('A' + i);

        if (!(ulMap & (1UL << i)))
            continue;

        /* the phantom second diskette drive */
        if (cLetter == 'B' && bFloppies < 2)
            continue;

        aDrives[nDriveCount].szLabel[0] = cLetter;
        aDrives[nDriveCount].szLabel[1] = ':';
        aDrives[nDriveCount].szLabel[2] = '\\';
        aDrives[nDriveCount].szLabel[3] = '\0';

        aDrives[nDriveCount].nType =
            QueryDriveKind(cLetter,
                           aDrives[nDriveCount].szFSType,
                           (int)sizeof(aDrives[nDriveCount].szFSType));

        BitmapIDsForType(aDrives[nDriveCount].nType,
                         &aDrives[nDriveCount].nBmpSm,
                         &aDrives[nDriveCount].nBmpMd,
                         &aDrives[nDriveCount].nBmpLg);

        nDriveCount++;
    }

    return nDriveCount;
}

int GetDriveCount(void)
{
    return nDriveCount;
}

/*------------------------------------------------------------------
  GetDriveInfo - 0-based index into the present-drive list
------------------------------------------------------------------*/
DRIVEINFO *GetDriveInfo(int i)
{
    if (i < 0 || i >= nDriveCount)
        return NULL;

    return &aDrives[i];
}

/*------------------------------------------------------------------
  FindDriveInfo - look a drive up by letter, either case
------------------------------------------------------------------*/
DRIVEINFO *FindDriveInfo(char cDrive)
{
    int i;

    if (cDrive >= 'a' && cDrive <= 'z')
        cDrive = (char)(cDrive - 'a' + 'A');

    for (i = 0; i < nDriveCount; i++) {
        if (aDrives[i].szLabel[0] == cDrive)
            return &aDrives[i];
    }

    return NULL;
}

/*------------------------------------------------------------------
  PopulatePathCombo - fill the toolbar combo with drive labels

    Windows' CB_RESETCONTENT / CB_ADDSTRING become PM's LM_* list messages:
    a PM combobox forwards them to the listbox it owns.
------------------------------------------------------------------*/
void PopulatePathCombo(HWND hwndCombo)
{
    int i;

    if (hwndCombo == NULLHANDLE)
        return;

    WinSendMsg(hwndCombo, LM_DELETEALL, MPVOID, MPVOID);

    for (i = 0; i < nDriveCount; i++) {
        WinSendMsg(hwndCombo, LM_INSERTITEM,
                   MPFROMLONG(LIT_END),
                   MPFROMP(aDrives[i].szLabel));
    }

    if (nDriveCount > 0) {
        /* Seeding the combo is not the user picking a drive, and PM's
           LM_SELECTITEM notifies where Windows' CB_SETCURSEL did not.  */
        bComboSyncing = TRUE;
        WinSendMsg(hwndCombo, LM_SELECTITEM,
                   MPFROMLONG(0), MPFROMSHORT(TRUE));
        bComboSyncing = FALSE;
    }
}
