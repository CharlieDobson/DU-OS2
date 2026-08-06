/*===========================================================================
 * NETWORK.CPP  -  Network drive dialogs: connect, disconnect, browse
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * THE LEAST CERTAIN FILE IN THE PORT.  READ THE WARNING BELOW BEFORE
 * TRUSTING ANYTHING HERE.
 *
 *   OS/2 has no redirector in the base operating system, so mapping drives
 *   and browsing the network both go through one place: the IBM LAN
 *   Requester / LAN Server API in NETAPI32.DLL.
 *
 *      WNetAddConnection      ->  NetUseAdd(level 1)
 *      WNetCancelConnection   ->  NetUseDel(USE_NOFORCE / USE_FORCE)
 *      WNetGetConnection      ->  NetUseGetInfo(level 1) -> ui1_remote
 *      GetSystemMetrics(SM_NETWORK) + a WNet probe
 *                             ->  "did NETAPI32.DLL load and resolve"
 *      NetServerEnum2 / NetShareEnum / NetGetDCName
 *                             ->  the same calls, 32-bit flavour
 *      LoadLibrary/GetProcAddress -> DosLoadModule / DosQueryProcAddr
 *
 *   The dialogs, the "+"/"-" expanding browse list, the path history and
 *   the reconnect-at-startup list all keep their behaviour; the history and
 *   reconnect entries move from EXPLORER.INI to OS2.INI.
 *
 * !! UNVERIFIED ABI !!
 *
 *   Open Watcom ships no LAN Server headers, so the structures and
 *   prototypes below are written by hand from the published API and have
 *   never been compiled against a real header nor called against a real
 *   redirector.  The 32-bit API is naturally aligned -- explicit padding
 *   fields are spelled out so the intended layout is stated rather than
 *   left to the compiler -- and every entry point is resolved by name at
 *   run time, so on a machine without LAN Requester nothing here is ever
 *   reached: LoadNetApi fails, IsNetworkPresent returns FALSE, and the
 *   Network menu is disabled.  That is what limits the blast radius.
 *===========================================================================*/
#define INCL_WIN
#define INCL_WINSHELLDATA
#define INCL_DOSMODULEMGR
#define INCL_DOSERRORS

#include <os2.h>
#include <string.h>
#include <stdio.h>

#include "res/resource.h"
#include "explorer.h"
#include "drives.h"
#include "network.h"

/*---------------------------------------------------------------------------
 * LAN Requester constants
 *-------------------------------------------------------------------------*/
#define NETAPI_CNLEN    15          /* computer name length */
#define NETAPI_NNLEN    12          /* net (share) name length */

#define USE_DISKDEV     0
#define USE_NOFORCE     0
#define USE_FORCE       1

#define SV_TYPE_ALL         0xFFFFFFFFUL
#define SV_TYPE_DOMAIN_ENUM 0x80000000UL

/* NERR_BASE is 2100; the two we care about are documented offsets from it */
#define NERR_UseNotFound    2250UL      /* NERR_BASE + 150 */
#define NERR_OpenFiles      2401UL      /* NERR_BASE + 301 */

/*---------------------------------------------------------------------------
 * LAN Requester structures - see the ABI warning in the file header
 *-------------------------------------------------------------------------*/

typedef struct _USE_INFO_1 {
    PSZ    ui1_local;               /* "X:"                    */
    PSZ    ui1_remote;              /* "\\\\server\\share"     */
    PSZ    ui1_password;
    USHORT ui1_status;
    USHORT ui1_asg_type;
    USHORT ui1_refcount;
    USHORT ui1_usecount;
} USE_INFO_1;

typedef struct _SERVER_INFO_0 {
    UCHAR sv0_name[NETAPI_CNLEN + 1];       /* 16 bytes */
} SERVER_INFO_0;

typedef struct _SERVER_INFO_1 {
    UCHAR sv1_name[NETAPI_CNLEN + 1];       /* offset  0, 16 bytes */
    UCHAR sv1_version_major;                /* offset 16           */
    UCHAR sv1_version_minor;                /* offset 17           */
    UCHAR sv1_pad1[2];                      /* offset 18, to align */
    ULONG sv1_type;                         /* offset 20           */
    PSZ   sv1_comment;                      /* offset 24           */
} SERVER_INFO_1;                            /* size   28           */

typedef struct _SHARE_INFO_1 {
    UCHAR  shi1_netname[NETAPI_NNLEN + 1];  /* offset  0, 13 bytes */
    UCHAR  shi1_pad1;                       /* offset 13           */
    USHORT shi1_type;                       /* offset 14           */
    PSZ    shi1_remark;                     /* offset 16           */
} SHARE_INFO_1;                             /* size   20           */

/*---------------------------------------------------------------------------
 * Entry points
 *-------------------------------------------------------------------------*/

typedef ULONG (APIENTRY *PFNNETUSEADD)(PSZ, ULONG, PBYTE, ULONG);
typedef ULONG (APIENTRY *PFNNETUSEDEL)(PSZ, PSZ, ULONG);
typedef ULONG (APIENTRY *PFNNETUSEGETINFO)(PSZ, PSZ, ULONG, PBYTE, ULONG,
                                           PULONG);
typedef ULONG (APIENTRY *PFNNETSERVERENUM2)(PSZ, ULONG, PBYTE, ULONG,
                                            PULONG, PULONG, ULONG, PSZ);
typedef ULONG (APIENTRY *PFNNETSHAREENUM)(PSZ, ULONG, PBYTE, ULONG,
                                          PULONG, PULONG);
typedef ULONG (APIENTRY *PFNNETGETDCNAME)(PSZ, PSZ, PBYTE, ULONG);

