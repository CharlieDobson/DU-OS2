/*===========================================================================
 * PROPS.CPP  -  File, directory, and drive properties dialogs
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 *   GetProfileString("intl", ...)      ->  PrfQueryProfileString on OS2.INI's
 *                                          "PM_National" application, which is
 *                                          where OS/2 keeps the same settings
 *   _dos_getfileattr / _dos_setfileattr->  DosQueryPathInfo / DosSetPathInfo
 *   _dos_getdiskfree                   ->  DosQueryFSInfo(FSIL_ALLOC)
 *   _dos_findfirst(_A_VOLID)           ->  DosQueryFSInfo(FSIL_VOLSER)
 *
 * TWO PANELS HAVE NO OS/2 EQUIVALENT AND SHOW SOMETHING ELSE.
 *
 *   There is no version-information list: OS/2 executables carry no
 *   VERSIONINFO resource.  The slot shows the file's extended-attribute size
 *   instead, which is the OS/2-native fact a properties box ought to show.
 *
 *   There is no volume serial number: OS/2's FSINFO does not carry one.  The
 *   slot shows the attached file system name (FAT / HPFS / CDFS).
 *===========================================================================*/
#define INCL_WIN
#define INCL_WINSHELLDATA
#define INCL_DOSFILEMGR
#define INCL_DOSERRORS

#include <os2.h>
#include <string.h>
#include <stdio.h>

#include "res/resource.h"
#include "explorer.h"
#include "dirview.h"
#include "drives.h"
#include "props.h"

/*------------------------------------------------------------------
  Data blocks filled by the Do* helpers and read by the dialog procs
------------------------------------------------------------------*/

static struct {
    char  szFullPath[CCHMAXPATH];
    char  szName[CCHMAXPATHCOMP];
    char  szParentPath[CCHMAXPATH];
    char  szDateTime[64];
    char  szSize[64];
    char  szEASize[64];
    ULONG ulAttr;
} g_FileProps;

static struct {
    char  szName[CCHMAXPATHCOMP];
    char  szParentPath[CCHMAXPATH];
    char  szDateTime[64];
    ULONG ulAttr;
} g_DirProps;

static struct {
    char szDriveLetter[8];
    char szLabel[16];
    char szFSType[16];
    char szBPS[16];
    char szSPC[16];
    char szClusters[24];
    char szTotal[64];
    char szFree[64];
} g_DriveProps;

/*------------------------------------------------------------------
  NationalString / NationalInt - one PM_National setting
------------------------------------------------------------------*/
static void NationalString(const char *pszKey, const char *pszDefault,
                           char *pszOut, ULONG cbOut)
{
    pszOut[0] = '\0';

    PrfQueryProfileString(HINI_USERPROFILE, (PCSZ)"PM_National",
                          (PCSZ)pszKey, (PCSZ)pszDefault,
                          (PVOID)pszOut, cbOut);

    if (pszOut[0] == '\0')
        strcpy(pszOut, pszDefault);
}

static int NationalInt(const char *pszKey, int nDefault)
{
    return (int)PrfQueryProfileInt(HINI_USERPROFILE, (PCSZ)"PM_National",
                                   (PCSZ)pszKey, (LONG)nDefault);
}