static HMODULE           s_hNetApi        = NULLHANDLE;
static BOOL              s_bNetApiTried   = FALSE;
static PFNNETUSEADD      s_pfnUseAdd      = NULL;
static PFNNETUSEDEL      s_pfnUseDel      = NULL;
static PFNNETUSEGETINFO  s_pfnUseGetInfo  = NULL;
static PFNNETSERVERENUM2 s_pfnServerEnum2 = NULL;
static PFNNETSHAREENUM   s_pfnShareEnum   = NULL;
static PFNNETGETDCNAME   s_pfnGetDCName   = NULL;

/* The computer currently picked in the browse list ("\\NAME", or "") */
static char s_szCurrentComp[NETAPI_CNLEN + 8] = "";

/* Scratch buffer for the enumerations.  Static rather than automatic: 16 KB
   has no business on a window procedure's stack. */
static BYTE s_abEnum[16384];

/*---------------------------------------------------------------------------
 * OS2.INI applications.
 *-------------------------------------------------------------------------*/
#define INI_APP_HISTORY    "Explorer_NetHistory"
#define INI_APP_RECONNECT  "Explorer_NetReconnect"
#define MAX_NET_HISTORY    10

/*------------------------------------------------------------------
  ProcAddr - resolve one entry point, trying the mixed-case name first

    LAN Requester exports mixed case; some builds and some third-party
    stacks export upper case only.  Try both.
------------------------------------------------------------------*/
static PFN ProcAddr(const char *pszMixed, const char *pszUpper)
{
    PFN pfn = NULL;

    if (DosQueryProcAddr(s_hNetApi, 0, (PCSZ)pszMixed, &pfn) == NO_ERROR)
        return pfn;

    pfn = NULL;
    if (DosQueryProcAddr(s_hNetApi, 0, (PCSZ)pszUpper, &pfn) == NO_ERROR)
        return pfn;

    return NULL;
}

/*------------------------------------------------------------------
  LoadNetApi - load NETAPI32.DLL once and resolve what we need

    Returns TRUE only if the two calls that actually map a drive resolved.
    Browsing is optional: without NetServerEnum2 the dialog still connects,
    it just cannot show a tree.
------------------------------------------------------------------*/
static BOOL LoadNetApi(void)
{
    char szErr[CCHMAXPATH];

    if (s_bNetApiTried)
        return (BOOL)(s_pfnUseAdd != NULL && s_pfnUseDel != NULL);

    s_bNetApiTried = TRUE;

    szErr[0] = '\0';
    if (DosLoadModule((PSZ)szErr, sizeof(szErr),
                      (PCSZ)"NETAPI32", &s_hNetApi) != NO_ERROR) {
        s_hNetApi = NULLHANDLE;
        return FALSE;
    }

    s_pfnUseAdd      = (PFNNETUSEADD)     ProcAddr("NetUseAdd", "NETUSEADD");
    s_pfnUseDel      = (PFNNETUSEDEL)     ProcAddr("NetUseDel", "NETUSEDEL");
    s_pfnUseGetInfo  = (PFNNETUSEGETINFO) ProcAddr("NetUseGetInfo",
                                                   "NETUSEGETINFO");
    s_pfnServerEnum2 = (PFNNETSERVERENUM2)ProcAddr("NetServerEnum2",
                                                   "NETSERVERENUM2");
    if (s_pfnServerEnum2 == NULL)
        s_pfnServerEnum2 = (PFNNETSERVERENUM2)ProcAddr("NetServerEnum",
                                                       "NETSERVERENUM");

    s_pfnShareEnum   = (PFNNETSHAREENUM)  ProcAddr("NetShareEnum",
                                                   "NETSHAREENUM");
    s_pfnGetDCName   = (PFNNETGETDCNAME)  ProcAddr("NetGetDCName",
                                                   "NETGETDCNAME");

    return (BOOL)(s_pfnUseAdd != NULL && s_pfnUseDel != NULL);
}

BOOL IsNetworkPresent(void)
{
    return LoadNetApi();
}

/*------------------------------------------------------------------
  QueryConnection - the UNC path a drive letter is mapped to
------------------------------------------------------------------*/
static BOOL QueryConnection(char cDrive, char *pszOut, ULONG cbOut)
{
    USE_INFO_1 *pui;
    char        szLocal[8];
    ULONG       cbAvail = 0;

    pszOut[0] = '\0';

    if (!LoadNetApi() || s_pfnUseGetInfo == NULL)
        return FALSE;

    sprintf(szLocal, "%c:", cDrive);

    if (s_pfnUseGetInfo(NULL, (PSZ)szLocal, 1UL,
                        s_abEnum, sizeof(s_abEnum), &cbAvail) != NO_ERROR)
        return FALSE;

    pui = (USE_INFO_1 *)s_abEnum;

    if (pui->ui1_remote == NULL || pui->ui1_remote[0] == '\0')
        return FALSE;

    strncpy(pszOut, (const char *)pui->ui1_remote, (size_t)cbOut - 1);
    pszOut[cbOut - 1] = '\0';
    return TRUE;
}

/*==================================================================
 * OS2.INI helpers
 *==================================================================*/

static void LoadNetHistory(HWND hwnd, ULONG idCtrl)
{
    char  szKey[16];
    char  szPath[CCHMAXPATH];
    int   i;

    for (i = 1; i <= MAX_NET_HISTORY; i++) {

        sprintf(szKey, "Path%d", i);
        szPath[0] = '\0';

        PrfQueryProfileString(HINI_USERPROFILE, (PCSZ)INI_APP_HISTORY,
                              (PCSZ)szKey, (PCSZ)"",
                              (PVOID)szPath, sizeof(szPath));

        if (szPath[0] != '\0')
            WinSendDlgItemMsg(hwnd, idCtrl, LM_INSERTITEM,
                              MPFROMLONG(LIT_END), MPFROMP(szPath));
    }
}

static void SaveNetHistory(const char *pszNewPath)
{
    char aPaths[MAX_NET_HISTORY][CCHMAXPATH];
    char szKey[16];
    char szExisting[CCHMAXPATH];
    int  nCount = 0;
    int  i;

    for (i = 1; i <= MAX_NET_HISTORY && nCount < MAX_NET_HISTORY - 1; i++) {

        sprintf(szKey, "Path%d", i);
        szExisting[0] = '\0';

        PrfQueryProfileString(HINI_USERPROFILE, (PCSZ)INI_APP_HISTORY,
                              (PCSZ)szKey, (PCSZ)"",
                              (PVOID)szExisting, sizeof(szExisting));

        if (szExisting[0] != '\0' && stricmp(szExisting, pszNewPath) != 0)
            strcpy(aPaths[nCount++], szExisting);
    }

    PrfWriteProfileString(HINI_USERPROFILE, (PCSZ)INI_APP_HISTORY,
                          (PCSZ)"Path1", (PCSZ)pszNewPath);

    for (i = 0; i < nCount; i++) {
        sprintf(szKey, "Path%d", i + 2);
        PrfWriteProfileString(HINI_USERPROFILE, (PCSZ)INI_APP_HISTORY,
                              (PCSZ)szKey, (PCSZ)aPaths[i]);
    }

    /* NULL as the value deletes the key */
    for (i = nCount + 2; i <= MAX_NET_HISTORY; i++) {
        sprintf(szKey, "Path%d", i);
        PrfWriteProfileString(HINI_USERPROFILE, (PCSZ)INI_APP_HISTORY,
                              (PCSZ)szKey, (PCSZ)NULL);
    }
}

/*------------------------------------------------------------------
  SaveReconnectEntry - remember, or forget, one drive's mapping
------------------------------------------------------------------*/
static void SaveReconnectEntry(const char *pszLocal, const char *pszPath,
                               BOOL bReconnect)
{
    char szKey[4];

    PrfWriteProfileString(HINI_USERPROFILE, (PCSZ)INI_APP_HISTORY,
                          (PCSZ)"ReconnectAtStartup",
                          (PCSZ)(bReconnect ? "1" : "0"));

    szKey[0] = pszLocal[0];
    if (szKey[0] >= 'a' && szKey[0] <= 'z')
        szKey[0] = (char)(szKey[0] - 'a' + 'A');
    szKey[1] = '\0';

    PrfWriteProfileString(HINI_USERPROFILE, (PCSZ)INI_APP_RECONNECT,
                          (PCSZ)szKey,
                          (PCSZ)(bReconnect ? pszPath : NULL));
}

/*------------------------------------------------------------------
  MapDrive - NetUseAdd wrapper
------------------------------------------------------------------*/
static ULONG MapDrive(const char *pszLocal, const char *pszRemote)
{
    USE_INFO_1 ui;
    char       szLocal[8];
    char       szRemote[CCHMAXPATH];
    char       szPassword[2];

    if (!LoadNetApi())
        return ERROR_NOT_SUPPORTED;

    strcpy(szLocal, pszLocal);
    strncpy(szRemote, pszRemote, sizeof(szRemote) - 1);
    szRemote[sizeof(szRemote) - 1] = '\0';
    szPassword[0] = '\0';

    memset(&ui, 0, sizeof(ui));
    ui.ui1_local    = (PSZ)szLocal;
    ui.ui1_remote   = (PSZ)szRemote;
    ui.ui1_password = (PSZ)szPassword;
    ui.ui1_asg_type = USE_DISKDEV;

    return s_pfnUseAdd(NULL, 1UL, (PBYTE)&ui, sizeof(ui));
}

/*------------------------------------------------------------------
  UnmapDrive - NetUseDel wrapper
------------------------------------------------------------------*/
static ULONG UnmapDrive(const char *pszLocal, BOOL bForce)
{
    char szLocal[8];

    if (!LoadNetApi())
        return ERROR_NOT_SUPPORTED;

    strcpy(szLocal, pszLocal);

    return s_pfnUseDel(NULL, (PSZ)szLocal,
                       bForce ? USE_FORCE : USE_NOFORCE);
}

/*------------------------------------------------------------------
  ReportUseError - the shared message for a failed NetUseAdd
------------------------------------------------------------------*/
static void ReportUseError(HWND hwnd, ULONG ulRet, const char *pszLocal)
{
    char szMsg[CCHMAXPATH + 160];

    switch (ulRet) {

    case ERROR_ALREADY_ASSIGNED:
        sprintf(szMsg,
                "Drive %s is already connected to a network path.\n"
                "Disconnect it first.", pszLocal);
        break;

    case ERROR_ACCESS_DENIED:
    case ERROR_INVALID_PASSWORD:
        strcpy(szMsg,
               "Access was denied. Check the path and ensure you have "
               "permission to connect.");
        break;

    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        strcpy(szMsg,
               "The network path was not found or is invalid.\n"
               "Use the form \\\\server\\share");
        break;

    case ERROR_NOT_SUPPORTED:
        strcpy(szMsg,
               "Network connections are not supported: the LAN Requester "
               "is not installed.");
        break;

    default:
        sprintf(szMsg,
                "Could not connect to the network path (error %lu).",
                (unsigned long)ulRet);
        break;
    }

    WinMessageBox(HWND_DESKTOP, hwnd, (PCSZ)szMsg,
                  (PCSZ)"Connect Network Drive", 0, MB_OK | MB_WARNING);
}

/*==================================================================
 * Browsing
 *==================================================================*/