/*------------------------------------------------------------------
  FormatDateTime - a file's date and time in the user's conventions

    iDate 0 = M/D/Y, 1 = D/M/Y, 2 = Y/M/D;  iTime 0 = 12-hour, 1 = 24-hour.
------------------------------------------------------------------*/
void FormatDateTime(FDATE fdate, FTIME ftime, char *pszOut)
{
    char szDateSep[8], szTimeSep[8];
    char szDatePart[48], szTimePart[48];
    int  iDate, iTime, dispHr;

    int yr = (int)fdate.year + 1980;
    int mo = (int)fdate.month;
    int dy = (int)fdate.day;
    int hr = (int)ftime.hours;
    int mn = (int)ftime.minutes;
    int sc = (int)ftime.twosecs * 2;

    NationalString("sDate", "/", szDateSep, sizeof(szDateSep));
    NationalString("sTime", ":", szTimeSep, sizeof(szTimeSep));
    iDate = NationalInt("iDate", 0);
    iTime = NationalInt("iTime", 1);

    if (iDate == 1)                     /* D/M/Y */
        sprintf(szDatePart, "%02d%s%02d%s%04d",
                dy, szDateSep, mo, szDateSep, yr);
    else if (iDate == 2)                /* Y/M/D */
        sprintf(szDatePart, "%04d%s%02d%s%02d",
                yr, szDateSep, mo, szDateSep, dy);
    else                                /* M/D/Y */
        sprintf(szDatePart, "%02d%s%02d%s%04d",
                mo, szDateSep, dy, szDateSep, yr);

    if (iTime == 0) {                   /* 12-hour */

        char  szAM[16], szPM[16];
        char *pszSuffix;

        NationalString("s1159", "AM", szAM, sizeof(szAM));
        NationalString("s2359", "PM", szPM, sizeof(szPM));

        if (hr == 0)            { dispHr = 12;      pszSuffix = szAM; }
        else if (hr < 12)       { dispHr = hr;      pszSuffix = szAM; }
        else if (hr == 12)      { dispHr = 12;      pszSuffix = szPM; }
        else                    { dispHr = hr - 12; pszSuffix = szPM; }

        sprintf(szTimePart, "%d%s%02d%s%02d %s",
                dispHr, szTimeSep, mn, szTimeSep, sc, pszSuffix);
    }
    else {                              /* 24-hour */
        sprintf(szTimePart, "%02d%s%02d%s%02d",
                hr, szTimeSep, mn, szTimeSep, sc);
    }

    sprintf(pszOut, "%s  %s", szDatePart, szTimePart);
}

/*------------------------------------------------------------------
  FormatGrouped - "N,NNN bytes" using the national group separator
------------------------------------------------------------------*/
static void FormatGrouped(unsigned __int64 cb, char *pszOut)
{
    char szSep[8];
    char szDigits[32];
    char szGrouped[48];
    int  nSepLen, nDigits, nOut = 0, i, k;

    NationalString("sThousand", ",", szSep, sizeof(szSep));
    nSepLen = (int)strlen(szSep);

    sprintf(szDigits, "%lu", (unsigned long)cb);
    nDigits = (int)strlen(szDigits);

    for (i = 0; i < nDigits; i++) {

        if (i > 0 && ((nDigits - i) % 3) == 0) {
            for (k = 0; k < nSepLen && nOut < (int)sizeof(szGrouped) - 4; k++)
                szGrouped[nOut++] = szSep[k];
        }

        if (nOut < (int)sizeof(szGrouped) - 2)
            szGrouped[nOut++] = szDigits[i];
    }

    szGrouped[nOut] = '\0';
    sprintf(pszOut, "%s bytes", szGrouped);
}

/*------------------------------------------------------------------
  StripTrailingSlash - tidy a path for display, but leave a root alone
------------------------------------------------------------------*/
static void StripTrailingSlash(char *pszPath)
{
    int n = (int)strlen(pszPath);

    if (n > 3 && pszPath[n - 1] == '\\')
        pszPath[n - 1] = '\0';
}

/*==================================================================
 * File properties
 *==================================================================*/