/*------------------------------------------------------------------
  ListItemText - bounded read of one list item

    PM's LM_QUERYITEMTEXT takes the buffer size, so no scratch buffer is
    needed to bound the read.
------------------------------------------------------------------*/
static void ListItemText(HWND hwnd, ULONG idCtrl, int nIdx,
                         char *pszOut, int cbOut)
{
    pszOut[0] = '\0';

    WinSendDlgItemMsg(hwnd, idCtrl, LM_QUERYITEMTEXT,
                      MPFROM2SHORT((SHORT)nIdx, (SHORT)cbOut),
                      MPFROMP(pszOut));
}

static int ListItemCount(HWND hwnd, ULONG idCtrl)
{
    return (int)(LONG)WinSendDlgItemMsg(hwnd, idCtrl, LM_QUERYITEMCOUNT,
                                        MPVOID, MPVOID);
}

static void ListInsert(HWND hwnd, ULONG idCtrl, int nIdx, const char *psz)
{
    WinSendDlgItemMsg(hwnd, idCtrl, LM_INSERTITEM,
                      MPFROMLONG((LONG)nIdx), MPFROMP(psz));
}

static void ListDelete(HWND hwnd, ULONG idCtrl, int nIdx)
{
    WinSendDlgItemMsg(hwnd, idCtrl, LM_DELETEITEM,
                      MPFROMLONG((LONG)nIdx), MPVOID);
}

/*------------------------------------------------------------------
  ParseComputerName - "  \\NAME" or "  \\NAME<tab>desc" -> "\\NAME"
------------------------------------------------------------------*/
static BOOL ParseComputerName(const char *pszItem, char *pszOut, int cbOut)
{
    const char *p;
    int         n = 0;

    if (pszItem[0] != ' ' || pszItem[1] != ' ')
        return FALSE;

    p = pszItem + 2;
    if (p[0] != '\\' || p[1] != '\\')
        return FALSE;

    while (*p != '\0' && *p != '\t' && *p != ' ') {
        if (n >= cbOut - 1)
            return FALSE;
        pszOut[n++] = *p++;
    }

    pszOut[n] = '\0';
    return (BOOL)(n > 2);
}

static void SetSharesLabel(HWND hwnd, const char *pszServer)
{
    char szLabel[NETAPI_CNLEN + 32];

    if (pszServer != NULL && pszServer[0] != '\0')
        sprintf(szLabel, "Shares on %s:", pszServer);
    else
        strcpy(szLabel, "Shares:");

    WinSetDlgItemText(hwnd, IDC_NET_SHARES_LABEL, (PCSZ)szLabel);
}

/*------------------------------------------------------------------
  PopulateShares - the disk shares on one computer
------------------------------------------------------------------*/
static void PopulateShares(HWND hwnd, const char *pszServer)
{
    SHARE_INFO_1 *pShr;
    ULONG         cRead = 0, cTotal = 0, ulRet;
    char          szItem[NETAPI_NNLEN + 96];
    char          szServer[NETAPI_CNLEN + 8];
    int           i, nLen;

    WinSendDlgItemMsg(hwnd, IDC_NET_SHARES, LM_DELETEALL, MPVOID, MPVOID);

    if (!LoadNetApi() || s_pfnShareEnum == NULL)
        return;

    strncpy(szServer, pszServer, sizeof(szServer) - 1);
    szServer[sizeof(szServer) - 1] = '\0';

    WinSetPointer(HWND_DESKTOP,
                  WinQuerySysPointer(HWND_DESKTOP, SPTR_WAIT, FALSE));

    memset(s_abEnum, 0, sizeof(s_abEnum));
    ulRet = s_pfnShareEnum((PSZ)szServer, 1UL,
                           s_abEnum, sizeof(s_abEnum), &cRead, &cTotal);

    if ((ulRet == NO_ERROR || ulRet == ERROR_MORE_DATA) && cRead > 0) {

        pShr = (SHARE_INFO_1 *)s_abEnum;

        for (i = 0; i < (int)cRead; i++) {

            if (pShr[i].shi1_type != 0)                 /* disk shares only */
                continue;
            if (pShr[i].shi1_netname[0] == '\0')
                continue;

            /* Skip the hidden administrative shares */
            nLen = (int)strlen((const char *)pShr[i].shi1_netname);
            if (nLen > 0 && pShr[i].shi1_netname[nLen - 1] == '$')
                continue;

            if (pShr[i].shi1_remark != NULL &&
                pShr[i].shi1_remark[0] != '\0')
                sprintf(szItem, "%s\t%s",
                        (const char *)pShr[i].shi1_netname,
                        (const char *)pShr[i].shi1_remark);
            else
                strcpy(szItem, (const char *)pShr[i].shi1_netname);

            WinSendDlgItemMsg(hwnd, IDC_NET_SHARES, LM_INSERTITEM,
                              MPFROMLONG(LIT_END), MPFROMP(szItem));
        }
    }

    WinSetPointer(HWND_DESKTOP,
                  WinQuerySysPointer(HWND_DESKTOP, SPTR_ARROW, FALSE));

    if (ListItemCount(hwnd, IDC_NET_SHARES) == 0) {
        if (ulRet != NO_ERROR && ulRet != ERROR_MORE_DATA) {
            sprintf(szItem, "(error %lu)", (unsigned long)ulRet);
            WinSendDlgItemMsg(hwnd, IDC_NET_SHARES, LM_INSERTITEM,
                              MPFROMLONG(LIT_END), MPFROMP(szItem));
        }
        else {
            WinSendDlgItemMsg(hwnd, IDC_NET_SHARES, LM_INSERTITEM,
                              MPFROMLONG(LIT_END),
                              MPFROMP("(no shares found)"));
        }
    }
}

/*------------------------------------------------------------------
  CollapseWorkgroup - drop the indented computers under nIdx
------------------------------------------------------------------*/
static void CollapseWorkgroup(HWND hwnd, int nIdx)
{
    char szItem[256];
    char szNext[256];
    int  nCount, nNext;

    ListItemText(hwnd, IDC_NET_BROWSE, nIdx, szItem, sizeof(szItem));
    if (szItem[0] != '-')
        return;

    szItem[0] = '+';
    ListDelete(hwnd, IDC_NET_BROWSE, nIdx);
    ListInsert(hwnd, IDC_NET_BROWSE, nIdx, szItem);

    nCount = ListItemCount(hwnd, IDC_NET_BROWSE);
    nNext  = nIdx + 1;

    while (nNext < nCount) {

        ListItemText(hwnd, IDC_NET_BROWSE, nNext, szNext, sizeof(szNext));

        if (szNext[0] != ' ')
            break;                      /* the next workgroup */

        ListDelete(hwnd, IDC_NET_BROWSE, nNext);
        nCount--;                       /* items shift up; nNext stays */
    }
}

/*------------------------------------------------------------------
  ExpandWorkgroup - list the computers in the workgroup at nIdx
------------------------------------------------------------------*/
static void ExpandWorkgroup(HWND hwnd, int nIdx)
{
    char  szItem[256];
    char  szOther[256];
    char  szWgrp[64];
    char  szMaster[64];
    char  szComp[256];
    ULONG cRead = 0, cTotal = 0, ulRet;
    int   i, k, nInsert, nCount;

    ListItemText(hwnd, IDC_NET_BROWSE, nIdx, szItem, sizeof(szItem));
    if (szItem[0] != '+')
        return;

    if (!LoadNetApi() || s_pfnServerEnum2 == NULL)
        return;

    /* Only one workgroup open at a time: collapse whichever is */
    nCount = ListItemCount(hwnd, IDC_NET_BROWSE);

    for (k = 0; k < nCount; k++) {

        ListItemText(hwnd, IDC_NET_BROWSE, k, szOther, sizeof(szOther));

        if (szOther[0] == '-') {

            int nWas = nIdx;

            CollapseWorkgroup(hwnd, k);

            /* Collapsing above us shifts our index down by what went */
            if (k < nWas)
                nIdx -= (nCount - ListItemCount(hwnd, IDC_NET_BROWSE));

            break;
        }
    }

    ListItemText(hwnd, IDC_NET_BROWSE, nIdx, szItem, sizeof(szItem));
    if (szItem[0] != '+')
        return;

    szItem[0] = '-';
    ListDelete(hwnd, IDC_NET_BROWSE, nIdx);
    ListInsert(hwnd, IDC_NET_BROWSE, nIdx, szItem);

    strncpy(szWgrp, szItem + 1, sizeof(szWgrp) - 1);
    szWgrp[sizeof(szWgrp) - 1] = '\0';

    WinSetPointer(HWND_DESKTOP,
                  WinQuerySysPointer(HWND_DESKTOP, SPTR_WAIT, FALSE));

    /* Ask the workgroup's master browser, if we can find it */
    szMaster[0] = '\0';
    if (s_pfnGetDCName != NULL)
        s_pfnGetDCName(NULL, (PSZ)szWgrp,
                       (PBYTE)szMaster, sizeof(szMaster));

    nInsert = nIdx + 1;

    memset(s_abEnum, 0, sizeof(s_abEnum));
    ulRet = s_pfnServerEnum2(szMaster[0] ? (PSZ)szMaster : NULL, 1UL,
                             s_abEnum, sizeof(s_abEnum),
                             &cRead, &cTotal, SV_TYPE_ALL, (PSZ)szWgrp);

    if ((ulRet == NO_ERROR || ulRet == ERROR_MORE_DATA) && cRead > 0) {

        SERVER_INFO_1 *pSI = (SERVER_INFO_1 *)s_abEnum;

        for (i = 0; i < (int)cRead; i++) {

            if (pSI[i].sv1_name[0] == '\0')
                continue;

            if (pSI[i].sv1_comment != NULL && pSI[i].sv1_comment[0] != '\0')
                sprintf(szComp, "  \\\\%s\t%s",
                        (const char *)pSI[i].sv1_name,
                        (const char *)pSI[i].sv1_comment);
            else
                sprintf(szComp, "  \\\\%s", (const char *)pSI[i].sv1_name);

            ListInsert(hwnd, IDC_NET_BROWSE, nInsert, szComp);
            nInsert++;
        }
    }
    else {
        /* Fall back to level 0: names only */
        cRead = cTotal = 0;
        memset(s_abEnum, 0, sizeof(s_abEnum));

        ulRet = s_pfnServerEnum2(szMaster[0] ? (PSZ)szMaster : NULL, 0UL,
                                 s_abEnum, sizeof(s_abEnum),
                                 &cRead, &cTotal, SV_TYPE_ALL, (PSZ)szWgrp);

        if ((ulRet == NO_ERROR || ulRet == ERROR_MORE_DATA) && cRead > 0) {

            SERVER_INFO_0 *pSI = (SERVER_INFO_0 *)s_abEnum;

            for (i = 0; i < (int)cRead; i++) {

                if (pSI[i].sv0_name[0] == '\0')
                    continue;

                sprintf(szComp, "  \\\\%s", (const char *)pSI[i].sv0_name);
                ListInsert(hwnd, IDC_NET_BROWSE, nInsert, szComp);
                nInsert++;
            }
        }
    }

    WinSetPointer(HWND_DESKTOP,
                  WinQuerySysPointer(HWND_DESKTOP, SPTR_ARROW, FALSE));

    if (nInsert == nIdx + 1)
        ListInsert(hwnd, IDC_NET_BROWSE, nInsert, "  (no computers found)");
}