MRESULT EXPENTRY FilePropsDlgProc(HWND hwnd, ULONG msg,
                                  MPARAM mp1, MPARAM mp2)
{
    switch (msg) {

    case WM_INITDLG:
        WinSetDlgItemText(hwnd, IDC_FILENAME,   (PCSZ)g_FileProps.szName);
        WinSetDlgItemText(hwnd, IDC_PATHNAME,   (PCSZ)g_FileProps.szParentPath);
        WinSetDlgItemText(hwnd, IDC_LASTCHANGE, (PCSZ)g_FileProps.szDateTime);
        WinSetDlgItemText(hwnd, IDC_FILESIZE,   (PCSZ)g_FileProps.szSize);
        WinSetDlgItemText(hwnd, IDC_EASIZE,     (PCSZ)g_FileProps.szEASize);

        WinCheckButton(hwnd, IDC_READONLY,
                       (USHORT)((g_FileProps.ulAttr & FILE_READONLY) ? 1 : 0));
        WinCheckButton(hwnd, IDC_ARCHIVE,
                       (USHORT)((g_FileProps.ulAttr & FILE_ARCHIVED) ? 1 : 0));
        WinCheckButton(hwnd, IDC_HIDDEN,
                       (USHORT)((g_FileProps.ulAttr & FILE_HIDDEN)   ? 1 : 0));
        WinCheckButton(hwnd, IDC_SYSTEM,
                       (USHORT)((g_FileProps.ulAttr & FILE_SYSTEM)   ? 1 : 0));
        return (MRESULT)FALSE;

    case WM_COMMAND: {
        USHORT id = SHORT1FROMMP(mp1);

        if (id == DID_OK) {

            FILESTATUS3 fs3;
            ULONG       ulNew = 0;

            if (WinQueryButtonCheckstate(hwnd, IDC_READONLY))
                ulNew |= FILE_READONLY;
            if (WinQueryButtonCheckstate(hwnd, IDC_ARCHIVE))
                ulNew |= FILE_ARCHIVED;
            if (WinQueryButtonCheckstate(hwnd, IDC_HIDDEN))
                ulNew |= FILE_HIDDEN;
            if (WinQueryButtonCheckstate(hwnd, IDC_SYSTEM))
                ulNew |= FILE_SYSTEM;

            if (DosQueryPathInfo((PCSZ)g_FileProps.szFullPath, FIL_STANDARD,
                                 (PVOID)&fs3, sizeof(fs3)) == NO_ERROR) {
                fs3.attrFile = ulNew;
                DosSetPathInfo((PCSZ)g_FileProps.szFullPath, FIL_STANDARD,
                               (PVOID)&fs3, sizeof(fs3), 0UL);
            }

            WinDismissDlg(hwnd, DID_OK);
            return (MRESULT)FALSE;
        }

        if (id == DID_CANCEL) {
            WinDismissDlg(hwnd, DID_CANCEL);
            return (MRESULT)FALSE;
        }
        break;
    }
    }

    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

/*------------------------------------------------------------------
  DoFileProperties - gather the facts and put up the dialog
------------------------------------------------------------------*/
void DoFileProperties(HWND hwnd, int nIdx)
{
    FILEITEM   *pFile;
    FILESTATUS4 fs4;

    pFile = GetFileItem(nIdx);
    if (pFile == NULL)
        return;

    strcpy(g_FileProps.szFullPath, szCurrentPath);
    EnsureTrailingSlash(g_FileProps.szFullPath);
    strcat(g_FileProps.szFullPath, pFile->szName);

    strcpy(g_FileProps.szName, pFile->szName);

    strcpy(g_FileProps.szParentPath, szCurrentPath);
    StripTrailingSlash(g_FileProps.szParentPath);

    FormatDateTime(pFile->fdate, pFile->ftime, g_FileProps.szDateTime);
    FormatGrouped((unsigned __int64)pFile->cbFile, g_FileProps.szSize);

    /* Attributes and the extended-attribute size, read fresh from disk.
       FIL_QUERYEASIZE fills a FILESTATUS4, whose cbList is the total size
       of the file's extended attributes. */
    memset(&fs4, 0, sizeof(fs4));

    if (DosQueryPathInfo((PCSZ)g_FileProps.szFullPath, FIL_QUERYEASIZE,
                         (PVOID)&fs4, sizeof(fs4)) == NO_ERROR) {
        g_FileProps.ulAttr = fs4.attrFile;
        FormatGrouped((unsigned __int64)fs4.cbList, g_FileProps.szEASize);
    }
    else {
        g_FileProps.ulAttr = pFile->attrFile;
        strcpy(g_FileProps.szEASize, "(not available)");
    }

    if (WinDlgBox(HWND_DESKTOP, hwnd, FilePropsDlgProc,
                  NULLHANDLE, IDD_FILEPROPERTIES, NULL) == DID_OK) {

        /* The dialog may have written new attributes; refresh the cached
           copy in place so the details view stays accurate without
           losing the selection or the scroll position. */
        FILESTATUS3 fs3;

        if (DosQueryPathInfo((PCSZ)g_FileProps.szFullPath, FIL_STANDARD,
                             (PVOID)&fs3, sizeof(fs3)) == NO_ERROR) {
            pFile->attrFile = fs3.attrFile;
            WinInvalidateRect(hwnd, NULL, FALSE);
        }
    }
}

/*==================================================================
 * Directory properties
 *==================================================================*/

MRESULT EXPENTRY DirPropsDlgProc(HWND hwnd, ULONG msg,
                                 MPARAM mp1, MPARAM mp2)
{
    switch (msg) {

    case WM_INITDLG:
        WinSetDlgItemText(hwnd, IDC_DIRECTORYNAME,
                          (PCSZ)g_DirProps.szName);
        WinSetDlgItemText(hwnd, IDC_DIRECTORYPATH,
                          (PCSZ)g_DirProps.szParentPath);
        WinSetDlgItemText(hwnd, IDC_DIRECTORYLASTCHANGE,
                          (PCSZ)g_DirProps.szDateTime);

        /* Display only - the template disables these */
        WinCheckButton(hwnd, IDC_DIRREADONLY,
                       (USHORT)((g_DirProps.ulAttr & FILE_READONLY) ? 1 : 0));
        WinCheckButton(hwnd, IDC_DIRARCHIVE,
                       (USHORT)((g_DirProps.ulAttr & FILE_ARCHIVED) ? 1 : 0));
        WinCheckButton(hwnd, IDC_DIRHIDDEN,
                       (USHORT)((g_DirProps.ulAttr & FILE_HIDDEN)   ? 1 : 0));
        WinCheckButton(hwnd, IDC_DIRSYSTEM,
                       (USHORT)((g_DirProps.ulAttr & FILE_SYSTEM)   ? 1 : 0));
        return (MRESULT)FALSE;

    case WM_COMMAND:
        if (SHORT1FROMMP(mp1) == DID_OK ||
            SHORT1FROMMP(mp1) == DID_CANCEL) {
            WinDismissDlg(hwnd, SHORT1FROMMP(mp1));
            return (MRESULT)FALSE;
        }
        break;
    }

    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

void DoDirProperties(HWND hwnd, int nIdx)
{
    FILEITEM   *pFile;
    FILESTATUS3 fs3;
    char        szFull[CCHMAXPATH];

    pFile = GetFileItem(nIdx);
    if (pFile == NULL)
        return;

    strcpy(g_DirProps.szName, pFile->szName);

    strcpy(g_DirProps.szParentPath, szCurrentPath);
    StripTrailingSlash(g_DirProps.szParentPath);

    FormatDateTime(pFile->fdate, pFile->ftime, g_DirProps.szDateTime);

    strcpy(szFull, szCurrentPath);
    EnsureTrailingSlash(szFull);
    strcat(szFull, pFile->szName);

    if (DosQueryPathInfo((PCSZ)szFull, FIL_STANDARD,
                         (PVOID)&fs3, sizeof(fs3)) == NO_ERROR)
        g_DirProps.ulAttr = fs3.attrFile;
    else
        g_DirProps.ulAttr = pFile->attrFile;

    WinDlgBox(HWND_DESKTOP, hwnd, DirPropsDlgProc,
              NULLHANDLE, IDD_DIRPROPERTIES, NULL);
}

/*------------------------------------------------------------------
  DoProperties - dispatch to the file or the directory dialog
------------------------------------------------------------------*/
void DoProperties(HWND hwnd, int nIdx)
{
    FILEITEM *pFile;

    if (nIdx < 0)
        return;

    pFile = GetFileItem(nIdx);
    if (pFile == NULL)
        return;

    if (pFile->bIsDir)
        DoDirProperties(hwnd, nIdx);
    else
        DoFileProperties(hwnd, nIdx);
}

/*==================================================================
 * Drive properties
 *==================================================================*/

MRESULT EXPENTRY DrivePropsDlgProc(HWND hwnd, ULONG msg,
                                   MPARAM mp1, MPARAM mp2)
{
    switch (msg) {

    case WM_INITDLG:
        WinSetDlgItemText(hwnd, IDC_DRIVEPROPS_LETTER,
                          (PCSZ)g_DriveProps.szDriveLetter);
        WinSetDlgItemText(hwnd, IDC_DRIVEPROPS_LABEL,
                          (PCSZ)g_DriveProps.szLabel);
        WinSetDlgItemText(hwnd, IDC_DRIVEPROPS_FSTYPE,
                          (PCSZ)g_DriveProps.szFSType);
        WinSetDlgItemText(hwnd, IDC_DRIVEPROPS_BPS,
                          (PCSZ)g_DriveProps.szBPS);
        WinSetDlgItemText(hwnd, IDC_DRIVEPROPS_SPC,
                          (PCSZ)g_DriveProps.szSPC);
        WinSetDlgItemText(hwnd, IDC_DRIVEPROPS_CLUSTERS,
                          (PCSZ)g_DriveProps.szClusters);
        WinSetDlgItemText(hwnd, IDC_DRIVEPROPS_TOTAL,
                          (PCSZ)g_DriveProps.szTotal);
        WinSetDlgItemText(hwnd, IDC_DRIVEPROPS_FREE,
                          (PCSZ)g_DriveProps.szFree);
        return (MRESULT)FALSE;

    case WM_COMMAND:
        if (SHORT1FROMMP(mp1) == DID_OK ||
            SHORT1FROMMP(mp1) == DID_CANCEL) {
            WinDismissDlg(hwnd, SHORT1FROMMP(mp1));
            return (MRESULT)FALSE;
        }
        break;
    }

    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

/*------------------------------------------------------------------
  DoDriveProperties - gather the volume's facts and put up the dialog

    Soft-fails with a message if the volume is not ready.
------------------------------------------------------------------*/
void DoDriveProperties(HWND hwnd, int nDriveIdx)
{
    DRIVEINFO       *pDrv;
    FSALLOCATE       fsa;
    FSINFO           fsi;
    unsigned __int64 bpu, cbTotal, cbFree;
    char             szMsg[80];
    char             cDrive;
    ULONG            ulDrive;

    pDrv = GetDriveInfo(nDriveIdx);
    if (pDrv == NULL)
        return;

    cDrive  = pDrv->szLabel[0];
    ulDrive = (ULONG)(cDrive - 'A' + 1);

    memset(&fsa, 0, sizeof(fsa));

    if (DosQueryFSInfo(ulDrive, FSIL_ALLOC,
                       (PVOID)&fsa, sizeof(fsa)) != NO_ERROR ||
        fsa.cUnit == 0) {
        sprintf(szMsg, "Drive %c: is not ready.", cDrive);
        ExpMessageBox(hwnd, szMsg, MB_OK | MB_WARNING);
        return;
    }

    strcpy(g_DriveProps.szDriveLetter, pDrv->szLabel);
    strcpy(g_DriveProps.szFSType,
           pDrv->szFSType[0] ? pDrv->szFSType : "(unknown)");

    /* Volume label.  VOLUMELABEL carries its own length and is not
       null-terminated when it fills the field. */
    memset(&fsi, 0, sizeof(fsi));

    if (DosQueryFSInfo(ulDrive, FSIL_VOLSER,
                       (PVOID)&fsi, sizeof(fsi)) == NO_ERROR &&
        fsi.vol.cch > 0) {

        ULONG cch = fsi.vol.cch;

        if (cch > sizeof(g_DriveProps.szLabel) - 1)
            cch = sizeof(g_DriveProps.szLabel) - 1;

        memcpy(g_DriveProps.szLabel, fsi.vol.szVolLabel, (size_t)cch);
        g_DriveProps.szLabel[cch] = '\0';
    }
    else {
        strcpy(g_DriveProps.szLabel, "(No label)");
    }

    sprintf(g_DriveProps.szBPS,      "%lu", (unsigned long)fsa.cbSector);
    sprintf(g_DriveProps.szSPC,      "%lu", (unsigned long)fsa.cSectorUnit);
    sprintf(g_DriveProps.szClusters, "%lu", (unsigned long)fsa.cUnit);

    bpu     = (unsigned __int64)fsa.cSectorUnit *
              (unsigned __int64)fsa.cbSector;
    cbTotal = (unsigned __int64)fsa.cUnit      * bpu;
    cbFree  = (unsigned __int64)fsa.cUnitAvail * bpu;

    FormatGrouped(cbTotal, g_DriveProps.szTotal);
    FormatGrouped(cbFree,  g_DriveProps.szFree);

    WinDlgBox(HWND_DESKTOP, hwnd, DrivePropsDlgProc,
              NULLHANDLE, IDD_DRIVEPROPERTIES, NULL);
}