/*------------------------------------------------------------------
  PopulateBrowseWorkgroups - every visible workgroup, collapsed
------------------------------------------------------------------*/
static void PopulateBrowseWorkgroups(HWND hwnd)
{
    SERVER_INFO_0 *pSI;
    ULONG          cRead = 0, cTotal = 0, ulRet;
    char           szItem[NETAPI_CNLEN + 8];
    int            i;

    WinSendDlgItemMsg(hwnd, IDC_NET_BROWSE, LM_DELETEALL, MPVOID, MPVOID);

    if (!LoadNetApi() || s_pfnServerEnum2 == NULL) {
        WinSendDlgItemMsg(hwnd, IDC_NET_BROWSE, LM_INSERTITEM,
                          MPFROMLONG(LIT_END),
                          MPFROMP("(No network browsing available)"));
        return;
    }

    memset(s_abEnum, 0, sizeof(s_abEnum));
    ulRet = s_pfnServerEnum2(NULL, 0UL, s_abEnum, sizeof(s_abEnum),
                             &cRead, &cTotal, SV_TYPE_DOMAIN_ENUM, NULL);

    if ((ulRet == NO_ERROR || ulRet == ERROR_MORE_DATA) && cRead > 0) {

        pSI = (SERVER_INFO_0 *)s_abEnum;

        for (i = 0; i < (int)cRead; i++) {

            if (pSI[i].sv0_name[0] == '\0')
                continue;

            sprintf(szItem, "+%s", (const char *)pSI[i].sv0_name);

            if ((LONG)WinSendDlgItemMsg(hwnd, IDC_NET_BROWSE,
                                        LM_SEARCHSTRING,
                                        MPFROM2SHORT(LSS_CASESENSITIVE,
                                                     LIT_FIRST),
                                        MPFROMP(szItem)) == LIT_NONE) {
                WinSendDlgItemMsg(hwnd, IDC_NET_BROWSE, LM_INSERTITEM,
                                  MPFROMLONG(LIT_END), MPFROMP(szItem));
            }
        }
    }

    if (ListItemCount(hwnd, IDC_NET_BROWSE) == 0)
        WinSendDlgItemMsg(hwnd, IDC_NET_BROWSE, LM_INSERTITEM,
                          MPFROMLONG(LIT_END),
                          MPFROMP("(no workgroups found)"));
}

/*==================================================================
 * Reconnect at startup
 *==================================================================*/

void ReconnectNetDrives(HWND hwnd)
{
    char szKey[4];
    char szPath[CCHMAXPATH];
    char szLocal[8];
    int  i;

    (void)hwnd;

    if (!IsNetworkPresent())
        return;

    for (i = 0; i < 26; i++) {

        szKey[0] = (char)('A' + i);
        szKey[1] = '\0';
        szPath[0] = '\0';

        PrfQueryProfileString(HINI_USERPROFILE, (PCSZ)INI_APP_RECONNECT,
                              (PCSZ)szKey, (PCSZ)"",
                              (PVOID)szPath, sizeof(szPath));

        if (szPath[0] == '\0')
            continue;

        sprintf(szLocal, "%c:", (char)('A' + i));

        /* A failure here is not worth a dialog at start-up: the share may
           simply be offline, and the user can map it by hand. */
        MapDrive(szLocal, szPath);
    }
}

/*==================================================================
 * Connect dialog
 *==================================================================*/

static MRESULT EXPENTRY ConnectNetDriveDlgProc(HWND hwnd, ULONG msg,
                                               MPARAM mp1, MPARAM mp2)
{
    switch (msg) {

    case WM_INITDLG: {
        char szItem[CCHMAXPATH + 16];
        char szNetPath[CCHMAXPATH];
        char szVal[8];
        int  i, nFirstFree = -1, nSelIdx = 0, nItems;

        s_szCurrentComp[0] = '\0';

        for (i = 0; i < 26; i++) {

            char       cLetter = (char)('A' + i);
            DRIVEINFO *pDrv    = FindDriveInfo(cLetter);

            if (pDrv == NULL) {
                /* Free letter: offer it */
                sprintf(szItem, "%c:", cLetter);
                WinSendDlgItemMsg(hwnd, IDC_NET_LOCALDRIVE, LM_INSERTITEM,
                                  MPFROMLONG(LIT_END), MPFROMP(szItem));
                if (nFirstFree < 0 && cLetter >= 'D')
                    nFirstFree = i;
            }
            else if (pDrv->nType == DRV_REMOTE) {
                /* Already mapped: show what to */
                if (QueryConnection(cLetter, szNetPath, sizeof(szNetPath)))
                    sprintf(szItem, "%c: [%s]", cLetter, szNetPath);
                else
                    sprintf(szItem, "%c:", cLetter);

                WinSendDlgItemMsg(hwnd, IDC_NET_LOCALDRIVE, LM_INSERTITEM,
                                  MPFROMLONG(LIT_END), MPFROMP(szItem));
            }
            /* local fixed / removable / CD: not offered */
        }

        /* Pre-select the first free letter at D: or later */
        nItems = ListItemCount(hwnd, IDC_NET_LOCALDRIVE);

        if (nFirstFree >= 0 && nItems > 0) {

            char cTarget = (char)('A' + nFirstFree);
            int  k;

            for (k = 0; k < nItems; k++) {
                ListItemText(hwnd, IDC_NET_LOCALDRIVE, k,
                             szItem, sizeof(szItem));
                if (szItem[0] == cTarget) {
                    nSelIdx = k;
                    break;
                }
            }
        }

        WinSendDlgItemMsg(hwnd, IDC_NET_LOCALDRIVE, LM_SELECTITEM,
                          MPFROMLONG((LONG)nSelIdx), MPFROMSHORT(TRUE));

        LoadNetHistory(hwnd, IDC_NET_PATH);

        szVal[0] = '\0';
        PrfQueryProfileString(HINI_USERPROFILE, (PCSZ)INI_APP_HISTORY,
                              (PCSZ)"ReconnectAtStartup", (PCSZ)"0",
                              (PVOID)szVal, sizeof(szVal));
        WinCheckButton(hwnd, IDC_RECONNECTATSTART,
                       (USHORT)(szVal[0] == '1' ? 1 : 0));

        PopulateBrowseWorkgroups(hwnd);

        SetSharesLabel(hwnd, NULL);
        WinSendDlgItemMsg(hwnd, IDC_NET_SHARES, LM_DELETEALL,
                          MPVOID, MPVOID);
        return (MRESULT)FALSE;
    }

    case WM_CONTROL: {
        USHORT idCtrl  = SHORT1FROMMP(mp1);
        USHORT usEvent = SHORT2FROMMP(mp1);

        if (idCtrl == IDC_NET_BROWSE) {

            char szItem[256];
            LONG nSel;

            if (usEvent != LN_SELECT && usEvent != LN_ENTER)
                break;

            nSel = (LONG)WinSendDlgItemMsg(hwnd, IDC_NET_BROWSE,
                                           LM_QUERYSELECTION,
                                           MPFROMLONG(LIT_FIRST), MPVOID);
            if (nSel == LIT_NONE)
                break;

            ListItemText(hwnd, IDC_NET_BROWSE, (int)nSel,
                         szItem, sizeof(szItem));

            if (usEvent == LN_SELECT) {
                /* A computer: fill the path and list its shares */
                if (szItem[0] == ' ' && szItem[1] == ' ') {

                    char szComp[NETAPI_CNLEN + 8];

                    if (ParseComputerName(szItem, szComp, sizeof(szComp))) {
                        strcpy(s_szCurrentComp, szComp);
                        WinSetDlgItemText(hwnd, IDC_NET_PATH, (PCSZ)szComp);
                        SetSharesLabel(hwnd, szComp);
                        PopulateShares(hwnd, szComp);
                    }
                }
                break;
            }

            /* LN_ENTER (double click): expand or collapse a workgroup */
            if (szItem[0] == '+')
                ExpandWorkgroup(hwnd, (int)nSel);
            else if (szItem[0] == '-')
                CollapseWorkgroup(hwnd, (int)nSel);

            break;
        }

        if (idCtrl == IDC_NET_SHARES && usEvent == LN_SELECT) {

            char  szShare[256];
            char  szPath[CCHMAXPATH];
            char *pTab;
            LONG  nSel;

            nSel = (LONG)WinSendDlgItemMsg(hwnd, IDC_NET_SHARES,
                                           LM_QUERYSELECTION,
                                           MPFROMLONG(LIT_FIRST), MPVOID);
            if (nSel == LIT_NONE)
                break;

            ListItemText(hwnd, IDC_NET_SHARES, (int)nSel,
                         szShare, sizeof(szShare));

            pTab = strchr(szShare, '\t');
            if (pTab != NULL)
                *pTab = '\0';

            if (s_szCurrentComp[0] != '\0' && szShare[0] != '(') {
                sprintf(szPath, "%s\\%s", s_szCurrentComp, szShare);
                WinSetDlgItemText(hwnd, IDC_NET_PATH, (PCSZ)szPath);
            }
            break;
        }
        break;
    }

    case WM_COMMAND:
        switch (SHORT1FROMMP(mp1)) {

        case DID_OK: {
            char  szDriveEntry[CCHMAXPATH + 16];
            char  szPath[CCHMAXPATH];
            char  szLocalName[8];
            ULONG ulRet;
            LONG  nSel;

            szDriveEntry[0] = '\0';

            nSel = (LONG)WinSendDlgItemMsg(hwnd, IDC_NET_LOCALDRIVE,
                                           LM_QUERYSELECTION,
                                           MPFROMLONG(LIT_FIRST), MPVOID);
            if (nSel != LIT_NONE)
                ListItemText(hwnd, IDC_NET_LOCALDRIVE, (int)nSel,
                             szDriveEntry, sizeof(szDriveEntry));
            else
                WinQueryDlgItemText(hwnd, IDC_NET_LOCALDRIVE,
                                    (LONG)sizeof(szDriveEntry),
                                    (PSZ)szDriveEntry);

            if (szDriveEntry[0] == '\0') {
                WinMessageBox(HWND_DESKTOP, hwnd,
                              (PCSZ)"Please select a drive letter.",
                              (PCSZ)"Connect Network Drive", 0,
                              MB_OK | MB_WARNING);
                return (MRESULT)FALSE;
            }

            sprintf(szLocalName, "%c:", szDriveEntry[0]);

            WinQueryDlgItemText(hwnd, IDC_NET_PATH,
                                (LONG)sizeof(szPath), (PSZ)szPath);

            if (szPath[0] == '\0') {
                WinMessageBox(HWND_DESKTOP, hwnd,
                              (PCSZ)"Please enter a network path "
                                    "(for example \\\\server\\share).",
                              (PCSZ)"Connect Network Drive", 0,
                              MB_OK | MB_WARNING);
                return (MRESULT)FALSE;
            }

            ulRet = MapDrive(szLocalName, szPath);

            if (ulRet == NO_ERROR) {

                BOOL bReconnect = (BOOL)WinQueryButtonCheckstate(
                                            hwnd, IDC_RECONNECTATSTART);

                SaveNetHistory(szPath);
                SaveReconnectEntry(szLocalName, szPath, bReconnect);
                WinDismissDlg(hwnd, DID_OK);
            }
            else {
                ReportUseError(hwnd, ulRet, szLocalName);
            }

            return (MRESULT)FALSE;
        }

        case DID_CANCEL:
            WinDismissDlg(hwnd, DID_CANCEL);
            return (MRESULT)FALSE;
        }
        break;
    }

    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

/*==================================================================
 * Disconnect
 *==================================================================*/

/*------------------------------------------------------------------
  DropDrive - NetUseDel with the open-files prompt

    NetUseDel refuses with NERR_OpenFiles rather than closing somebody's
    file behind their back; USE_FORCE is what the Yes answer means.
------------------------------------------------------------------*/
static void DropDrive(HWND hwnd, const char *pszLocal, const char *pszRemote)
{
    char  szMsg[CCHMAXPATH + 160];
    ULONG ulRet;

    ulRet = UnmapDrive(pszLocal, FALSE);

    if (ulRet == NERR_OpenFiles) {

        if (pszRemote != NULL && pszRemote[0] != '\0')
            sprintf(szMsg, "Drive %s (%s) has open files.\n"
                           "Force disconnect anyway?", pszLocal, pszRemote);
        else
            sprintf(szMsg, "Drive %s has open files.\n"
                           "Force disconnect anyway?", pszLocal);

        if (WinMessageBox(HWND_DESKTOP, hwnd, (PCSZ)szMsg,
                          (PCSZ)"Disconnect Network Drive", 0,
                          MB_YESNO | MB_WARNING) == MBID_YES) {
            UnmapDrive(pszLocal, TRUE);
            SaveReconnectEntry(pszLocal, "", FALSE);
        }
    }
    else if (ulRet == NO_ERROR || ulRet == NERR_UseNotFound) {
        SaveReconnectEntry(pszLocal, "", FALSE);
    }
    else {
        sprintf(szMsg, "Could not disconnect drive %s (error %lu).",
                pszLocal, (unsigned long)ulRet);
        WinMessageBox(HWND_DESKTOP, hwnd, (PCSZ)szMsg,
                      (PCSZ)"Disconnect Network Drive", 0,
                      MB_OK | MB_WARNING);
    }
}

static MRESULT EXPENTRY DisconnectNetDriveDlgProc(HWND hwnd, ULONG msg,
                                                  MPARAM mp1, MPARAM mp2)
{
    switch (msg) {

    case WM_INITDLG: {
        int i, nCount = GetDriveCount();

        for (i = 0; i < nCount; i++) {

            DRIVEINFO *pDrv = GetDriveInfo(i);
            char       szNetPath[CCHMAXPATH];
            char       szItem[CCHMAXPATH + 16];

            if (pDrv == NULL || pDrv->nType != DRV_REMOTE)
                continue;

            if (QueryConnection(pDrv->szLabel[0], szNetPath,
                                sizeof(szNetPath)))
                sprintf(szItem, "%c: %s", pDrv->szLabel[0], szNetPath);
            else
                sprintf(szItem, "%c:", pDrv->szLabel[0]);

            WinSendDlgItemMsg(hwnd, IDC_NET_DISCONNLIST, LM_INSERTITEM,
                              MPFROMLONG(LIT_END), MPFROMP(szItem));
        }

        if (ListItemCount(hwnd, IDC_NET_DISCONNLIST) == 0)
            WinEnableWindow(WinWindowFromID(hwnd, DID_OK), FALSE);

        return (MRESULT)FALSE;
    }

    case WM_COMMAND:
        switch (SHORT1FROMMP(mp1)) {

        case DID_OK: {
            LONG nSel = LIT_FIRST;

            /* Walk the multiple selection: each query takes the previous
               index and returns the next selected one. */
            for (;;) {

                char szItem[CCHMAXPATH + 16];
                char szDrive[8];
                char szRemote[CCHMAXPATH];

                nSel = (LONG)WinSendDlgItemMsg(hwnd, IDC_NET_DISCONNLIST,
                                               LM_QUERYSELECTION,
                                               MPFROMLONG((LONG)nSel),
                                               MPVOID);
                if (nSel == LIT_NONE)
                    break;

                ListItemText(hwnd, IDC_NET_DISCONNLIST, (int)nSel,
                             szItem, sizeof(szItem));

                sprintf(szDrive, "%c:", szItem[0]);

                if (!QueryConnection(szItem[0], szRemote, sizeof(szRemote)))
                    szRemote[0] = '\0';

                DropDrive(hwnd, szDrive, szRemote);
            }

            WinDismissDlg(hwnd, DID_OK);
            return (MRESULT)FALSE;
        }

        case DID_CANCEL:
            WinDismissDlg(hwnd, DID_CANCEL);
            return (MRESULT)FALSE;
        }
        break;
    }

    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

/*==================================================================
 * Entry points
 *==================================================================*/

void OpenConnectNetDrive(HWND hwnd)
{
    WinDlgBox(HWND_DESKTOP, hwnd, ConnectNetDriveDlgProc,
              NULLHANDLE, IDD_CONNECTNETDRIVE, NULL);
}

void OpenDisconnectNetDrive(HWND hwnd)
{
    WinDlgBox(HWND_DESKTOP, hwnd, DisconnectNetDriveDlgProc,
              NULLHANDLE, IDD_DISCONNECTNETDRIVE, NULL);
}

void DisconnectNetDriveDirect(HWND hwnd, char cDrive)
{
    char szDrive[8];
    char szRemote[CCHMAXPATH];

    sprintf(szDrive, "%c:", cDrive);

    if (!QueryConnection(cDrive, szRemote, sizeof(szRemote)))
        szRemote[0] = '\0';

    DropDrive(hwnd, szDrive, szRemote);
}
